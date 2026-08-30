#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#ifdef REHLDS_SVEN

// Tests for the per-client protocol dialect layer (engine/sv_proto.h).
//
// The property that matters is symmetry: whatever a buffer is stamped for, a
// reader stamped the same way has to recover the value. These drive the real
// MSG_* primitives rather than a model of them, so a width that drifts at a
// call site shows up here rather than as a desync on a live server.

static byte s_protoBuf[8192];

static void Proto_ResetBuffer(sizebuf_t *buf, proto_dialect_t dialect)
{
	buf->buffername = "proto_test";
	buf->data = s_protoBuf;
	buf->maxsize = sizeof(s_protoBuf);
	buf->cursize = 0;
	buf->flags = SIZEBUF_CHECK_OVERFLOW;
	SV_Proto_StampBuffer(buf, dialect);
}

TEST(CoordWidthFollowsBufferDialect, Proto, 1000)
{
	sizebuf_t *buf = &net_message;

	// Sven: byte-aligned coordinates are longs, so three of them are 12 bytes.
	Proto_ResetBuffer(buf, PROTO_DIALECT_SVEN);
	MSG_WriteCoord(buf, 1.0f);
	MSG_WriteCoord(buf, -2.5f);
	MSG_WriteCoord(buf, 3000.0f);
	UINT32_EQUALS("Sven coords should be 4 bytes each", 12, buf->cursize);

	MSG_BeginReading();
	DOUBLES_EQUAL("Sven coord 1 roundtrip", 1.0f, MSG_ReadCoord(), 0.001f);
	DOUBLES_EQUAL("Sven coord 2 roundtrip", -2.5f, MSG_ReadCoord(), 0.001f);
	DOUBLES_EQUAL("Sven coord 3 roundtrip", 3000.0f, MSG_ReadCoord(), 0.001f);

	// Half-Life: shorts, so the same three are 6 bytes.
	Proto_ResetBuffer(buf, PROTO_DIALECT_HL);
	MSG_WriteCoord(buf, 1.0f);
	MSG_WriteCoord(buf, -2.5f);
	MSG_WriteCoord(buf, 3000.0f);
	UINT32_EQUALS("HL coords should be 2 bytes each", 6, buf->cursize);

	MSG_BeginReading();
	DOUBLES_EQUAL("HL coord 1 roundtrip", 1.0f, MSG_ReadCoord(), 0.001f);
	DOUBLES_EQUAL("HL coord 2 roundtrip", -2.5f, MSG_ReadCoord(), 0.001f);
	DOUBLES_EQUAL("HL coord 3 roundtrip", 3000.0f, MSG_ReadCoord(), 0.001f);
}

TEST(CoordClampedNotWrappedForHL, Proto, 1000)
{
	sizebuf_t *buf = &net_message;

	// 8192 units is 65536 in eighths, which does not fit a signed short. The
	// value must saturate, not wrap round to the far side of the map.
	Proto_ResetBuffer(buf, PROTO_DIALECT_HL);
	MSG_WriteCoord(buf, 8192.0f);
	MSG_BeginReading();
	float got = MSG_ReadCoord();
	CHECK("HL coord must clamp positive, not wrap", got > 0.0f);
	DOUBLES_EQUAL("HL coord clamps to +32767/8", 32767.0f / 8.0f, got, 0.001f);

	Proto_ResetBuffer(buf, PROTO_DIALECT_HL);
	MSG_WriteCoord(buf, -8192.0f);
	MSG_BeginReading();
	got = MSG_ReadCoord();
	CHECK("HL coord must clamp negative, not wrap", got < 0.0f);

	// Sven keeps the full value.
	Proto_ResetBuffer(buf, PROTO_DIALECT_SVEN);
	MSG_WriteCoord(buf, 8192.0f);
	MSG_BeginReading();
	DOUBLES_EQUAL("Sven coord keeps the full range", 8192.0f, MSG_ReadCoord(), 0.001f);
}

TEST(BitsProtoRoundtrip, Proto, 1000)
{
	sizebuf_t *buf = &net_message;

	// Every entry of the divergence table, written and read back through the
	// same stamp. The widths themselves come from the table, so this fails if
	// a call site and the table ever disagree about which is which.
	struct { int sven; int hl; const char *name; } widths[] = {
		{ PROTO_BITS_SVEN_DELTA_BYTECOUNT,    PROTO_BITS_HL_DELTA_BYTECOUNT,    "DELTA_BYTECOUNT" },
		{ PROTO_BITS_SVEN_ENTITY_NUMBER,      PROTO_BITS_HL_ENTITY_NUMBER,      "ENTITY_NUMBER" },
		{ PROTO_BITS_SVEN_SOUND_ENTITY,       PROTO_BITS_HL_SOUND_ENTITY,       "SOUND_ENTITY" },
		{ PROTO_BITS_SVEN_DELTA_SEQUENCE,     PROTO_BITS_HL_DELTA_SEQUENCE,     "DELTA_SEQUENCE" },
		{ PROTO_BITS_SVEN_WEAPON_INDEX,       PROTO_BITS_HL_WEAPON_INDEX,       "WEAPON_INDEX" },
		{ PROTO_BITS_SVEN_EVENT_INDEX,        PROTO_BITS_HL_EVENT_INDEX,        "EVENT_INDEX" },
		{ PROTO_BITS_SVEN_RESOURCE_INDEX,     PROTO_BITS_HL_RESOURCE_INDEX,     "RESOURCE_INDEX" },
		{ PROTO_BITS_SVEN_CONSISTENCY_INDEX,  PROTO_BITS_HL_CONSISTENCY_INDEX,  "CONSISTENCY_INDEX" },
		{ PROTO_BITS_SVEN_BITCOORD_INT,       PROTO_BITS_HL_BITCOORD_INT,       "BITCOORD_INT" },
	};

	for (int i = 0; i < (int)ARRAYSIZE(widths); i++)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			proto_dialect_t dialect = pass ? PROTO_DIALECT_HL : PROTO_DIALECT_SVEN;
			int bits = pass ? widths[i].hl : widths[i].sven;
			uint32 value = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);

			Proto_ResetBuffer(buf, dialect);
			MSG_StartBitWriting(buf);
			MSG_WriteBitsProto(value, widths[i].sven, widths[i].hl);
			MSG_EndBitWriting(buf);

			// The field must occupy exactly the advertised number of bits.
			int expectedBytes = (bits + 7) / 8;
			UINT32_EQUALS(widths[i].name, expectedBytes, buf->cursize);

			MSG_BeginReading();
			MSG_StartBitReading(buf);
			uint32 got = MSG_ReadBitsProto(widths[i].sven, widths[i].hl);
			MSG_EndBitReading(buf);

			UINT32_EQUALS(widths[i].name, value, got);
		}
	}
}

TEST(EventIndexIsNarrowerUnderSven, Proto, 1000)
{
	// Guarding the one entry that runs the other way. Every other widened
	// field grew under Sven; the event packet_index SHRANK, and folding it in
	// with the entity indices is a mistake that has already been made once and
	// cost a whole debugging session.
	CHECK("EVENT_INDEX must be narrower under Sven",
		PROTO_BITS_SVEN_EVENT_INDEX < PROTO_BITS_HL_EVENT_INDEX);

	CHECK("ENTITY_NUMBER must be wider under Sven",
		PROTO_BITS_SVEN_ENTITY_NUMBER > PROTO_BITS_HL_ENTITY_NUMBER);
}

TEST(HLDeltaFieldCapFitsTheBitmask, Proto, 1000)
{
	// A Half-Life client reads the bitmask length as 3 bits, so at most 7
	// bytes, so at most 56 fields. If these ever disagree the truncation in
	// _DELTA_WriteDelta will emit a mask the client cannot address.
	int maxBytes = (1 << PROTO_BITS_HL_DELTA_BYTECOUNT) - 1;
	UINT32_EQUALS("HL delta field cap must match the 3-bit byte count",
		PROTO_HL_MAX_DELTA_FIELDS, maxBytes * 8);

	int svenMaxBytes = (1 << PROTO_BITS_SVEN_DELTA_BYTECOUNT) - 1;
	CHECK("Sven's byte count must reach DELTA_MAX_FIELDS",
		svenMaxBytes * 8 >= DELTA_MAX_FIELDS - 8);
}

TEST(BitCoordRoundtripBothDialects, Proto, 1000)
{
	sizebuf_t *buf = &net_message;

	// The bit-packed encoding is a separate code path from the byte-aligned
	// one, and fixing only one of the two is the documented way to produce a
	// desync that takes a packet trace to find.
	//
	// Integral samples only, deliberately. MSG_WriteBitCoord computes its
	// fractional part as `abs((int32)f * 8) & 7` -- the cast binds before the
	// multiply, so the fraction is always zero and this encoding has never
	// carried sub-unit precision in either dialect. That is reversed-from-the-
	// binary behaviour shared by both engines, so it is not ours to change; the
	// samples just avoid asserting a precision the wire does not have.
	const float samples[] = { 0.0f, 1.0f, -1.0f, 512.0f, -512.0f, 4000.0f };

	for (int pass = 0; pass < 2; pass++)
	{
		proto_dialect_t dialect = pass ? PROTO_DIALECT_HL : PROTO_DIALECT_SVEN;

		for (int i = 0; i < (int)ARRAYSIZE(samples); i++)
		{
			// A Half-Life client's 12-bit integer part tops out at 4095.
			if (dialect == PROTO_DIALECT_HL && samples[i] >= 4095.0f)
				continue;

			Proto_ResetBuffer(buf, dialect);
			MSG_StartBitWriting(buf);
			MSG_WriteBitCoord(samples[i]);
			MSG_EndBitWriting(buf);

			MSG_BeginReading();
			MSG_StartBitReading(buf);
			float got = MSG_ReadBitCoord();
			MSG_EndBitReading(buf);

			DOUBLES_EQUAL("MSG_WriteBitCoord roundtrip", samples[i], got, 0.001f);
		}
	}
}

TEST(BufferStampIsStickyAcrossClear, Proto, 1000)
{
	sizebuf_t *buf = &net_message;

	// SZ_Clear only drops SIZEBUF_OVERFLOWED. If it ever started clearing the
	// whole flags word, every per-client buffer would silently revert to the
	// native encoding one frame after it was stamped.
	Proto_ResetBuffer(buf, PROTO_DIALECT_HL);
	MSG_WriteCoord(buf, 1.0f);
	SZ_Clear(buf);
	CHECK("SZ_Clear must preserve the dialect stamp", MSG_BufIsHL(buf));

	SV_Proto_StampBuffer(buf, PROTO_DIALECT_SVEN);
	CHECK("Stamping back to Sven must clear the flag", !MSG_BufIsHL(buf));
}

#endif // REHLDS_SVEN
