/*
 * Account-bound achievements for AzerothCore.
 */

#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "StringFormat.h"
#include "WorldScript.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
struct ModuleConfig
{
    bool Enabled = true;
    bool StartupBackfill = true;
    bool SameFactionOnly = false;
    bool SyncOnCreate = true;
    bool SyncRealmFirst = false;
    bool SyncHidden = false;
    bool ConvertFactionSpecific = true;
    bool SyncUnpairedFactionSpecific = false;
    AccountBound::IdFilter Filter;
};

struct CharacterInfo
{
    uint32 Guid = 0;
    uint32 AccountId = 0;
    uint8 Race = 0;
};

ModuleConfig Config;

bool CanShareBetweenRaces(uint8 sourceRace, uint8 targetRace)
{
    return !Config.SameFactionOnly || Player::TeamIdForRace(sourceRace) == Player::TeamIdForRace(targetRace);
}

bool IsRealmFirstAchievement(AchievementEntry const* achievement)
{
    return achievement && (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL));
}

bool IsAchievementAllowed(AchievementEntry const* achievement)
{
    if (!achievement || (achievement->flags & ACHIEVEMENT_FLAG_COUNTER))
        return false;

    if (!Config.Filter.Allows(achievement->ID))
        return false;

    if (!Config.SyncRealmFirst && IsRealmFirstAchievement(achievement))
        return false;

    if (!Config.SyncHidden && (achievement->flags & ACHIEVEMENT_FLAG_HIDDEN))
        return false;

    return true;
}

uint32 GetFactionAchievementForTeam(uint32 achievementId, TeamId targetTeam)
{
    AchievementEntry const* achievement = sAchievementStore.LookupEntry(achievementId);
    if (!IsAchievementAllowed(achievement))
        return 0;

    if (achievement->requiredFaction == ACHIEVEMENT_FACTION_ANY)
        return achievementId;

    if (achievement->requiredFaction == ACHIEVEMENT_FACTION_ALLIANCE && targetTeam == TEAM_ALLIANCE)
        return achievementId;

    if (achievement->requiredFaction == ACHIEVEMENT_FACTION_HORDE && targetTeam == TEAM_HORDE)
        return achievementId;

    if (!Config.ConvertFactionSpecific)
        return Config.SyncUnpairedFactionSpecific ? achievementId : 0;

    for (auto const& [allianceAchievement, hordeAchievement] : sObjectMgr->FactionChangeAchievements)
    {
        uint32 mappedAchievement = 0;

        if (achievementId == allianceAchievement && targetTeam == TEAM_HORDE)
            mappedAchievement = hordeAchievement;
        else if (achievementId == hordeAchievement && targetTeam == TEAM_ALLIANCE)
            mappedAchievement = allianceAchievement;

        if (!mappedAchievement)
            continue;

        AchievementEntry const* mappedEntry = sAchievementStore.LookupEntry(mappedAchievement);
        return IsAchievementAllowed(mappedEntry) ? mappedAchievement : 0;
    }

    return Config.SyncUnpairedFactionSpecific ? achievementId : 0;
}

uint32 GetAchievementForRace(uint32 achievementId, uint8 race)
{
    return GetFactionAchievementForTeam(achievementId, Player::TeamIdForRace(race));
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

void InsertAchievementForCharacter(CharacterDatabaseTransaction& trans, uint32 targetGuid, uint32 achievementId, uint32 date)
{
    AppendOrCommit(trans, Acore::StringFormat(
        "INSERT IGNORE INTO character_achievement (guid, achievement, date) VALUES ({}, {}, {})",
        targetGuid, achievementId, date));
}

std::vector<CharacterInfo> LoadAccountCharacters(uint32 accountId)
{
    std::vector<CharacterInfo> characters;
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account, race FROM characters WHERE account = {}", accountId);

    if (!result)
        return characters;

    do
    {
        Field* fields = result->Fetch();
        characters.push_back({
            fields[0].Get<uint32>(),
            fields[1].Get<uint32>(),
            fields[2].Get<uint8>()
        });
    } while (result->NextRow());

    return characters;
}

void SyncAchievementToAccount(Player* player, uint32 achievementId, uint32 date)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    uint8 const sourceRace = player->getRace(true);
    std::vector<CharacterInfo> characters = LoadAccountCharacters(player->GetSession()->GetAccountId());
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 synced = 0;

    for (CharacterInfo const& target : characters)
    {
        if (target.Guid == player->GetGUID().GetCounter() || !CanShareBetweenRaces(sourceRace, target.Race))
            continue;

        uint32 const targetAchievementId = GetAchievementForRace(achievementId, target.Race);
        if (!targetAchievementId)
            continue;

        InsertAchievementForCharacter(trans, target.Guid, targetAchievementId, date);
        ++synced;
    }

    CommitIfNeeded(trans);

    if (synced)
        LOG_DEBUG("module.accountboundachievements",
            "AccountBoundAchievements: synced achievement {} from player {} to {} account character(s).",
            achievementId, player->GetGUID().GetCounter(), synced);
}

void BackfillAchievementsForCharacter(Player* player)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    uint32 const accountId = player->GetSession()->GetAccountId();
    uint32 const targetGuid = player->GetGUID().GetCounter();
    uint8 const targetRace = player->getRace(true);
    QueryResult result = CharacterDatabase.Query(
        "SELECT ca.achievement, MIN(ca.date), c.race "
        "FROM character_achievement ca "
        "INNER JOIN characters c ON c.guid = ca.guid "
        "WHERE c.account = {} AND c.guid <> {} "
        "GROUP BY ca.achievement, c.race "
        "ORDER BY MIN(ca.date)",
        accountId, targetGuid);

    if (!result)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 inserted = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 const achievementId = fields[0].Get<uint32>();
        uint32 const date = fields[1].Get<uint32>();
        uint8 const sourceRace = fields[2].Get<uint8>();

        if (!CanShareBetweenRaces(sourceRace, targetRace))
            continue;

        uint32 const targetAchievementId = GetAchievementForRace(achievementId, targetRace);
        if (!targetAchievementId)
            continue;

        InsertAchievementForCharacter(trans, targetGuid, targetAchievementId, date);
        ++inserted;
    } while (result->NextRow());

    CommitIfNeeded(trans);

    if (inserted)
        LOG_INFO("module.accountboundachievements",
            "AccountBoundAchievements: seeded {} achievement row(s) for new character {}.",
            inserted, targetGuid);
}

void BackfillAllAchievements()
{
    if (!Config.Enabled || !Config.StartupBackfill)
        return;

    QueryResult charactersResult = CharacterDatabase.Query(
        "SELECT guid, account, race FROM characters WHERE account <> 0");

    if (!charactersResult)
        return;

    std::unordered_map<uint32, std::vector<CharacterInfo>> charactersByAccount;
    do
    {
        Field* fields = charactersResult->Fetch();
        CharacterInfo character = {
            fields[0].Get<uint32>(),
            fields[1].Get<uint32>(),
            fields[2].Get<uint8>()
        };
        if (!AccountBound::IsExcludedAccount(character.AccountId))
            charactersByAccount[character.AccountId].push_back(character);
    } while (charactersResult->NextRow());

    QueryResult achievementsResult = CharacterDatabase.Query(
        "SELECT c.account, c.race, ca.achievement, MIN(ca.date) "
        "FROM character_achievement ca "
        "INNER JOIN characters c ON c.guid = ca.guid "
        "WHERE c.account <> 0 "
        "GROUP BY c.account, c.race, ca.achievement "
        "ORDER BY MIN(ca.date)");

    if (!achievementsResult)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint64 inserted = 0;

    do
    {
        Field* fields = achievementsResult->Fetch();
        uint32 const accountId = fields[0].Get<uint32>();
        uint8 const sourceRace = fields[1].Get<uint8>();
        uint32 const achievementId = fields[2].Get<uint32>();
        uint32 const date = fields[3].Get<uint32>();
        auto accountItr = charactersByAccount.find(accountId);

        if (accountItr == charactersByAccount.end())
            continue;

        for (CharacterInfo const& target : accountItr->second)
        {
            if (!CanShareBetweenRaces(sourceRace, target.Race))
                continue;

            uint32 const targetAchievementId = GetAchievementForRace(achievementId, target.Race);
            if (!targetAchievementId)
                continue;

            InsertAchievementForCharacter(trans, target.Guid, targetAchievementId, date);
            ++inserted;
        }
    } while (achievementsResult->NextRow());

    CommitIfNeeded(trans);
    LOG_INFO("module.accountboundachievements",
        "AccountBoundAchievements: startup backfill queued {} achievement row(s).", inserted);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Achievements");
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Achievements.StartupBackfill", true);
    Config.SameFactionOnly = sConfigMgr->GetOption<bool>("AccountBound.Achievements.SameFactionOnly", false);
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Achievements.SyncOnCreate", true);
    Config.SyncRealmFirst = sConfigMgr->GetOption<bool>("AccountBound.Achievements.SyncRealmFirst", false);
    Config.SyncHidden = sConfigMgr->GetOption<bool>("AccountBound.Achievements.SyncHidden", false);
    Config.ConvertFactionSpecific = sConfigMgr->GetOption<bool>("AccountBound.Achievements.ConvertFactionSpecific", true);
    Config.SyncUnpairedFactionSpecific = sConfigMgr->GetOption<bool>("AccountBound.Achievements.SyncUnpairedFactionSpecific", false);
    Config.Filter = AccountBound::LoadIdFilter("Achievements");
}
}

class AccountBoundAchievementsWorldScript : public WorldScript
{
public:
    AccountBoundAchievementsWorldScript() : WorldScript("AccountBoundAchievementsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();
        LOG_INFO("module.accountboundachievements",
            "AccountBoundAchievements: {}. CreateSync={}, StartupBackfill={}, SameFactionOnly={}, RealmFirst={}.",
            Config.Enabled ? (reload ? "configuration reloaded" : "configuration loaded") : "disabled",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.StartupBackfill ? "on" : "off",
            Config.Enabled && Config.SameFactionOnly ? "on" : "off",
            Config.Enabled && Config.SyncRealmFirst ? "on" : "off");
    }

    void OnStartup() override
    {
        BackfillAllAchievements();
    }
};

class AccountBoundAchievementsPlayerScript : public PlayerScript
{
public:
    AccountBoundAchievementsPlayerScript() : PlayerScript("AccountBoundAchievementsPlayerScript", {
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_ACHI_COMPLETE
    }) { }

    void OnPlayerCreate(Player* player) override
    {
        if (Config.SyncOnCreate)
            BackfillAchievementsForCharacter(player);
    }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (achievement)
            SyncAchievementToAccount(player, achievement->ID, uint32(GameTime::GetGameTime().count()));
    }
};

void AddAccountBoundAchievementsScripts()
{
    new AccountBoundAchievementsWorldScript();
    new AccountBoundAchievementsPlayerScript();
}
