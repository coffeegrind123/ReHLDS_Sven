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
| **Mixed Sven + Half-Life clients** | The dialect is chosen **per client, at runtime**, so retail Sven Co-op 5.26 players and stock Half-Life players (vanilla, or the [SevenKewp](https://github.com/wootguy/SevenKewp) client) can play on the same server — see [Mixed-client servers](#mixed-client-servers) |
| **Ready-to-run plugin stack** | Releases bundle [metamod-fallguys](https://github.com/hzqst/metamod-fallguys) + [ReUnion](https://github.com/rehlds/reunion) in a `gamedir/` tree, configured to accept non-Steam clients |
| **Automatic ReUnion salt** | The engine generates a per-server `SteamIdHashSalt` on first run and preserves it across upgrades |
| **Sven-specific cvars** | `sv_rehlds_sven_block_game_bans`, `sv_rehlds_sven_tolerate_steam_deny`, `sv_rehlds_maxusrcmdprocessticks`, `sv_rehlds_force_allow_lagcompensation`, `sv_log_daily` |
| **Retail `server.so` fixes** | Recovers from the unterminated `MESSAGE_BEGIN` that killed servers ~30x/day, fixes a `DELTA_ParseDelta` stack overflow, honours `-nobreakpad` |
| **Steam deny diagnostics** | Every Steam client deny is logged with its reason code, and the four Steam-*connectivity* reasons no longer drop legitimate players |

## Mixed-client servers

Sven Co-op and Half-Life both announce **protocol 48**, but Svengine widened a set of wire
fields and dropped packet munging. Historically this fork picked one of the two encodings at
compile time, so a server spoke Sven *or* Half-Life and never both.

It is now chosen **per client**, at runtime. One server, running the retail Sven Co-op
`server.so`, can serve a retail Sven 5.26 player and a stock Half-Life player at the same
time.

### How a client's dialect is decided

By its **first netchan packet**, not by anything it claims about itself.

The netchan header (bytes 0-7) is plain in both dialects; everything from byte 8 on is
`COM_Munge2`'d for Half-Life and plain for Sven. The client speaks first over the netchan —
its opening message is `clc_stringcmd "new"` — so the server decodes that payload both ways
and keeps whichever parses as a valid `clc` stream. A userinfo key would be a claim the
client makes about itself, and can be absent, stale or spoofed; the munge either round-trips
or it does not.

Nothing dialect-dependent is emitted before that point: everything up to and including the
`connect` reply is connectionless, and `svc_serverinfo` is composed in response to the very
`new` that resolves the dialect.

| cvar | default | |
|---|---|---|
| `sv_proto_dialect` | `auto` | `auto` detects; `sven` or `hl` forces every client, for testing |
| `sv_proto_fallback` | `sven` | what to assume if four probes in a row are inconclusive |
| `sv_proto_log` | `0` | `1` logs each verdict and every `connect`'s protinfo/userinfo; `2` adds hex dumps of both decodings |

`status` gains a `proto` column showing what each connected player is being served.

### What a Half-Life client gives up

These are ceilings of the stock protocol, not bugs — a Half-Life client cannot represent
them at all, so the engine clamps or truncates rather than sending something it will
misparse:

| | |
|---|---|
| **Coordinates** | Byte-aligned coords are shorts (±4096 units) and bit-packed ones have a 12-bit integer part. Values past that are **clamped**, not wrapped — a clamped position is wrong but bounded; a wrapped one lands on the far side of the map. |
| **Entity indices** | 11 bits, so entities past 2047 are not addressable. Sven's are 13. |
| **Delta fields** | The bitmask length is 3 bits, so at most 7 bytes, so at most **56 fields**. Measured against retail 5.26's `svencoop/delta.lst` this costs exactly one: `entity_state_t::gaitsequence` (#57). Every other struct fits — `clientdata_t` 34, `entity_state_player_t` 51, `weapon_data_t` 22, `custom_entity_state_t` 20, `usercmd_t` 15, `event_t` 14. Half-Life clients lose leg animation on non-player entities and nothing else. |
| **Weapon slots** | 64, not 256 — and the engine sends at most **63**, because a stock client's reader exits on its loop bound and would leave the list terminator unread at exactly 64. |
| **User messages** | Registered as variable-length, since narrowing coordinates changes the payload length after the size was advertised at signon. One that still exceeds 255 bytes cannot be framed and is dropped with a `Con_DPrintf`. |
| **Unreliable payload** | Capped at 4000 bytes and at `MAX_ROUTEABLE_PACKET`, rather than Sven's 65000. |
| **Resource / consistency indices** | 12 and 10 bits respectively, against Sven's 16. |

The game DLL is a separate matter from the wire. The engine will frame every message
correctly for both clients; whether a Half-Life client has the *content* (models, sounds,
sprites) and the client-side message handlers to make sense of what a Sven mod sends it is up
to the mod and the gamedir, not the engine.

### How it is built

The whole thing keys off one bit on `sizebuf_t::flags` (`SIZEBUF_PROTO_HL`). A buffer with
no stamp is native Sven, so **every path that predates this layer behaves exactly as it did**
— only buffers destined for a Half-Life client diverge. The `MSG_*` primitives read the stamp
off the buffer they are writing to, or off the bit writer's current buffer, so a call site
says `PROTO_BITS(ENTITY_NUMBER, num)` and never has to know who the recipient is.

Every divergence is declared once, in `PROTO_BITFIELD_LIST` in
[`rehlds/engine/sv_proto.h`](rehlds/engine/sv_proto.h), so a field's two widths cannot drift
apart at the call sites. ⚠ Note that the event `packet_index` is **narrower** under Sven (10
bits vs 11) while everything else is wider — folding it in with the entity indices by reflex
is a mistake that has already cost one debugging session.

Four buffers are composed once and replayed to every client — the signon block, the broadcast
datagram, the multicast staging buffer and the spectator datagram — so those are built
**twice**, once per dialect, and the consumer picks by recipient. Transcoding on delivery was
rejected: it would need a parser for every `svc` message and would rot the first time one
changed. `g_psv.reliable_datagram` is deliberately not twinned; its `MSG_ALL` user messages
are already fanned out per client and what remains is dialect-neutral.

`rehlds/unittests/proto_tests.cpp` round-trips every field in the table through both stamps.

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

## How do I use it?
ReHLDS_Sven is a drop-in replacement for the engine in an official SvenDS install. Install
SvenDS first, then overwrite the files below.

> [!CAUTION]  
> This project works only with Sven Co-op 5.26.

#### Downloading SvenDS via steamcmd

```
app_update 276060 validate
```

### Installing over a SvenDS install

From `bin/linux32/` in the release (`bin/win32/` on Windows), copy into the SvenDS root:

| file | why |
|---|---|
| `engine_i486.so` / `swds.dll` | the fork itself |
| `hlds_linux` / `hlds.exe` | dedicated launcher, matched to the engine |
| `libsteam_api.so` / `steam_api.dll` | **required**, and easy to miss — SvenDS ships its own copy and it has to be overwritten too |

Then copy the `gamedir/` overlay into your mod directory — see
[The bundled `gamedir/` overlay](#the-bundled-gamedir-overlay).

> [!IMPORTANT]
> `steamclient.so` is **not** in our releases and is not part of this project. It comes from
> steamcmd — the Steamworks SDK redist depot, alongside SvenDS itself — and the engine expects
> to find it at `~/.steam/sdk32/steamclient.so`, *not* in the server directory. A symlink is
> the usual way:
>
> ```sh
> mkdir -p ~/.steam/sdk32
> ln -sf /path/to/svends/steamclient.so ~/.steam/sdk32/steamclient.so
> ```

The release also ships `filesystem_stdio.so`, `hltv`, `core.so`, `demoplayer.so` and
`proxy.so`. These are ReHLDS builds of stock components and are not required: SvenDS's own
copies work, and the reference deployment for this fork runs retail's `filesystem_stdio.so`
against our engine. Replace them only if you have a reason to.

### Upgrading an existing install

Overwrite the same three files and restart. Two things not to do:

- **Do not delete `<gamedir>/.reunion_salt`.** A new salt changes every generated
  `STEAM_x:y:z`, which invalidates every ban and stored per-player record. See
  [the salt section](#the-reunion-salt-is-handled-for-you).
- **Do not re-copy `gamedir/` unless the pinned plugin versions changed.** It would overwrite
  a `reunion.cfg` you have edited. The pinned versions are in the release notes and in
  [Bundled plugin versions](#bundled-plugin-versions).

To confirm what is actually running, check the engine's own build string:

```sh
strings -a engine_i486.so | grep -E '^(ReHLDS version|Build from)'
```

A `+m` suffix on the version (`3.15.0.898-dev+m`) means that binary was built from a modified
working tree rather than from the commit it names — a local build, not a release one.

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
| `bin/linux32/`, `bin/win32/` | engine, dedicated launcher, `libsteam_api.so` / `steam_api.dll`, plus HLTV and filesystem binaries |
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
| `addons/metamod/dlls/metamod.so`, `metamod.dll` | [metamod-fallguys](https://github.com/hzqst/metamod-fallguys) `1.21p38` — note the `dlls/` subdirectory |
| `addons/metamod/config.ini` | points metamod at the real game library (key: `gamedll`) |
| `addons/metamod/plugins.ini` | plugin list, ReUnion first |
| `addons/reunion/reunion_mm_i386.so`, `reunion_mm.dll` | [ReUnion](https://github.com/rehlds/reunion) `0.2.0.25` |
| `reunion.cfg` | ReUnion config, **non-Steam clients accepted** |
| `rotate-reunion-salt.sh` | forces a new `SteamIdHashSalt` (see below) |

> [!WARNING]
> **The metamod binary is at `addons/metamod/dlls/metamod.so`**, not `addons/metamod/metamod_i386.so`
> as it was under Metamod-R (changed 2026-08-23). This overlay does **not** ship `liblist.gam`,
> so whatever writes it on your side must point at the new path — the change does not propagate
> on its own, and getting it wrong yields `Failure to load game DLL` with no mention of the path.
>
> Two more differences worth knowing before you debug a healthy server:
> `meta version` reports `Metamod-P (mm-p)` / `1.21p38` (a `1.3.x` means an old overlay), and
> **this metamod prints nothing on a successful plugin load** — there is no
> `Read plugin config for: Reunion`, so a startup-log grep for `reunion` is empty even when it
> is running. `meta list` showing `[ 1] Reunion  RUN` is the only proof.

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
<li>sv_reconnect_timeout // Hard deadline in seconds for a client to re-initiate its connection after a level change, independent of netchan activity. Closes a phantom-slot exploit where a cheat blocks the "reconnect" command and keeps the netchan warm so sv_timeout never fires. 0 - disabled. Default: 30
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

metamod-fallguys and ReUnion are **pinned** in `.github/workflows/build.yml` (`MMFG_REF` at
the workflow level, `REUNION_VERSION` in the bundling step) rather than tracking "latest", so
a release is reproducible and only ships versions this stack has actually been run against.
Bump them deliberately.

`MMFG_REF` is one ref for **both** halves of metamod, and that is load-bearing. The Linux
`.so` is **built from source** in the `linux` job; the Windows `.dll` comes from that ref's
release asset. Pinning them separately would ship a `gamedir/` whose halves were built from
different source, and nothing downstream would notice.

⚠ The Linux binary cannot be downloaded, and upstream's README says otherwise. It claims the
makefile forces glibc 2.24 so the shipped `.so` is portable — true of the *make* path, false
of what ships, because metamod-fallguys' own CI ends on its **cmake** script and
`-DLINK_AGAINST_OLDER_GLIBC=TRUE` is a flag no `CMakeLists.txt` in that tree reads. Measured
on release `v20260730a`: **`GLIBC_2.38`**, against deployment targets that supply `GLIBC_2.31`.
So it is compiled in the `linux` job — the only bullseye environment here; `publish` is a bare
`ubuntu-24.04` runner where building would reproduce the bug exactly — and its glibc floor is
asserted before it is uploaded.

⚠ That assertion is **separate from `glibc_test.sh`**, on a deliberately different threshold.
The engine's script hardcodes `GLIBC 2.11`, which is upstream ReHLDS's portability target for
ancient distros; metamod-fallguys cannot meet it (its own release needs 2.38, and even the
glibc-forcing path its README describes targets 2.24). The number that matters for this fork
is the documented runtime, bullseye = **2.31**. Do not reconcile the two by lowering
`glibc_test.sh` — that weakens the *engine's* guarantee to accommodate a plugin.

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
