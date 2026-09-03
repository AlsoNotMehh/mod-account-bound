/*
 * Account-bound character titles for AzerothCore.
 */

#include "AchievementMgr.h"
#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "StringFormat.h"
#include "WorldScript.h"

#include <array>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using KnownTitlesMask = std::array<uint32, KNOWN_TITLES_SIZE * 2>;

struct ModuleConfig
{
    bool Enabled = true;
    bool StartupBackfill = true;
    bool SyncOnCreate = true;
    bool SyncOnSave = true;
    bool SyncRealmFirst = false;
    bool ConvertFactionSpecific = true;
    AccountBound::IdFilter Filter;
};

ModuleConfig Config;
std::unordered_map<uint32, uint32> TitleIdByBitIndex;
std::unordered_set<uint32> RealmFirstTitleIds;
std::unordered_map<uint32, KnownTitlesMask> LastKnownTitlesByCharacter;

bool IsRealmFirstAchievement(AchievementEntry const* achievement)
{
    return achievement && (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL));
}

bool IsTitleAllowed(uint32 titleId)
{
    return Config.Filter.Allows(titleId) &&
        (Config.SyncRealmFirst || !RealmFirstTitleIds.contains(titleId));
}

void BuildTitleCache()
{
    TitleIdByBitIndex.clear();
    RealmFirstTitleIds =
    {
        // Restored beta/datamined Realm First titles used by RealmFirstTitles.
        85, 86, 87, 89, 90, 91, 92, 93, 94, 95,
        96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
        106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
        116, 117, 118, 119, 123, 156
    };

    for (uint32 titleId = 0; titleId < sCharTitlesStore.GetNumRows(); ++titleId)
        if (CharTitlesEntry const* title = sCharTitlesStore.LookupEntry(titleId))
            TitleIdByBitIndex.try_emplace(title->bit_index, titleId);

    for (uint32 achievementId = 0; achievementId < sAchievementStore.GetNumRows(); ++achievementId)
    {
        AchievementEntry const* achievement = sAchievementStore.LookupEntry(achievementId);
        if (!IsRealmFirstAchievement(achievement))
            continue;

        if (AchievementReward const* reward = sAchievementMgr->GetAchievementReward(achievement))
            for (uint32 titleId : reward->titleId)
                if (titleId)
                    RealmFirstTitleIds.insert(titleId);
    }

    LOG_INFO("module.accountboundtitles",
        "AccountBoundTitles: cached {} title bit mapping(s) and {} Realm First exclusion(s).",
        TitleIdByBitIndex.size(), RealmFirstTitleIds.size());
}

KnownTitlesMask ParseKnownTitles(std::string const& text)
{
    KnownTitlesMask titles = {};
    std::stringstream stream(text);

    for (uint32& value : titles)
        stream >> value;

    return titles;
}

KnownTitlesMask GetPlayerKnownTitles(Player const* player)
{
    KnownTitlesMask titles = {};
    if (!player)
        return titles;

    for (uint32 index = 0; index < titles.size(); ++index)
        titles[index] = player->GetUInt32Value(PLAYER__FIELD_KNOWN_TITLES + index);

    return titles;
}

std::string KnownTitlesToString(KnownTitlesMask const& titles)
{
    std::ostringstream stream;

    for (uint32 index = 0; index < titles.size(); ++index)
    {
        if (index)
            stream << ' ';

        stream << titles[index];
    }

    return stream.str();
}

bool HasKnownTitle(KnownTitlesMask const& titles, uint32 bitIndex)
{
    if (bitIndex >= titles.size() * 32)
        return false;

    return (titles[bitIndex / 32] & (1u << (bitIndex % 32))) != 0;
}

bool AddKnownTitle(KnownTitlesMask& titles, uint32 bitIndex)
{
    if (bitIndex >= titles.size() * 32)
        return false;

    uint32& mask = titles[bitIndex / 32];
    uint32 const flag = 1u << (bitIndex % 32);

    if (mask & flag)
        return false;

    mask |= flag;
    return true;
}

uint32 GetTitleForRace(uint32 titleId, uint8 sourceRace, uint8 targetRace)
{
    if (!IsTitleAllowed(titleId))
        return 0;

    TeamId const sourceTeam = Player::TeamIdForRace(sourceRace);
    TeamId const targetTeam = Player::TeamIdForRace(targetRace);

    if (sourceTeam == targetTeam)
        return titleId;

    for (auto const& [allianceTitle, hordeTitle] : sObjectMgr->FactionChangeTitles)
    {
        if (titleId == allianceTitle)
            return Config.ConvertFactionSpecific && targetTeam == TEAM_HORDE &&
                IsTitleAllowed(hordeTitle) ? hordeTitle : 0;

        if (titleId == hordeTitle)
            return Config.ConvertFactionSpecific && targetTeam == TEAM_ALLIANCE &&
                IsTitleAllowed(allianceTitle) ? allianceTitle : 0;
    }

    return titleId;
}

uint32 MergeTitlesForRace(KnownTitlesMask const& sourceTitles, uint8 sourceRace, uint8 targetRace, KnownTitlesMask& targetTitles)
{
    uint32 added = 0;

    for (uint32 sourceBitIndex = 0; sourceBitIndex < sourceTitles.size() * 32; ++sourceBitIndex)
    {
        if (!HasKnownTitle(sourceTitles, sourceBitIndex))
            continue;

        auto titleItr = TitleIdByBitIndex.find(sourceBitIndex);
        if (titleItr == TitleIdByBitIndex.end())
            continue;

        uint32 const targetTitleId = GetTitleForRace(titleItr->second, sourceRace, targetRace);
        CharTitlesEntry const* targetTitle = sCharTitlesStore.LookupEntry(targetTitleId);

        if (targetTitle && AddKnownTitle(targetTitles, targetTitle->bit_index))
            ++added;
    }

    return added;
}

void AppendOrCommit(CharacterDatabaseTransaction& trans, std::string_view sql)
{
    if (!trans)
        trans = CharacterDatabase.BeginTransaction();

    trans->Append(sql);

    if (trans->GetSize() >= 1000)
    {
        CharacterDatabase.DirectCommitTransaction(trans);
        trans = CharacterDatabase.BeginTransaction();
    }
}

void CommitIfNeeded(CharacterDatabaseTransaction& trans)
{
    if (trans && trans->GetSize())
        CharacterDatabase.DirectCommitTransaction(trans);
}

void UpdateTitlesForCharacter(CharacterDatabaseTransaction& trans, uint32 targetGuid, KnownTitlesMask const& titles)
{
    AppendOrCommit(trans, Acore::StringFormat(
        "UPDATE characters SET knownTitles = '{}' WHERE guid = {}",
        KnownTitlesToString(titles), targetGuid));
}

void BackfillTitlesForCharacter(Player* player)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    uint32 const accountId = player->GetSession()->GetAccountId();
    uint32 const targetGuid = player->GetGUID().GetCounter();
    uint8 const targetRace = player->getRace(true);
    KnownTitlesMask targetTitles = {};
    uint32 added = 0;
    QueryResult targetResult = CharacterDatabase.Query(
        "SELECT COALESCE(knownTitles, '') FROM characters WHERE guid = {}", targetGuid);

    if (targetResult)
        targetTitles = ParseKnownTitles(targetResult->Fetch()[0].Get<std::string>());

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, race, COALESCE(knownTitles, '') "
        "FROM characters WHERE account = {} AND guid <> {}",
        accountId, targetGuid);

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 const sourceRace = fields[1].Get<uint8>();
            KnownTitlesMask const sourceTitles = ParseKnownTitles(fields[2].Get<std::string>());
            added += MergeTitlesForRace(sourceTitles, sourceRace, targetRace, targetTitles);
        } while (result->NextRow());
    }

    if (added)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        UpdateTitlesForCharacter(trans, targetGuid, targetTitles);
        CommitIfNeeded(trans);

        LOG_INFO("module.accountboundtitles",
            "AccountBoundTitles: seeded {} account title(s) for character {}.",
            added, targetGuid);
    }
}

void SyncTitlesFromPlayerToAccount(Player* player)
{
    if (!Config.Enabled || !Config.SyncOnSave || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    uint32 const sourceGuid = player->GetGUID().GetCounter();
    uint8 const sourceRace = player->getRace(true);
    KnownTitlesMask const sourceTitles = GetPlayerKnownTitles(player);
    auto snapshotItr = LastKnownTitlesByCharacter.find(sourceGuid);

    if (snapshotItr != LastKnownTitlesByCharacter.end() && snapshotItr->second == sourceTitles)
        return;

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, race, COALESCE(knownTitles, '') "
        "FROM characters WHERE account = {} AND guid <> {}",
        player->GetSession()->GetAccountId(), sourceGuid);

    if (!result)
    {
        LastKnownTitlesByCharacter[sourceGuid] = sourceTitles;
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 updatedCharacters = 0;
    uint32 addedTitles = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 const targetGuid = fields[0].Get<uint32>();
        uint8 const targetRace = fields[1].Get<uint8>();
        KnownTitlesMask targetTitles = ParseKnownTitles(fields[2].Get<std::string>());
        uint32 const added = MergeTitlesForRace(sourceTitles, sourceRace, targetRace, targetTitles);

        if (!added)
            continue;

        UpdateTitlesForCharacter(trans, targetGuid, targetTitles);

        ++updatedCharacters;
        addedTitles += added;
    } while (result->NextRow());

    CommitIfNeeded(trans);
    LastKnownTitlesByCharacter[sourceGuid] = sourceTitles;

    if (updatedCharacters)
        LOG_INFO("module.accountboundtitles",
            "AccountBoundTitles: propagated {} title(s) from character {} to {} account character(s).",
            addedTitles, sourceGuid, updatedCharacters);
}

void BackfillAllTitles()
{
    if (!Config.Enabled || !Config.StartupBackfill)
        return;

    struct TitleCharacterInfo
    {
        uint32 Guid;
        uint8 Race;
        KnownTitlesMask Titles;
    };

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account, race, COALESCE(knownTitles, '') "
        "FROM characters WHERE account <> 0");

    if (!result)
        return;

    std::unordered_map<uint32, std::vector<TitleCharacterInfo>> charactersByAccount;
    do
    {
        Field* fields = result->Fetch();
        uint32 const accountId = fields[1].Get<uint32>();
        if (AccountBound::IsExcludedAccount(accountId))
            continue;

        charactersByAccount[accountId].push_back({
            fields[0].Get<uint32>(),
            fields[2].Get<uint8>(),
            ParseKnownTitles(fields[3].Get<std::string>())
        });
    } while (result->NextRow());

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 updatedCharacters = 0;
    uint32 addedTitles = 0;

    for (auto const& [accountId, characters] : charactersByAccount)
    {
        (void)accountId;

        for (TitleCharacterInfo const& target : characters)
        {
            KnownTitlesMask mergedTitles = target.Titles;
            uint32 added = 0;

            for (TitleCharacterInfo const& source : characters)
            {
                if (source.Guid != target.Guid)
                    added += MergeTitlesForRace(source.Titles, source.Race, target.Race, mergedTitles);
            }

            if (!added)
                continue;

            UpdateTitlesForCharacter(trans, target.Guid, mergedTitles);
            ++updatedCharacters;
            addedTitles += added;
        }
    }

    CommitIfNeeded(trans);
    LOG_INFO("module.accountboundtitles",
        "AccountBoundTitles: startup backfill added {} title(s) to {} character(s).",
        addedTitles, updatedCharacters);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Titles");
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Titles.StartupBackfill", true);
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Titles.SyncOnCreate", true);
    Config.SyncOnSave = sConfigMgr->GetOption<bool>("AccountBound.Titles.SyncOnSave", true);
    Config.SyncRealmFirst = sConfigMgr->GetOption<bool>("AccountBound.Titles.SyncRealmFirst", false);
    Config.ConvertFactionSpecific = sConfigMgr->GetOption<bool>("AccountBound.Titles.ConvertFactionSpecific", true);
    Config.Filter = AccountBound::LoadIdFilter("Titles");
}
}

class AccountBoundTitlesWorldScript : public WorldScript
{
public:
    AccountBoundTitlesWorldScript() : WorldScript("AccountBoundTitlesWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();
        LOG_INFO("module.accountboundtitles",
            "AccountBoundTitles: {}. CreateSync={}, SaveSync={}, StartupBackfill={}, RealmFirst={}.",
            Config.Enabled ? (reload ? "configuration reloaded" : "configuration loaded") : "disabled",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.SyncOnSave ? "on" : "off",
            Config.Enabled && Config.StartupBackfill ? "on" : "off",
            Config.Enabled && Config.SyncRealmFirst ? "on" : "off");
    }

    void OnStartup() override
    {
        if (!Config.Enabled)
            return;

        BuildTitleCache();
        BackfillAllTitles();
    }
};

class AccountBoundTitlesPlayerScript : public PlayerScript
{
public:
    AccountBoundTitlesPlayerScript() : PlayerScript("AccountBoundTitlesPlayerScript", {
        PLAYERHOOK_ON_LOAD_FROM_DB,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_SAVE
    }) { }

    void OnPlayerLoadFromDB(Player* player) override
    {
        if (player)
            LastKnownTitlesByCharacter[player->GetGUID().GetCounter()] = GetPlayerKnownTitles(player);
    }

    void OnPlayerCreate(Player* player) override
    {
        if (Config.SyncOnCreate)
            BackfillTitlesForCharacter(player);
    }

    void OnPlayerSave(Player* player) override
    {
        SyncTitlesFromPlayerToAccount(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            LastKnownTitlesByCharacter.erase(player->GetGUID().GetCounter());
    }
};

void AddAccountBoundTitlesScripts()
{
    new AccountBoundTitlesWorldScript();
    new AccountBoundTitlesPlayerScript();
}
