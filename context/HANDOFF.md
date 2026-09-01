# Handoff: a stock Half-Life client on a Sven Co-op server

**Goal.** A retail Half-Life client connects to this ReHLDS_Sven server, spawns, and plays,
alongside Sven Co-op clients on the same server.

**Status: reached.** On `abandoned`, a retail Sven Co-op client and a stock `hl.exe` were on
the server at the same time, both active:

```
#      name userid uniqueid frag time ping loss proto adr
# 1     ":P" 1 STEAM_0:1:36684470     0 00:56   17    0  sven 172.17.0.1:46674
# 2  "(1):P" 2 STEAM_6:1:1774456220   0 00:07   24    0    hl 172.17.0.1:40577
```

The Half-Life client rendered the world and drew the Sven player's model. With Sven's survival
mode off, a human then played on the Half-Life client and scored a frag:

```
# 1     ":P" 3 STEAM_6:1:1774456220   1 01:36   21    0    hl 172.17.0.1:46674
```

Remaining faults are in "Open faults"; none of them stop a client getting in and playing.

---

## Corrections to carry forward

Three things earlier sessions (including the one that wrote the previous version of this file)
got wrong. All three were "the control looked like it passed".

1. **The client every measurement before 2026-08-31 was taken against was not a retail client.**
   `valve_addon/cl_dlls/client.dll` was SevenKewp's and `valve_downloads/` held a complete Sven
   Co-op 5.26 mod install. GoldSrc's search order is `valve_addon` > `valve_downloads` > `valve`,
   so both shadowed the retail files. Those files are staged in
   `C:\Program Files (x86)\Steam\steamapps\common\Half-Life\_stock_test_disabled\`.
   Move them back to restore the SevenKewp setup; leave them aside to keep testing retail.

2. **"`-sp_campaign_portal` loads standalone" was false**, and so was the previous handoff's
   claim that this was still unproven. It has now been run properly, with a screenshot: it
   fails. But see (3) — it fails for a reason that does not transfer.

3. **A standalone `map <name>` is NOT a control for what a connected client does.** `map` starts
   a *listen server*, so the retail engine's SERVER code runs too, and it has its own ceilings.
   `-sp_campaign_portal` standalone dies in `SV_SpawnServer`, before the renderer is ever
   reached — which says nothing about the `AllocBlock: full` a connected client hits. Use
   `+connect` against the real server when the question is about the client.

4. **`health="-1"` and Half-Life's spectator HUD on the HL client was not a protocol bug.**
   It was Sven's own survival mode (`mp_survival_supported 1`, `mp_survival_starton 1`): the HL
   client joined 46 s after the Sven client had started the round, so Sven correctly held it dead
   as an observer. The Sven client beside it was at 100 HP throughout, which was misread as
   evidence that the HL client was broken. With survival off and a fresh join, the same client
   spawns, holds a weapon and scores frags. **Check the game rules before blaming the dialect.**

## Open faults

None of these prevent play. In rough order of how much they matter.

### `Failed to reconnect after level change`

A `changelevel` drops the Half-Life client. **The Sven client dropped in the same event**, so it
is not yet established that this is HL-specific — that needs a Sven-only control (one Sven client,
`changelevel`, see whether it comes back). Run that first; if the Sven client survives alone, the
fault is ours and the reconnect path is the place to look.

### `Error: could not load file models/player/eirin/eirin.mdl`

The HL client's userinfo carries `model eirin`, a Sven player model it has no way to obtain.
Non-fatal (it warns and carries on), but it means an HL player has no player model. Either force
a stock model into the userinfo of an HL-dialect client, or make the model resource obtainable.

### `Unknown command: closemenus`

The server stuffs a Sven-only console command at HL clients. Harmless noise; worth filtering with
the same "does this client understand it" rule the rest of `sv_proto` uses.

### Content ceilings (not fixable server-side)

`-sp_campaign_portal` cannot be rendered by any retail client. **Measured, not inferred:** a
retail client connected to the live server under cdb, with breakpoints on all six known error
sites, fires exactly one — `AllocBlock: full` at RVA `0x24715D`. The map needs 124 lightmap
blocks; the client's atlas is 64. Nothing the server sends changes this, because the client
renders its own copy of the BSP.

The open design question is whether the server should refuse to put an HL-dialect client on a map
`tools/hlmapcheck.py` rejects, and what to tell them. Nothing is implemented for this yet.

## The renderer's actual ceilings, read out of hw.dll

All of these are from the 25th-anniversary `hw.dll` (`md5 ba0ae7ebdef6891b1e822830145f3415`,
image base `0x10000000`). **Do not take them from a header** — two of the three differ from the
original GoldSrc values.

| ceiling | value | where |
|---|---|---|
| lightmap atlas | 64 blocks of 128x128 | `GL_BuildLightmaps` `0x246F80`; `memset(allocated, 0, 0x8000)` at `0x246FC4`, error at `0x24715D` |
| surface extents | **512**, not 256 | `cmp ecx,0x200` at `0x23E4AB`, error at `0x23E4D3`; TEX_SPECIAL faces are exempt |
| inline submodels | **511** (`*1`..`*511`) | table of 512 x 5 bytes at `0x1198480`..`0x1198E80`, built at `0x20B803`, indexed unbounded at `0x20EE70` |

The submodel ceiling is a **listen-server** ceiling: it is `SV_SpawnServer` that walks off the end
of that table, and a client joining a dedicated server never runs that loop
(`Mod_LoadBrushModel` names its submodels through a local buffer). It is why
`-sp_campaign_portal` (712 submodels) and `hl_t00` (541) die with `Mod_FindName: NULL name` under
`map`, and why that error has never been seen on a connected client.

Proof it is the table and not something else: the failing name pointer was `hw+0x1198E80`, which
is exactly `table_base + 5 * 511` — one past the last entry, in zeroed memory. `Mod_FindName`
errors on an *empty string*, not a null pointer (`cmp BYTE PTR [eax],bl` at `0x23E61A` would
fault on NULL), so "NULL name" is misleading and always has been.

## `tools/hlmapcheck.py` is exact

It reimplements `GL_BuildLightmaps` against a BSP and reports whether a retail client can render
the map. It used to be an estimate with a +/-3 margin; it is now exact, and that was measured:

- Breaking at `0x2470DB` and logging `(w, h)` for every surface of `abandoned.bsp` gives 20453
  surfaces. This code produces the same 20453, in the same order, with **zero differing sizes**.
- Breaking at the function epilogue `0x247334` (conditional on `allocated[0] != 0`, or you catch
  an empty call at startup) and reading `allocated[]` back gives the block count the client
  actually reached: `abandoned` **64** (atlas completely full), `toadsnatch` **61**. The model
  says 64 and 61. `turretfortress` (model 65) really does die with `AllocBlock: full`.

Two things had to be fixed to get there, and both are the kind of error that hides:

1. `MAX_SURFACE_EXTENTS` was 256. This client's is **512**.
2. `CalcSurfaceExtents` accumulates the dot product in double but **rounds it to float** before
   the min/max (`cvtpd2ps` at `0x23E393`). Keeping full double made 1207 of `abandoned`'s 20453
   surfaces one luxel wider — because a grid-built map puts most faces exactly on 16-unit
   boundaries, which is precisely where `floor`/`ceil` flip.

`MARGIN` is now 0. Over 64 blocks fails, full stop.

Scan of the 108-map pack (`--dir`): **43 cannot be rendered by a retail client**, and 8 more load
on a dedicated server but not a retail listen server. `abandoned` sits at exactly 64/64 — it works,
but there is no headroom, so prefer something like `toadsnatch` (61), `quarter` (51) or
`svencoop1` (9) when the map itself is not what is under test.

## Fixed in earlier sessions

| commit | bug |
|---|---|
| `32860ad` | `events/*.sc` a Half-Life client can never obtain were named in its resource list; a missing one is fatal. Sven's dedicated-server package ships no `.sc` files at all, so they were unobtainable by construction. Withheld; `SV_EmitEvents` got the matching index guard. |
| `f3eda01` | `SV_CreateBaseline` runs **before** `SV_CreateResourceList`, so `fb16961`'s model filter consulted the previous map's resources and sent an **empty** `svc_spawnbaseline`. A stock client then memcpys 340 bytes out of its own delta registration, twice, over its delta registry. |
| `879fd43` | entity numbers past the client's `cl_entities[]` (`-num_edicts` + `15 * (maxclients - 1)` = 1305 here). `CL_EntityNum` calls **Host_Error**, not a warning. |
| `bf3a3f8` | (diagnostics) `sv_proto_log 2` prints the exact baseline sequence a client should read. |
| `b718cad` | **the desync.** The HL delta trim unmarks fields past 56 so the mask fits 3 bits, and got the *length* wrong: `DELTAJit_SetSendFlagBits` returns `markedFieldsMaskSize`, cached when the fields were MARKED. On any entity marking `entity_state_t::gaitsequence` (field 57) the count still said 8, and **8 written in three bits is 0**. |

The last one is the one to remember: **silent on both sides**. The client parsed 178 of 503
baselines in perfect ascending order, then read `769, 101, 4, 1164, 1968` and died on
`CL_EntityNum: 1968 is an invalid number` — a real but unrelated bug. **Never diagnose from the
number in the error message once a bit stream can desync.**

Earlier commits (`fcbeeb4` split framing, `2961fea` resource indices, `110adf7` lightstyles,
`f1bf95e` `cl.resourcelist[1280]`, `7abf0bb` unmunged pre-probe packet, `bc2c410` shed by use not
position, `fb16961`/`f747647` unusable model indices) are in the git log.

## Tools

- **cdb** — `C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe`, driven through
  `~/claude-host-bridge/hostexec`. Use it before theorising. Drivers written this session live in
  `~/hlctl/` (not in the repo): `cdbmap.ps1` (load a map standalone under cdb), `cdbconnect.ps1`
  (connect to the server under cdb), `lmcal.ps1` (batch: load N maps, dump `allocated[]`),
  `lmtrace.ps1` (log every surface's `(w,h)`), `raise.ps1` (raise a window and screenshot it
  without stealing focus), `joint.ps1` (launch both clients).
- **`tools/hlmapcheck.py`** — which maps a retail client can render. `--dir <maps>` scans a
  directory; ~30-60 s per large map, so run a full pack in the background.
- **`tools/hlprobe.py`** — a Half-Life-dialect client that drives the signon and dumps the svc
  stream. It does not render, so it cannot see renderer faults. Largely superseded by cdb.
- **`tools/hlshrink.py`** — resamples oversized model/sprite textures down to the 256 a stock
  client can upload.
- `~/hw.dll` — the client engine, for disassembly:
  `objdump -d --target=pei-i386 -M intel ~/hw.dll` (addresses there are `0x10000000 + RVA`).
- rcon: `python3 <scratchpad>/rcon.py <cmd>`, password `cs16test`, `172.17.0.2:27015`.

### cdb recipes that actually worked

```
sxd av            # second-chance only; first-chance AVs in VGUI are normal noise
sxd eh
sxe ld:hw         # break on module load, then set RVA breakpoints, then sxd ld:hw
bp hw+23E61E                                   # stop at an error site
bp hw+247334 ".if (poi(hw+0xdbb640)==0) {gc}"  # conditional: skip the empty startup call
bp hw+2470DB "r ebx,eax;gc"                    # log a value on every hit without stopping
```

- `-cf <file>` runs a command script — use it as soon as you need nested quotes, and write it
  `-Encoding ascii`.
- `kb` is usually useless without unwind info. `dds @esp` plus mapping return addresses back to
  RVAs in the local disassembly is what worked.
- **Page heap is NOT available**: the bridge is not elevated. Use `ba w4 <addr>` watchpoints.
- `gflags.exe /p` **hangs the bridge** (it opens a GUI). Do not run it.

Error sites located so far:

| RVA | error |
|---|---|
| `0x23E61A` | `Mod_FindName: NULL name` (guard); `0x23E61E` is the Host_Error push |
| `0x23E4D3` | `Bad surface extents %d/%d at position (%d,%d,%d)` |
| `0x2405B4` | `Mod_NumForName: %s not found` |
| `0x24715D` | `AllocBlock: full` |
| `0x1A6B9C` | `CL_EntityNum: ... invalid ...` (packet-entity path) |
| `0x1A6D72` | `CL_EntityNum: ... invalid ...` (spawn-baseline path) |
| `0x1A6D4F` | baseline loop, `esi` = the entity number just read |
| `0x1BFDD0` | `DELTA_LookupRegistration(name)`; registry head at `0x4B77EC` |
| `0x1A3681` | end of delta registration (7 entries) — safe place to arm a watchpoint |

## Current state

- Server: docker `svencoop-server`, engine from CI of `b718cad` at `/server/engine_i486.so`
  (in the volume, so it survives a restart). Map **`abandoned`**.
  `172.17.0.2:27015` in the container, `127.0.0.1:27016` from Windows.
- `server.cfg` in the volume persists `sv_proto_hl_gamedir "valve"` and `sv_proto_log "2"`.
  Runtime-only cvars are lost on restart; that has cost a round more than once.
- **`mp_survival_supported` and `mp_survival_starton` were set to 0 at runtime** so a late joiner
  spawns. They are NOT in `server.cfg`, so a restart brings survival back and the next HL client
  to join second will look broken again. Put them in `server.cfg` if that matters.
- Sven Co-op is installed on the Windows host, so both clients can be driven from this machine.

## Hazards learned the hard way

1. **Never deploy a locally built engine.** It needs a newer `libstdc++` than the runtime image
   (`GLIBCXX_3.4.29` -> `Unable to load engine, image is corrupt`) and the server enters a restart
   loop. Take the binary from CI:
   `gh run download <id> -R coffeegrind123/ReHLDS_Sven -n linux32`. Verify before deploying —
   `grep -ac sv_proto_hl_max_resources` must be non-zero. (`strings` is not in the container;
   `grep -a` works. `gh` defaults to the *upstream* repo — always pass `-R`.)
2. **A control is not a control until you have seen it.** Process alive + a plausible log line is
   not proof a map rendered. Screenshot it.
3. **A standalone `map` is a listen server.** See correction (3) above.
4. **`git add -A` after a killed gate run once committed the removal of `REHLDS_SVEN`** and CI
   silently built stock ReHLDS. Add named paths.
5. **A restart reverts the map to `bm_sts`** (container CMD), which no retail client can load.
   `changelevel` after a restart and give it ~30 s — rcon does not answer while a map loads.
6. **The engine source files are CRLF.** Editing with a plain Python `read()/write()` rewrites
   every line ending. Use `newline=''` and put `\r\n` in the patterns. (`sv_proto.h` lines 125-132
   are LF-only; leave them.)
7. **Don't blind-kill processes on the user's desktop.** Kill by the PID you launched, or by
   `Win32_Process` parent id.
8. **Map names beginning with `-`** cannot be passed via `+map` — the engine's argv parser takes
   them as a switch. Write a cfg and `+exec` it.
9. **`SetForegroundWindow` from the bridge is blocked by Windows**, so keystrokes sent after it go
   to whatever the user actually had focused. Use `PostMessage` to the game's HWND, and
   `SetWindowPos(..., HWND_TOPMOST, SWP_NOACTIVATE)` if you need to see it.
10. **The virtual screen origin is not (0,0)** on this machine — it is `0,-615`. Subtract
    `VirtualScreen.Left/Top` from a window rect before cropping a full-screen capture.
11. **`'\xff\xff\xff\xff'.encode()` in Python 3 is UTF-8** and produces `c3 bf` four times, not
    four `ff` bytes. A GoldSrc server drops the result silently, with nothing logged, and it looks
    exactly like an rcon ban. Build connectionless packets as `bytes`.

## What is genuinely unresolved

- **`changelevel` drops clients.** Run the Sven-only control first; see "Open faults".
- **The HL player has no player model** (`models/player/eirin/eirin.mdl`).
- **Whether the server should gate HL clients off maps `hlmapcheck.py` rejects**, and what to tell
  them. Nothing implemented.
- **Temp entities.** Composed by the game DLL through `MESSAGE_BEGIN`, where the engine cannot
  tell which shorts are model indices or entity numbers, so none of the filtering covers them.
- **`-num_edicts` is assumed, not negotiated.** `879fd43` assumes the documented default of 1200;
  a player launching with a lower value will still overflow. `sv_proto_hl_max_edicts` sets it by
  hand.
- **`args.entindex` on events** is not filtered against the edict ceiling. Not observed to fail,
  not proven safe.
- **`mp_consistency` is 0**, so the consistency path from `2961fea` / `bc2c410` has still never
  been exercised.
- **Sustained gameplay is barely tested.** One frag on one map. No objective completed, no Sven
  entity exercised beyond the first room.
