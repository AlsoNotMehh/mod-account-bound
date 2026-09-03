# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore Module: mod-account-bound

[![AzerothCore Module](https://img.shields.io/badge/AzerothCore-Module-red?style=flat-square&logo=github)](https://github.com/azerothcore/azerothcore-wotlk)
[![C++20](https://img.shields.io/badge/Language-C++20-00599C?style=flat-square&logo=c%2B%2B)](https://isocpp.org/)
[![Branch 3.3.5a](https://img.shields.io/badge/Branch-3.3.5a-orange?style=flat-square)](https://github.com/azerothcore/azerothcore-wotlk)
[![License GPLv3](https://img.shields.io/badge/License-GPLv3-blue?style=flat-square)](LICENSE)

A unified, high-performance account-wide progression system for **AzerothCore (WotLK 3.3.5a)** sharing achievements, mounts, companion pets, titles, reputations, professions, and friends across characters on the same account.

### 💡 Why this module?
Vanilla WotLK isolates every unlock to a single character. Players often hesitate to level alts because grinding the same reputations, achievements, rare mount drops, and titles repeatedly feels like a chore. Other account-wide modules suffer from massive login chat spam, achievement audio playback, or custom database dependencies.

**`AccountBound`** consolidates all account-wide systems into a single module using silent direct database synchronization, ensuring zero login lag, zero audio spam, and automatic faction conversions for cross-faction alts.

## 📊 Feature Comparison

| Feature | Stock AzerothCore | AccountBound |
| :--- | :---: | :---: |
| **Account-Wide Achievements** | ❌ Per-character only | ✅ **Synced with automatic Alliance $leftrightarrow$ Horde conversion** |
| **Account-Wide Mounts & Pets** | ❌ Per-character only | ✅ **Synced with riding skill, faction, and class checks** |
| **Account-Wide Titles & Reputations** | ❌ Per-character only | ✅ **Synced upwards (highest reputation standing shared)** |
| **Account-Wide Professions** | ❌ Per-character only | ✅ **Optional sync of profession skill levels and learned recipes** |
| **Account-Wide Friend Lists** | ❌ Per-character only | ✅ **Shared friends and notes across all characters on the account** |
| **Login Experience** | ⚠️ Chat spam / achievement popups | ✅ **Silent sync directly to core tables without popup spam** |
| **Database Requirements** | ❌ Requires custom tables in other mods | ✅ **Zero custom SQL tables; writes directly to core tables** |

## ⚙️ Technical Architecture

### 1. Silent Direct Table Synchronization
Instead of executing runtime `learnSpell()` calls or re-broadcasting achievement criteria on player login (which triggers network bursts, audio stutters, and chat spam), `AccountBound` syncs directly into AzerothCore's native character tables:
- `StartupBackfill`: Pre-migrates existing account data on server startup.
- `SyncOnCreate`: Pre-populates newly created characters before their very first world login.
- Real-time gameplay hooks seamlessly replicate newly earned unlocks to account siblings.

### 2. Intelligent Requirement Gates & Faction Conversion
- **Mount Requirements:** Mount spells are only activated on alts that meet the required riding skill level (`RequireRiding = 1`). If an alt levels up Riding, missing mounts backfill automatically.
- **Faction Mapping:** Faction-specific mounts (e.g. Wolf $leftrightarrow$ Horse) and titles (e.g. *of the Horde* $leftrightarrow$ *of the Alliance*) convert automatically when logging into opposite-faction alts.
- **Reputation Upward Merge:** Standings only move upward; a lower-reputation alt will never overwrite a character with Exalted status.

## 📋 Configuration Reference (`AccountBound.conf`)

| Setting | Default | Description |
| :--- | :---: | :--- |
| `AccountBound.Enable` | `1` | Master switch for all account-bound synchronization. |
| `AccountBound.ExcludedAccountNamePrefix` | `RND` | Account name prefix to exclude from synchronization (e.g. Playerbot random bots). |
| `AccountBound.Achievements.Enable` | `1` | Enables account-wide achievement synchronization. |
| `AccountBound.Achievements.ConvertFactionSpecific` | `1` | Converts faction-specific achievements across alts. |
| `AccountBound.Mounts.Enable` | `1` | Enables account-wide mount sharing. |
| `AccountBound.Mounts.RequireRiding` | `1` | Enforces riding skill requirements before granting mounts. |
| `AccountBound.Pets.Enable` | `1` | Enables account-wide companion vanity pets. |
| `AccountBound.Titles.Enable` | `1` | Enables account-wide player titles. |
| `AccountBound.Reputations.Enable` | `1` | Enables account-wide reputation standings (highest rank). |
| `AccountBound.Professions.Enable` | `0` | Optional account-wide profession ranks and recipe sync. |
| `AccountBound.Friends.Enable` | `1` | Enables account-wide friend lists and personal notes. |

## 🛠️ Installation

1. Place the module in `azerothcore-wotlk/modules/`:
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/AlsoNotMehh/mod-account-bound.git
   ```

2. Re-run CMake and compile your server:
   ```bash
   cmake -B build
   cmake --build build --config Release
   ```

3. Copy `conf/AccountBound.conf.dist` to your `worldserver` configs directory as `AccountBound.conf` and customize as needed.

## ⭐ Show your support

If you find this module helpful for your server, please consider giving it a star on GitHub! It helps more developers in the AzerothCore community discover the project.

## 🤝 Credits

- **Author & Enhancements:** [AlsoNotMehh](https://github.com/AlsoNotMehh) ([Discord](https://discord.com/users/1063304041419001966) / [Email](mailto:itsbrayanrodriguez@gmail.com))
- **Framework:** [AzerothCore](https://www.azerothcore.org)

## 📜 License

This project is licensed under the [GPL-3.0 License](LICENSE).

