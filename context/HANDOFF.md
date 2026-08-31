# Handoff: a stock Half-Life client on a Sven Co-op server

**Goal.** A retail Half-Life client connects to this ReHLDS_Sven server, spawns, and plays,
alongside Sven Co-op clients on the same server.

**Status: reached.** On 2026-09-01 a stock `hl.exe` (Valve's `valve/cl_dlls/client.dll`,
Half-Life's own `liblist.gam` and `delta.lst`, no Sven Co-op mod files in the game path)
connected to this server, precached, loaded `abandoned`, spawned and stayed in:

```
#      name userid uniqueid frag time ping loss proto adr
# 1     ":P" 1 STEAM_6:1:1774456220   0 01:53   15    0    hl 172.17.0.1:38890
1 users
```

`proto hl`, active, 0% loss, world rendered, HUD showing 100 health. Screenshots at
`~/hlshot.png` and `~/hlshot2.png`.

---

## Read this before trusting the previous handoff

The client used for every measurement before this session **was not a stock Half-Life
client**, and nobody had checked. Two separate contaminations were live in the game path:

| path | what was there |
|---|---|
| `valve_addon/cl_dlls/client.dll` | SevenKewp's client (1556480 bytes, 2026-04-18) |
| `valve_downloads/` | a complete **Sven Co-op 5.26 mod install** — `liblist.gam`, `dlls/server.dll`, `cl_dlls/client.dll`, `delta.lst`, `valve.rc`, `config.cfg`, `resource/`, `scripts/`, every WAD |

GoldSrc's search order is `valve_addon` > `valve_downloads` > `valve`, so **both shadowed
the retail files**. Proven, not assumed: the client printed `This is not a SevenKewp
server` (that string exists only in `valve_addon`'s client.dll) *and* `Sven Co-op 5.26`
(the game name and version out of `valve_downloads/liblist.gam`).

Consequences for the old handoff:

- The "single most important fact" — `hl.exe +map abandoned` loads with no server and does
  not crash — was measuring **a Sven Co-op listen server**, because `valve_downloads`
  supplied `liblist.gam` (`gamedll "dlls/server.dll"`) and the Sven client DLL. It said
  nothing about a Half-Life client. (The control has since been re-run properly on a clean
  client: it does load and render `abandoned` standalone. The conclusion held; the evidence
  behind it did not.)
- The crash at `hw.dll+0x247d03` (`r_worldmodel` NULL during lightmap building) was
  produced by a third-party client.dll and **has not reappeared** on a stock client. Do not
  start from it.

The offending files were moved to
`C:\Program Files (x86)\Steam\steamapps\common\Half-Life\_stock_test_disabled\`, one
directory, nothing deleted. **Move them back to restore the SevenKewp/Sven Co-op client**;
leave them aside to keep testing a retail client.

## The tool that changed everything: cdb is installed

`C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe` (and x64). Reach it through
`~/claude-host-bridge/hostexec`. This replaced days of static RE with direct answers.

```powershell
# second-chance only -- first-chance AVs in VGUI are normal noise and will stop you
# at the wrong place if you leave them enabled
$c = "sxd av;sxd eh;g;.echo ===XCRASH===;.lastevent;r;u @eip-18 @eip+8;kb 30;q"
Start-Process $cdb -WorkingDirectory $hl -ArgumentList `
  "-o -logo C:\temp\cdb.log -c `"$c`" `"$hl\hl.exe`" -console -condebug -window -game valve -novid +connect 127.0.0.1:27016"
```

- `-cf <file>` runs a command script — use it instead of `-c` as soon as you need nested
  quotes. Write it `-Encoding ascii`.
- **Hardware watchpoints work and are the sharpest instrument here.** `ba w4 <addr>` caught
  the heap corruption at the exact `rep movsd` that did it. To arm one after engine init,
  `bp` a known init-time address first, then set the `ba` at that break.
- `bp hw+<RVA> "r esi;gc"` logs a value on every hit without stopping — that is how the
  client's baseline entity-number sequence was recovered.
- **Page heap is NOT available**: the bridge is not elevated, and
  `HKLM\...\Image File Execution Options\hl.exe` is denied. Watchpoints instead.
- `gflags.exe /p` **hangs** the bridge (it opens a GUI). Do not run it. Kill it with
  `Get-Process gflags | Stop-Process -Force` if you already did.
- hw.dll has no symbols; work in RVAs. Image base 0x10000000, so
  `RVA = runtime_addr - <hw base from ModLoad>`, and the local dump is at
  `objdump -d --target=pei-i386 -M intel ~/hw.dll` (`0x10000000 + RVA`).

## Fixed this session

| commit | bug |
|---|---|
| `32860ad` | event scripts a Half-Life client can never obtain (`events/*.sc`) were named in its resource list; the engine treats a missing one as fatal — `Cannot continue without script events/clcheck.sc, disconnecting.` Sven's **dedicated-server** package ships no `.sc` files at all, so they were unobtainable by construction. Withheld; `SV_EmitEvents` got the matching index guard. |
| `f3eda01` | `SV_CreateBaseline` runs **before** `SV_CreateResourceList`, so `fb16961`'s model-usability filter consulted the previous map's resources, skipped every entity, and sent an **empty** `svc_spawnbaseline`. A stock client that reads 0xFFFF as the first index leaves its "last baseline" pointer holding the address of the `entity_state_t` delta registration and then memcpys 340 bytes out of it **twice** — straight over its own delta registry. Emit the HL twin after the list exists; never skip entity 0. |
| `879fd43` | entity numbers past the client's `cl_entities[]`. It is `-num_edicts` (default 1200) `+ 15 * (maxclients - 1)`; 1305 here. `CL_EntityNum` calls **Host_Error**, not a warning. Baselines and packet entities filter on it; sounds fall back to the world. |
| `bf3a3f8` | (diagnostics) `sv_proto_log 2` now prints the exact baseline sequence a client should read. |
| `b718cad` | **the desync.** The HL delta trim unmarks fields past 56 so the mask fits 3 bits — and got the *length* wrong. `DELTAJit_SetSendFlagBits` returns `markedFieldsMaskSize`, cached when the fields were MARKED; `DELTAJit_UnsetFieldByIndex` only clears the bit. So on any entity marking `entity_state_t::gaitsequence` (field 57 — the one field the trim costs) the count still said 8, and **8 written in three bits is 0**. The client read no mask, then 8 bytes of mask and every field after it at the wrong bit offset. |

The last one is the one to remember: it was **silent on both sides**. The client parsed 178
of 503 baselines in perfect ascending order, then read `769, 101, 4, 1164, 1968` and died
on `CL_EntityNum: 1968 is an invalid number, cl.max_edicts is 1305`. Every number after the
break was garbage, and the fatal one pointed at a completely different (real, but not
active) bug. Do not diagnose from the number in the error message.

Earlier commits (`fcbeeb4` split framing, `2961fea` resource indices, `110adf7` lightstyles,
`f1bf95e` `cl.resourcelist[1280]`, `7abf0bb` unmunged pre-probe packet, `bc2c410` shed by
use not position, `fb16961`/`f747647` unusable model indices) are described in the git log.

## Current state

- Server: docker `svencoop-server`, engine from CI of `b718cad`, map `abandoned`,
  `172.17.0.2:27015` from the container, `127.0.0.1:27016` from Windows.
- `server.cfg` in the volume persists `sv_proto_hl_gamedir "valve"` and `sv_proto_log "2"`.
  Runtime-only cvars are lost on restart — that has cost a round more than once.
- Working tree clean at `b718cad`. Local build passes.
- The Half-Life client is (or was) left running and connected.

## Tools

- **cdb** — see above. Use it first.
- **`~/claude-host-bridge/hostexec`** — PowerShell on the Windows host. `-t <ms>` for a
  longer timeout, `-j` to detach. Anything over ~60s must be detached or it kills the
  listener's single thread.
- **`tools/hlprobe.py`** (gitignored) — a Half-Life-dialect client that drives the full
  signon and dumps the svc stream. It does not render, so it cannot see rendering faults,
  but it proves the wire. Not needed once cdb is available.
- `~/hw.dll` — the client engine, for disassembly.
- rcon: `python3 <scratchpad>/rcon.py <cmd>`, password `cs16test`, host `172.17.0.2:27015`.
  Trivially rewritten (challenge, then `rcon <challenge> "<pw>" <cmd>`).
- `~/hl-safe-content/` — 1019 files, textures resampled within 256x256. Not needed; the
  stock client renders `abandoned` as shipped.

## Hazards learned the hard way

1. **Never deploy a locally built engine.** It needs a newer `libstdc++` than the runtime
   image (`GLIBCXX_3.4.29` → `Unable to load engine, image is corrupt`) and the server
   enters a restart loop. Take the binary from CI:
   `gh run download <id> -R coffeegrind123/ReHLDS_Sven -n linux32`, then
   `bin/linux32/engine_i486.so`. Verify before deploying — `grep -ac sv_proto_hl_max_resources`
   must be non-zero. (`strings` is not installed in the container; `grep -a` works.)
2. **`git add -A` after a killed gate run once committed the removal of `REHLDS_SVEN`**, and
   CI silently built stock ReHLDS. Add named paths.
3. **A restart reverts the map to `bm_sts`** (container CMD), which this client cannot load
   (surface extents 544 > 256). Always `changelevel abandoned` after a restart, and give it
   ~30s — rcon does not answer while the map loads.
4. **The source files are CRLF.** Editing them with a Python `open(...).read()/write()`
   rewrites every line ending and produces a whole-file diff. Use `newline=''` and put
   `\r\n` in the patterns. (`sv_proto.h` lines 125-132 are LF-only; leave them alone.)
5. **Don't put a blind auto-kill on a process on the user's desktop.** A 75-second
   `Stop-Process -Name hl -Force` killed their client mid-session. Kill by the PID you
   launched.
6. Error strings from the client have been worth more than every inference made from the
   server side — but **only the first one**. Once a stream desyncs, everything the client
   reports is downstream garbage.

## What is genuinely unresolved

- **`-num_edicts` is assumed, not negotiated.** `879fd43` assumes the documented default of
  1200. A player who launches with a *lower* `-num_edicts` will still overflow; there is no
  way to ask. `sv_proto_hl_max_edicts` exists to set it by hand.
- **Temp entities.** They are composed by the game DLL through `MESSAGE_BEGIN`, where the
  engine cannot tell which shorts are model indices or entity numbers, so none of the
  filtering covers them. This is the most likely source of the next fault.
- **`args.entindex` on events** is not filtered against the client's edict ceiling. Not
  observed to fail; not proven safe either.
- **`mp_consistency` is 0** on this server, so the consistency path added in `2961fea` /
  `bc2c410` has still never been exercised.
- **Gameplay beyond spawning is untested.** The client is standing in the map with the MOTD
  up. Nobody has fired a weapon, triggered a Sven entity, or changed level with it
  connected.
- **A Sven client and a Half-Life client have not been on the server at the same time**,
  which is the actual goal.
