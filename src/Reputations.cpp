/*
 * Account-bound reputations for AzerothCore.
 */

#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ReputationMgr.h"
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
    bool SyncOnCreate = true;
    bool SyncOnChange = true;
    bool SameFactionOnly = true;
    bool ConvertFactionSpecific = true;
    bool SyncUnpairedCrossFaction = false;
    AccountBound::IdFilter Filter;
};

struct ReputationInfo
{
    int32 Standing = 0;
    uint16 Flags = 0;
};

struct CharacterInfo
{
    uint32 Guid = 0;
    uint32 AccountId = 0;
    uint8 Race = 0;
    uint8 Class = 0;
    std::unordered_map<uint32, ReputationInfo> Reputations;
};

ModuleConfig Config;

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

bool CanShareBetweenRaces(uint8 sourceRace, uint8 targetRace)
{
    return !Config.SameFactionOnly || Player::TeamIdForRace(sourceRace) == Player::TeamIdForRace(targetRace);
}

uint32 GetFactionForTarget(uint32 factionId, uint8 sourceRace, uint8 targetRace)
{
    if (!Config.Filter.Allows(factionId))
        return 0;

    TeamId const sourceTeam = Player::TeamIdForRace(sourceRace);
    TeamId const targetTeam = Player::TeamIdForRace(targetRace);

    if (sourceTeam == targetTeam)
        return factionId;

    if (Config.SameFactionOnly)
        return 0;

    if (Config.ConvertFactionSpecific)
    {
        for (auto const& [allianceFaction, hordeFaction] : sObjectMgr->FactionChangeReputation)
        {
            if (sourceTeam == TEAM_ALLIANCE && factionId == allianceFaction)
                return Config.Filter.Allows(targetTeam == TEAM_HORDE ? hordeFaction : allianceFaction) ?
                    (targetTeam == TEAM_HORDE ? hordeFaction : allianceFaction) : 0;

            if (sourceTeam == TEAM_HORDE && factionId == hordeFaction)
                return Config.Filter.Allows(targetTeam == TEAM_ALLIANCE ? allianceFaction : hordeFaction) ?
                    (targetTeam == TEAM_ALLIANCE ? allianceFaction : hordeFaction) : 0;
        }
    }

    return Config.SyncUnpairedCrossFaction ? factionId : 0;
}

int32 GetAbsoluteStanding(CharacterInfo const& character, uint32 factionId, ReputationInfo const& reputation)
{
    FactionEntry const* faction = sFactionStore.LookupEntry(factionId);
    if (!faction || !faction->CanHaveReputation())
        return ReputationMgr::Reputation_Bottom;

    int32 const base = sObjectMgr->GetBaseReputationOf(faction, character.Race, character.Class);
    return std::clamp(base + reputation.Standing, ReputationMgr::Reputation_Bottom, ReputationMgr::Reputation_Cap);
}

int32 GetStoredStanding(CharacterInfo const& character, uint32 factionId, int32 absoluteStanding)
{
    FactionEntry const* faction = sFactionStore.LookupEntry(factionId);
    if (!faction)
        return 0;

    int32 const base = sObjectMgr->GetBaseReputationOf(faction, character.Race, character.Class);
    return std::clamp(absoluteStanding, ReputationMgr::Reputation_Bottom, ReputationMgr::Reputation_Cap) - base;
}

std::vector<CharacterInfo> LoadCharacters(uint32 accountId = 0)
{
    std::vector<CharacterInfo> characters;
    std::unordered_map<uint32, std::size_t> characterIndexes;
    QueryResult result;

    if (accountId)
    {
        result = CharacterDatabase.Query(
            "SELECT c.guid, c.account, c.race, c.class, cr.faction, cr.standing, cr.flags "
            "FROM characters c "
            "LEFT JOIN character_reputation cr ON cr.guid = c.guid "
            "WHERE c.account = {} "
            "ORDER BY c.guid",
            accountId);
    }
    else
    {
        result = CharacterDatabase.Query(
            "SELECT c.guid, c.account, c.race, c.class, cr.faction, cr.standing, cr.flags "
            "FROM characters c "
            "LEFT JOIN character_reputation cr ON cr.guid = c.guid "
            "WHERE c.account <> 0 "
            "ORDER BY c.account, c.guid");
    }

    if (!result)
        return characters;

    do
    {
        Field* fields = result->Fetch();
        uint32 const guid = fields[0].Get<uint32>();
        auto [itr, inserted] = characterIndexes.try_emplace(guid, characters.size());

        if (inserted)
        {
            characters.push_back({
                guid,
                fields[1].Get<uint32>(),
                fields[2].Get<uint8>(),
                fields[3].Get<uint8>(),
                {}
            });
        }

        if (!fields[4].IsNull())
        {
            characters[itr->second].Reputations[fields[4].Get<uint32>()] = {
                fields[5].Get<int32>(),
                fields[6].Get<uint16>()
            };
        }
    } while (result->NextRow());

    return characters;
}

void PersistReputation(
    CharacterDatabaseTransaction& trans,
    CharacterInfo const& target,
    uint32 factionId,
    int32 absoluteStanding,
    uint16 flags)
{
    int32 const storedStanding = GetStoredStanding(target, factionId, absoluteStanding);
    AppendOrCommit(trans, Acore::StringFormat(
        "INSERT INTO character_reputation (guid, faction, standing, flags) "
        "VALUES ({}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE standing = VALUES(standing), flags = flags | VALUES(flags)",
        target.Guid, factionId, storedStanding, flags));
}

bool ApplyReputation(
    CharacterDatabaseTransaction& trans,
    CharacterInfo& target,
    uint32 factionId,
    int32 absoluteStanding,
    uint16 flags)
{
    FactionEntry const* faction = sFactionStore.LookupEntry(factionId);
    if (!faction || !faction->CanHaveReputation())
        return false;

    int32 currentStanding = sObjectMgr->GetBaseReputationOf(faction, target.Race, target.Class);
    auto currentItr = target.Reputations.find(factionId);
    if (currentItr != target.Reputations.end())
        currentStanding = GetAbsoluteStanding(target, factionId, currentItr->second);

    absoluteStanding = std::clamp(absoluteStanding, ReputationMgr::Reputation_Bottom, ReputationMgr::Reputation_Cap);
    if (absoluteStanding <= currentStanding)
        return false;

    uint16 const mergedFlags = currentItr == target.Reputations.end() ? flags : uint16(currentItr->second.Flags | flags);
    PersistReputation(trans, target, factionId, absoluteStanding, mergedFlags);
    target.Reputations[factionId] = { GetStoredStanding(target, factionId, absoluteStanding), mergedFlags };

    return true;
}

uint32 MergeReputations(CharacterInfo const& source, CharacterInfo& target, CharacterDatabaseTransaction& trans)
{
    if (source.Guid == target.Guid || !CanShareBetweenRaces(source.Race, target.Race))
        return 0;

    uint32 updated = 0;
    for (auto const& [sourceFactionId, sourceReputation] : source.Reputations)
    {
        uint32 const targetFactionId = GetFactionForTarget(sourceFactionId, source.Race, target.Race);
        if (!targetFactionId)
            continue;

        int32 const absoluteStanding = GetAbsoluteStanding(source, sourceFactionId, sourceReputation);
        if (ApplyReputation(trans, target, targetFactionId, absoluteStanding, sourceReputation.Flags))
            ++updated;
    }

    return updated;
}

void BackfillReputationsForCharacter(Player* player)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    std::vector<CharacterInfo> characters = LoadCharacters(player->GetSession()->GetAccountId());
    auto targetItr = std::find_if(characters.begin(), characters.end(), [player](CharacterInfo const& character)
    {
        return character.Guid == player->GetGUID().GetCounter();
    });

    if (targetItr == characters.end())
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 updated = 0;
    for (CharacterInfo const& source : characters)
        updated += MergeReputations(source, *targetItr, trans);

    CommitIfNeeded(trans);

    if (updated)
        LOG_INFO("module.accountboundreputations",
            "AccountBoundReputations: seeded {} improved reputation(s) for character {}.",
            updated, targetItr->Guid);
}

void SyncReputationToAccount(Player* player, uint32 factionId, int32 absoluteStanding)
{
    if (!Config.Enabled || !Config.SyncOnChange || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    FactionEntry const* sourceFaction = sFactionStore.LookupEntry(factionId);
    if (!sourceFaction || !sourceFaction->CanHaveReputation())
        return;

    std::vector<CharacterInfo> characters = LoadCharacters(player->GetSession()->GetAccountId());
    auto sourceItr = std::find_if(characters.begin(), characters.end(), [player](CharacterInfo const& character)
    {
        return character.Guid == player->GetGUID().GetCounter();
    });

    if (sourceItr == characters.end())
        return;

    uint16 flags = 0;
    if (FactionState const* state = player->GetReputationMgr().GetState(sourceFaction))
        flags = state->Flags;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 updated = 0;

    for (CharacterInfo& target : characters)
    {
        if (target.Guid == sourceItr->Guid || !CanShareBetweenRaces(sourceItr->Race, target.Race))
            continue;

        uint32 const targetFactionId = GetFactionForTarget(factionId, sourceItr->Race, target.Race);
        if (targetFactionId && ApplyReputation(trans, target, targetFactionId, absoluteStanding, flags))
            ++updated;
    }

    CommitIfNeeded(trans);

    if (updated)
        LOG_DEBUG("module.accountboundreputations",
            "AccountBoundReputations: propagated faction {} from character {} to {} account character(s).",
            factionId, sourceItr->Guid, updated);
}

void BackfillAllReputations()
{
    if (!Config.Enabled || !Config.StartupBackfill)
        return;

    std::vector<CharacterInfo> characters = LoadCharacters();
    std::unordered_map<uint32, std::vector<std::size_t>> charactersByAccount;

    for (std::size_t index = 0; index < characters.size(); ++index)
        charactersByAccount[characters[index].AccountId].push_back(index);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint64 updated = 0;

    for (auto const& [accountId, indexes] : charactersByAccount)
    {
        if (AccountBound::IsExcludedAccount(accountId))
            continue;
        std::vector<CharacterInfo> const snapshot = [&]()
        {
            std::vector<CharacterInfo> result;
            result.reserve(indexes.size());
            for (std::size_t index : indexes)
                result.push_back(characters[index]);
            return result;
        }();

        for (std::size_t targetIndex : indexes)
            for (CharacterInfo const& source : snapshot)
                updated += MergeReputations(source, characters[targetIndex], trans);
    }

    CommitIfNeeded(trans);
    LOG_INFO("module.accountboundreputations",
        "AccountBoundReputations: startup backfill queued {} improved reputation row(s).", updated);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Reputations");
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Reputations.StartupBackfill", true);
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Reputations.SyncOnCreate", true);
    Config.SyncOnChange = sConfigMgr->GetOption<bool>("AccountBound.Reputations.SyncOnChange", true);
    Config.SameFactionOnly = sConfigMgr->GetOption<bool>("AccountBound.Reputations.SameFactionOnly", true);
    Config.ConvertFactionSpecific = sConfigMgr->GetOption<bool>("AccountBound.Reputations.ConvertFactionSpecific", true);
    Config.SyncUnpairedCrossFaction = sConfigMgr->GetOption<bool>("AccountBound.Reputations.SyncUnpairedCrossFaction", false);
    Config.Filter = AccountBound::LoadIdFilter("Reputations");
}
}

class AccountBoundReputationsWorldScript : public WorldScript
{
public:
    AccountBoundReputationsWorldScript() : WorldScript("AccountBoundReputationsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();
        LOG_INFO("module.accountboundreputations",
            "AccountBoundReputations: {}. CreateSync={}, ChangeSync={}, StartupBackfill={}, SameFactionOnly={}.",
            Config.Enabled ? (reload ? "configuration reloaded" : "configuration loaded") : "disabled",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.SyncOnChange ? "on" : "off",
            Config.Enabled && Config.StartupBackfill ? "on" : "off",
            Config.Enabled && Config.SameFactionOnly ? "on" : "off");
    }

    void OnStartup() override
    {
        BackfillAllReputations();
    }
};

class AccountBoundReputationsPlayerScript : public PlayerScript
{
public:
    AccountBoundReputationsPlayerScript() : PlayerScript("AccountBoundReputationsPlayerScript", {
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_REPUTATION_CHANGE
    }) { }

    void OnPlayerCreate(Player* player) override
    {
        if (Config.SyncOnCreate)
            BackfillReputationsForCharacter(player);
    }

    bool OnPlayerReputationChange(Player* player, uint32 factionId, int32& standing, bool /*incremental*/) override
    {
        SyncReputationToAccount(player, factionId, standing);
        return true;
    }
};

void AddAccountBoundReputationsScripts()
{
    new AccountBoundReputationsWorldScript();
    new AccountBoundReputationsPlayerScript();
}
