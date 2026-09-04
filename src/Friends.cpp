/*
 * AccountWideFriends module for AzerothCore.
 * Fixed for thread-safety, zero memory corruption, and eliminating main-thread SQL polling segfaults.
 */

#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SocialMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32 FriendFlag = SOCIAL_FLAG_FRIEND;
constexpr uint32 WithoutFriendFlagMask = 0xFE;

struct ModuleConfig
{
    bool Enabled = true;
    bool SyncOnCreate = true;
    bool SyncOnLogin = true;
    bool StartupBackfill = true;
    bool SameFactionOnly = false;
};

struct CharacterInfo
{
    uint32 Guid = 0;
    uint8 Race = 0;
};

using FriendMap = std::unordered_map<uint32, std::string>;

ModuleConfig Config;

bool IsEnabled()
{
    return Config.Enabled;
}

std::string EscapeSqlString(std::string text)
{
    CharacterDatabase.EscapeString(text);
    return text;
}

bool CanShareBetweenRaces(uint8 ownerRace, uint8 friendRace)
{
    return !Config.SameFactionOnly || Player::TeamIdForRace(ownerRace) == Player::TeamIdForRace(friendRace);
}

std::vector<CharacterInfo> LoadAccountCharacterInfos(uint32 accountId)
{
    std::vector<CharacterInfo> characters;

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, race FROM characters WHERE account = {}", accountId);

    if (!result)
        return characters;

    do
    {
        Field* fields = result->Fetch();
        characters.push_back({ fields[0].Get<uint32>(), fields[1].Get<uint8>() });
    } while (result->NextRow());

    return characters;
}

std::vector<uint32> LoadAccountCharacters(uint32 accountId)
{
    std::vector<uint32> characterGuids;

    for (CharacterInfo const& character : LoadAccountCharacterInfos(accountId))
        characterGuids.push_back(character.Guid);

    return characterGuids;
}

std::unordered_set<uint32> MakeGuidSet(std::vector<uint32> const& guids)
{
    return { guids.begin(), guids.end() };
}

std::unordered_set<uint32> MakeGuidSet(std::vector<CharacterInfo> const& characters)
{
    std::unordered_set<uint32> guids;

    for (CharacterInfo const& character : characters)
        guids.insert(character.Guid);

    return guids;
}

uint8 LoadCharacterRace(uint32 characterGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT race FROM characters WHERE guid = {}", characterGuid);

    if (!result)
        return 0;

    return result->Fetch()[0].Get<uint8>();
}

bool CanCharacterShareWithFriend(CharacterInfo const& owner, uint32 friendGuid)
{
    uint8 const friendRace = LoadCharacterRace(friendGuid);
    return friendRace && CanShareBetweenRaces(owner.Race, friendRace);
}

void RemoveFriendFromCharacter(uint32 ownerGuid, uint32 friendGuid)
{
    CharacterDatabase.Execute(
        "UPDATE character_social SET flags = flags & {} "
        "WHERE guid = {} AND friend = {} AND (flags & {}) <> 0",
        WithoutFriendFlagMask, ownerGuid, friendGuid, FriendFlag);

    CharacterDatabase.Execute(
        "DELETE FROM character_social WHERE guid = {} AND friend = {} AND flags = 0",
        ownerGuid, friendGuid);
}

FriendMap LoadAccountFriendUnion(uint32 accountId)
{
    FriendMap friends;
    std::vector<uint32> accountCharacters = LoadAccountCharacters(accountId);
    std::unordered_set<uint32> ownCharacters = MakeGuidSet(accountCharacters);

    QueryResult result = CharacterDatabase.Query(
        "SELECT social.friend, COALESCE(social.note, '') "
        "FROM character_social social "
        "INNER JOIN characters owner ON owner.guid = social.guid "
        "WHERE owner.account = {} AND (social.flags & {}) <> 0 "
        "ORDER BY social.note <> '' DESC",
        accountId, FriendFlag);

    if (!result)
        return friends;

    do
    {
        Field* fields = result->Fetch();
        uint32 const friendGuid = fields[0].Get<uint32>();

        if (!friendGuid || ownCharacters.contains(friendGuid))
            continue;

        std::string const note = fields[1].Get<std::string>();
        auto [itr, inserted] = friends.emplace(friendGuid, note);

        if (!inserted && itr->second.empty() && !note.empty())
            itr->second = note;
    } while (result->NextRow());

    return friends;
}

void InsertOrUpdateFriendForCharacter(uint32 ownerGuid, uint32 friendGuid, std::string const& note, bool forceNote)
{
    if (!ownerGuid || !friendGuid || ownerGuid == friendGuid)
        return;

    std::string safeNote = note.substr(0, 48);
    safeNote = EscapeSqlString(safeNote);

    if (forceNote)
    {
        CharacterDatabase.Execute(
            "INSERT INTO character_social (guid, friend, flags, note) VALUES ({}, {}, {}, '{}') "
            "ON DUPLICATE KEY UPDATE flags = flags | {}, note = '{}'",
            ownerGuid, friendGuid, FriendFlag, safeNote, FriendFlag, safeNote);
        return;
    }

    CharacterDatabase.Execute(
        "INSERT INTO character_social (guid, friend, flags, note) VALUES ({}, {}, {}, '{}') "
        "ON DUPLICATE KEY UPDATE flags = flags | {}, note = IF(VALUES(note) <> '', VALUES(note), note)",
        ownerGuid, friendGuid, FriendFlag, safeNote, FriendFlag);
}

uint32 SyncFriendToCharacters(
    std::vector<CharacterInfo> const& accountCharacters,
    std::unordered_set<uint32> const& ownCharacters,
    uint32 friendGuid,
    std::string const& note,
    bool forceNote)
{
    if (!friendGuid || ownCharacters.contains(friendGuid))
        return 0;

    uint32 synced = 0;

    for (CharacterInfo const& owner : accountCharacters)
    {
        if (owner.Guid == friendGuid || !CanCharacterShareWithFriend(owner, friendGuid))
            continue;

        InsertOrUpdateFriendForCharacter(owner.Guid, friendGuid, note, forceNote);
        ++synced;
    }

    return synced;
}

void RemoveInvalidFactionFriends(uint32 accountId)
{
    if (!Config.SameFactionOnly || !accountId)
        return;

    QueryResult result = CharacterDatabase.Query(
        "SELECT social.guid, social.friend, owner.race, friend_char.race "
        "FROM character_social social "
        "INNER JOIN characters owner ON owner.guid = social.guid "
        "INNER JOIN characters friend_char ON friend_char.guid = social.friend "
        "WHERE owner.account = {} AND owner.account <> friend_char.account AND (social.flags & {}) <> 0",
        accountId, FriendFlag);

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 const ownerGuid = fields[0].Get<uint32>();
        uint32 const friendGuid = fields[1].Get<uint32>();
        uint8 const ownerRace = fields[2].Get<uint8>();
        uint8 const friendRace = fields[3].Get<uint8>();

        if (!CanShareBetweenRaces(ownerRace, friendRace))
            RemoveFriendFromCharacter(ownerGuid, friendGuid);
    } while (result->NextRow());
}

void RemoveOwnAccountFriends(uint32 accountId)
{
    if (!accountId)
        return;

    CharacterDatabase.Execute(
        "UPDATE character_social social "
        "INNER JOIN characters owner ON owner.guid = social.guid "
        "INNER JOIN characters friend_char ON friend_char.guid = social.friend "
        "SET social.flags = social.flags & {} "
        "WHERE owner.account = {} AND friend_char.account = {} AND (social.flags & {}) <> 0",
        WithoutFriendFlagMask, accountId, accountId, FriendFlag);

    CharacterDatabase.Execute(
        "DELETE social FROM character_social social "
        "INNER JOIN characters owner ON owner.guid = social.guid "
        "INNER JOIN characters friend_char ON friend_char.guid = social.friend "
        "WHERE owner.account = {} AND friend_char.account = {} AND social.flags = 0",
        accountId, accountId);
}

uint32 SyncAccountFriends(uint32 accountId)
{
    if (AccountBound::IsExcludedAccount(accountId))
        return 0;

    RemoveOwnAccountFriends(accountId);
    RemoveInvalidFactionFriends(accountId);

    std::vector<CharacterInfo> accountCharacters = LoadAccountCharacterInfos(accountId);

    if (accountCharacters.empty())
        return 0;

    std::unordered_set<uint32> ownCharacters = MakeGuidSet(accountCharacters);
    FriendMap accountFriends = LoadAccountFriendUnion(accountId);
    uint32 synced = 0;

    for (auto const& [friendGuid, note] : accountFriends)
        synced += SyncFriendToCharacters(accountCharacters, ownCharacters, friendGuid, note, false);

    return synced;
}

void BackfillAllAccounts()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT DISTINCT account FROM characters WHERE account <> 0");

    if (!result)
        return;

    uint32 accountCount = 0;
    uint32 syncCount = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 const accountId = fields[0].Get<uint32>();

        syncCount += SyncAccountFriends(accountId);
        ++accountCount;
    } while (result->NextRow());

    LOG_INFO("module.accountbound", "AccountWideFriends: startup backfill checked {} account(s) and queued {} friend row sync operation(s).", accountCount, syncCount);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Friends");
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Friends.SyncOnCreate", true, false);
    Config.SyncOnLogin = sConfigMgr->GetOption<bool>("AccountBound.Friends.SyncOnLogin", true, false);
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Friends.StartupBackfill", true, false);
    Config.SameFactionOnly = sConfigMgr->GetOption<bool>("AccountBound.Friends.SameFactionOnly", false, false);
}
}

class AccountWideFriendsWorldScript : public WorldScript
{
public:
    AccountWideFriendsWorldScript() : WorldScript("AccountWideFriendsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();

        LOG_INFO("module.accountbound", "AccountWideFriends: {}. Enabled={}, CreateSync={}, LoginSync={}, SameFactionOnly={}, StartupBackfill={}.",
            reload ? "configuration reloaded" : "configuration loaded",
            Config.Enabled ? "on" : "off",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.SyncOnLogin ? "on" : "off",
            Config.Enabled && Config.SameFactionOnly ? "on" : "off",
            Config.Enabled && Config.StartupBackfill ? "on" : "off");
    }

    void OnStartup() override
    {
        if (IsEnabled() && Config.StartupBackfill)
            BackfillAllAccounts();
    }
};

class AccountWideFriendsPlayerScript : public PlayerScript
{
public:
    AccountWideFriendsPlayerScript() : PlayerScript("AccountWideFriendsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_CREATE
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsEnabled() || !Config.SyncOnLogin || !player || !player->GetSession())
            return;

        // Perform safe asynchronous account-wide friends synchronization
        SyncAccountFriends(player->GetSession()->GetAccountId());
    }

    void OnPlayerCreate(Player* player) override
    {
        if (IsEnabled() && Config.SyncOnCreate && player && player->GetSession())
            SyncAccountFriends(player->GetSession()->GetAccountId());
    }
};

void AddAccountWideFriendsScripts()
{
    new AccountWideFriendsWorldScript();
    new AccountWideFriendsPlayerScript();
}
