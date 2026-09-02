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

void CSteam3Server::OnGSPolicyResponse(GSPolicyResponse_t *pPolicyResponse)
{
	if (CRehldsPlatformHolder::get()->SteamGameServer()->BSecure())
		Con_Printf("   VAC secure mode is activated.\n");
	else
		Con_Printf("   VAC secure mode disabled.\n");
}

void CSteam3Server::OnLogonSuccess(SteamServersConnected_t *pLogonSuccess)
{
	if (m_bLogOnResult)
	{
		if (!m_bLanOnly)
			Con_Printf("Reconnected to Steam servers.\n");
	}
	else
	{
		m_bLogOnResult = true;
		if (!m_bLanOnly)
			Con_Printf("Connection to Steam servers successful.\n");
	}

	m_SteamIDGS = CRehldsPlatformHolder::get()->SteamGameServer()->GetSteamID();
#ifdef REHLDS_SVEN
	// Publish it for plugins. This is the ONLY place the engine learns its own Steam ID,
	// and it is asynchronous -- it happens on logon, well after plugins have loaded -- so a
	// plugin reading it at init would always see the default. Anything consuming this must
	// read it late and tolerate "0".
	{
		char szSteamId[32];
		Q_snprintf(szSteamId, sizeof(szSteamId), "%llu", (unsigned long long)m_SteamIDGS.ConvertToUint64());
		Cvar_Set("sv_steamid", szSteamId);
	}
#endif
	CSteam3Server::SendUpdatedServerDetails();
}

uint64 CSteam3Server::GetSteamID()
{
	if (m_bLanOnly)
		return CSteamID(0, k_EUniversePublic, k_EAccountTypeInvalid).ConvertToUint64();
	else
		return m_SteamIDGS.ConvertToUint64();
}

void CSteam3Server::OnLogonFailure(SteamServerConnectFailure_t *pLogonFailure)
{
	if (!m_bLogOnResult)
	{
		if (pLogonFailure->m_eResult == k_EResultServiceUnavailable)
		{
			if (!m_bLanOnly)
			{
				Con_Printf("Connection to Steam servers successful (SU).\n");
				if (m_bWantToBeSecure)
				{
					Con_Printf("   VAC secure mode not available.\n");
					m_bLogOnResult = true;
					return;
				}
			}
		}
		else
		{
			if (!m_bLanOnly)
				Con_Printf("Could not establish connection to Steam servers.\n");
		}
	}

	m_bLogOnResult = true;
}

void CSteam3Server::OnGSClientDeny(GSClientDeny_t *pGSClientDeny)
{
	client_t* cl = CSteam3Server::ClientFindFromSteamID(pGSClientDeny->m_SteamID);
	if (cl)
		OnGSClientDenyHelper(cl, pGSClientDeny->m_eDenyReason, pGSClientDeny->m_rgchOptionalText);
}

#ifdef REHLDS_SVEN
/*
=================
SV_Steam3DenyReasonName

The deny reason as a NAME, because the numeric EDenyReason is what actually distinguishes the
twelve cases below and none of the drop messages carries it.

⚠ This exists because "Unable to connect to Steam" is a MESSAGE, not a diagnosis. It is the text
for exactly one reason (k_EDenySteamConnectionError), it is emitted for both GSClientDeny AND
GSClientKick, and nothing in the log said which fired or what Steam actually reported. Measured
2026-08-19 on the self-hosted Sven server: a legitimate retail Steam client
(":P<1><STEAM_0:1:36684470>") connected, played through a level change, and was dropped ~100s in
with that message and no other trace — on a server whose own log a minute earlier said
"Connection to Steam servers successful." and "VAC secure mode is activated."
=================
*/
static const char *SV_Steam3DenyReasonName(EDenyReason eDenyReason)
{
	switch (eDenyReason)
	{
	case k_EDenyInvalidVersion:        return "InvalidVersion";
	case k_EDenyGeneric:               return "Generic";
	case k_EDenyNotLoggedOn:           return "NotLoggedOn";
	case k_EDenyNoLicense:             return "NoLicense";
	case k_EDenyCheater:               return "Cheater";
	case k_EDenyLoggedInElseWhere:     return "LoggedInElseWhere";
	case k_EDenyUnknownText:           return "UnknownText";
	case k_EDenyIncompatibleAnticheat: return "IncompatibleAnticheat";
	case k_EDenyMemoryCorruption:      return "MemoryCorruption";
	case k_EDenyIncompatibleSoftware:  return "IncompatibleSoftware";
	case k_EDenySteamConnectionLost:   return "SteamConnectionLost";
	case k_EDenySteamConnectionError:  return "SteamConnectionError";
	case k_EDenySteamResponseTimedOut: return "SteamResponseTimedOut";
	case k_EDenySteamValidationStalled: return "SteamValidationStalled";
	case k_EDenySteamOwnerLeftGuestUser: return "SteamOwnerLeftGuestUser";
	case k_EDenyInvalid:               return "Invalid";
	default:                           return "unknown";
	}
}

/*
=================
SV_Steam3DenyIsTransient

Is this deny a STEAM CONNECTIVITY problem rather than a statement about the player?

The distinction is the whole point. Steam reports two different kinds of thing through one
callback: "this account is banned / does not own the game / is elsewhere" (a verdict about the
CLIENT) and "I could not reach Steam to check" (a verdict about the SERVER'S link to Steam). The
first must stand. The second must not cost a playing user their session on a server that does not
derive authority from Steam in the first place.

⚠ THIS SERVER RUNS ReUnion. Non-Steam clients are accepted BY DESIGN — that is the entire reason
the self-hosted Sven server exists (deploy/svencoop-server/README.md). Identity comes from
ReUnion, not from Steam. So a Steam-side connectivity failure is, for us, information — not
grounds to eject somebody mid-game while every non-Steam player beside them keeps playing. Left
as it was, a legitimate Steam owner gets a WORSE experience on this server than a non-Steam
client, which is precisely backwards.

⚠ NOT in this list, deliberately, and do not add them: Cheater (VAC), NoLicense, InvalidVersion,
IncompatibleAnticheat, IncompatibleSoftware, MemoryCorruption. Those are verdicts about the
client and are still enforced in full.
=================
*/
static bool SV_Steam3DenyIsTransient(EDenyReason eDenyReason)
{
	switch (eDenyReason)
	{
	case k_EDenySteamConnectionError:
	case k_EDenySteamConnectionLost:
	case k_EDenySteamResponseTimedOut:
	case k_EDenyNotLoggedOn:
		return true;
	default:
		return false;
	}
}
#endif // REHLDS_SVEN

void CSteam3Server::OnGSClientDenyHelper(client_t *cl, EDenyReason eDenyReason, const char *pchOptionalText)
{
#ifdef REHLDS_SVEN
	// One line that says everything the old log did not: which reason, numerically and by name,
	// for whom, and with whatever text Steam attached. Print it for EVERY deny, kept or not.
	Con_Printf("[steam3] client deny: reason=%d (%s) client=\"%s\" id=%s%s%s\n",
		(int)eDenyReason, SV_Steam3DenyReasonName(eDenyReason),
		cl ? cl->name : "<none>",
		cl ? SV_GetClientIDString(cl) : "<none>",
		(pchOptionalText && *pchOptionalText) ? " text=" : "",
		(pchOptionalText && *pchOptionalText) ? pchOptionalText : "");

	if (cl && sv_rehlds_sven_tolerate_steam_deny.value > 0.f && SV_Steam3DenyIsTransient(eDenyReason))
	{
		// Keep the client. See SV_Steam3DenyIsTransient for why this is correct on a ReUnion
		// server and why the client-verdict reasons are deliberately excluded from it.
		Con_Printf("[steam3] keeping \"%s\" connected: %s is a Steam-side connectivity result, not a "
			"verdict about the player, and this server authenticates through ReUnion "
			"(sv_rehlds_sven_tolerate_steam_deny 0 to restore the drop)\n",
			cl->name, SV_Steam3DenyReasonName(eDenyReason));
		return;
	}
#endif // REHLDS_SVEN

	switch (eDenyReason)
	{
	case k_EDenyInvalidVersion:
		SV_DropClient(cl, 0, "Client version incompatible with server. \nPlease exit and restart");
		break;

	case k_EDenyNotLoggedOn:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "No Steam logon\n");
		break;

	case k_EDenyLoggedInElseWhere:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "This Steam account is being used in another location\n");
		break;

	case k_EDenyNoLicense:
		SV_DropClient(cl, 0, "This Steam account does not own this game. \nPlease login to the correct Steam account.");
		break;

	case k_EDenyCheater:
		SV_DropClient(cl, 0, "VAC banned from secure server\n");
		break;

	case k_EDenyUnknownText:
		if (pchOptionalText && *pchOptionalText)
			SV_DropClient(cl, 0, pchOptionalText);
		else
			SV_DropClient(cl, 0, "Client dropped by server");
		break;

	case k_EDenyIncompatibleAnticheat:
		SV_DropClient(cl, 0, "You are running an external tool that is incompatible with Secure servers.");
		break;

	case k_EDenyMemoryCorruption:
		SV_DropClient(cl, 0, "Memory corruption detected.");
		break;

	case k_EDenyIncompatibleSoftware:
		SV_DropClient(cl, 0, "You are running software that is not compatible with Secure servers.");
		break;

	case k_EDenySteamConnectionLost:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "Steam connection lost\n");
		break;

	case k_EDenySteamConnectionError:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "Unable to connect to Steam\n");
		break;

	case k_EDenySteamResponseTimedOut:
		SV_DropClient(cl, 0, "Client timed out while answering challenge.\n---> Please make sure that you have opened the appropriate ports on any firewall you are connected behind.\n---> See http://support.steampowered.com for help with firewall configuration.");
		break;

	case k_EDenySteamValidationStalled:
		if (m_bLanOnly)
			cl->network_userid.m_SteamID = 1;
		break;

	default:
		SV_DropClient(cl, 0, "Client dropped by server");
		break;
	}
}

void CSteam3Server::OnGSClientApprove(GSClientApprove_t *pGSClientSteam2Accept)
{
	client_t* cl = ClientFindFromSteamID(pGSClientSteam2Accept->m_SteamID);
	if (!cl)
		return;

	if (SV_FilterUser(&cl->network_userid))
	{
		char msg[256];
		Q_sprintf(msg, "You have been banned from this server\n");
		SV_RejectConnection(&cl->netchan.remote_address, msg);
		SV_DropClient(cl, 0, "STEAM UserID %s is in server ban list\n", SV_GetClientIDString(cl));
	}
	else if (SV_CheckForDuplicateSteamID(cl) != -1)
	{
		char msg[256];
		Q_sprintf(msg, "Your UserID is already in use on this server.\n");
		SV_RejectConnection(&cl->netchan.remote_address, msg);
		SV_DropClient(cl, 0, "STEAM UserID %s is already\nin use on this server\n", SV_GetClientIDString(cl));
	}
	else
	{
		char msg[512];
		Q_snprintf(msg, ARRAYSIZE(msg), "\"%s<%i><%s><>\" STEAM USERID validated\n", cl->name, cl->userid, SV_GetClientIDString(cl));
#ifdef REHLDS_CHECKS
		msg[ARRAYSIZE(msg) - 1] = 0;
#endif
		Con_DPrintf("%s", msg);
		Log_Printf("%s", msg);
	}
}

void CSteam3Server::OnGSClientKick(GSClientKick_t *pGSClientKick)
{
	client_t* cl = CSteam3Server::ClientFindFromSteamID(pGSClientKick->m_SteamID);
	if (cl)
		CSteam3Server::OnGSClientDenyHelper(cl, pGSClientKick->m_eDenyReason, 0);
}

client_t *CSteam3Server::ClientFindFromSteamID(CSteamID &steamIDFind)
{
	for (int i = 0; i < g_psvs.maxclients; i++)
	{
		auto cl = &g_psvs.clients[i];
		if (!cl->connected && !cl->active && !cl->spawned)
			continue;

		if (cl->network_userid.idtype != AUTH_IDTYPE_STEAM)
			continue;

		if (steamIDFind == cl->network_userid.m_SteamID)
			return cl;
	}

	return NULL;
}

CSteam3Server::CSteam3Server() :
	m_CallbackGSClientApprove(this, &CSteam3Server::OnGSClientApprove),
	m_CallbackGSClientDeny(this, &CSteam3Server::OnGSClientDeny),
	m_CallbackGSClientKick(this, &CSteam3Server::OnGSClientKick),
	m_CallbackGSPolicyResponse(this, &CSteam3Server::OnGSPolicyResponse),
	m_CallbackLogonSuccess(this, &CSteam3Server::OnLogonSuccess),
	m_CallbackLogonFailure(this, &CSteam3Server::OnLogonFailure),
	m_SteamIDGS(1, 0, k_EUniverseInvalid, k_EAccountTypeInvalid)
{
#ifdef REHLDS_FIXES
	m_GameTagsData[0] = '\0';
#endif

	m_bHasActivePlayers = false;
	m_bWantToBeSecure = false;
	m_bLanOnly = false;
}

void CSteam3Server::Activate()
{
	bool bLanOnly;
	int argSteamPort;
	EServerMode eSMode;
	int gamePort;
	char gamedir[MAX_PATH];
	int usSteamPort;
	uint32 unIP;

	if (m_bLoggedOn)
	{
		bLanOnly = sv_lan.value != 0.0;
		if (m_bLanOnly != bLanOnly)
		{
			m_bLanOnly = bLanOnly;
			m_bWantToBeSecure = !COM_CheckParm("-insecure") && !bLanOnly;
		}
	}
	else
	{
		m_bLoggedOn = true;
		unIP = 0;
		usSteamPort = 26900;
		argSteamPort = COM_CheckParm("-sport");
		if (argSteamPort > 0)
			usSteamPort = Q_atoi(com_argv[argSteamPort + 1]);
		eSMode = eServerModeAuthenticationAndSecure;
		if (net_local_adr.type == NA_IP)
			unIP = ntohl(*(u_long *)&net_local_adr.ip[0]);

		m_bLanOnly = sv_lan.value > 0.0;
		m_bWantToBeSecure = !COM_CheckParm("-insecure") && !m_bLanOnly;
		COM_FileBase(com_gamedir, gamedir);

		if (!m_bWantToBeSecure)
			eSMode = eServerModeAuthentication;

		if (m_bLanOnly)
			eSMode = eServerModeNoAuthentication;

		gamePort = (int)iphostport.value;
		if (gamePort == 0)
			gamePort = (int)hostport.value;

		int nAppId = GetGameAppID();
		if (nAppId > 0 && g_pcls.state == ca_dedicated)
		{
			FILE* f = fopen("steam_appid.txt", "w+");
			if (f)
			{
				fprintf(f, "%d\n", nAppId);
				fclose(f);
			}
		}

		if (!CRehldsPlatformHolder::get()->SteamGameServer_Init(unIP, usSteamPort, gamePort, 0xFFFFu, eSMode, gpszVersionString))
			Sys_Error("Unable to initialize Steam.");

		CRehldsPlatformHolder::get()->SteamGameServer()->SetProduct(gpszProductString);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetModDir(gamedir);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetDedicatedServer(g_pcls.state == ca_dedicated);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetGameDescription(gEntityInterface.pfnGetGameDescription());
		CRehldsPlatformHolder::get()->SteamGameServer()->LogOnAnonymous();
		m_bLogOnResult = false;

		if (COM_CheckParm("-nomaster"))
		{
			Con_Printf("Master server communication disabled.\n");
			gfNoMasterServer = TRUE;
		}
		else
		{
			if (!gfNoMasterServer && g_psvs.maxclients > 1)
			{
				CRehldsPlatformHolder::get()->SteamGameServer()->SetAdvertiseServerActive(true);
				double fMasterHeartbeatTimeout = 200.0;
				if (!Q_strcmp(gamedir, "dmc"))
					fMasterHeartbeatTimeout = 150.0;
				if (!Q_strcmp(gamedir, "tfc"))
					fMasterHeartbeatTimeout = 400.0;
				if (!Q_strcmp(gamedir, "cstrike"))
					fMasterHeartbeatTimeout = 400.0;

				CRehldsPlatformHolder::get()->SteamGameServer()->SetMasterServerHeartbeatInterval((int)fMasterHeartbeatTimeout);
				CSteam3Server::NotifyOfLevelChange(true);
			}
		}
	}
}

void CSteam3Server::Shutdown()
{
	if (m_bLoggedOn)
	{
		SteamGameServer()->SetAdvertiseServerActive(false);
		SteamGameServer()->LogOff();

		SteamGameServer_Shutdown();
		m_bLoggedOn = false;
	}
}

bool CSteam3Server::NotifyClientConnect(client_t *client, const void *pvSteam2Key, uint32 ucbSteam2Key)
{
	class CSteamID steamIDClient;
	bool bRet = false;

	if (client == NULL || !m_bLoggedOn)
		return false;

	client->network_userid.idtype = AUTH_IDTYPE_STEAM;

	bRet = CRehldsPlatformHolder::get()->SteamGameServer()->SendUserConnectAndAuthenticate(htonl(client->network_userid.clientip), pvSteam2Key, ucbSteam2Key, &steamIDClient);
	client->network_userid.m_SteamID = steamIDClient.ConvertToUint64();

	return bRet;
}

bool CSteam3Server::NotifyBotConnect(client_t *client)
{
	if (client == NULL || !m_bLoggedOn)
		return false;

	client->network_userid.idtype = AUTH_IDTYPE_LOCAL;
	CSteamID steamId = CRehldsPlatformHolder::get()->SteamGameServer()->CreateUnauthenticatedUserConnection();
	client->network_userid.m_SteamID = steamId.ConvertToUint64();
	return true;
}

void CSteam3Server::NotifyClientDisconnect(client_t *cl)
{
	if (!cl || !m_bLoggedOn)
		return;

	if (cl->network_userid.idtype == AUTH_IDTYPE_STEAM || cl->network_userid.idtype == AUTH_IDTYPE_LOCAL)
	{
		CRehldsPlatformHolder::get()->SteamGameServer()->SendUserDisconnect(cl->network_userid.m_SteamID);
	}
}

void CSteam3Server::NotifyOfLevelChange(bool bForce)
{
	SendUpdatedServerDetails();
	bool iHasPW = (sv_password.string[0] && Q_stricmp(sv_password.string, "none"));
	CRehldsPlatformHolder::get()->SteamGameServer()->SetPasswordProtected(iHasPW);
	CRehldsPlatformHolder::get()->SteamGameServer()->ClearAllKeyValues();

	for (cvar_t *var = cvar_vars; var; var = var->next)
	{
		if (!(var->flags & FCVAR_SERVER))
			continue;

		const char *szVal;
		if (var->flags & FCVAR_PROTECTED)
		{
			if (Q_strlen(var->string) > 0 && Q_stricmp(var->string, "none"))
				szVal = "1";
			else
				szVal = "0";
		}
		else
		{
			szVal = var->string;
		}
		CRehldsPlatformHolder::get()->SteamGameServer()->SetKeyValue(var->name, szVal);
	}
}

void CSteam3Server::RunFrame()
{
	bool bHasPlayers;
	char szOutBuf[4096];
	double fCurTime;

	static double s_fLastRunFragsUpdate;
	static double s_fLastRunCallback;
	static double s_fLastRunSendPackets;

	if (g_psvs.maxclients <= 1)
		return;

	fCurTime = Sys_FloatTime();
	if (fCurTime - s_fLastRunFragsUpdate > 1.0)
	{
		s_fLastRunFragsUpdate = fCurTime;
		bHasPlayers = false;
		for (int i = 0; i < g_psvs.maxclients; i++)
		{
			client_t* cl = &g_psvs.clients[i];
			if (cl->active || cl->spawned || cl->connected)
			{
				bHasPlayers = true;
				break;
			}
		}

		m_bHasActivePlayers = bHasPlayers;
		SendUpdatedServerDetails();
		bool iHasPW = (sv_password.string[0] && Q_stricmp(sv_password.string, "none"));
		CRehldsPlatformHolder::get()->SteamGameServer()->SetPasswordProtected(iHasPW);

#ifdef REHLDS_FIXES
		// Let's get it an up-to-date description of the game
		CRehldsPlatformHolder::get()->SteamGameServer()->SetGameDescription(gEntityInterface.pfnGetGameDescription());
#endif

		for (int i = 0; i < g_psvs.maxclients; i++)
		{
			client_t* cl = &g_psvs.clients[i];
			if (!cl->active)
				continue;

#ifdef REHLDS_FIXES
			ISteamGameServer_BUpdateUserData(cl->network_userid.m_SteamID, cl->name, cl->edict->v.frags);
#else
			CRehldsPlatformHolder::get()->SteamGameServer()->BUpdateUserData(cl->network_userid.m_SteamID, cl->name, cl->edict->v.frags);
#endif
		}

		if (CRehldsPlatformHolder::get()->SteamGameServer()->WasRestartRequested())
		{
			Con_Printf("%cMasterRequestRestart\n", 3);
			if (COM_CheckParm("-steam"))
			{
				Con_Printf("Your server needs to be restarted in order to receive the latest update.\n");
				Log_Printf("Your server needs to be restarted in order to receive the latest update.\n");
			}
			else
			{
				Con_Printf("Your server is out of date.  Please update and restart.\n");
			}
		}
	}

	if (fCurTime - s_fLastRunCallback > 0.1)
	{
		CRehldsPlatformHolder::get()->SteamGameServer_RunCallbacks();
		s_fLastRunCallback = fCurTime;
	}

	if (fCurTime - s_fLastRunSendPackets > 0.01)
	{
		s_fLastRunSendPackets = fCurTime;

		uint16 port;
		uint32 ip;
		int iLen = CRehldsPlatformHolder::get()->SteamGameServer()->GetNextOutgoingPacket(szOutBuf, sizeof(szOutBuf), &ip, &port);
		while (iLen > 0)
		{
			netadr_t netAdr;
			*((uint32*)&netAdr.ip[0]) = htonl(ip);
			netAdr.port = htons(port);
			netAdr.type = NA_IP;

			NET_SendPacket(NS_SERVER, iLen, szOutBuf, netAdr);

			iLen = CRehldsPlatformHolder::get()->SteamGameServer()->GetNextOutgoingPacket(szOutBuf, sizeof(szOutBuf), &ip, &port);
		}
	}
}

void CSteam3Server::UpdateGameTags()
{
#ifdef REHLDS_FIXES
	if (!m_GameTagsData[0] && !sv_tags.string[0])
		return;

	if (m_GameTagsData[0] && !Q_stricmp(m_GameTagsData, sv_tags.string))
		return;

	Q_strlcpy(m_GameTagsData, sv_tags.string);
	Q_strlwr(m_GameTagsData);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetGameTags(m_GameTagsData);
#endif
}

void CSteam3Server::SendUpdatedServerDetails()
{
	int botCount = 0;
	if (g_psvs.maxclients > 0)
	{

		for (int i = 0; i < g_psvs.maxclients; i++)
		{
			auto cl = &g_psvs.clients[i];
			if ((cl->active || cl->spawned || cl->connected) && cl->fakeclient)
				++botCount;
		}
	}

	int maxPlayers = sv_visiblemaxplayers.value;
	if (maxPlayers < 0)
		maxPlayers = g_psvs.maxclients;

	CRehldsPlatformHolder::get()->SteamGameServer()->SetMaxPlayerCount(maxPlayers);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetBotPlayerCount(botCount);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetServerName(Cvar_VariableString("hostname"));
	CRehldsPlatformHolder::get()->SteamGameServer()->SetMapName(g_psv.name);

	UpdateGameTags();
}

void CSteam3Client::Shutdown()
{
	if (m_bLoggedOn)
	{
		SteamAPI_Shutdown();
		m_bLoggedOn = false;
	}
}

int CSteam3Client::InitiateGameConnection(void *pData, int cbMaxData, uint64 steamID, uint32 unIPServer, uint16 usPortServer, bool bSecure)
{
	return SteamUser()->InitiateGameConnection(pData, cbMaxData, CSteamID(steamID), ntohl(unIPServer), ntohs(usPortServer), bSecure);
}

void CSteam3Client::TerminateConnection(uint32 unIPServer, uint16 usPortServer)
{
	SteamUser()->TerminateGameConnection(ntohl(unIPServer), ntohs(usPortServer));
}

void CSteam3Client::InitClient()
{
	if (m_bLoggedOn)
		return;

	m_bLoggedOn = true;
	_unlink("steam_appid.txt");
	if (!getenv("SteamAppId"))
	{
		int nAppID = GetGameAppID();
		if (nAppID > 0)
		{
			FILE* f = fopen("steam_appid.txt", "w+");
			if (f)
			{
				fprintf(f, "%d\n", nAppID);
				fclose(f);
			}
		}
	}

	if (!SteamAPI_Init())
		Sys_Error("Failed to initalize authentication interface. Exiting...\n");

	m_bLogOnResult = false;
}

void CSteam3Client::OnClientGameServerDeny(ClientGameServerDeny_t *pClientGameServerDeny)
{
	COM_ExplainDisconnection(TRUE, "Invalid server version, unable to connect.");
	CL_Disconnect();
}

void CSteam3Client::OnGameServerChangeRequested(GameServerChangeRequested_t *pGameServerChangeRequested)
{
#ifndef SWDS
	char *cmd;

	Cvar_DirectSet(&password, pGameServerChangeRequested->m_rgchPassword);
	Con_Printf("Connecting to %s\n", pGameServerChangeRequested->m_rgchServer);
	cmd = va("connect %s\n", pGameServerChangeRequested->m_rgchServer);
	Cbuf_AddText(cmd);
#endif
}

void CSteam3Client::OnGameOverlayActivated(GameOverlayActivated_t *pGameOverlayActivated)
{
#ifndef SWDS
	if (Host_IsSinglePlayerGame())
	{
		if (pGameOverlayActivated->m_bActive)
		{
			Cbuf_AddText("setpause;");
		}
		else
		{
			if (!(unsigned __int8)(*(int(**)())(*(_DWORD *)g_pGameUI007 + 44))())
			{
				Cbuf_AddText("unpause;");
			}
		}
	}
#endif
}

void CSteam3Client::RunFrame()
{
	CRehldsPlatformHolder::get()->SteamAPI_RunCallbacks();
}

uint64 ISteamGameServer_CreateUnauthenticatedUserConnection()
{
	if (!CRehldsPlatformHolder::get()->SteamGameServer())
	{
		return 0L;
	}

	return CRehldsPlatformHolder::get()->SteamGameServer()->CreateUnauthenticatedUserConnection().ConvertToUint64();
}

bool Steam_GSBUpdateUserData(uint64 steamIDUser, const char *pchPlayerName, uint32 uScore)
{
	return CRehldsPlatformHolder::get()->SteamGameServer()->BUpdateUserData(steamIDUser, pchPlayerName, uScore);
}

bool ISteamGameServer_BUpdateUserData(uint64 steamid, const char *netname, uint32 score)
{
	if (!CRehldsPlatformHolder::get()->SteamGameServer())
	{
		return false;
	}

	return g_RehldsHookchains.m_Steam_GSBUpdateUserData.callChain(Steam_GSBUpdateUserData, steamid, netname, score);
}

bool ISteamApps_BIsSubscribedApp(uint32 appid)
{
	if (CRehldsPlatformHolder::get()->SteamApps())
	{
		ISteamApps* apps = CRehldsPlatformHolder::get()->SteamApps();
		return apps->BIsSubscribedApp(appid);
	}

	return false;
}

const char *Steam_GetCommunityName()
{
	if (SteamFriends())
		return SteamFriends()->GetPersonaName();

	return NULL;
}

qboolean EXT_FUNC Steam_NotifyClientConnect_api(IGameClient *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	return Steam_NotifyClientConnect_internal(cl->GetClient(), pvSteam2Key, ucbSteam2Key);
}

qboolean Steam_NotifyClientConnect(client_t *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	return g_RehldsHookchains.m_Steam_NotifyClientConnect
		.callChain(Steam_NotifyClientConnect_api, GetRehldsApiClient(cl), pvSteam2Key, ucbSteam2Key);
}

qboolean Steam_NotifyClientConnect_internal(client_t *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	if (Steam3Server())
	{
		return Steam3Server()->NotifyClientConnect(cl, pvSteam2Key, ucbSteam2Key);
	}
	return FALSE;
}

qboolean EXT_FUNC Steam_NotifyBotConnect_api(IGameClient* cl)
{
	return Steam_NotifyBotConnect_internal(cl->GetClient());
}

qboolean Steam_NotifyBotConnect(client_t *cl)
{
	return g_RehldsHookchains.m_Steam_NotifyBotConnect.callChain(Steam_NotifyBotConnect_api, GetRehldsApiClient(cl));
}

qboolean Steam_NotifyBotConnect_internal(client_t *cl)
{
	if (Steam3Server())
	{
		return Steam3Server()->NotifyBotConnect(cl);
	}
	return FALSE;
}

void EXT_FUNC Steam_NotifyClientDisconnect_api(IGameClient* cl)
{
	g_RehldsHookchains.m_Steam_NotifyClientDisconnect.callChain(Steam_NotifyClientDisconnect_internal, cl);
}

void Steam_NotifyClientDisconnect(client_t *cl)
{
	Steam_NotifyClientDisconnect_api(GetRehldsApiClient(cl));
}

void Steam_NotifyClientDisconnect_internal(IGameClient* cl)
{
	if (Steam3Server())
	{
		Steam3Server()->NotifyClientDisconnect(cl->GetClient());
	}
}

void Steam_NotifyOfLevelChange()
{
	if (Steam3Server())
	{
		Steam3Server()->NotifyOfLevelChange(false);
	}
}

void Steam_Shutdown()
{
	if (Steam3Server())
	{
		Steam3Server()->Shutdown();
		delete s_Steam3Server;
		s_Steam3Server = NULL;
	}
}

void Steam_Activate()
{
	if (!Steam3Server())
	{
		s_Steam3Server = new CSteam3Server();
		if (s_Steam3Server == NULL)
			return;
	}

	Steam3Server()->Activate();
}

#ifdef REHLDS_SVEN
// ===========================================================================
// Half-Life master-list beacon (sv_hl_beacon, default OFF)
//
// WHAT IT IS
//   A SECOND Steam game-server registration, from this same process, under a
//   different appid -- so one server appears in two master lists at once. We
//   list under Sven Co-op (225840) as ourselves, and additionally under the
//   Half-Life family (70, gamedir "valve") so stock Half-Life clients can find
//   us. They can already JOIN: the per-client dialect probe and
//   sv_proto_hl_gamedir handle a stock GoldSrc client end to end. Only
//   DISCOVERY was missing, and discovery is keyed by appid.
//
// WHY IT IS POSSIBLE AT ALL
//   SteamGameServer_Init() takes no appid -- it reads steam_appid.txt, which is
//   process-global, and that is where "one process, one appid" comes from. But
//   that helper is only a wrapper: the interface underneath,
//       ISteamGameServer::InitGameServer(ip, gamePort, queryPort, flags,
//                                        AppId_t nGameAppId, version)
//   takes the appid as a PARAMETER. ISteamClient::CreateLocalUser() hands out an
//   independent game-server user, so a second registration is a second local
//   user plus a second InitGameServer with a different appid. No child process,
//   no relay, no second game server.
//
// WHY THE PORTS ARE SPLIT
//   Steam stores a server's connection port and query port separately
//   (servernetadr_t::m_usConnectionPort / m_usQueryPort), and a client connects
//   to the CONNECTION port. So the beacon answers queries on its own port while
//   advertising the REAL game port -- a Half-Life client finds it under "valve"
//   and then connects straight to the real server. Nothing is proxied and the
//   player pool is not split.
//
//   Passing a real query port (not MASTERSERVERUPDATERPORT_USEGAMESOCKETSHARE)
//   means steamclient opens that socket and answers A2S itself from the details
//   set below. That is why there is no A2S code here.
//
// ** KNOWN LIMITATION, AND IT IS VISIBLE TO PLAYERS **
//   The beacon has no connected users of its own, so the Half-Life listing
//   reports 0 players however busy the real server is. Player counts on the
//   Steam side come from BUpdateUserData for users of THAT registration, and
//   the real players belong to the other one. Reporting them as bots would fill
//   the number in with a lie, so it is left alone. Max players, hostname and map
//   are all real.
//
// ** UNPROVEN IN THE WILD **
//   Of 1483 appid-70 servers, only 26 advertise a query port different from
//   their game port, and all 26 are HLTV proxies with gameport 0. The data model
//   supports what this does; no ordinary server exercises it. If a Half-Life
//   client turns out to connect to the query port instead, that is the thing to
//   look at first.
// ===========================================================================

// Steamworks EServerFlags. This SDK snapshot does not carry them, so they are
// restated rather than guessed at the call site.
#define HLB_FLAG_ACTIVE     0x01
#define HLB_FLAG_SECURE     0x02
#define HLB_FLAG_DEDICATED  0x04
#define HLB_FLAG_LINUX      0x08

// steamclient opens and owns the beacon's query socket, but it only services it when the
// owning pipe is pumped -- and SteamGameServer_RunCallbacks() pumps only the pipe
// SteamGameServer_GetHSteamPipe() names, not ours. Measured on the first deployment: the
// socket was bound (ss showed hlds_linux holding udp/27016) and queries piled up unread,
// Recv-Q 1280 and climbing, so the server was listed and then silently unanswerable.
//
// ISteamClient::RunFrame() is the only pump that reaches another pipe. Manual dispatch is
// NOT an option -- the SDK forbids mixing SteamAPI_ManualDispatch_* with
// SteamGameServer_RunCallbacks and STEAM_CALLBACK members, and CSteam3Server is built on
// both, so switching would break client approve/deny. STEAM_PRIVATE_API only makes
// RunFrame `protected`; a derived type republishes it. Same object, same vtable entry,
// nothing but the C++ access check changes.
struct CHLBeaconClientAccess : public ISteamClient { using ISteamClient::RunFrame; };

static HSteamPipe        g_hHLBeaconPipe = 0;
static double            g_fHLBeaconNextPump = 0.0;
static bool              g_bHLBeaconLoggedOn = false;
static double            g_fHLBeaconStarted  = 0.0;
static bool              g_bHLBeaconWarned   = false;
static HSteamUser        g_hHLBeaconUser = 0;
static ISteamGameServer *g_pHLBeaconGS   = NULL;
static char              g_szHLBeaconMap[64];
static double            g_fHLBeaconNextUpdate = 0.0;

static void HLBeacon_Stop()
{
	if (!g_hHLBeaconPipe)
		return;

	if (g_pHLBeaconGS)
	{
		g_pHLBeaconGS->LogOff();
		g_pHLBeaconGS = NULL;
	}

	ISteamClient *pClient = SteamGameServerClient();
	if (pClient)
	{
		if (g_hHLBeaconUser)
			pClient->ReleaseUser(g_hHLBeaconPipe, g_hHLBeaconUser);
		pClient->BReleaseSteamPipe(g_hHLBeaconPipe);
	}

	g_hHLBeaconPipe = 0;
	g_hHLBeaconUser = 0;
	g_szHLBeaconMap[0] = 0;
	Con_Printf("[hl-beacon] stopped\n");
}

static void HLBeacon_Start()
{
	ISteamClient *pClient = SteamGameServerClient();
	if (!pClient)
		return;   // steamclient not up yet; try again next frame

	int queryPort = (int)sv_hl_beacon_port.value;
	int gamePort  = (int)iphostport.value;
	if (gamePort == 0)
		gamePort = (int)hostport.value;

	// A beacon that answers on the game port would fight the engine for the socket,
	// and one on port 0 would never be reachable. Refuse instead of half-starting.
	if (queryPort <= 0 || queryPort > 65535 || queryPort == gamePort)
	{
		Con_Printf("[hl-beacon] refusing to start: sv_hl_beacon_port %d is invalid or equals the game port %d\n", queryPort, gamePort);
		Cvar_SetValue("sv_hl_beacon", 0.0f);
		return;
	}

	uint32 unIP = 0;
	if (net_local_adr.type == NA_IP)
		unIP = ntohl(*(u_long *)&net_local_adr.ip[0]);

	// A second game-server user needs its own local binding. The primary gets one from
	// SteamGameServer_Init's usSteamPort (26900 by default, -sport overrides);
	// InitGameServer has no equivalent parameter, so without this the beacon has no
	// distinct address to reach the Steam CM from -- it registers, opens its query socket,
	// and then never logs on, which is exactly what sven8/sven9 did.
	//
	// The header is explicit that this must happen BEFORE CreateLocalUser(), so it cannot
	// be folded in with the InitGameServer call below.
	SteamIPAddress_t bindAddr;
	Q_memset(&bindAddr, 0, sizeof(bindAddr));
	bindAddr.m_eType = k_ESteamIPTypeIPv4;
	bindAddr.m_unIPv4 = unIP;
	pClient->SetLocalIPBinding(bindAddr, (uint16)(int)sv_hl_beacon_sport.value);

	HSteamPipe hPipe = 0;
	HSteamUser hUser = pClient->CreateLocalUser(&hPipe, k_EAccountTypeGameServer);

	// Put the binding back immediately. It is a STORED setting on ISteamClient consumed by
	// the NEXT CreateLocalUser(), so leaving ours in place would silently hand the beacon's
	// Steam port to whatever creates a user after us -- including the primary, if it ever
	// re-creates its own. Restore the value CSteam3Server::Activate() would have used.
	{
		int primarySPort = 26900;
		int argSteamPort = COM_CheckParm("-sport");
		if (argSteamPort > 0)
			primarySPort = Q_atoi(com_argv[argSteamPort + 1]);
		pClient->SetLocalIPBinding(bindAddr, (uint16)primarySPort);
	}
	if (!hPipe || !hUser)
	{
		Con_Printf("[hl-beacon] steamclient would not create a second game-server user\n");
		Cvar_SetValue("sv_hl_beacon", 0.0f);
		return;
	}

	ISteamGameServer *pGS = pClient->GetISteamGameServer(hUser, hPipe, STEAMGAMESERVER_INTERFACE_VERSION);
	if (!pGS)
	{
		Con_Printf("[hl-beacon] steamclient has no %s for the beacon user\n", STEAMGAMESERVER_INTERFACE_VERSION);
		pClient->ReleaseUser(hPipe, hUser);
		pClient->BReleaseSteamPipe(hPipe);
		Cvar_SetValue("sv_hl_beacon", 0.0f);
		return;
	}

	uint32 unFlags = HLB_FLAG_ACTIVE | HLB_FLAG_DEDICATED;
#ifndef _WIN32
	unFlags |= HLB_FLAG_LINUX;
#endif

	// NOT secure: VAC protects the primary registration, not this one. Claiming it
	// here would advertise a guarantee nothing is enforcing.
	AppId_t nAppId = (AppId_t)(int)sv_hl_beacon_appid.value;

	if (!pGS->InitGameServer(unIP, (uint16)gamePort, (uint16)queryPort, unFlags, nAppId, sv_hl_beacon_version.string))
	{
		Con_Printf("[hl-beacon] InitGameServer failed (appid %u, query port %d)\n", (unsigned)nAppId, queryPort);
		pClient->ReleaseUser(hPipe, hUser);
		pClient->BReleaseSteamPipe(hPipe);
		Cvar_SetValue("sv_hl_beacon", 0.0f);
		return;
	}

	// Product and mod dir are what put us under "valve" rather than beside Sven.
	pGS->SetProduct(sv_hl_beacon_gamedir.string);
	pGS->SetGameDescription(sv_hl_beacon_desc.string);
	pGS->SetModDir(sv_hl_beacon_gamedir.string);
	pGS->SetDedicatedServer(true);
	pGS->LogOnAnonymous();

	g_hHLBeaconPipe = hPipe;
	g_hHLBeaconUser = hUser;
	g_pHLBeaconGS   = pGS;
	g_fHLBeaconNextUpdate = 0.0;
	g_szHLBeaconMap[0] = 0;
	g_bHLBeaconLoggedOn = false;
	g_bHLBeaconWarned = false;
	g_fHLBeaconStarted = Sys_FloatTime();

	Con_Printf("[hl-beacon] appid %u gamedir '%s' version '%s' -- answering on udp %d, advertising game port %d\n",
		(unsigned)nAppId, sv_hl_beacon_gamedir.string, sv_hl_beacon_version.string, queryPort, gamePort);
}

static void HLBeacon_UpdateDetails()
{
	double fCurTime = Sys_FloatTime();
	bool bMapChanged = Q_stricmp(g_szHLBeaconMap, g_psv.name) != 0;

	if (!bMapChanged && fCurTime < g_fHLBeaconNextUpdate)
		return;

	g_fHLBeaconNextUpdate = fCurTime + 5.0;
	Q_strncpy(g_szHLBeaconMap, g_psv.name, sizeof(g_szHLBeaconMap) - 1);
	g_szHLBeaconMap[sizeof(g_szHLBeaconMap) - 1] = 0;

	// Whether the SECOND registration ever logs on is the whole question, and there is no
	// console output for it: the "Connection to Steam servers successful" line belongs to
	// CSteam3Server, i.e. the primary. Poll it -- BLoggedOn needs no callback plumbing --
	// so a beacon that registers and then silently never lists says so.
	bool bNow = g_pHLBeaconGS->BLoggedOn();
	if (bNow && !g_bHLBeaconLoggedOn)
	{
		g_bHLBeaconLoggedOn = true;
		Con_Printf("[hl-beacon] logged on, steamid %llu\n",
			(unsigned long long)g_pHLBeaconGS->GetSteamID().ConvertToUint64());
	}
	else if (!bNow && !g_bHLBeaconWarned && fCurTime - g_fHLBeaconStarted > 30.0)
	{
		g_bHLBeaconWarned = true;
		Con_Printf("[hl-beacon] still not logged on after 30s -- steamclient accepted the second\n");
		Con_Printf("[hl-beacon] registration but will not log it on; the listing will not appear\n");
	}

	int maxPlayers = (int)sv_visiblemaxplayers.value;
	if (maxPlayers < 0)
		maxPlayers = g_psvs.maxclients;

	g_pHLBeaconGS->SetMaxPlayerCount(maxPlayers);
	g_pHLBeaconGS->SetBotPlayerCount(0);
	g_pHLBeaconGS->SetServerName(Cvar_VariableString("hostname"));
	g_pHLBeaconGS->SetMapName(g_psv.name);
	g_pHLBeaconGS->SetPasswordProtected(Cvar_VariableString("sv_password")[0] != 0
		&& Q_stricmp(Cvar_VariableString("sv_password"), "none") != 0
		&& Q_stricmp(Cvar_VariableString("sv_password"), "0") != 0);
}

static void HLBeacon_RunFrame()
{
	// sv_lan hides the primary registration from the master; advertising a second
	// one from the same box would contradict that outright.
	bool bWant = sv_hl_beacon.value > 0.0f
		&& sv_lan.value <= 0.0f
		&& g_psvs.maxclients > 1
		&& g_psv.active;

	if (!bWant)
	{
		HLBeacon_Stop();
		return;
	}

	if (!g_hHLBeaconPipe)
		HLBeacon_Start();

	if (!g_hHLBeaconPipe)
		return;

	HLBeacon_UpdateDetails();

	// Same 0.1s cadence CSteam3Server::RunFrame() uses for its own callbacks. Without this
	// the query socket fills and never answers -- see the note on CHLBeaconClientAccess.
	double fCurTime = Sys_FloatTime();
	if (fCurTime - g_fHLBeaconNextPump > 0.1)
	{
		g_fHLBeaconNextPump = fCurTime;
		ISteamClient *pClient = SteamGameServerClient();
		if (pClient)
			((CHLBeaconClientAccess *)pClient)->RunFrame();
	}
}
#endif // REHLDS_SVEN

void Steam_RunFrame()
{
#ifdef REHLDS_SVEN
	HLBeacon_RunFrame();
#endif
	if (Steam3Server())
	{
		Steam3Server()->RunFrame();
	}
}

void Steam_SetCVar(const char *pchKey, const char *pchValue)
{
	if (Steam3Server())
	{
		CRehldsPlatformHolder::get()->SteamGameServer()->SetKeyValue(pchKey, pchValue);
	}
}

void Steam_ClientRunFrame()
{
	Steam3Client()->RunFrame();
}

void Steam_InitClient()
{
	Steam3Client()->InitClient();
}

int Steam_GSInitiateGameConnection(void *pData, int cbMaxData, uint64 steamID, uint32 unIPServer, uint16 usPortServer, qboolean bSecure)
{
	return Steam3Client()->InitiateGameConnection(pData, cbMaxData, steamID, unIPServer, usPortServer, bSecure != 0);
}

void Steam_GSTerminateGameConnection(uint32 unIPServer, uint16 usPortServer)
{
	Steam3Client()->TerminateConnection(unIPServer, usPortServer);
}

void Steam_ShutdownClient()
{
	Steam3Client()->Shutdown();
}

uint64 Steam_GSGetSteamID()
{
	return Steam3Server()->GetSteamID();
}

qboolean Steam_GSBSecure()
{
	//useless call
	//Steam3Server();
	return CRehldsPlatformHolder::get()->SteamGameServer()->BSecure();
}

qboolean Steam_GSBLoggedOn()
{
	return Steam3Server()->BLoggedOn() && CRehldsPlatformHolder::get()->SteamGameServer()->BLoggedOn();
}

qboolean Steam_GSBSecurePreference()
{
	return Steam3Server()->BWantsSecure();
}

TSteamGlobalUserID Steam_Steam3IDtoSteam2(uint64 unSteamID)
{
	return SteamIDToSteam2UserID(unSteamID);
}

uint64 Steam_StringToSteamID(const char *pStr)
{
	CSteamID steamID;
	if (Steam3Server())
	{
		CSteamID serverSteamId(Steam3Server()->GetSteamID());
		SteamIDFromSteam2String(pStr, serverSteamId.GetEUniverse(), &steamID);
	}
	else
	{
		SteamIDFromSteam2String(pStr, k_EUniversePublic, &steamID);
	}

	return steamID.ConvertToUint64();
}

const char *Steam_GetGSUniverse()
{
	CSteamID steamID(Steam3Server()->GetSteamID());
	switch (steamID.GetEUniverse())
	{
	case k_EUniversePublic:
		return "";

	case k_EUniverseBeta:
		return "(beta)";

	case k_EUniverseInternal:
		return "(internal)";

	default:
		return "(u)";
	}
}

CSteam3Server *s_Steam3Server;
CSteam3Client s_Steam3Client;

CSteam3Server *Steam3Server()
{
	return s_Steam3Server;
}

CSteam3Client *Steam3Client()
{
	return &s_Steam3Client;
}

void Master_SetMaster_f()
{
	int i;
	const char * pszCmd;

	i = Cmd_Argc();
	if (!Steam3Server())
	{
		Con_Printf("Usage:\nSetmaster unavailable, start a server first.\n");
		return;
	}

	if (i < 2 || i > 5)
	{
		Con_Printf("Usage:\nSetmaster <enable | disable>\n");
		return;
	}

	pszCmd = Cmd_Argv(1);
	if (!pszCmd || !pszCmd[0])
		return;

	if (Q_stricmp(pszCmd, "disable") || gfNoMasterServer)
	{
		if (!Q_stricmp(pszCmd, "enable"))
		{
			if (gfNoMasterServer)
			{
				gfNoMasterServer = FALSE;
				CRehldsPlatformHolder::get()->SteamGameServer()->SetAdvertiseServerActive(gfNoMasterServer != 0);
			}
		}
	}
	else
	{
		gfNoMasterServer = TRUE;
		CRehldsPlatformHolder::get()->SteamGameServer()->SetAdvertiseServerActive(gfNoMasterServer != 0);
	}
}

void Steam_HandleIncomingPacket(byte *data, int len, int fromip, uint16 port)
{
	CRehldsPlatformHolder::get()->SteamGameServer()->HandleIncomingPacket(data, len, fromip, port);
}
