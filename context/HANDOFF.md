# Handoff: a stock Half-Life client on a Sven Co-op server

**Goal.** A retail Half-Life client connects to this ReHLDS_Sven server, spawns, and plays,
alongside Sven Co-op clients on the same server.

**Status.** Reached on one map. On `abandoned`, a stock `hl.exe` — Valve's
`valve/cl_dlls/client.dll`, Half-Life's own `liblist.gam` and `delta.lst`, no Sven mod
files in the game path — connected, precached, loaded, spawned and stayed in:

```
#      name userid uniqueid frag time ping loss proto adr
# 1     ":P" 1 STEAM_6:1:1774456220   0 05:24   16    0    hl 172.17.0.1:38890
```

`proto hl`, active, 0% loss, world rendered, HUD alive. Screenshots `~/hlshot.png`,
`~/hlshot2.png`.

**On `-sp_campaign_portal` it does not.** Two client-side errors, in this order:

1. `AllocBlock: full`
2. `Mod_FindName: NULL name`  ← where it stands now

Neither is diagnosed. Section "Open faults" has what is known and what is only guessed.

---

## Corrections to carry forward

Two things this session got wrong and had to walk back. Both were "the control looked like
it passed"; neither control was actually valid.

1. **The client every earlier measurement was taken against was not a retail client.**
   `valve_addon/cl_dlls/client.dll` was SevenKewp's, and `valve_downloads/` held a complete
   Sven Co-op 5.26 mod install — `liblist.gam`, `dlls/server.dll`, `cl_dlls/client.dll`,
   `delta.lst`, `valve.rc`, `config.cfg`, `resource/`, `scripts/`. GoldSrc's search order is
   `valve_addon` > `valve_downloads` > `valve`, so both shadowed the retail files. Proof:
   the client printed `This is not a SevenKewp server` (that string exists only in
   `valve_addon`'s client.dll) *and* `Sven Co-op 5.26` (out of `valve_downloads/liblist.gam`).
   The old handoff's "loads standalone with no server" was therefore a **Sven Co-op listen
   server**, and the `hw.dll+0x247d03` crash it was built around came from a third-party
   client.dll. That crash has not reappeared on a stock client — do not start from it.
   Those files are staged in
   `C:\Program Files (x86)\Steam\steamapps\common\Half-Life\_stock_test_disabled\`.
   Move them back to restore the SevenKewp setup; leave them aside to keep testing retail.

2. **"`-sp_campaign_portal` loads standalone" was reported here and is false.** It was based
   on `hl.exe` still being alive plus a `No detail texture mapping file:` line in
   `qconsole.log`. The user watched the screen: it never rendered the map. So that control
   does **not** exonerate the map, and the conclusion drawn from it ("something the server
   adds pushes it over") is unsupported.
   **Rule: a map-load control is only valid with a screenshot.** The process surviving and a
   load-time log line both happen when the load fails to a message box.

## Open faults

### `AllocBlock: full`

The lightmap atlas, and it is a hard client-side array. From hw.dll:

- allocator at RVA **0x247030**, `allocated[]` at `0x10dbb640`
- `BLOCK_WIDTH` = `BLOCK_HEIGHT` = **128** (`cmp edx,0x80` at 0x24713D)
- base index advances 128 per block and errors at `0x2000` → **64 blocks** (RVA 0x24715D)

That is 64 × 128 × 128 = 1,048,576 luxels for the whole map, and Sven Co-op raised it.

`tools/hlmapcheck.py` (new; `/tools/` is gitignored, so it lives on disk only — `git add -f`
it if you want it in the repo) reimplements that allocator instruction-for-instruction
against a BSP and reports how many blocks a map needs:

```
map                                        faces  blocks   maxext  verdict
abandoned.bsp                              21081      65      256  borderline: ~65 blocks vs 64
-sp_campaign_portal.bsp                    45790     127      256  needs ~127 blocks, client has 64
```

Calibration, stated honestly: `abandoned` **does** render, so the true figure for it is ≤64
and the model is about one block pessimistic — hence the ±3 "borderline" band. At 127 vs 64,
`-sp_campaign_portal` is not near that band, so **the most likely reading is that this map is
simply beyond a retail client and no server change fixes it.** That has not been proven,
because the one control that would prove it (standalone load, screenshot) has not been run.

**Next step:** run that control properly. The map name starts with `-`, so `+map` will not
work — write a cfg and `+exec` it:

```powershell
$q=[char]34
[System.IO.File]::WriteAllText("$hl\valve\hlmapctl.cfg", "map " + $q + "-sp_campaign_portal" + $q + "`n")
# then: hl.exe -console -condebug -window -game valve -novid +exec hlmapctl.cfg
```

...and **screenshot it**. If it fails standalone, this is a content ceiling: the server's job
is then to keep HL-dialect clients off maps `hlmapcheck.py` rejects, not to fix the render.
If it succeeds standalone, something in the precache adds surfaces — put a breakpoint at
`hw+247055` with `"da @eax;gc"` to list every model whose surfaces get lightmapped, and
compare connected vs standalone.

### `Mod_FindName: NULL name`  ← current

Reported by the user; **not yet reproduced under a debugger**, so everything below is the
binary, not a measurement of this fault.

The guard is at RVA **0x23E61A**, inside `Mod_FindName(arg0, const char *name)`:

```
1023e613: mov  eax,[ebp+0xc]     ; name -- the SECOND argument
1023e61a: cmp  BYTE PTR [eax],bl ; bl = 0
1023e61c: jne  ok
1023e61e: push "Mod_FindName: NULL name" ; Host_Error
```

It dereferences `name` without a null check, so a genuinely NULL pointer would fault instead.
**The name is a valid pointer to an empty string.** That is a much narrower fact than the
message suggests, and it is the thing to reason from: something handed the client `""` as a
model name.

Leading candidates, in the order worth checking — all are the same shape as the bugs already
fixed, "the client was told about something it does not have":

- a `t_model` resource with an empty `szFileName` surviving into the list sent to HL clients
- a precache slot the client was told about but never filled, then resolved by name
- `svc_temp_entity` / a game-DLL `MESSAGE_BEGIN` naming a model index, which nothing filters
  (see "unresolved") — the client would resolve an index whose name slot is empty

**Next step, and it is cheap:** breakpoint the guard and get the caller.

```
sxd av
sxd eh
bp hw+23E61A
g
.echo ===MODFINDNAME===
r
kb 30
dds @esp L60
q
```

`kb` is usually useless without unwind info; `dds @esp` and mapping return addresses back to
RVAs in the local disassembly is what actually worked all session.

## The tool that changed everything: cdb is installed

`C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe` (and x64), driven through
`~/claude-host-bridge/hostexec`. This replaced days of static RE with direct answers. Use it
first, before theorising.

```powershell
# second-chance only -- first-chance AVs in VGUI are normal noise and WILL stop you in the
# wrong place if you leave them enabled
$c = "sxd av;sxd eh;g;.echo ===XCRASH===;.lastevent;r;u @eip-18 @eip+8;kb 30;q"
Start-Process $cdb -WorkingDirectory $hl -ArgumentList `
  "-o -logo C:\temp\cdb.log -c `"$c`" `"$hl\hl.exe`" -console -condebug -window -game valve -novid +connect 127.0.0.1:27016"
```

- `-cf <file>` runs a command script — use it as soon as you need nested quotes. Write it
  `-Encoding ascii`.
- **Hardware watchpoints are the sharpest instrument here.** `ba w4 <addr>` caught a heap
  corruption at the exact `rep movsd` that did it. To arm one after engine init, `bp` an
  init-time address first, then set the `ba` at that break.
- `bp hw+<RVA> "r esi;gc"` logs a value on every hit without stopping — that is how the
  client's baseline entity-number sequence was recovered.
- **Page heap is NOT available**: the bridge is not elevated and
  `HKLM\...\Image File Execution Options\hl.exe` is denied. Watchpoints instead.
- `gflags.exe /p` **hangs the bridge** (it opens a GUI). Do not run it.
- hw.dll has no symbols; work in RVAs. Image base 0x10000000, so
  `RVA = runtime_addr - <hw base from the ModLoad line>`, and the local dump is
  `objdump -d --target=pei-i386 -M intel ~/hw.dll` (addresses there are `0x10000000 + RVA`).

Useful error sites already located:

| RVA | error |
|---|---|
| `0x23E61A` | `Mod_FindName: NULL name` |
| `0x23E4D3` | `Bad surface extents %d/%d at position (%d,%d,%d)` |
| `0x2405B4` | `Mod_NumForName: %s not found` |
| `0x24715D` | `AllocBlock: full` |
| `0x1A6B9C` | `CL_EntityNum: ... invalid ...` (packet-entity path) |
| `0x1A6D72` | `CL_EntityNum: ... invalid ...` (spawn-baseline path) |
| `0x1A6D4F` | baseline loop, `esi` = the entity number just read |
| `0x1BFDD0` | `DELTA_LookupRegistration(name)`; registry head at `0x4B77EC` |
| `0x1A3681` | end of delta registration (7 entries) — safe place to arm a watchpoint |

## Fixed this session

| commit | bug |
|---|---|
| `32860ad` | `events/*.sc` a Half-Life client can never obtain were named in its resource list; a missing one is fatal — `Cannot continue without script events/clcheck.sc, disconnecting.` Sven's **dedicated-server** package ships no `.sc` files at all, so they were unobtainable by construction. Withheld; `SV_EmitEvents` got the matching index guard. |
| `f3eda01` | `SV_CreateBaseline` runs **before** `SV_CreateResourceList`, so `fb16961`'s model filter consulted the previous map's resources, skipped every entity, and sent an **empty** `svc_spawnbaseline`. A stock client that reads 0xFFFF as the first index leaves its "last baseline" pointer holding the address of the `entity_state_t` delta registration and memcpys 340 bytes out of it **twice**, over its own delta registry. Emit the HL twin after the list exists; never skip entity 0. |
| `879fd43` | entity numbers past the client's `cl_entities[]`, which is `-num_edicts` (default 1200) `+ 15 * (maxclients - 1)` = 1305 here. `CL_EntityNum` calls **Host_Error**, not a warning. |
| `bf3a3f8` | (diagnostics) `sv_proto_log 2` prints the exact baseline sequence a client should read. |
| `b718cad` | **the desync.** The HL delta trim unmarks fields past 56 so the mask fits 3 bits, and got the *length* wrong: `DELTAJit_SetSendFlagBits` returns `markedFieldsMaskSize`, cached when the fields were MARKED, while `DELTAJit_UnsetFieldByIndex` only clears the bit. On any entity marking `entity_state_t::gaitsequence` (field 57 — the one field the trim costs) the count still said 8, and **8 written in three bits is 0**. The client read no mask, then 8 bytes of mask and every field after it at the wrong bit offset. |

The last one is the one to remember: **silent on both sides**. The client parsed 178 of 503
baselines in perfect ascending order, then read `769, 101, 4, 1164, 1968` and died on
`CL_EntityNum: 1968 is an invalid number`. Every number after the break was garbage, and the
fatal one pointed at a real but unrelated bug. **Never diagnose from the number in the error
message once a bit stream can desync.**

Earlier commits (`fcbeeb4` split framing, `2961fea` resource indices, `110adf7` lightstyles,
`f1bf95e` `cl.resourcelist[1280]`, `7abf0bb` unmunged pre-probe packet, `bc2c410` shed by use
not position, `fb16961`/`f747647` unusable model indices) are in the git log.

## Current state

- Server: docker `svencoop-server`, engine from CI of `b718cad`, **map
  `-sp_campaign_portal`** — `changelevel abandoned` to get back to the working case.
  `172.17.0.2:27015` in the container, `127.0.0.1:27016` from Windows.
- `server.cfg` in the volume persists `sv_proto_hl_gamedir "valve"` and `sv_proto_log "2"`.
  Runtime-only cvars are lost on restart; that has cost a round more than once.
- Working tree clean. Local build passes.
- The temp `valve/hlmapctl.cfg` written for the standalone test has been removed.

## Tools

- **cdb** — see above. Use it first.
- **`tools/hlmapcheck.py`** (gitignored, local only) — which maps a retail client can render. `--dir <maps>` scans a
  whole directory. Takes ~30-60s per large map (pure Python packing loop); run it in the
  background over the map pack to get the playable list.
- **`~/claude-host-bridge/hostexec`** — PowerShell on the Windows host. `-t <ms>` for a
  longer timeout, `-j` to detach. Anything over ~60s must be detached or it kills the
  listener's single thread.
- **`tools/hlprobe.py`** (gitignored) — a Half-Life-dialect client that drives the signon and
  dumps the svc stream. It does not render, so it cannot see renderer faults. Largely
  superseded by cdb.
- `~/hw.dll` — the client engine, for disassembly.
- rcon: `python3 <scratchpad>/rcon.py <cmd>`, password `cs16test`, `172.17.0.2:27015`.
  Trivially rewritten (challenge, then `rcon <challenge> "<pw>" <cmd>`).

## Hazards learned the hard way

1. **Never deploy a locally built engine.** It needs a newer `libstdc++` than the runtime
   image (`GLIBCXX_3.4.29` → `Unable to load engine, image is corrupt`) and the server enters
   a restart loop. Take the binary from CI:
   `gh run download <id> -R coffeegrind123/ReHLDS_Sven -n linux32` → `bin/linux32/engine_i486.so`.
   Verify before deploying — `grep -ac sv_proto_hl_max_resources` must be non-zero.
   (`strings` is not installed in the container; `grep -a` works. `gh` defaults to the
   *upstream* repo — always pass `-R coffeegrind123/ReHLDS_Sven`.)
2. **A control is not a control until you have seen it.** Process alive + a plausible log line
   is not proof a map rendered. Screenshot it. This cost a wrong conclusion twice.
3. **`git add -A` after a killed gate run once committed the removal of `REHLDS_SVEN`** and CI
   silently built stock ReHLDS. Add named paths.
4. **A restart reverts the map to `bm_sts`** (container CMD), which this client cannot load
   (surface extents 544 > 256). `changelevel` after a restart and give it ~30s — rcon does not
   answer while a map loads.
5. **The source files are CRLF.** Editing with a plain Python `read()/write()` rewrites every
   line ending and produces a whole-file diff. Use `newline=''` and put `\r\n` in the
   patterns. (`sv_proto.h` lines 125-132 are LF-only; leave them.)
6. **Don't put a blind auto-kill on a process on the user's desktop.** A 75-second
   `Stop-Process -Name hl -Force` killed their client mid-session. Kill by the PID you launched.
7. **Map names beginning with `-`** cannot be passed via `+map` — the engine's argv parser
   takes them as a switch. Use a cfg and `+exec`.

## What is genuinely unresolved

- **`Mod_FindName: NULL name`** — the live fault. See above.
- **`AllocBlock: full`** — probably a content ceiling on that map; the proving control has not
  been run. If it is a ceiling, the open design question is whether the server should refuse
  to put HL-dialect clients on a map `hlmapcheck.py` rejects, and what to tell them.
- **Which maps are playable at all.** `hlmapcheck.py` has not been run over the map pack.
  That list is probably the most useful next artefact.
- **Temp entities.** Composed by the game DLL through `MESSAGE_BEGIN`, where the engine cannot
  tell which shorts are model indices or entity numbers, so none of the filtering covers them.
  A strong candidate for the empty-model-name fault.
- **`-num_edicts` is assumed, not negotiated.** `879fd43` assumes the documented default of
  1200; a player launching with a lower value will still overflow. `sv_proto_hl_max_edicts`
  sets it by hand.
- **`args.entindex` on events** is not filtered against the edict ceiling. Not observed to
  fail, not proven safe.
- **`mp_consistency` is 0**, so the consistency path from `2961fea` / `bc2c410` has still never
  been exercised.
- **Gameplay past spawning is untested** — no weapon fired, no Sven entity triggered, no
  changelevel with a client connected.
- **A Sven client and a Half-Life client have never been on the server at the same time**,
  which is the actual goal.
