#ifndef ACCOUNT_BOUND_H
#define ACCOUNT_BOUND_H

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <charconv>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace AccountBound
{
inline std::unordered_map<uint32, bool> ExcludedAccountCache;

inline void ClearExcludedAccountCache()
{
    ExcludedAccountCache.clear();
}

inline bool HasPrefixIgnoreCase(std::string_view value, std::string_view prefix)
{
    if (value.size() < prefix.size())
        return false;

    for (std::size_t index = 0; index < prefix.size(); ++index)
        if (std::toupper(static_cast<unsigned char>(value[index])) !=
            std::toupper(static_cast<unsigned char>(prefix[index])))
            return false;

    return true;
}

inline bool IsExcludedAccount(uint32 accountId)
{
    std::string const excludedAccountPrefix = sConfigMgr->GetOption<std::string>(
        "AccountBound.ExcludedAccountNamePrefix", "RND");
    if (!accountId || excludedAccountPrefix.empty())
        return false;

    if (auto const itr = ExcludedAccountCache.find(accountId); itr != ExcludedAccountCache.end())
        return itr->second;

    QueryResult const result = LoginDatabase.Query("SELECT username FROM account WHERE id = {}", accountId);
    bool const isExcluded = result &&
        HasPrefixIgnoreCase(result->Fetch()[0].Get<std::string>(), excludedAccountPrefix);
    ExcludedAccountCache.emplace(accountId, isExcluded);
    return isExcluded;
}

struct IdFilter
{
    bool AllowAll = true;
    std::unordered_set<uint32> Allowed;
    std::unordered_set<uint32> Blocked;

    bool Allows(uint32 id) const
    {
        return id && !Blocked.contains(id) && (AllowAll || Allowed.contains(id));
    }
};

inline std::string Trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;

    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;

    return std::string(value.substr(first, last - first));
}

inline void ParseIds(std::string const& raw, std::unordered_set<uint32>& destination)
{
    std::size_t start = 0;
    while (start <= raw.size())
    {
        std::size_t end = raw.find(',', start);
        if (end == std::string::npos)
            end = raw.size();

        std::string token = Trim(std::string_view(raw).substr(start, end - start));
        if (!token.empty())
        {
            uint32 id = 0;
            auto const [ptr, error] = std::from_chars(token.data(), token.data() + token.size(), id);
            if (error == std::errc() && ptr == token.data() + token.size() && id)
                destination.insert(id);
            else
                LOG_WARN("module.accountbound", "AccountBound: ignored invalid ID '{}' in an AllowList or BlockList.", token);
        }

        if (end == raw.size())
            break;

        start = end + 1;
    }
}

inline bool IsCategoryEnabled(std::string_view category, bool defaultValue = true)
{
    if (!sConfigMgr->GetOption<bool>("AccountBound.Enable", true))
        return false;

    return sConfigMgr->GetOption<bool>(
        "AccountBound." + std::string(category) + ".Enable", defaultValue);
}

inline IdFilter LoadIdFilter(std::string_view category)
{
    IdFilter filter;
    std::string const prefix = "AccountBound." + std::string(category);
    std::string const allowList = Trim(sConfigMgr->GetOption<std::string>(prefix + ".AllowList", "all"));

    if (!allowList.empty() && allowList != "all" && allowList != "ALL" && allowList != "*")
    {
        filter.AllowAll = false;
        if (allowList != "none" && allowList != "NONE")
            ParseIds(allowList, filter.Allowed);
    }

    ParseIds(sConfigMgr->GetOption<std::string>(prefix + ".BlockList", ""), filter.Blocked);
    return filter;
}
}

#endif
