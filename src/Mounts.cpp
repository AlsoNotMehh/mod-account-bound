/*
 * Account-bound mounts for AzerothCore.
 */

#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCEnums.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "RaceMgr.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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
    bool ConvertFactionSpecific = false;
    bool RespectFactionRestrictions = true;
    bool RequireRiding = true;
    bool SyncClassMounts = true;
    bool RequireClass = true;
    bool ClassMountsSameFactionOnly = true;
    bool CleanupInvalid = true;
    bool BackfillOnRidingSkillChange = true;
    AccountBound::IdFilter Filter;
};

struct CharacterInfo
{
    uint32 Guid = 0;
    uint32 AccountId = 0;
    uint8 Race = 0;
    uint8 Class = 0;
    uint16 RidingSkill = 0;
};

struct MountSpellRequirements
{
    uint16 RequiredRidingRank = 1;
    uint32 ClassMask = 0;
};

ModuleConfig Config;
std::unordered_map<uint32, TeamId> MountSpellFactionMap;
std::unordered_map<uint32, MountSpellRequirements> MountSpellRequirementsMap;

bool IsMountSpell(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    return spellInfo && spellInfo->HasAura(SPELL_AURA_MOUNTED);
}

uint32 GetClassMaskForClass(uint8 classId)
{
    if (!classId || classId >= MAX_CLASSES)
        return 0;

    return 1u << (classId - 1);
}

uint32 NormalizeClassMask(uint32 classMask)
{
    classMask &= CLASSMASK_ALL_PLAYABLE;
    return classMask && classMask != CLASSMASK_ALL_PLAYABLE ? classMask : 0;
}

uint16 GetRidingRankFromRequiredSpell(uint32 spellId)
{
    switch (spellId)
    {
        case 33388: return 75;  // Apprentice Riding
        case 33391: return 150; // Journeyman Riding
        case 34090: return 225; // Expert Riding
        case 34091: return 300; // Artisan Riding
        default: return 0;
    }
}

MountSpellRequirements& GetMountSpellRequirements(uint32 spellId)
{
    return MountSpellRequirementsMap.try_emplace(spellId).first->second;
}

void SetMountRidingRequirement(uint32 spellId, uint16 requiredRidingRank)
{
    if (!IsMountSpell(spellId) || !requiredRidingRank)
        return;

    MountSpellRequirements& requirements = GetMountSpellRequirements(spellId);
    requirements.RequiredRidingRank = std::max(requirements.RequiredRidingRank, requiredRidingRank);
}

void SetMountClassRestriction(uint32 spellId, uint32 classMask)
{
    classMask = NormalizeClassMask(classMask);
    if (!IsMountSpell(spellId) || !classMask)
        return;

    GetMountSpellRequirements(spellId).ClassMask |= classMask;
}

void ApplyClassMountFallbacks()
{
    uint32 const paladinMask = GetClassMaskForClass(CLASS_PALADIN);
    uint32 const warlockMask = GetClassMaskForClass(CLASS_WARLOCK);
    uint32 const deathKnightMask = GetClassMaskForClass(CLASS_DEATH_KNIGHT);
    uint32 const druidMask = GetClassMaskForClass(CLASS_DRUID);

    for (uint32 spellId : { 13819u, 23214u, 34767u, 34769u, 66906u })
        SetMountClassRestriction(spellId, paladinMask);

    for (uint32 spellId : { 5784u, 23161u })
        SetMountClassRestriction(spellId, warlockMask);

    for (uint32 spellId : { 48778u, 54729u })
        SetMountClassRestriction(spellId, deathKnightMask);

    for (uint32 spellId : { 33943u, 40120u, 40121u })
        SetMountClassRestriction(spellId, druidMask);
}

TeamId GetAllowedMountTeamForRaceMask(uint32 allowableRace)
{
    uint32 const playableRaceMask = sRaceMgr->GetPlayableRaceMask();
    uint32 const allowedRaceMask = allowableRace ? (allowableRace & playableRaceMask) : playableRaceMask;

    if (!allowedRaceMask)
        return TEAM_NEUTRAL;

    bool const hasAllianceRace = (allowedRaceMask & sRaceMgr->GetAllianceRaceMask()) != 0;
    bool const hasHordeRace = (allowedRaceMask & sRaceMgr->GetHordeRaceMask()) != 0;

    if (hasAllianceRace && !hasHordeRace)
        return TEAM_ALLIANCE;

    if (hasHordeRace && !hasAllianceRace)
        return TEAM_HORDE;

    return TEAM_NEUTRAL;
}

void SetMountSpellFaction(uint32 spellId, TeamId teamId, bool force = false)
{
    if (!IsMountSpell(spellId))
        return;

    auto itr = MountSpellFactionMap.find(spellId);
    if (itr == MountSpellFactionMap.end())
    {
        MountSpellFactionMap.emplace(spellId, teamId);
        return;
    }

    if (force)
    {
        itr->second = teamId;
        return;
    }

    if (teamId == TEAM_NEUTRAL)
    {
        itr->second = TEAM_NEUTRAL;
        return;
    }

    if (itr->second == TEAM_NEUTRAL)
    {
        itr->second = teamId;
        return;
    }

    if (itr->second != teamId)
        itr->second = TEAM_NEUTRAL;
}

void BuildMountCache()
{
    MountSpellFactionMap.clear();
    MountSpellRequirementsMap.clear();

    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        if (!IsMountSpell(spellId))
            continue;

        MountSpellFactionMap.emplace(spellId, TEAM_NEUTRAL);
        MountSpellRequirementsMap.emplace(spellId, MountSpellRequirements{});

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* skillLineAbility = itr->second;
            if (!skillLineAbility)
                continue;

            SetMountClassRestriction(spellId, skillLineAbility->ClassMask);

            if (skillLineAbility->SkillLine == SKILL_RIDING)
                SetMountRidingRequirement(spellId, uint16(skillLineAbility->MinSkillLineRank));
        }
    }

    if (ItemTemplateContainer const* itemTemplates = sObjectMgr->GetItemTemplateStore())
    {
        for (auto const& itemTemplatePair : *itemTemplates)
        {
            ItemTemplate const& itemTemplate = itemTemplatePair.second;
            if (itemTemplate.Class != ITEM_CLASS_MISC || itemTemplate.SubClass != ITEM_SUBCLASS_JUNK_MOUNT)
                continue;

            TeamId const itemTeam = GetAllowedMountTeamForRaceMask(itemTemplate.AllowableRace);

            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                int32 const itemSpellId = itemTemplate.Spells[i].SpellId;
                if (itemSpellId <= 0 || !IsMountSpell(uint32(itemSpellId)))
                    continue;

                SetMountSpellFaction(uint32(itemSpellId), itemTeam);
                SetMountClassRestriction(uint32(itemSpellId), itemTemplate.AllowableClass);

                if (itemTemplate.RequiredSkill == SKILL_RIDING)
                    SetMountRidingRequirement(uint32(itemSpellId), uint16(itemTemplate.RequiredSkillRank));

                SetMountRidingRequirement(uint32(itemSpellId), GetRidingRankFromRequiredSpell(itemTemplate.RequiredSpell));
            }
        }
    }

    for (auto const& [allianceSpell, hordeSpell] : sObjectMgr->FactionChangeSpells)
    {
        if (!IsMountSpell(allianceSpell) || !IsMountSpell(hordeSpell))
            continue;

        SetMountSpellFaction(allianceSpell, TEAM_ALLIANCE, true);
        SetMountSpellFaction(hordeSpell, TEAM_HORDE, true);
    }

    ApplyClassMountFallbacks();
    LOG_INFO("module.accountboundmounts",
        "AccountBoundMounts: cached {} faction rule(s) and {} requirement rule(s).",
        MountSpellFactionMap.size(), MountSpellRequirementsMap.size());
}

TeamId GetMountSpellFaction(uint32 spellId)
{
    auto itr = MountSpellFactionMap.find(spellId);
    return itr != MountSpellFactionMap.end() ? itr->second : TEAM_NEUTRAL;
}

uint32 GetFactionConvertedMountSpell(uint32 spellId, TeamId targetTeam)
{
    for (auto const& [allianceSpell, hordeSpell] : sObjectMgr->FactionChangeSpells)
    {
        if (!IsMountSpell(allianceSpell) || !IsMountSpell(hordeSpell))
            continue;

        if (spellId == allianceSpell && targetTeam == TEAM_HORDE)
            return hordeSpell;

        if (spellId == hordeSpell && targetTeam == TEAM_ALLIANCE)
            return allianceSpell;
    }

    return 0;
}

bool IsMountSpellAllowedForTeam(uint32 spellId, TeamId targetTeam)
{
    if (!Config.RespectFactionRestrictions)
        return true;

    TeamId const mountTeam = GetMountSpellFaction(spellId);
    return mountTeam == TEAM_NEUTRAL || mountTeam == targetTeam;
}

uint32 GetMountSpellForRace(uint32 spellId, uint8 race)
{
    if (!Config.Filter.Allows(spellId) || !IsMountSpell(spellId))
        return 0;

    TeamId const targetTeam = Player::TeamIdForRace(race);
    if (IsMountSpellAllowedForTeam(spellId, targetTeam))
        return spellId;

    if (!Config.ConvertFactionSpecific)
        return 0;

    uint32 const convertedSpellId = GetFactionConvertedMountSpell(spellId, targetTeam);
    return Config.Filter.Allows(convertedSpellId) &&
        IsMountSpellAllowedForTeam(convertedSpellId, targetTeam) ? convertedSpellId : 0;
}

bool IsClassRestrictedMount(uint32 spellId)
{
    return GetMountSpellRequirements(spellId).ClassMask != 0;
}

bool ShouldAccountSyncMount(uint32 spellId)
{
    return Config.Filter.Allows(spellId) && IsMountSpell(spellId) &&
        (Config.SyncClassMounts || !IsClassRestrictedMount(spellId));
}

bool CanShareClassMountFromRace(uint32 spellId, uint8 sourceRace, uint8 targetRace)
{
    if (!Config.ClassMountsSameFactionOnly || !sourceRace || !IsClassRestrictedMount(spellId))
        return true;

    return Player::TeamIdForRace(sourceRace) == Player::TeamIdForRace(targetRace);
}

bool CanCharacterReceiveMount(uint32 spellId, uint8 classId, uint16 ridingSkill, uint8 sourceRace = 0, uint8 targetRace = 0)
{
    MountSpellRequirements const& requirements = GetMountSpellRequirements(spellId);

    if (Config.RequireRiding && ridingSkill < requirements.RequiredRidingRank)
        return false;

    if (Config.RequireClass && requirements.ClassMask && !(requirements.ClassMask & GetClassMaskForClass(classId)))
        return false;

    return !targetRace || CanShareClassMountFromRace(spellId, sourceRace, targetRace);
}

bool CanCharacterReceiveMount(uint32 spellId, CharacterInfo const& character, uint8 sourceRace = 0)
{
    return CanCharacterReceiveMount(spellId, character.Class, character.RidingSkill, sourceRace, character.Race);
}

bool CanPlayerReceiveMount(uint32 spellId, Player* player, uint8 sourceRace = 0)
{
    return player && CanCharacterReceiveMount(
        spellId, player->getClass(), player->GetSkillValue(SKILL_RIDING), sourceRace, player->getRace(true));
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

void InsertMountForCharacter(CharacterDatabaseTransaction& trans, uint32 targetGuid, uint32 spellId)
{
    AppendOrCommit(trans, Acore::StringFormat(
        "INSERT IGNORE INTO character_spell (guid, spell, specMask) VALUES ({}, {}, {})",
        targetGuid, spellId, uint32(SPEC_MASK_ALL)));
}

void DeleteMountForCharacter(CharacterDatabaseTransaction& trans, uint32 targetGuid, uint32 spellId)
{
    AppendOrCommit(trans, Acore::StringFormat(
        "DELETE FROM character_spell WHERE guid = {} AND spell = {}",
        targetGuid, spellId));
}

std::vector<CharacterInfo> LoadAccountCharacters(uint32 accountId)
{
    std::vector<CharacterInfo> characters;
    QueryResult result = CharacterDatabase.Query(
        "SELECT c.guid, c.account, c.race, c.`class`, COALESCE(cs.value, 0) "
        "FROM characters c "
        "LEFT JOIN character_skills cs ON cs.guid = c.guid AND cs.skill = {} "
        "WHERE c.account = {}",
        uint32(SKILL_RIDING), accountId);

    if (!result)
        return characters;

    do
    {
        Field* fields = result->Fetch();
        characters.push_back({
            fields[0].Get<uint32>(),
            fields[1].Get<uint32>(),
            fields[2].Get<uint8>(),
            fields[3].Get<uint8>(),
            fields[4].Get<uint16>()
        });
    } while (result->NextRow());

    return characters;
}

void SyncMountToAccount(Player* player, uint32 spellId)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()) || !ShouldAccountSyncMount(spellId))
        return;

    uint8 const sourceRace = player->getRace(true);
    std::vector<CharacterInfo> characters = LoadAccountCharacters(player->GetSession()->GetAccountId());
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 synced = 0;

    for (CharacterInfo const& target : characters)
    {
        if (target.Guid == player->GetGUID().GetCounter())
            continue;

        uint32 const targetSpellId = GetMountSpellForRace(spellId, target.Race);
        if (!targetSpellId || !CanCharacterReceiveMount(targetSpellId, target, sourceRace))
            continue;

        InsertMountForCharacter(trans, target.Guid, targetSpellId);
        ++synced;
    }

    CommitIfNeeded(trans);

    if (synced)
        LOG_DEBUG("module.accountboundmounts",
            "AccountBoundMounts: synced mount spell {} from player {} to {} account character(s).",
            spellId, player->GetGUID().GetCounter(), synced);
}

void BackfillMountsForCharacter(Player* player)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()))
        return;

    uint32 const accountId = player->GetSession()->GetAccountId();
    uint32 const targetGuid = player->GetGUID().GetCounter();
    uint8 const targetRace = player->getRace(true);
    QueryResult result = CharacterDatabase.Query(
        "SELECT DISTINCT cs.spell, c.race "
        "FROM character_spell cs "
        "INNER JOIN characters c ON c.guid = cs.guid "
        "WHERE c.account = {} AND c.guid <> {}",
        accountId, targetGuid);

    if (!result)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 inserted = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 const spellId = fields[0].Get<uint32>();
        uint8 const sourceRace = fields[1].Get<uint8>();

        if (!ShouldAccountSyncMount(spellId))
            continue;

        uint32 const targetSpellId = GetMountSpellForRace(spellId, targetRace);
        if (!targetSpellId || !CanPlayerReceiveMount(targetSpellId, player, sourceRace))
            continue;

        InsertMountForCharacter(trans, targetGuid, targetSpellId);
        ++inserted;
    } while (result->NextRow());

    CommitIfNeeded(trans);

    if (inserted)
        LOG_INFO("module.accountboundmounts",
            "AccountBoundMounts: seeded {} mount spell row(s) for character {}.",
            inserted, targetGuid);
}

void BackfillAllMounts()
{
    if (!Config.Enabled || !Config.StartupBackfill)
        return;

    QueryResult charactersResult = CharacterDatabase.Query(
        "SELECT c.guid, c.account, c.race, c.`class`, COALESCE(cs.value, 0) "
        "FROM characters c "
        "LEFT JOIN character_skills cs ON cs.guid = c.guid AND cs.skill = {} "
        "WHERE c.account <> 0",
        uint32(SKILL_RIDING));

    if (!charactersResult)
        return;

    std::unordered_map<uint32, std::vector<CharacterInfo>> charactersByAccount;
    do
    {
        Field* fields = charactersResult->Fetch();
        CharacterInfo character = {
            fields[0].Get<uint32>(),
            fields[1].Get<uint32>(),
            fields[2].Get<uint8>(),
            fields[3].Get<uint8>(),
            fields[4].Get<uint16>()
        };
        if (!AccountBound::IsExcludedAccount(character.AccountId))
            charactersByAccount[character.AccountId].push_back(character);
    } while (charactersResult->NextRow());

    QueryResult spellsResult = CharacterDatabase.Query(
        "SELECT DISTINCT c.account, c.race, cs.spell "
        "FROM character_spell cs "
        "INNER JOIN characters c ON c.guid = cs.guid "
        "WHERE c.account <> 0");

    if (!spellsResult)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint64 inserted = 0;

    do
    {
        Field* fields = spellsResult->Fetch();
        uint32 const accountId = fields[0].Get<uint32>();
        uint8 const sourceRace = fields[1].Get<uint8>();
        uint32 const spellId = fields[2].Get<uint32>();

        if (!ShouldAccountSyncMount(spellId))
            continue;

        auto accountItr = charactersByAccount.find(accountId);
        if (accountItr == charactersByAccount.end())
            continue;

        for (CharacterInfo const& target : accountItr->second)
        {
            uint32 const targetSpellId = GetMountSpellForRace(spellId, target.Race);
            if (!targetSpellId || !CanCharacterReceiveMount(targetSpellId, target, sourceRace))
                continue;

            InsertMountForCharacter(trans, target.Guid, targetSpellId);
            ++inserted;
        }
    } while (spellsResult->NextRow());

    CommitIfNeeded(trans);
    LOG_INFO("module.accountboundmounts",
        "AccountBoundMounts: startup backfill queued {} mount spell row(s).", inserted);
}

void CleanupInvalidMounts()
{
    if (!Config.Enabled || !Config.CleanupInvalid)
        return;

    QueryResult result = CharacterDatabase.Query(
        "SELECT c.guid, c.race, c.`class`, COALESCE(sk.value, 0), cs.spell "
        "FROM character_spell cs "
        "INNER JOIN characters c ON c.guid = cs.guid "
        "LEFT JOIN character_skills sk ON sk.guid = c.guid AND sk.skill = {}",
        uint32(SKILL_RIDING));

    if (!result)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint64 removed = 0;

    do
    {
        Field* fields = result->Fetch();
        CharacterInfo character = {
            fields[0].Get<uint32>(),
            0,
            fields[1].Get<uint8>(),
            fields[2].Get<uint8>(),
            fields[3].Get<uint16>()
        };
        uint32 const spellId = fields[4].Get<uint32>();

        if (!ShouldAccountSyncMount(spellId))
            continue;

        uint32 const allowedSpellId = GetMountSpellForRace(spellId, character.Race);
        if (allowedSpellId == spellId && CanCharacterReceiveMount(spellId, character))
            continue;

        DeleteMountForCharacter(trans, character.Guid, spellId);
        ++removed;
    } while (result->NextRow());

    CommitIfNeeded(trans);

    if (removed)
        LOG_INFO("module.accountboundmounts",
            "AccountBoundMounts: removed {} invalid mount spell row(s).", removed);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Mounts");
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Mounts.StartupBackfill", true);
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Mounts.SyncOnCreate", true);
    Config.ConvertFactionSpecific = sConfigMgr->GetOption<bool>("AccountBound.Mounts.ConvertFactionSpecific", true);
    Config.RespectFactionRestrictions = sConfigMgr->GetOption<bool>("AccountBound.Mounts.RespectFactionRestrictions", true);
    Config.RequireRiding = sConfigMgr->GetOption<bool>("AccountBound.Mounts.RequireRiding", true);
    Config.SyncClassMounts = sConfigMgr->GetOption<bool>("AccountBound.Mounts.SyncClassMounts", true);
    Config.RequireClass = sConfigMgr->GetOption<bool>("AccountBound.Mounts.RequireClass", true);
    Config.ClassMountsSameFactionOnly = sConfigMgr->GetOption<bool>("AccountBound.Mounts.ClassMountsSameFactionOnly", true);
    Config.CleanupInvalid = sConfigMgr->GetOption<bool>("AccountBound.Mounts.CleanupInvalid", true);
    Config.BackfillOnRidingSkillChange = sConfigMgr->GetOption<bool>("AccountBound.Mounts.BackfillOnRidingSkillChange", true);
    Config.Filter = AccountBound::LoadIdFilter("Mounts");
}
}

class AccountBoundMountsWorldScript : public WorldScript
{
public:
    AccountBoundMountsWorldScript() : WorldScript("AccountBoundMountsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();
        LOG_INFO("module.accountboundmounts",
            "AccountBoundMounts: {}. CreateSync={}, StartupBackfill={}, RequireRiding={}, RequireClass={}.",
            Config.Enabled ? (reload ? "configuration reloaded" : "configuration loaded") : "disabled",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.StartupBackfill ? "on" : "off",
            Config.Enabled && Config.RequireRiding ? "on" : "off",
            Config.Enabled && Config.RequireClass ? "on" : "off");
    }

    void OnStartup() override
    {
        if (!Config.Enabled)
            return;

        BuildMountCache();
        CleanupInvalidMounts();
        BackfillAllMounts();
    }
};

class AccountBoundMountsPlayerScript : public PlayerScript
{
public:
    AccountBoundMountsPlayerScript() : PlayerScript("AccountBoundMountsPlayerScript", {
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_LEARN_SPELL,
        PLAYERHOOK_ON_SET_SKILL
    }) { }

    void OnPlayerCreate(Player* player) override
    {
        if (Config.SyncOnCreate)
            BackfillMountsForCharacter(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        SyncMountToAccount(player, spellId);
    }

    void OnPlayerSetSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        if (!Config.Enabled || !Config.BackfillOnRidingSkillChange || skillId != SKILL_RIDING || newValue <= value)
            return;

        BackfillMountsForCharacter(player);
    }
};

void AddAccountBoundMountsScripts()
{
    new AccountBoundMountsWorldScript();
    new AccountBoundMountsPlayerScript();
}
