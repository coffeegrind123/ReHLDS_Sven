/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

#pragma once

#include "maintypes.h"
#include "common.h"
#include "const.h"

#ifdef REHLDS_SVEN

// ---------------------------------------------------------------------------
// Per-client protocol dialect.
//
// Sven Co-op and Half-Life both call themselves protocol 48, but Svengine
// widened a set of fields on the wire and dropped packet munging. The engine
// used to pick one of the two encodings at compile time (REHLDS_SVEN); it now
// picks per client, so a retail Sven Co-op 5.26 client and a stock Half-Life
// client (vanilla, or running the SevenKewp client) can sit on one server.
//
// PROTO_DIALECT_SVEN is the native encoding of this build -- everything that
// is not explicitly marked as Half-Life keeps behaving exactly as it did
// before this layer existed. That is deliberate: the Sven path stays the
// tested path, and only buffers destined for a Half-Life client diverge.
// ---------------------------------------------------------------------------

enum proto_dialect_t
{
	PROTO_DIALECT_UNKNOWN = -1,
	PROTO_DIALECT_SVEN    = 0,	// Svengine 5.26: no munging, long coords, wide indices
	PROTO_DIALECT_HL      = 1,	// stock GoldSrc protocol 48
};

// Every wire divergence between the two dialects, in one table. Each entry is
// (name, sven_bits, hl_bits); the emitters call PROTO_BITS(name) and the
// parsers call PROTO_READ_BITS(name), so a field's two widths are declared
// once and can never drift apart at the call sites.
//
// The comment on each line is the emitter that owns it.
#define PROTO_BITFIELD_LIST(_)                                                            \
	/*        name                     sven  hl   owner                              */   \
	_(DELTA_BYTECOUNT,                    4,  3)  /* DELTA_WriteDelta/ParseDelta      */   \
	_(ENTITY_NUMBER,                     13, 11)  /* SV_WriteDeltaHeader, baselines   */   \
	_(SOUND_ENTITY,                      13, 11)  /* SV_BuildSoundMsg (MAX_EDICT_BITS)*/   \
	_(DELTA_SEQUENCE,                    16,  8)  /* svc_deltapacketentities, clientdata */\
	_(WEAPON_INDEX,                       8,  6)  /* SV_WriteClientdataToMessage      */   \
	_(EVENT_INDEX,                       10, 11)  /* SV_EmitEvents (Sven took a bit)  */   \
	_(RESOURCE_INDEX,                    16, 12)  /* SV_SendResources                 */   \
	_(CONSISTENCY_INDEX,                 16, 10)  /* SV_WriteClientdataToMessage      */   \
	_(BITCOORD_INT,                      24, 12)  /* MSG_WriteBitCoord                */

#define PROTO_DECL_BITFIELD(name, sven, hl) \
	PROTO_BITS_SVEN_##name = (sven), PROTO_BITS_HL_##name = (hl),

enum
{
	PROTO_BITFIELD_LIST(PROTO_DECL_BITFIELD)
	PROTO_BITS_DUMMY_ = 0
};

#undef PROTO_DECL_BITFIELD

// Emit `name` at whichever width the buffer currently being bit-written wants.
#define PROTO_BITS(name, value) \
	MSG_WriteBitsProto((value), PROTO_BITS_SVEN_##name, PROTO_BITS_HL_##name)

// Parse `name` at whichever width the buffer currently being bit-read wants.
#define PROTO_READ_BITS(name) \
	MSG_ReadBitsProto(PROTO_BITS_SVEN_##name, PROTO_BITS_HL_##name)

// A Half-Life client cannot express a delta bitmask wider than 7 bytes
// (DELTA_ParseDelta reads the byte count as 3 bits), so it can never be told
// about a field past this index. Sven's delta.lst is longer than that; fields
// beyond the cut simply never update for Half-Life clients, and the delta
// descriptions those clients receive are truncated to match.
#define PROTO_HL_MAX_DELTA_FIELDS	56

// ---------------------------------------------------------------------------
// Per-client state
// ---------------------------------------------------------------------------

struct client_s;
typedef struct client_s client_t;
struct netchan_s;
typedef struct netchan_s netchan_t;

extern cvar_t sv_proto_dialect;		// "auto" | "sven" | "hl"
extern cvar_t sv_proto_fallback;	// dialect to assume when detection is inconclusive
extern cvar_t sv_proto_log;		// 0 off, 1 verdicts, 2 full hex dumps
extern cvar_t sv_proto_hl_gamedir;	// gamedir reported to Half-Life clients

void SV_Proto_Init(void);

// Slot bookkeeping. SV_Proto_ResetClient is called when a slot is (re)connected.
void SV_Proto_ResetClient(int slot);
void SV_Proto_SetClientDialect(int slot, proto_dialect_t dialect, const char *why);
proto_dialect_t SV_Proto_GetClientDialect(int slot);

// Resolve a client/netchan back to its dialect. Anything the engine cannot
// attribute to a connected client is treated as native (Sven).
int SV_Proto_ClientSlot(const client_t *cl);
int SV_Proto_NetchanSlot(const netchan_t *chan);
proto_dialect_t SV_Proto_DialectOfClient(const client_t *cl);
proto_dialect_t SV_Proto_DialectOfNetchan(const netchan_t *chan);
proto_dialect_t SV_Proto_DialectOfAddress(netadr_t *adr);

inline bool SV_Proto_ClientIsHL(const client_t *cl)
{
	return SV_Proto_DialectOfClient(cl) == PROTO_DIALECT_HL;
}

// Stamp / clear the dialect flag on a buffer so the MSG_* primitives writing
// into it pick the right encoding.
void SV_Proto_StampBuffer(sizebuf_t *sb, proto_dialect_t dialect);
void SV_Proto_StampClientBuffers(client_t *cl);

// Shared buffers composed twice, once per dialect. See sv_proto.cpp.
extern sizebuf_t g_sv_signon_hl;
extern sizebuf_t g_sv_datagram_hl;
extern sizebuf_t g_sv_multicast_hl;
extern sizebuf_t g_sv_spectator_hl;

void SV_Proto_InitSharedBuffers(void);
sizebuf_t *SV_Proto_TwinOf(sizebuf_t *primary);
sizebuf_t *SV_Proto_PickShared(sizebuf_t *primary, const client_t *cl);
void SV_Proto_ClearShared(sizebuf_t *primary);

// Detection. Called from SV_ReadPackets with the raw datagram for a client
// whose dialect is not yet known; returns the dialect it settled on (possibly
// still UNKNOWN, in which case the caller should try again next packet).
proto_dialect_t SV_Proto_ProbeIncoming(client_t *cl, const byte *data, int len);

// Instrumentation: dump what a connecting client actually sent. Logged under
// sv_proto_log so the detector can be checked against observed bytes rather
// than inference.
void SV_Proto_LogConnect(const netadr_t *adr, const char *protinfo, const char *userinfo);

const char *SV_Proto_DialectName(proto_dialect_t dialect);

#endif // REHLDS_SVEN
