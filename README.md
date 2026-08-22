# ReHLDS_Sven [![C/C++ CI](https://github.com/coffeegrind123/ReHLDS_Sven/actions/workflows/build.yml/badge.svg)](https://github.com/coffeegrind123/ReHLDS_Sven/actions/workflows/build.yml) [![GitHub release](https://img.shields.io/github/v/release/coffeegrind123/ReHLDS_Sven?sort=semver)](https://github.com/coffeegrind123/ReHLDS_Sven/releases/latest) ![GitHub all releases](https://img.shields.io/github/downloads/coffeegrind123/ReHLDS_Sven/total) [![Upstream](https://img.shields.io/badge/upstream-rehlds%2FReHLDS%203.15.0.898-blue)](https://github.com/rehlds/ReHLDS) [![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](./LICENSE) <img align="right" width="128" src="https://cdn2.steamgriddb.com/icon_thumb/6a9aeddfc689c1d0e3b9ccc3ab651bc5.png" alt="Sven Co-op" />
Reverse-engineered (and bugfixed) HLDS, extended to speak the Sven Co-op protocol

## What is this?
ReHLDS is a result of reverse engineering of original HLDS (build 6152/6153) using DWARF debug info embedded into linux version of HLDS, engine_i486.so

**ReHLDS_Sven is an extension to the original ReHLDS project, which adds support of the Sven Co-op (Svengine, a game engine based on the original Half-Life 1 engine - GoldSrc) protocol.**

Along with reverse engineering, a lot of defects and (potential) bugs were found and fixed

> [!NOTE]
> This is a fork of [rehlds/ReHLDS](https://github.com/rehlds/ReHLDS), rebuilt on top of current
> upstream rather than drifting from an old branch point. The Sven Co-op work — originally from
> [autisoid/ReHLDS_Sven](https://github.com/autisoid/ReHLDS_Sven) — is replayed commit by commit
> onto upstream, and all of it is guarded by the `REHLDS_SVEN` compile-time define, so the tree
> still builds as stock ReHLDS with that define removed from `rehlds/CMakeLists.txt` (or the MSVC
> project).
>
> The engine reports the **upstream** version it is built on, so `sv_version` lines up with the
> ReHLDS release this actually corresponds to. See [Maintaining this fork](#maintaining-this-fork).

## What this fork adds

| | |
|---|---|
| **Sven Co-op protocol** | Protocol 48 as Svengine speaks it: widened user messages, long-encoded coords and fragment headers, raised `qlimits`, no packet munging, Sven's consistency/delta framing |
| **Ready-to-run plugin stack** | Releases bundle [Metamod-R](https://github.com/rehlds/Metamod-R) + [ReUnion](https://github.com/rehlds/reunion) in a `gamedir/` tree, configured to accept non-Steam clients |
| **Automatic ReUnion salt** | The engine generates a per-server `SteamIdHashSalt` on first run and preserves it across upgrades |
| **Sven-specific cvars** | `sv_rehlds_sven_block_game_bans`, `sv_rehlds_sven_tolerate_steam_deny`, `sv_rehlds_maxusrcmdprocessticks`, `sv_rehlds_force_allow_lagcompensation`, `sv_log_daily` |
| **Retail `server.so` fixes** | Recovers from the unterminated `MESSAGE_BEGIN` that killed servers ~30x/day, fixes a `DELTA_ParseDelta` stack overflow, honours `-nobreakpad` |
| **Steam deny diagnostics** | Every Steam client deny is logged with its reason code, and the four Steam-*connectivity* reasons no longer drop legitimate players |

## Goals of the project
<ul>
<li>Provide more stable (than official) version of Sven Co-op dedicated server with extended API for mods and plugins</li>
<li>Performance optimizations (use of SSE for vector math for example) is another goal for the future</li>
<li>Stay rebasable on upstream ReHLDS rather than drifting into a hard fork</li>
</ul>

## 🛠 License

ReHLDS is licensed under the [MIT License](./LICENSE).

### License Transition
> [!NOTE]  
> Originally released under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html), ReHLDS transitioned to the MIT License in July 2025 with the agreement of the core contributors.  
> See [LICENSE-TRANSITION.md](./LICENSE-TRANSITION.md) for details.

## How can use it?
ReHLDS_Sven is fully compatible with the official SvenDS binaries, although it requires replacing `libsteam_api.so` (located at `rehlds/lib(/linux32)`) and `steamclient.so` (either take it from our releases or from steamcmd) to work.

> [!CAUTION]  
> This project works only with Sven Co-op 5.26.

#### Downloading SvenDS via steamcmd

```
app_update 276060 validate
```

## Downloads
* [Release builds](https://github.com/coffeegrind123/ReHLDS_Sven/releases)
* [Dev builds](https://github.com/coffeegrind123/ReHLDS_Sven/actions/workflows/build.yml)

ReHLDS_Sven binaries require `SSE`, `SSE2` and `SSE3` instruction sets to run and can benefit from `SSE4.1` and `SSE4.2`

<b>Warning!</b> ReHLDS_Sven is not binary compatible with original svends since it's compiled with compilers other than ones used for original svends.
This means that plugins that do binary code analysis (Orpheu for example) probably will not work with ReHLDS_Sven.

### What is in a release

`rehlds-sven-bin-<version>.zip`:

| path | what |
|---|---|
| `bin/linux32/`, `bin/win32/` | engine, dedicated launcher, HLTV and filesystem binaries |
| `hlsdk/` | headers for building plugins against this engine |
| `gamedir/` | drop-in overlay for your mod directory — see below |

`rehlds-sven-dbg-<version>.7z` holds the matching debug symbols.

> [!TIP]
> Linux release binaries are built in `debian:11-slim`, so they require only `GLIBC_2.7`
> (`engine_i486.so`) and `GLIBC_2.1` (`hlds_linux`) and will run on anything newer.

### The bundled `gamedir/` overlay

Copy its contents into your mod directory (e.g. `svencoop/`). It is the plugin stack a
non-Steam server needs, already wired up:

| path | what |
|---|---|
| `addons/metamod/metamod_i386.so`, `metamod.dll` | [Metamod-R](https://github.com/rehlds/Metamod-R) `1.3.0.149` |
| `addons/metamod/config.ini` | points Metamod at the real game library |
| `addons/metamod/plugins.ini` | plugin list, ReUnion first |
| `addons/reunion/reunion_mm_i386.so`, `reunion_mm.dll` | [ReUnion](https://github.com/rehlds/reunion) `0.2.0.25` |
| `reunion.cfg` | ReUnion config, **non-Steam clients accepted** |
| `rotate-reunion-salt.sh` | forces a new `SteamIdHashSalt` (see below) |

> [!IMPORTANT]
> ReUnion ships `cid_NoSteam47 = 5` and `cid_NoSteam48 = 5` by default, and `5` means
> **drop the client** — the opposite of why ReUnion gets deployed. The bundled config sets
> both to `3` (accept, assign a generated `STEAM_x:y:z`). Everything else is ReUnion's own
> shipped config for the pinned version.

#### The ReUnion salt is handled for you

`reunion.cfg` ships with `SteamIdHashSalt = GENERATE_ON_FIRST_RUN`, and the engine replaces
it with a value from the OS CSPRNG the first time the server starts — a salt baked into a
public release would be the same for everyone who downloaded it, and so no salt at all.

The generated value is also written to `<gamedir>/.reunion_salt` (mode `0600`).

> [!WARNING]
> **Keep `.reunion_salt`.** It is what lets a newer release be unpacked over an existing
> install without minting a *new* salt — and a new salt changes every generated
> `STEAM_x:y:z`, which invalidates every ban and every stored per-player record.

A salt you set by hand is never touched, and `-noreunionsalt` disables the behaviour entirely.
An empty salt is not a safe default: with `AuthVersion = 3` it makes ReUnion fail to initialise
almost silently — Metamod still reports the plugin as configured and the server boots and plays
normally, and only `meta list` reveals it never loaded.

## Configuring
<details>
<summary>Click to expand</summary>
<ul>
<li>listipcfgfile &lt;filename&gt; // File for permanent ip bans. Default: listip.cfg
<li>syserror_logfile &lt;filename&gt; // File for the system error log. Default: sys_error.log
<li>sv_auto_precache_sounds_in_models &lt;1|0&gt; // Automatically precache sounds attached to models. Deault: 0
<li>sv_delayed_spray_upload &lt;1|0&gt; // Upload custom sprays after entering the game instead of when connecting. It increases upload speed. Default: 0
<li>sv_echo_unknown_cmd &lt;1|0&gt; // Echo in the console when trying execute an unknown command. Default: 0
<li>sv_rcon_condebug &lt;1|0&gt; // Print rcon debug in the console. Default: 1
<li>sv_force_ent_intersection &lt;1|0&gt; // In a 3-rd party plugins used to force colliding of SOLID_SLIDEBOX entities. Default: 0
<li>sv_rehlds_force_dlmax &lt;1|0&gt; // Force a client's cl_dlmax cvar to 1024. It avoids an excessive packets fragmentation. Default: 0
<li>sv_rehlds_hull_centering &lt;1|0&gt; // Use center of hull instead of corner. Default: 0
<li>sv_rehlds_movecmdrate_max_avg // Max average level of 'move' cmds for ban. Default: 400
<li>sv_rehlds_movecmdrate_avg_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_movecmdrate_max_burst // Max burst level of 'move' cmds for ban. Default: 2500
<li>sv_rehlds_movecmdrate_burst_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_send_mapcycle &lt;1|0&gt; // Send mapcycle.txt in serverinfo message (HLDS behavior, but it is unused on the client). Default: 0
<li>sv_rehlds_stringcmdrate_max_avg // Max average level of 'string' cmds for ban. Default: 80
<li>sv_rehlds_stringcmdrate_avg_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_stringcmdrate_max_burst // Max burst level of 'string' cmds for ban. Default: 400
<li>sv_rehlds_stringcmdrate_burst_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_userinfo_transmitted_fields // Userinfo fields only with these keys will be transmitted to clients via network. If not set then all fields will be transmitted (except prefixed with underscore). Each key must be prefixed by backslash, for example "\name\model\*sid\*hltv\bottomcolor\topcolor". See [wiki](https://github.com/rehlds/ReHLDS/wiki/Userinfo-keys) to collect sufficient set of keys for your server. Default: ""
<li>sv_rehlds_attachedentities_playeranimationspeed_fix // Fixes bug with gait animation speed increase when player has some attached entities (aiments). Can cause animation lags when cl_updaterate is low. Default: 0
<li>sv_rehlds_maxclients_from_single_ip // Limit number of connections at the same time from single IP address, not confuse to already connected players. Default: 12
<li>sv_rehlds_local_gametime &lt;1|0&gt; // A feature of local gametime which decrease "lags" if you run same map for a long time. Default: 0
<li>sv_rehlds_allow_large_sprays &lt;1|0&gt; // Allow larger custom logos than 64x64. Default: 1
<li>sv_use_entity_file // Use custom entity file for a map. Path to an entity file will be "maps/[map name].ent". 0 - use original entities. 1 - use .ent files from maps directory. 2 - use .ent files from maps directory and create new .ent file if not exist.
<li>sv_usercmd_custom_random_seed // When enabled server will populate an additional random seed independent of the client. Default: 0
<li>sv_net_incoming_decompression &lt;1|0&gt; // When enabled server will decompress of incoming compressed file transfer payloads. Default: 1
<li>sv_net_incoming_decompression_max_ratio &lt;0|100&gt; // Sets the max allowed ratio between compressed and uncompressed data for file transfer. (A ratio close to 90 indicates large uncompressed data with low entropy) Default: 80.0
<li>sv_net_incoming_decompression_max_size &lt;16|65536&gt; // Sets the max allowed size for decompressed file transfer data. Default: 65536 bytes
<li>sv_net_incoming_decompression_min_failures &lt;0|10&gt; // Sets the min number of decompression failures required before a player's connection is flagged for potential punishment. Default: 4
<li>sv_net_incoming_decompression_max_failures &lt;0|10&gt; // Sets the max number of decompression failures allowed within a specified time window before action is taken against the player. Default: 10
<li>sv_net_incoming_decompression_min_failuretime: &lt;0.1|10.0&gt; // Sets the min time in secs within which decompression failures are tracked to determine if the player exceeds the failure thresholds. Default: 0.1
<li>sv_net_incoming_decompression_punish // Time in minutes for which the player will be banned for malformed/abnormal bzip2 fragments (0 - Permanent, use a negative number for a kick). Default: -1
<li>sv_tags &lt;comma-delimited string list of tags&gt; // Sets a string defining the "gametags" for this server, this is optional, but if it is set it allows users/scripts to filter in the matchmaking/server-browser interfaces based on the value. Default: ""
<li>sv_filterban &lt;-1|0|1&gt;// Set packet filtering by IP mode. -1 - All players will be rejected without any exceptions. 0 - No checks will happen. 1 - All incoming players will be checked if they're IP banned (if they have an IP filter entry), if they are, they will be kicked. Default: 1
<li>sv_rehlds_movecmd_max_ticks // Set maximum amount of movement commands the server is able to process from a single player in a single frame. This includes the commands itself, not packets. Default: 24
<li>sv_rehlds_movecmd_max_null_streak // Defines the maximum allowed consecutive movement commands with zero time duration (empty commands). 0 - disables the check. Default: 0
<li>sv_rehlds_movecmd_clamp_interp &lt;1|0&gt; // Defines whether should the server block movement commands with invalid (out of range) "ex_interp" value. Default: 1
<li>sv_rehlds_movecmdtime_samples // Defines the number of frames the server takes to average the client's movement speed. Higher - more accurate but slower detection, lower - vice versa. Default: 120
<li>sv_rehlds_movecmdtime_max_error // Defines how far a client's internal game clock can go ahead of or behind the server's clock in milliseconds. If this limit is exceeded, the server evaluates the client's game speed. Penalties are ONLY applied if the client also violates the "sv_rehlds_movecmdtime_max_scale" or "sv_rehlds_movecmdtime_min_scale" limits. Default: 300
<li>sv_rehlds_movecmdtime_max_scale // Defines the max client's base game speed ratio. Clients speeding the game up beyond this multiplier will receive warnings. Default: 3.0
<li>sv_rehlds_movecmdtime_min_scale // Defines the min client's base game speed ratio. Clients slowing the game down below this multiplier will receive warnings. Default: 0.5
<li>sv_rehlds_movecmdtime_max_warnings // Maximum allowed speedhack/slowmo warnings before the punishment is applied. -1 - disable detection. Default: -1
<li>sv_rehlds_movecmdtime_punish // Time in minutes for which the player will be banned for speedhacking/slowing (-1 - Kick, 0 - Permanent, use a negative number for a kick). Default: -1
</ul>

#### Sven Co-op specific (built with `REHLDS_SVEN`)
<ul>
<li>sv_rehlds_sven_block_game_bans &lt;1|0&gt; // Block shadow bans issued by the game .dll (kick/ban commands the game library injects for players it has blacklisted server-side). Default: 1
<li>sv_rehlds_sven_tolerate_steam_deny &lt;1|0&gt; // Keep a client that Steam denies for a Steam-<i>connectivity</i> reason (SteamConnectionError, SteamConnectionLost, SteamResponseTimedOut, NotLoggedOn) instead of dropping them. Those four are verdicts about the server's own link to Steam, not about the player, and on a server whose identities come from ReUnion they would otherwise give a legitimate Steam owner a worse experience than a non-Steam client. Client-side verdicts (Cheater/VAC, NoLicense, InvalidVersion, IncompatibleAnticheat, IncompatibleSoftware, MemoryCorruption, LoggedInElseWhere) are always enforced. Every deny is logged with its reason code either way. 0 restores stock behaviour. Default: 1
<li>sv_rehlds_maxusrcmdprocessticks // Max number of usercmds processed for a single client within one server frame; 0 disables the limit. Default: 24
<li>sv_rehlds_force_allow_lagcompensation &lt;1|0&gt; // Force lag compensation on regardless of what the client asks for. Default: 0
<li>sv_log_daily &lt;1|0&gt; // Roll the server log over to a new file each day rather than only on map change. Default: 1
</ul>

#### Command-line parameters changed by this fork
<ul>
<li>-noreunionsalt // Skip localising the ReUnion SteamIdHashSalt on startup. Only relevant with the bundled reunion.cfg; a salt set by hand is never touched regardless.
<li>-nobreakpad // Stock HLDS parameter, now also honoured in FileSystem_SetGameDirectory — previously that call pulled in Steam's breakpad crash handler regardless, so the parameter only half worked.
</ul>
</details>

## Commands
<ul>
<li>rescount // Prints the total count of precached resources in the server console
<li>reslist &lt;sound | model | decal | generic | event&gt; // Separately prints the details of the precached resources for sounds, models, decals, generic and events in server console. Useful for managing resources and dealing with the goldsource precache limits.
<li>rcon_adduser &lt;ipaddress/CIDR&gt; // Add a new IP address or CIDR range to RCON user list (This command adds a new IP address to the RCON user list. The specified IP or CIDR range is granted privileged access to server console. Without any Rcon users, access is allowed to anyone with a valid password)</li>
<li>rcon_deluser &lt;ipaddress&gt; {removeAll} // Remove an IP address or CIDR range from RCON user list</li>
<li>rcon_users // List all IP addresses and CIDR ranges in RCON user list</li>
</ul>

## Build instructions
### Checking requirements
There are several software requirements for building ReHLDS:

#### Windows
<pre>
Visual Studio 2015 (C++14 standard) and later
</pre>

#### Linux
<pre>
cmake >= 3.10
GCC >= 4.9.2 (Optional)
ICC >= 15.0.1 20141023 (Optional)
LLVM (Clang) >= 6.0 (Optional)
</pre>

### Building

#### Windows
Use `Visual Studio` to build, open `msvc/ReHLDS.sln` and just select from the solution configurations list `Release Swds` or `Debug Swds`

#### Linux

* Optional options using `build.sh --compiler=[gcc] --jobs=[N] -D[option]=[ON or OFF]` (without square brackets)

<pre>
-c=|--compiler=[icc|gcc|clang]  - Select preferred C/C++ compiler to build
-j=|--jobs=[N]                  - Specifies the number of jobs (commands) to run simultaneously (For faster building)

<sub>Definitions (-D)</sub>
DEBUG                           - Enables debugging mode
USE_STATIC_LIBSTDC              - Enables static linking library libstdc++
</pre>

* ICC          <pre>./build.sh --compiler=intel</pre>
* LLVM (Clang) <pre>./build.sh --compiler=clang</pre>
* GCC          <pre>./build.sh --compiler=gcc</pre>

##### Checking build environment (Debian / Ubuntu)

<details>
<summary>Click to expand</summary>

<ul>
<li>
Installing required packages
<pre>
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y gcc-multilib g++-multilib
sudo apt-get install -y build-essential
sudo apt-get install -y libc6-dev libc6-dev-i386
sudo apt-get install -y cmake			
</pre>
</li>

<li>
Select the preferred C/C++ Compiler installation
<pre>
1) sudo apt-get install -y gcc g++
2) sudo apt-get install -y clang
</pre>
</li>
</ul>
</details>

## Maintaining this fork

<details>
<summary>Click to expand</summary>

### Versioning

The engine reports the **upstream ReHLDS version this fork is built on**, not a
fork-inflated one. ReHLDS derives the build number from `git rev-list --count`, so
the commits this fork adds would otherwise inflate it past build numbers upstream
has not reached yet.

`rehlds/version/upstream_base` records the upstream commit currently rebased onto;
`appversion.sh` / `appversion.bat` count from there. Both fall back to counting from
`HEAD` if the file is missing or the SHA does not resolve.

### Rebasing onto a newer upstream

1. Replay this fork's commits onto the new `rehlds/ReHLDS` master.
2. Update `rehlds/version/upstream_base` to the new upstream commit **in the same
   commit that performs the rebase**, or the reported version will be wrong.
3. Confirm: `bash rehlds/version/appversion.sh .` should print the upstream version.

### Releases

Tags are `<upstream-version>-sven<n>`, e.g. `3.15.0.898-sven1`. The upstream version
comes first so the base is obvious and releases sort correctly; `-sven<n>` increments
for further releases on the same upstream base and resets after a rebase.

### Bundled plugin versions

Metamod-R and ReUnion are **pinned** in `.github/workflows/build.yml` (`METAMOD_VERSION`,
`REUNION_VERSION`) rather than tracking "latest", so a release is reproducible and only
ships versions this stack has actually been run against. Bump them deliberately.

The packaging step derives `reunion.cfg` from ReUnion's *own* shipped config for the pinned
version and changes only the authid policy, so it does not go stale against a bump. It then
guards its own output — both plugin binaries per platform, `cid_NoSteam47/48 == 3`, and the
salt sentinel matching the engine's `REUNION_SALT_SENTINEL` — and fails the build rather than
publishing a subtly broken archive.

Publishing a GitHub release triggers the build, and the `publish` job attaches
`rehlds-sven-bin-<version>.zip` and `rehlds-sven-dbg-<version>.7z`.

> [!NOTE]
> GitHub disables automatic workflow triggers on newly created forks. If a release or
> push does not start a run, open the **Actions** tab once and confirm the prompt to
> enable workflows. Until then a build can be started manually against the tag with
> `gh workflow run "C/C++ CI" --ref <tag>`, which still satisfies the publish job.

</details>

## How can I help the project?
Just install it on your game server and report problems you faced.
Merge requests are also welcome :)
