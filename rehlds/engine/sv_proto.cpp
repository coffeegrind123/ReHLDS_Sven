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

#include "precompiled.h"

#ifdef REHLDS_SVEN

cvar_t sv_proto_dialect  = { "sv_proto_dialect",  "auto", FCVAR_SERVER, 0.0f, nullptr };
cvar_t sv_proto_fallback = { "sv_proto_fallback", "sven", FCVAR_SERVER, 0.0f, nullptr };
cvar_t sv_proto_log      = { "sv_proto_log",      "0",    0,            0.0f, nullptr };

// The gamedir reported to Half-Life-dialect clients, or empty to report the
// real one. See SV_SendServerinfo_internal for why this exists.
cvar_t sv_proto_hl_gamedir = { "sv_proto_hl_gamedir", "", FCVAR_SERVER, 0.0f, nullptr };

// How many resources a Half-Life client is told about at most. It cannot store
// more than PROTO_HL_MAX_RESOURCE_LIST of them whatever this says, so that is
// both the default and the ceiling; lower it to trade content for a client that
// gets further, or to bisect which resource a client dies on.
cvar_t sv_proto_hl_max_resources = { "sv_proto_hl_max_resources", "1280", FCVAR_SERVER, 0.0f, nullptr };

// Per-slot dialect. Deliberately NOT a client_t member: client_t is walked by
// Metamod plugins that were compiled against the current layout, and growing
// it would shift g_psvs.clients' stride under them.
static proto_dialect_t s_clientDialect[MAX_CLIENTS];
static int s_clientProbeAttempts[MAX_CLIENTS];

// How many inconclusive probes to tolerate before falling back. Each probe is
// one datagram; a client that has sent this many packets without ever looking
// like either dialect is not going to start.
static const int PROTO_MAX_PROBE_ATTEMPTS = 4;

const char *SV_Proto_DialectName(proto_dialect_t dialect)
{
	switch (dialect)
	{
	case PROTO_DIALECT_SVEN: return "sven";
	case PROTO_DIALECT_HL:   return "hl";
	default:                 return "unknown";
	}
}

static proto_dialect_t SV_Proto_ParseDialectName(const char *s)
{
	if (!s || !s[0])
		return PROTO_DIALECT_UNKNOWN;

	if (!Q_stricmp(s, "sven") || !Q_stricmp(s, "svencoop") || !Q_stricmp(s, "1"))
		return PROTO_DIALECT_SVEN;

	if (!Q_stricmp(s, "hl") || !Q_stricmp(s, "halflife") || !Q_stricmp(s, "valve") || !Q_stricmp(s, "0"))
		return PROTO_DIALECT_HL;

	return PROTO_DIALECT_UNKNOWN;
}

// The dialect to assume when detection never reaches a verdict.
static proto_dialect_t SV_Proto_Fallback(void)
{
	proto_dialect_t d = SV_Proto_ParseDialectName(sv_proto_fallback.string);
	return (d == PROTO_DIALECT_UNKNOWN) ? PROTO_DIALECT_SVEN : d;
}

// A forced dialect, or UNKNOWN when sv_proto_dialect is "auto".
static proto_dialect_t SV_Proto_Forced(void)
{
	if (!sv_proto_dialect.string || !Q_stricmp(sv_proto_dialect.string, "auto"))
		return PROTO_DIALECT_UNKNOWN;

	return SV_Proto_ParseDialectName(sv_proto_dialect.string);
}

void SV_Proto_Init(void)
{
	Cvar_RegisterVariable(&sv_proto_dialect);
	Cvar_RegisterVariable(&sv_proto_fallback);
	Cvar_RegisterVariable(&sv_proto_log);
	Cvar_RegisterVariable(&sv_proto_hl_gamedir);
	Cvar_RegisterVariable(&sv_proto_hl_max_resources);

	// Also done per map in SV_SpawnServer; do it here so the twins are never
	// a null-data sizebuf, whatever order a caller reaches them in.
	SV_Proto_InitSharedBuffers();

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		s_clientDialect[i] = PROTO_DIALECT_UNKNOWN;
		s_clientProbeAttempts[i] = 0;
	}
}

void SV_Proto_ResetClient(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
		return;

	s_clientDialect[slot] = SV_Proto_Forced();
	s_clientProbeAttempts[slot] = 0;

	if (s_clientDialect[slot] != PROTO_DIALECT_UNKNOWN && sv_proto_log.value != 0.0f)
	{
		Con_Printf("[proto] slot %d forced to %s by sv_proto_dialect\n",
			slot, SV_Proto_DialectName(s_clientDialect[slot]));
	}
}

void SV_Proto_SetClientDialect(int slot, proto_dialect_t dialect, const char *why)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
		return;

	if (s_clientDialect[slot] == dialect)
		return;

	s_clientDialect[slot] = dialect;

	if (sv_proto_log.value != 0.0f)
	{
		Con_Printf("[proto] slot %d -> %s (%s)\n", slot, SV_Proto_DialectName(dialect), why ? why : "");
	}

	if (slot < g_psvs.maxclients && g_psvs.clients)
		SV_Proto_StampClientBuffers(&g_psvs.clients[slot]);
}

proto_dialect_t SV_Proto_GetClientDialect(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
		return PROTO_DIALECT_SVEN;

	// An undecided client is served the native encoding. That is the correct
	// bet: the undecided window is the signon handshake, and every byte the
	// server emits there is byte-aligned and dialect-neutral except for the
	// serverinfo CRC munge, which SV_SendServerinfo defers until the dialect
	// is known (the client sends "new" over the netchan first, which is what
	// resolves it).
	if (s_clientDialect[slot] == PROTO_DIALECT_UNKNOWN)
		return PROTO_DIALECT_SVEN;

	return s_clientDialect[slot];
}

bool SV_Proto_ClientDialectKnown(const client_t *cl)
{
	int slot = SV_Proto_ClientSlot(cl);
	if (slot < 0)
		return true;			// not a real client slot; nothing to wait for

	return s_clientDialect[slot] != PROTO_DIALECT_UNKNOWN;
}

int SV_Proto_ClientSlot(const client_t *cl)
{
	if (!cl || !g_psvs.clients)
		return -1;

	ptrdiff_t slot = cl - g_psvs.clients;
	if (slot < 0 || slot >= g_psvs.maxclients || slot >= MAX_CLIENTS)
		return -1;

	return (int)slot;
}

int SV_Proto_NetchanSlot(const netchan_t *chan)
{
	if (!chan || !g_psvs.clients)
		return -1;

	// netchan_t lives inside client_t; recover the owner and range-check it,
	// which also rejects the client-side and HLTV netchans.
	const client_t *cl = (const client_t *)((const byte *)chan - offsetof(client_t, netchan));
	int slot = SV_Proto_ClientSlot(cl);

	if (slot >= 0 && &g_psvs.clients[slot].netchan != chan)
		return -1;

	return slot;
}

proto_dialect_t SV_Proto_DialectOfClient(const client_t *cl)
{
	int slot = SV_Proto_ClientSlot(cl);
	return (slot < 0) ? PROTO_DIALECT_SVEN : SV_Proto_GetClientDialect(slot);
}

proto_dialect_t SV_Proto_DialectOfNetchan(const netchan_t *chan)
{
	int slot = SV_Proto_NetchanSlot(chan);
	return (slot < 0) ? PROTO_DIALECT_SVEN : SV_Proto_GetClientDialect(slot);
}

proto_dialect_t SV_Proto_DialectOfAddress(netadr_t *adr)
{
	if (!adr || !g_psvs.clients)
		return PROTO_DIALECT_SVEN;

	for (int i = 0; i < g_psvs.maxclients && i < MAX_CLIENTS; i++)
	{
		client_t *cl = &g_psvs.clients[i];
		if (!cl->connected && !cl->active && !cl->spawned)
			continue;

		if (NET_CompareAdr(*adr, cl->netchan.remote_address))
			return SV_Proto_GetClientDialect(i);
	}

	return PROTO_DIALECT_SVEN;
}

// ---------------------------------------------------------------------------
// Resources a Half-Life client can address
//
// Sven's qlimits give 8192 models / 8192 sounds / 8192 generics / 512 decals /
// 1024 events; a stock client has 512 / 512 / 512 / 512 / 256 and indexes those
// arrays with whatever index the resource list gives it. So the list has to be
// filtered for such a client, not merely encoded at the narrower width.
//
// The filter is a pure function of the resource array, so nothing has to be
// remembered per client: the same walk recovers the mapping in either
// direction.
// ---------------------------------------------------------------------------

static resource_t *SV_Proto_ResourceArray(void)
{
#ifdef REHLDS_FIXES
	return g_rehlds_sv.resources;
#else
	return g_psv.resourcelist;
#endif
}

bool SV_Proto_HLCanAddressResource(const struct resource_s *r)
{
	if (!r)
		return false;

	switch (r->type)
	{
	case t_model:
	case t_world:		return r->nIndex < PROTO_HL_MAX_MODELS;
	case t_sound:		return r->nIndex < PROTO_HL_MAX_SOUNDS;
	case t_generic:		return r->nIndex < PROTO_HL_MAX_GENERIC;
	case t_decal:		return r->nIndex < PROTO_HL_MAX_DECALS;
	case t_eventscript:	return r->nIndex < PROTO_HL_MAX_EVENTS;
	default:		return true;	// t_skin carries no precache index
	}
}

int SV_Proto_HLResourceCap(void)
{
	int cap = (int)sv_proto_hl_max_resources.value;

	if (cap <= 0 || cap > PROTO_HL_MAX_RESOURCE_LIST)
		cap = PROTO_HL_MAX_RESOURCE_LIST;

	return cap;
}

int SV_Proto_HLResourceCount(void)
{
	resource_t *r = SV_Proto_ResourceArray();
	int n = 0;

	const int cap = SV_Proto_HLResourceCap();

	for (int i = 0; i < g_psv.num_resources && n < cap; i++, r++)
	{
		if (SV_Proto_HLCanAddressResource(r))
			n++;
	}

	return n;
}

int SV_Proto_HLResourceRealIndex(int hlIndex)
{
	if (hlIndex < 0 || hlIndex >= SV_Proto_HLResourceCap())
		return -1;

	resource_t *r = SV_Proto_ResourceArray();
	int n = 0;

	for (int i = 0; i < g_psv.num_resources; i++, r++)
	{
		if (!SV_Proto_HLCanAddressResource(r))
			continue;

		if (n == hlIndex)
			return i;

		n++;
	}

	return -1;
}

void SV_Proto_StampBuffer(sizebuf_t *sb, proto_dialect_t dialect)
{
	if (!sb)
		return;

	if (dialect == PROTO_DIALECT_HL)
		sb->flags |= SIZEBUF_PROTO_HL;
	else
		sb->flags &= ~SIZEBUF_PROTO_HL;
}

// ---------------------------------------------------------------------------
// Twinned shared buffers
//
// Four of the server's message buffers are composed once and delivered to
// every client: the signon block, the broadcast datagram, the multicast
// staging buffer and the spectator datagram. All four carry dialect-dependent
// content -- coordinates, user-message length prefixes, and in the signon's
// case a whole bit-packed baseline block -- so one encoding cannot serve both
// kinds of client.
//
// Rather than transcode on delivery (which would need a parser for every svc
// message and would rot the first time one changed), each is composed TWICE:
// the existing buffer in the native encoding, and a twin stamped for
// Half-Life. Producers write to both; consumers pick by the recipient's
// dialect. g_psv.reliable_datagram is deliberately NOT twinned -- with
// REHLDS_FIXES its MSG_ALL user messages are already fanned out per client,
// and what remains (svc_setpause, svc_updateuserinfo) is dialect-neutral.
//
// The twins live here rather than in server_t because server_t's layout is
// visible to plugins.
// ---------------------------------------------------------------------------

sizebuf_t g_sv_signon_hl;
sizebuf_t g_sv_datagram_hl;
sizebuf_t g_sv_multicast_hl;
sizebuf_t g_sv_spectator_hl;

static byte g_sv_signon_hl_data[NET_MAX_PAYLOAD];
static byte g_sv_datagram_hl_data[MAX_DATAGRAM];
static byte g_sv_multicast_hl_data[MAX_DATAGRAM];
static byte g_sv_spectator_hl_data[MAX_DATAGRAM];

static void SV_Proto_InitTwin(sizebuf_t *sb, const char *name, byte *data, int size, uint16 extraFlags)
{
	sb->buffername = name;
	sb->data = data;
	sb->maxsize = size;
	sb->cursize = 0;
	sb->flags = extraFlags | SIZEBUF_PROTO_HL;
}

void SV_Proto_InitSharedBuffers(void)
{
	SV_Proto_InitTwin(&g_sv_signon_hl,    "Server Signon Buffer (HL)",    g_sv_signon_hl_data,    sizeof(g_sv_signon_hl_data),    0);
	SV_Proto_InitTwin(&g_sv_datagram_hl,  "Server Datagram (HL)",         g_sv_datagram_hl_data,  sizeof(g_sv_datagram_hl_data),  SIZEBUF_ALLOW_OVERFLOW);
	SV_Proto_InitTwin(&g_sv_multicast_hl, "Server Multicast Buffer (HL)", g_sv_multicast_hl_data, sizeof(g_sv_multicast_hl_data), 0);
	SV_Proto_InitTwin(&g_sv_spectator_hl, "Server Spectator Buffer (HL)", g_sv_spectator_hl_data, sizeof(g_sv_spectator_hl_data), SIZEBUF_ALLOW_OVERFLOW);
}

sizebuf_t *SV_Proto_TwinOf(sizebuf_t *primary)
{
	if (primary == &g_psv.signon)    return &g_sv_signon_hl;
	if (primary == &g_psv.datagram)  return &g_sv_datagram_hl;
	if (primary == &g_psv.multicast) return &g_sv_multicast_hl;
	if (primary == &g_psv.spectator) return &g_sv_spectator_hl;
	return nullptr;
}

sizebuf_t *SV_Proto_PickShared(sizebuf_t *primary, const client_t *cl)
{
	if (SV_Proto_DialectOfClient(cl) != PROTO_DIALECT_HL)
		return primary;

	sizebuf_t *twin = SV_Proto_TwinOf(primary);
	return twin ? twin : primary;
}

void SV_Proto_ClearShared(sizebuf_t *primary)
{
	SZ_Clear(primary);

	sizebuf_t *twin = SV_Proto_TwinOf(primary);
	if (twin)
		SZ_Clear(twin);
}

void SV_Proto_StampClientBuffers(client_t *cl)
{
	if (!cl)
		return;

	proto_dialect_t d = SV_Proto_DialectOfClient(cl);
	SV_Proto_StampBuffer(&cl->netchan.message, d);
	SV_Proto_StampBuffer(&cl->datagram, d);
}

// ---------------------------------------------------------------------------
// Detection
//
// The dialects differ in the netchan payload, not the netchan header: bytes
// 0..7 (sequence, sequence_ack) are plain in both, and everything from byte 8
// on is COM_Munge2'd for Half-Life and plain for Sven. So the first datagram a
// client sends over its netchan can be decoded both ways and scored, and the
// interpretation that yields a valid clc stream is the client's dialect.
//
// This is a content probe rather than a userinfo fingerprint on purpose: a
// userinfo key is a claim the client makes about itself and can be absent,
// stale or spoofed, whereas the munge either round-trips or it does not.
// ---------------------------------------------------------------------------

// Score how much a decoded payload looks like a real clc command stream.
// Higher is better; a negative score means "definitely not this dialect".
static int SV_Proto_ScorePayload(const byte *payload, int len)
{
	if (len <= 0)
		return -1;

	byte op = payload[0];

	switch (op)
	{
	case clc_stringcmd:
	{
		// The first thing a client sends after connect is
		// clc_stringcmd "new". Require a NUL-terminated printable command.
		int i = 1;
		while (i < len && payload[i] != '\0')
		{
			byte c = payload[i];
			if (c < 0x20 || c > 0x7E)
				return -1;
			i++;
		}

		if (i >= len)		// never terminated
			return -1;

		int cmdlen = i - 1;
		if (cmdlen == 0)
			return -1;

		// "new" and "sendres" are the two commands that open a signon; seeing
		// either is as strong as this probe gets.
		if (!Q_strncmp((const char *)&payload[1], "new", 4) ||
			!Q_strncmp((const char *)&payload[1], "sendres", 8))
			return 100;

		return 40;
	}

	case clc_nop:
		// A lone nop carries no evidence, but a payload that is nothing but
		// nops decodes identically under neither/both -- treat as weak.
		return 5;

	case clc_move:
	case clc_delta:
	case clc_resourcelist:
	case clc_tmove:
	case clc_fileconsistency:
	case clc_voicedata:
	case clc_cvarvalue:
	case clc_cvarvalue2:
		return 10;

	default:
		return -1;
	}
}

static void SV_Proto_HexDump(const char *tag, const byte *data, int len)
{
	char line[3 * 16 + 1];
	int shown = Q_min(len, 64);

	for (int off = 0; off < shown; off += 16)
	{
		int n = Q_min(16, shown - off);
		int pos = 0;
		for (int i = 0; i < n; i++)
			pos += Q_snprintf(line + pos, sizeof(line) - pos, "%02x ", data[off + i]);
		line[pos] = '\0';
		Con_Printf("[proto]   %s +%02x: %s\n", tag, off, line);
	}
}

proto_dialect_t SV_Proto_ProbeIncoming(client_t *cl, const byte *data, int len)
{
	int slot = SV_Proto_ClientSlot(cl);
	if (slot < 0)
		return PROTO_DIALECT_SVEN;

	if (s_clientDialect[slot] != PROTO_DIALECT_UNKNOWN)
		return s_clientDialect[slot];

	// Split packets are reassembled before we get here; a fragmented first
	// packet still arrives as a whole datagram. Anything shorter than a bare
	// netchan header is not decodable either way.
	const int kHeaderBytes = 8;
	if (len <= kHeaderBytes)
		return PROTO_DIALECT_UNKNOWN;

	uint32 sequence = *(const uint32 *)data;

	// A fragmented message carries per-stream fragment headers between the
	// netchan header and the payload, and those headers are themselves one of
	// the things that differ (long/long vs short/short). Skip probing on such
	// a packet rather than guess at where the payload starts.
	if (sequence & (1 << 30))
		return PROTO_DIALECT_UNKNOWN;

	int payloadLen = len - kHeaderBytes;
	if (payloadLen > NET_MAX_PAYLOAD)
		payloadLen = NET_MAX_PAYLOAD;

	// Candidate A: Sven -- payload is already plain.
	int scoreSven = SV_Proto_ScorePayload(data + kHeaderBytes, payloadLen);

	// Candidate B: Half-Life -- unmunge a copy keyed on the sequence, exactly
	// as Netchan_Process would.
	static byte unmunged[NET_MAX_PAYLOAD];
	Q_memcpy(unmunged, data + kHeaderBytes, payloadLen);
	COM_UnMunge2(unmunged, payloadLen, sequence & 0xFF);
	int scoreHL = SV_Proto_ScorePayload(unmunged, payloadLen);

	if (sv_proto_log.value != 0.0f)
	{
		Con_Printf("[proto] slot %d probe: len=%d seq=%u sven=%d hl=%d\n",
			slot, len, sequence, scoreSven, scoreHL);

		if (sv_proto_log.value >= 2.0f)
		{
			SV_Proto_HexDump("sven", data + kHeaderBytes, payloadLen);
			SV_Proto_HexDump("hl  ", unmunged, payloadLen);
		}
	}

	s_clientProbeAttempts[slot]++;

	if (scoreSven > scoreHL && scoreSven > 0)
	{
		SV_Proto_SetClientDialect(slot, PROTO_DIALECT_SVEN, "unmunged payload parses as clc");
		return PROTO_DIALECT_SVEN;
	}

	if (scoreHL > scoreSven && scoreHL > 0)
	{
		SV_Proto_SetClientDialect(slot, PROTO_DIALECT_HL, "munged payload parses as clc");
		return PROTO_DIALECT_HL;
	}

	if (s_clientProbeAttempts[slot] >= PROTO_MAX_PROBE_ATTEMPTS)
	{
		proto_dialect_t fallback = SV_Proto_Fallback();
		Con_DPrintf("[proto] slot %d: %d inconclusive probes, falling back to %s\n",
			slot, s_clientProbeAttempts[slot], SV_Proto_DialectName(fallback));
		SV_Proto_SetClientDialect(slot, fallback, "probe inconclusive, sv_proto_fallback");
		return fallback;
	}

	return PROTO_DIALECT_UNKNOWN;
}

void SV_Proto_LogConnect(const netadr_t *adr, const char *protinfo, const char *userinfo)
{
	if (sv_proto_log.value == 0.0f)
		return;

	Con_Printf("[proto] connect from %s\n", adr ? NET_AdrToString(*adr) : "?");
	Con_Printf("[proto]   protinfo: %s\n", protinfo ? protinfo : "");
	Con_Printf("[proto]   userinfo: %s\n", userinfo ? userinfo : "");
}

#endif // REHLDS_SVEN
