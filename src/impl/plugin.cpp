/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * ======================================================
 * Metamod:Source Sample Plugin
 * Written by AlliedModders LLC.
 * ======================================================
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from 
 * the use of this software.
 *
 * This sample plugin is public domain.
 */

#include <CGameRules.h>
#include <CBaseEntity.h>
#include <entitysystem.h>
#include <chrono>
#include <utils.hpp>

#include "../plugin.h"

#include "../shared/players_controller.h"

RubyPlugin g_Plugin;
PLUGIN_EXPOSE( RubyPlugin, g_Plugin );

CEntitySystem*	   g_pEntitySystem	= nullptr;
ConVarRefAbstract* mp_freezetime	= nullptr;

SH_DECL_HOOK3_void( IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool );
SH_DECL_HOOK6( IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, CPlayerSlot, const char*, uint64, const char*, bool, CBufferString* );
SH_DECL_HOOK5_void( IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char * );
SH_DECL_HOOK4_void( IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const*, int, uint64 );
SH_DECL_HOOK2( IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent*, bool );

CConVar< int >     sv_anti_afk_players_threshold( "sv_anti_afk_players_threshold", FCVAR_SPONLY | FCVAR_PROTECTED, "", 12, true, 1, true, 64 );
CConVar< float >   sv_anti_afk_inactive_time_threshold( "sv_anti_afk_inactive_time_threshold", FCVAR_SPONLY | FCVAR_PROTECTED, "", 5.f, true, 1.f, true, 1024.f );
CConVar< float >   sv_anti_afk_spectator_time_threshold( "sv_anti_afk_spectator_time_threshold", FCVAR_SPONLY | FCVAR_PROTECTED, "", 10.f, true, 1.f, true, 1024.f );

CGameEntitySystem* GameEntitySystem( )
{
	return *reinterpret_cast< CGameEntitySystem** >( reinterpret_cast< uintptr_t >( g_pGameResourceServiceServer ) + 0x58 );;
}

CEntityIdentity* CEntitySystem::GetEntityIdentity( const CEntityHandle& hEnt )
{
	return g_pCtx->m_Addresses.m_pGetBaseEntity.Call< CBaseEntity* >( this, hEnt.GetEntryIndex( ) )->m_pEntity;
}

CEntityIdentity* CEntitySystem::GetEntityIdentity( CEntityIndex iEnt )
{
	return g_pCtx->m_Addresses.m_pGetBaseEntity.Call< CBaseEntity* >( this, iEnt.Get( ) )->m_pEntity;
}

bool RubyPlugin::Load( PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late )
{
	PLUGIN_SAVEVARS( );

	if ( !g_pCtx->OnInit( ismm, error, maxlen ) )
		return false;

	if ( late )
		g_pEntitySystem = GameEntitySystem( );

	SH_ADD_HOOK( IServerGameDLL, GameFrame, g_pCtx->m_Interfaces.m_pServerDll, SH_MEMBER( this, &RubyPlugin::Hook_GameFrame ), true );
	SH_ADD_HOOK( IServerGameClients, ClientConnect, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientConnect ), false );
	SH_ADD_HOOK( IServerGameClients, ClientDisconnect, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientDisconnect ), true );
	SH_ADD_HOOK( IServerGameClients, ClientPutInServer, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientPutInServer ), true );
	SH_ADD_HOOK( IGameEventManager2, FireEvent, g_pCtx->m_Interfaces.m_pEventManager, SH_MEMBER( this, &RubyPlugin::Hook_FireEvent ), false );

	g_SMAPI->AddListener( this, this );

	g_pCVar = g_pCtx->m_Interfaces.m_pCvar;

	mp_freezetime = new ConVarRefAbstract( "mp_freezetime" );

	META_CONVAR_REGISTER( FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL );

	META_CONPRINTF( "Starting plugin.\n" );

	return true;
}

bool RubyPlugin::Unload( char* error, size_t maxlen )
{
	SH_REMOVE_HOOK( IServerGameDLL, GameFrame, g_pCtx->m_Interfaces.m_pServerDll, SH_MEMBER( this, &RubyPlugin::Hook_GameFrame ), true );
	SH_REMOVE_HOOK( IServerGameClients, ClientDisconnect, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientDisconnect ), true );
	SH_REMOVE_HOOK( IServerGameClients, ClientConnect, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientConnect ), false );
	SH_REMOVE_HOOK( IServerGameClients, ClientPutInServer, g_pCtx->m_Interfaces.m_pGameClients, SH_MEMBER( this, &RubyPlugin::Hook_ClientPutInServer ), true );
	SH_REMOVE_HOOK( IGameEventManager2, FireEvent, g_pCtx->m_Interfaces.m_pEventManager, SH_MEMBER( this, &RubyPlugin::Hook_FireEvent ), false );

	ConVar_Unregister( );

	return true;
}

void RubyPlugin::AllPluginsLoaded( )
{

}

void RubyPlugin::OnLevelInit( char const* pMapName, char const* pMapEntities, char const* pOldLevel, char const* pLandmarkName, bool loadGame, bool background )
{
	g_pCtx->OnLevelInit( );
}

void RubyPlugin::OnLevelShutdown( )
{
	g_pCtx->OnLevelShutdown( );

	g_pPlayersController->EraseAllPlayers( );
}

constexpr auto iMaxDisconnectedTicks = TIME_TO_TICKS( 300.f );

void RubyPlugin::Hook_GameFrame( bool bSimulating, bool bFirstTick, bool bLastTick )
{
	if ( !g_pEntitySystem )
		g_pEntitySystem = GameEntitySystem( );

	if ( !g_pCtx->m_Interfaces.m_pGameRules
		&& g_pEntitySystem )
	{
		const auto pGameRulesProxy = static_cast< CCSGameRulesProxy* >( UTIL_FindEntityByClassname( "cs_gamerules" ) );

		if ( pGameRulesProxy )
			g_pCtx->m_Interfaces.m_pGameRules = pGameRulesProxy->m_pGameRules( );
	}

	if ( g_pCtx->GetGlobalVars( )
		&& g_pCtx->m_Interfaces.m_pGameRules )
	{
		g_pPlayersController->QueueConnectedPlayers( );

		const auto iPlayersCount	  = g_pPlayersController->GetConnectedPlayersCount( );
		const auto iMaxSpectatorTicks = TIME_TO_TICKS( sv_anti_afk_spectator_time_threshold.GetFloat( ) );
		const auto iMaxInactiveTicks  = TIME_TO_TICKS( sv_anti_afk_inactive_time_threshold.GetFloat( ) );
		const auto bIsRoundNotBegin   = g_pCtx->m_Interfaces.m_pGameRules->m_fRoundStartTime( ).GetTime( ) >= g_pCtx->GetGlobalVars( )->curtime;

		g_pPlayersController->RunForUniquePlayers
		(
			[ & ]( ::Ruby::IPlayersUtilities* pUtilities, ::Ruby::CPlayerData& PlayerData )
			{
				if ( PlayerData.m_iDisconnectTick != TICK_NEVER_THINK )
				{
					// todo
					return;
				}

				if ( g_pCtx->m_Interfaces.m_pGameRules->m_bWarmupPeriod( )
					|| iPlayersCount < sv_anti_afk_players_threshold.GetInt( ) )
				{
					PlayerData.m_iLastIteractTick = g_pCtx->GetGlobalVars( )->tickcount;
					return;	
				}

				auto pPlayerController = g_pCtx->m_Addresses.m_pGetBaseEntityBySlot.Call< CCSPlayerController* >( PlayerData.m_Slot );

				if ( !pPlayerController )
					return;

				auto pPlayerPawn = reinterpret_cast< CCSPlayerPawn* >( pPlayerController->GetPawn( ) );

				if ( !pPlayerPawn )
					return;

				if ( pPlayerPawn->IsAlive( ) 
					&& ( pPlayerPawn->m_iTeamNum( ) != 1 ) )
				{	
					if ( ( ( pPlayerPawn->m_fFlags & FL_FROZEN ) != 0 )
						|| ( ( pPlayerPawn->m_fFlags & FL_ONGROUND ) == 0 )
						|| bIsRoundNotBegin )
					{
						PlayerData.m_iLastIteractTick = g_pCtx->GetGlobalVars( )->tickcount;
						return;
					}

					const auto vecPrevOrigin     = PlayerData.m_vecLastOrigin;
					const auto angPrevViewAngles = PlayerData.m_angLastViewAngles;

					PlayerData.m_vecLastOrigin     = pPlayerPawn->GetAbsOrigin( );
					PlayerData.m_angLastViewAngles = pPlayerPawn->m_angEyeAngles( );

					if ( ( PlayerData.m_vecLastOrigin - vecPrevOrigin ).Length( ) > 0.5f
						|| ( PlayerData.m_angLastViewAngles != angPrevViewAngles ) )
					{
						PlayerData.m_iLastIteractTick = g_pCtx->GetGlobalVars( )->tickcount;
						return;
					}

					if ( PlayerData.m_iLastIteractTick != TICK_NEVER_THINK )
					{
						if ( ( g_pCtx->GetGlobalVars( )->tickcount - PlayerData.m_iLastIteractTick >= iMaxInactiveTicks ) )
						{
							pUtilities->MoveToSpectators( PlayerData.m_Slot );
							META_CONPRINT( "Moving player to Spectators due to Inactivity\n" );
						}
					}

					return;
				}

				if ( pPlayerPawn->m_iTeamNum( ) == 1 )
				{
					if ( PlayerData.m_iLastIteractTick != TICK_NEVER_THINK
						&& g_pCtx->GetGlobalVars( )->tickcount - PlayerData.m_iLastIteractTick >= iMaxSpectatorTicks )
					{
						pUtilities->KickPlayer( PlayerData.m_Slot );
						META_CONPRINT( "Kicking player due to Inactivity\n" );
					}
					return;
				}

				PlayerData.m_iLastIteractTick = g_pCtx->GetGlobalVars( )->tickcount;
			}
		);
	}
}

bool RubyPlugin::Hook_ClientConnect( CPlayerSlot Slot, const char* szName, uint64 llUid, const char* szNetworkID, bool bUnk, CBufferString* pRejectReason )
{
	g_pPlayersController->RegisterPlayer( Slot );

	RETURN_META_VALUE( MRES_IGNORED, true );
}

void RubyPlugin::Hook_ClientDisconnect( CPlayerSlot Slot, ENetworkDisconnectionReason eReason, const char* szName, uint64 llUid, const char* szNetworkID )
{
	if ( g_pCtx->GetGlobalVars( ) )
		g_pPlayersController->PlayerDisconnect( Slot );
}

void RubyPlugin::Hook_ClientPutInServer( CPlayerSlot iSlot, char const* szName, int iType, uint64 llUid )
{
	g_pPlayersController->ServerPutPlayer( iSlot );
}

bool RubyPlugin::Hook_FireEvent( IGameEvent* pEvent, bool bDontBroadcast )
{
	if ( !pEvent )
		RETURN_META_VALUE( MRES_IGNORED, false );

	const auto szEventName = pEvent->GetName( );
	if ( szEventName )
	{
		const auto iNameHash = hash_64_fnv1a_const( szEventName );
		switch ( iNameHash )
		{
		case hash_64_fnv1a_const( "round_start" ):
		{
			const auto pGlobals = g_pCtx->GetGlobalVars( );
			if ( pGlobals )
			{
				g_pCtx->m_flAbsRoundStartTime = pGlobals->curtime;
			}
			break;
		}
		default:
			break;
		}
	}

	RETURN_META_VALUE( MRES_IGNORED, true );
}


