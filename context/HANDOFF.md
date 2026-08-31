# Handoff: a stock Half-Life client on a Sven Co-op server

**Goal.** A retail Half-Life client connects to this ReHLDS_Sven server, spawns, and plays,
alongside Sven Co-op clients on the same server.

**Status.** The client gets through the entire signon, precache and map load. It still
crashes. The current fault is understood down to the faulting instruction but not yet to a
cause. Everything below is measured, not assumed; where something is a guess it says so.

---

## The single most important fact

A test run on 2026-08-31 loaded the same map **with no server involved**:

```
hl.exe -console -condebug +map abandoned   ->  STILL RUNNING (no crash)
```

So the client can load `abandoned` and all its content on its own. **The remaining fault is
something the server sends.** An earlier conclusion in this session — "the wall is Sven's
content, the engine work is done" — was wrong, and the README section written on that basis
(`7f389ff`, "What a Half-Life client cannot render") overstates the case. The content
ceilings in it are real and correctly measured, but they are not what is crashing this
client on this map.

**Confirm this before anything else**, because the whole plan depends on it: re-run that
standalone map load, and separately confirm whether a connect now succeeds or crashes.

## Where the crash is

From the Windows Application event log (via `~/claude-host-bridge/hostexec`), five
consecutive identical entries:

```
Faulting module hw.dll, exception 0xc0000005, fault offset 0x00247d03
```

`hw.dll` copied to `~/hw.dll`, disassembled with
`objdump -d --target=pei-i386 -M intel`:

```
10247cfe:  a1 50 cc 3f 11        mov  eax, ds:0x113fcc50     <- a GLOBAL, not an array elem
10247d03:  83 b8 7c 01 00 00 00  cmp  DWORD PTR [eax+0x17c], 0
```

`0x113fcc50` is written in exactly one place in the whole binary:

```
101a85f5:  mov eax, ds:0x113f6414      ; cl.worldmodel
101a85fa:  mov ds:0x113fcc50, eax      ; r_worldmodel
101a85ff:  test eax,eax
101a8601:  je  ...                     ; null-checked HERE, not at the fault
```

So **`r_worldmodel` is NULL when a lightmap path runs**. Either that assignment never ran,
or it ran with `cl.worldmodel` still null. The enclosing function of the fault is
`0x10247b50`, called from four sites (`102471bd`, `1024b964`, `1024bed0`, `1024c06b`) — the
surrounding code tests a `0xFF` style sentinel and zeroes a 16-byte-stride accumulator, i.e.
lightmap building.

Ruled out: the client **has** `abandoned.bsp` in `valve_downloads`, and it is byte-identical
to the server's copy (`md5 233b6978973c2436b4b7ac8e5500fc60`). Not a missing or mismatched map.

**Next step:** identify the two early-return conditions at the top of `0x101a8580`
(`cmp ds:0x1140e87c,0` / `cmp ds:0x1140da6c,2` + `cmp ds:0x11408de0,5`) and work out which
one the server can influence. Also worth checking what the four callers of `0x10247b50` are
reached from during a signon.

## Fixed this session (all verified on the wire, not just compiled)

| commit | bug |
|---|---|
| `fcbeeb4` | split packets framed with Sven's 1390 stride for clients that reassemble at 1391 |
| `2961fea` | resource indices past the client's 512-entry precache arrays |
| `110adf7` | 256 lightstyles written into a 64-entry array — 192 writes past the end, every join |
| `f1bf95e` | 1296 resources into a 1280-entry `cl.resourcelist[]` |
| `7abf0bb` | a netchan packet emitted **before the dialect probe ran**, therefore unmunged; at seq 1 the padded keepalive unmunges to `00 01 19 5a ...` and that `0x00` is `svc_bad` |
| `bc2c410` | resource trim was positional, so it always sacrificed decals and kept generics |
| `fb16961` | unusable model indices clamped to 0 — but nothing precaches index 0, so `cl.model_precache[0]` is NULL |
| `f747647` | same, for `PF_makestatic_I` and `PF_StaticDecal`, which write model indices straight into the signon |

`b854586` is not a fix: it restores `REHLDS_SVEN` to `CMakeLists.txt`, which `fb16961`
dropped by accident (see Hazards).

## Current state

- Server: docker container `svencoop-server`, engine from CI of `f747647`, map `abandoned`,
  `sv_proto_hl_gamedir "valve"`, `sv_proto_log 2`, `sv_proto_hl_max_resources 1280`.
  Reachable from the container at `172.17.0.2:27015`, from Windows at `127.0.0.1:27016`.
- `server.cfg` in the volume persists `sv_proto_hl_gamedir` and `sv_proto_log`. Anything set
  only over rcon is lost on restart — that already cost one wasted round.
- Working tree clean at `f747647`. Local gates all pass.

## Tools

- **`tools/hlprobe.py`** (gitignored, local only). A Half-Life-dialect client: munges like
  one, drives the full signon, prints netchan header, fragment headers, split framing and the
  svc stream. `--dump DIR` keeps every reassembled payload; `--dlfile`, `--seconds`, `--name`.
  It reaches "has entered the game" against the current server. **It does not render**, so it
  cannot reproduce this crash — but it proves the wire.
- **`~/claude-host-bridge/hostexec`** runs PowerShell on the Windows host. This is how the
  event log, `hw.dll` and the client's file listing were obtained. Use it early; three rounds
  were spent binary-searching a silent crash that one event-log query explained.
- `~/hw.dll` — the client engine, for disassembly.
- `~/hl-safe-content/` — 1019 files, textures resampled to within 256x256 and `w*h % 4 == 0`.
  Built by a local `tools/hlshrink.py`. Validated by full re-parse: 0 violations across 1649
  models, 471 sprites, 9813 WAD textures. Probably **not** needed given the standalone map
  load succeeded, but it is correct and harmless.
- rcon password `cs16test`; a small helper script lives in the session scratchpad and is
  trivially rewritten (challenge, then `rcon <challenge> "<pw>" <cmd>`).

## Hazards learned the hard way

1. **Never deploy a locally built engine.** It needs a newer `libstdc++` than the runtime
   image (`GLIBCXX_3.4.29` not found → `Unable to load engine, image is corrupt`) and the
   server enters a restart loop. Always take the binary from CI. Verify before deploying:
   `strings engine_i486.so | grep -c sv_proto_hl_max_resources` must be non-zero.
2. **`git add -A` after a killed gate run committed the removal of `REHLDS_SVEN`.** CI then
   built stock ReHLDS, and the server died with
   `DELTA_BuildFromLinks: Too many fields in delta description 57 (MAX 56)`. The gate script
   now builds the stock config in a throwaway `git worktree` and asserts the define survives.
3. **A restart reverts the map to `bm_sts`** (container CMD) — which this client *cannot*
   load (surface extents 544 > 256). Always `changelevel abandoned` after a restart.
4. **Don't put a blind auto-kill on a process on the user's desktop.** A 75-second
   `Stop-Process -Name hl -Force` killed their client mid-session.
5. Error strings from the client have been worth more than every inference made from the
   server side. `crashes on precaching resources` found `MAX_RESOURCE_LIST`;
   `Used decal #68 without a name` found the positional trim. Ask for the text, or read the
   event log directly.

## What is genuinely unresolved

- Why `r_worldmodel` is NULL during lightmap building, given the map loads standalone.
- Whether temp entities are a remaining source of unusable model/sprite indices. They are
  composed by the game DLL through `MESSAGE_BEGIN`, where the engine cannot tell which shorts
  are indices, so `f747647` explicitly does not cover them.
- `mp_consistency` is 0 on this server, so the consistency path added in `2961fea` /
  `bc2c410` (filtered positions, index mapping) has **never been exercised**.
