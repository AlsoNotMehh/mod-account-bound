/*
 * Account-bound companion pets for AzerothCore.
 */

#include "AccountBound.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldScript.h"

#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct ModuleConfig
{
    bool Enabled = true;
    bool StartupBackfill = true;
    bool SyncOnCreate = true;
    bool SameFactionOnly = false;
    bool IncludeCompanionSkillLine = true;
    bool IncludeMinipetSummons = true;
    AccountBound::IdFilter Filter;
};

struct CharacterInfo
{
    uint32 Guid = 0;
    uint8 Race = 0;
};

ModuleConfig Config;
std::unordered_set<uint32> CompanionSpells;

bool HasCompanionSkillLine(uint32 spellId)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        if (itr->second && itr->second->SkillLine == SKILL_COMPANIONS)
            return true;

    return false;
}

bool IsMinipetSummon(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
    {
        SpellEffectInfo const& effect = spellInfo->Effects[effectIndex];
        if (effect.Effect != SPELL_EFFECT_SUMMON || effect.MiscValue <= 0)
            continue;

        SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(effect.MiscValueB);
        if (properties && properties->Type == SUMMON_TYPE_MINIPET)
            return true;
    }

    return false;
}

bool IsCompanionSpell(uint32 spellId)
{
    return Config.Filter.Allows(spellId) && CompanionSpells.find(spellId) != CompanionSpells.end();
}

void BuildCompanionCache()
{
    CompanionSpells.clear();

    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        bool const companionSkill = Config.IncludeCompanionSkillLine && HasCompanionSkillLine(spellId);
        bool const minipetSummon = Config.IncludeMinipetSummons && IsMinipetSummon(spellInfo);
        if (companionSkill || minipetSummon)
            CompanionSpells.insert(spellId);
    }

    LOG_INFO("module.accountboundpets",
        "AccountBoundPets: cached {} companion pet spell(s).", CompanionSpells.size());
}

bool CanShareBetweenRaces(uint8 sourceRace, uint8 targetRace)
{
    return !Config.SameFactionOnly || Player::TeamIdForRace(sourceRace) == Player::TeamIdForRace(targetRace);
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

void InsertCompanionForCharacter(CharacterDatabaseTransaction& trans, uint32 targetGuid, uint32 spellId)
{
    AppendOrCommit(trans, Acore::StringFormat(
        "INSERT IGNORE INTO character_spell (guid, spell, specMask) VALUES ({}, {}, {})",
        targetGuid, spellId, uint32(SPEC_MASK_ALL)));
}

std::vector<CharacterInfo> LoadAccountCharacters(uint32 accountId)
{
    std::vector<CharacterInfo> characters;
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, race FROM characters WHERE account = {}", accountId);

    if (!result)
        return characters;

    do
    {
        Field* fields = result->Fetch();
        characters.push_back({
            fields[0].Get<uint32>(),
            fields[1].Get<uint8>()
        });
    } while (result->NextRow());

    return characters;
}

void SyncCompanionToAccount(Player* player, uint32 spellId)
{
    if (!Config.Enabled || !player || AccountBound::IsExcludedAccount(player->GetSession()->GetAccountId()) || !IsCompanionSpell(spellId))
        return;

    uint32 const sourceGuid = player->GetGUID().GetCounter();
    uint8 const sourceRace = player->getRace(true);
    std::vector<CharacterInfo> characters = LoadAccountCharacters(player->GetSession()->GetAccountId());
    CharacterDatabaseTransaction trans;
    uint32 synced = 0;

    for (CharacterInfo const& target : characters)
    {
        if (target.Guid == sourceGuid || !CanShareBetweenRaces(sourceRace, target.Race))
            continue;

        InsertCompanionForCharacter(trans, target.Guid, spellId);
        ++synced;
    }

    CommitIfNeeded(trans);

    if (synced)
        LOG_DEBUG("module.accountboundpets",
            "AccountBoundPets: synced companion spell {} from player {} to {} account character(s).",
            spellId, sourceGuid, synced);
}

void SeedCompanionsForCharacter(Player* player)
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

    CharacterDatabaseTransaction trans;
    uint32 inserted = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 const spellId = fields[0].Get<uint32>();
        uint8 const sourceRace = fields[1].Get<uint8>();

        if (!IsCompanionSpell(spellId) || !CanShareBetweenRaces(sourceRace, targetRace))
            continue;

        InsertCompanionForCharacter(trans, targetGuid, spellId);
        ++inserted;
    } while (result->NextRow());

    CommitIfNeeded(trans);

    if (inserted)
        LOG_INFO("module.accountboundpets",
            "AccountBoundPets: seeded {} companion spell row(s) for character {}.",
            inserted, targetGuid);
}

void BackfillAllCompanions()
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
        uint32 const accountId = fields[1].Get<uint32>();
        if (AccountBound::IsExcludedAccount(accountId))
            continue;

        charactersByAccount[accountId].push_back({
            fields[0].Get<uint32>(),
            fields[2].Get<uint8>()
        });
    } while (charactersResult->NextRow());

    QueryResult spellsResult = CharacterDatabase.Query(
        "SELECT DISTINCT c.account, c.race, cs.spell "
        "FROM character_spell cs "
        "INNER JOIN characters c ON c.guid = cs.guid "
        "WHERE c.account <> 0");

    if (!spellsResult)
        return;

    CharacterDatabaseTransaction trans;
    uint64 queued = 0;

    do
    {
        Field* fields = spellsResult->Fetch();
        uint32 const accountId = fields[0].Get<uint32>();
        uint8 const sourceRace = fields[1].Get<uint8>();
        uint32 const spellId = fields[2].Get<uint32>();

        if (!IsCompanionSpell(spellId))
            continue;

        auto accountItr = charactersByAccount.find(accountId);
        if (accountItr == charactersByAccount.end())
            continue;

        for (CharacterInfo const& target : accountItr->second)
        {
            if (!CanShareBetweenRaces(sourceRace, target.Race))
                continue;

            InsertCompanionForCharacter(trans, target.Guid, spellId);
            ++queued;
        }
    } while (spellsResult->NextRow());

    CommitIfNeeded(trans);
    LOG_INFO("module.accountboundpets",
        "AccountBoundPets: startup backfill queued {} companion spell row(s).", queued);
}

void LoadModuleConfig()
{
    Config.Enabled = AccountBound::IsCategoryEnabled("Pets");
    Config.StartupBackfill = sConfigMgr->GetOption<bool>("AccountBound.Pets.StartupBackfill", true);
    Config.SyncOnCreate = sConfigMgr->GetOption<bool>("AccountBound.Pets.SyncOnCreate", true);
    Config.SameFactionOnly = sConfigMgr->GetOption<bool>("AccountBound.Pets.SameFactionOnly", false);
    Config.IncludeCompanionSkillLine = sConfigMgr->GetOption<bool>("AccountBound.Pets.IncludeCompanionSkillLine", true);
    Config.IncludeMinipetSummons = sConfigMgr->GetOption<bool>("AccountBound.Pets.IncludeMinipetSummons", true);
    Config.Filter = AccountBound::LoadIdFilter("Pets");
}
}

class AccountBoundPetsWorldScript : public WorldScript
{
public:
    AccountBoundPetsWorldScript() : WorldScript("AccountBoundPetsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadModuleConfig();
        LOG_INFO("module.accountboundpets",
            "AccountBoundPets: {}. StartupBackfill={}, CreateSync={}, SameFactionOnly={}.",
            Config.Enabled ? (reload ? "configuration reloaded" : "configuration loaded") : "disabled",
            Config.Enabled && Config.StartupBackfill ? "on" : "off",
            Config.Enabled && Config.SyncOnCreate ? "on" : "off",
            Config.Enabled && Config.SameFactionOnly ? "on" : "off");
    }

    void OnStartup() override
    {
        if (!Config.Enabled)
            return;

        BuildCompanionCache();
        BackfillAllCompanions();
    }
};

class AccountBoundPetsPlayerScript : public PlayerScript
{
public:
    AccountBoundPetsPlayerScript() : PlayerScript("AccountBoundPetsPlayerScript", {
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_LEARN_SPELL
    }) { }

    void OnPlayerCreate(Player* player) override
    {
        if (Config.SyncOnCreate)
            SeedCompanionsForCharacter(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        SyncCompanionToAccount(player, spellId);
    }
};

void AddAccountBoundPetsScripts()
{
    new AccountBoundPetsWorldScript();
    new AccountBoundPetsPlayerScript();
}
