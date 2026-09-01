# Mixed Sven Co-op and Half-Life clients

The detail behind the summary in [the README](../README.md#mixed-sven-and-half-life-clients).

Sven Co-op and Half-Life both announce **protocol 48**, but Svengine widened a set of wire
fields and dropped packet munging. Historically this fork picked one of the two encodings at
compile time, so a server spoke Sven *or* Half-Life and never both.

It is now chosen **per client**, at runtime. One server, running the retail Sven Co-op
`server.so`, can serve a retail Sven 5.26 player and a stock Half-Life player at the same
time.

## How a client's dialect is decided

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
| `sv_proto_hl_gamedir` | *(empty)* | gamedir reported to Half-Life clients only — see [The gamedir gate](#the-gamedir-gate) |

`status` gains a `proto` column showing what each connected player is being served.

## What diverges below the messages

Three things under the message layer differ, and all three key off that same per-client
answer. They are listed here because none of them is visible in a message dump — a mistake in
any of them corrupts the stream some way *into* a packet, so the client reports the failure at
a message that was written correctly.

| | |
|---|---|
| **Packet munging** | Half-Life `COM_Munge2`s everything from byte 8 on; Sven sends it plain. |
| **Netchan fragment headers** | Half-Life writes `frag_startpos` and `frag_length` as shorts; Sven writes longs. |
| **Split-packet framing** | Anything over `MAX_ROUTEABLE_PACKET` is cut into parts behind a `SPLITPACKET` header: 9 bytes for Half-Life, 10 for Sven, which widened `packetID` to a short. The receiver places part *N* at `N * (MAX_ROUTEABLE_PACKET - the header size **it** parses)`, so the payload stride differs as well — 1391 against 1390. The emitter has to take both numbers from the recipient's layout; taking the stride from this build's is a one-byte shift of every part after the first. |

`net_showpackets 4` names the layout, header size and stride on each part it sends.

## The gamedir gate

A stock Half-Life client checks the server's gamedir **client-side** and disconnects itself
before it ever spawns:

```
Sven Co-op 5.26
Server Engine: 5.0.18 (build 10493)
Server Number: 1

Server is running game svencoop.  Restart in that game to connect.
```

That is not a protocol failure — reaching it means the wire is correct.
[rehlds/rehlds#975](https://github.com/rehlds/rehlds/issues/975) settled the general case: the
only fix is for the server to report a different gamedir, and it was judged to need the client
to declare its game through `setinfo`, "*but this does not happen*". That is what upstream's
`_gd` userinfo key is for; it still works here and still takes precedence.

The dialect probe removes that dependency for this case. The server has already worked out
from the wire that a client is stock GoldSrc rather than Svengine, with no cooperation from
it, so it can spoof the gamedir for exactly those clients. It still cannot know *which* mod a
stock client runs — the probe reads the dialect, not the gamedir — so name it once:

```
sv_proto_hl_gamedir "valve"      // or whatever gamedir your Half-Life-side players run
```

Empty (the default) reports the real gamedir and changes nothing.

⚠ **Spoofing the gamedir does not conjure content.** The client still needs the map and the
models, and `mp_consistency` will fight you across two games with different content — the same
caveat rehlds#975 raises. This gets a Half-Life-side client *past the gate*; having something
to play once through is a mod and gamedir question, not an engine one.

## What a Half-Life client gives up

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

## What a Half-Life client cannot render

Past the wire there is a second wall, and it is higher. A stock client enforces content
ceilings that some Sven Co-op content is built past, and no amount of server-side work moves
them -- the server would have to withhold the content, which is not the same as the client
being able to play.

These were read out of the 25th-anniversary `hw.dll` rather than taken from a header, and two
of them are **not** the numbers the original engine used:

| ceiling | value | where |
|---|---|---|
| Lightmap atlas | 64 blocks of 128x128 luxels | `GL_BuildLightmaps` RVA `0x246F80`, error `AllocBlock: full` at `0x24715D` |
| Surface extents | **512**, not the original 256 | `cmp ecx,0x200` at `0x23E4AB`, error at `0x23E4D3`. `TEX_SPECIAL` faces are exempt |
| Inline submodels | **511** (`*1`..`*511`) | 512-entry table at `0x1198480`, built at `0x20B803`, indexed unbounded by `SV_SpawnServer` at `0x20EE70` |

> [!NOTE]
> The submodel ceiling is a **listen-server** ceiling. It is the retail engine's *server* that
> walks off the end of that table, so it is what you hit running `map <name>` locally; a client
> joining a dedicated server never runs that loop. It is why a 712-submodel map dies with
> `Mod_FindName: NULL name` under `map` and not when connected.
>
> That error is also misnamed: the guard is `cmp BYTE PTR [eax],bl`, which would fault on a
> genuine NULL, so the name is a valid pointer to an **empty string**.

`tools/hlmapcheck.py` reimplements `GL_BuildLightmaps` against a BSP and reports whether a
retail client can render a map. It is exact rather than an estimate, and that was measured:
logging `(w, h)` for every surface of `abandoned.bsp` out of the running engine gives 20453
surfaces, and the tool produces the same 20453 in the same order with zero differing sizes;
reading `allocated[]` back at the function epilogue gives `abandoned` 64 blocks (atlas
completely full) and `toadsnatch` 61, which the tool matches.

Measured over a retail Sven Co-op 5.26 map pack (108 maps):

| | maps |
|---|---|
| Over the 512-unit surface-extents ceiling | 36 |
| Over the 64-block lightmap ceiling | 2 |
| Both | 5 |
| **Cannot be rendered by a retail client** | **43** |
| Renders on a dedicated server, but 512+ submodels means no retail listen server | 8 |
| Clears everything | 57 |

> [!NOTE]
> Earlier versions of this document claimed a 256x256 texture ceiling (`GL_LoadTexture: too
> big`) and concluded that only 3 of 108 maps were playable. That is wrong for this client:
> both `GL_LoadTexture: too big` and `Can't upload (%ix%i) texture` are present in `hw.dll` as
> strings but have **no references** to them -- a control search for `AllocBlock: full` and
> `Mod_FindName: NULL name` finds their push sites correctly, so the search method is sound.
> The anniversary client does not raise that error. Whether some other size limit applies has
> not been tested. `tools/hlshrink.py`, which exists to resample oversized textures, is
> therefore not needed for this client, and is kept for older ones.

So the position is that the dialect layer makes a stock Half-Life client connect, spawn and
play, and just over half the map pack is renderable by it. A client with raised ceilings --
the [SevenKewp](https://github.com/wootguy/SevenKewp) client, or an engine like Xash3D-FWGS
that speaks protocol 48 -- reaches the rest.

The game DLL is a separate matter from the wire. The engine will frame every message correctly
for both clients; whether a Half-Life client has the *content* (models, sounds, sprites) and
the client-side message handlers to make sense of what a Sven mod sends it is up to the mod and
the gamedir, not the engine.

## How it is built

The whole thing keys off one bit on `sizebuf_t::flags` (`SIZEBUF_PROTO_HL`). A buffer with
no stamp is native Sven, so **every path that predates this layer behaves exactly as it did**
— only buffers destined for a Half-Life client diverge. The `MSG_*` primitives read the stamp
off the buffer they are writing to, or off the bit writer's current buffer, so a call site
says `PROTO_BITS(ENTITY_NUMBER, num)` and never has to know who the recipient is.

Every divergence is declared once, in `PROTO_BITFIELD_LIST` in
[`rehlds/engine/sv_proto.h`](../rehlds/engine/sv_proto.h), so a field's two widths cannot drift
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
