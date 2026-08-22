#include "precompiled.h"
#include "rehlds_tests_shared.h"

// Memory_Init() carves the main zone (ZONE_DYNAMIC_SIZE) out of this buffer via
// Hunk_AllocName(), so it must always be larger than the zone or the very first
// allocation Sys_Error()s. Derive it instead of hardcoding: REHLDS_SVEN raises
// ZONE_DYNAMIC_SIZE from 2 MB to 32 MB, which a fixed 4 MB buffer cannot satisfy.
// The +2 MB of headroom keeps the non-Sven size at the historical 4 MB.
static uint8 g_TestMemoryBuf[ZONE_DYNAMIC_SIZE + 1024 * 1024 * 2];

void Tests_InitEngine() {
	Memory_Init(g_TestMemoryBuf, sizeof(g_TestMemoryBuf));

	FR_Init();

#ifdef REHLDS_FLIGHT_REC
	FR_Rehlds_Init();
#endif //REHLDS_FLIGHT_REC

	Cbuf_Init();
	Cmd_Init();
	Cvar_Init();
	Cvar_CmdInit();
}

void Tests_ShutdownEngine() {
	Cvar_Shutdown();
	Cmd_Shutdown();

	mainzone = NULL;
	hunk_base = NULL;
	hunk_size = 0;

	FR_Shutdown();

	SV_Shutdown();
}
