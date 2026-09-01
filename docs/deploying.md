# Deploying ReHLDS_Sven

The detail behind [How do I use it?](../README.md#how-do-i-use-it) in the README.

## Installing
ReHLDS_Sven is a drop-in replacement for the engine in an official SvenDS install. Install
SvenDS first, then overwrite the files below.

> [!CAUTION]  
> This project works only with Sven Co-op 5.26.

### Downloading SvenDS via steamcmd

```
app_update 276060 validate
```

## Installing over a SvenDS install

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

## Upgrading an existing install

Overwrite the same three files and restart. Two things not to do:

- **Do not delete `<gamedir>/.reunion_salt`.** A new salt changes every generated
  `STEAM_x:y:z`, which invalidates every ban and stored per-player record. See
  [the salt section](#the-reunion-salt-is-handled-for-you).
- **Do not re-copy `gamedir/` unless the pinned plugin versions changed.** It would overwrite
  a `reunion.cfg` you have edited. The pinned versions are in the release notes and in
  [Bundled plugin versions](maintaining.md#bundled-plugin-versions).

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

## What is in a release

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

## The bundled `gamedir/` overlay

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

### The ReUnion salt is handled for you

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
