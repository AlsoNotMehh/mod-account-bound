#include "AccountBound.h"
#include "WorldScript.h"

namespace
{
class AccountBoundConfigurationScript : public WorldScript
{
public:
    AccountBoundConfigurationScript() : WorldScript("AccountBoundConfigurationScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        AccountBound::ClearExcludedAccountCache();
    }
};
}

void AddAccountBoundAchievementsScripts();
void AddAccountBoundMountsScripts();
void AddAccountBoundPetsScripts();
void AddAccountBoundProfessionsScripts();
void AddAccountBoundReputationsScripts();
void AddAccountBoundTitlesScripts();
void AddAccountWideFriendsScripts();

void AddAccountBoundScripts()
{
    new AccountBoundConfigurationScript();
    AddAccountBoundAchievementsScripts();
    AddAccountBoundMountsScripts();
    AddAccountBoundPetsScripts();
    AddAccountBoundProfessionsScripts();
    AddAccountBoundReputationsScripts();
    AddAccountBoundTitlesScripts();
    AddAccountWideFriendsScripts();
}

void Addmod_account_boundScripts()
{
    AddAccountBoundScripts();
}
