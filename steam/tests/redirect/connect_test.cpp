/* Wine-side twin of steam/tests/steamclient_probe.cpp --connect: goes through the
 * real Windows entry point (steamclient(64).dll -> lsteamclient -> host
 * steamclient.dylib) and talks to the running Steam client.
 * Needs the fake steamclient(64).dll next to it (the redirect replaces it) and
 * Steam running. C++ so that i686 gets thiscall vtables like the real SDK.
 *
 * --stats: also check 4 of steam/README.md on Spacewar (480): RequestCurrentStats ->
 * UserStatsReceived_t callback, SetAchievement + StoreStats -> UserStatsStored_t,
 * then ClearAchievement + StoreStats to leave the account as it was. Exercises
 * Steam_BGetCallback and the callback struct converters. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ISteamClient017
{
    virtual int32_t CreateSteamPipe() = 0;
    virtual int8_t BReleaseSteamPipe( int32_t ) = 0;
    virtual int32_t ConnectToGlobalUser( int32_t ) = 0;
    virtual int32_t CreateLocalUser( int32_t *, uint32_t ) = 0;
    virtual void ReleaseUser( int32_t, int32_t ) = 0;
    virtual void *GetISteamUser( int32_t, int32_t, const char * ) = 0;
    virtual void *GetISteamGameServer( int32_t, int32_t, const char * ) = 0;
    virtual void SetLocalIPBinding( uint32_t, uint16_t ) = 0;
    virtual void *GetISteamFriends( int32_t, int32_t, const char * ) = 0;
    virtual void *GetISteamUtils( int32_t, const char * ) = 0;
    virtual void *GetISteamMatchmaking( int32_t, int32_t, const char * ) = 0;
    virtual void *GetISteamMatchmakingServers( int32_t, int32_t, const char * ) = 0;
    virtual void *GetISteamGenericInterface( int32_t, int32_t, const char * ) = 0;
    virtual void *GetISteamUserStats( int32_t, int32_t, const char * ) = 0;
};

struct ISteamUtils009
{
    virtual uint32_t GetSecondsSinceAppActive() = 0;
    virtual uint32_t GetSecondsSinceComputerActive() = 0;
    virtual uint32_t GetConnectedUniverse() = 0;
    virtual uint32_t GetServerRealTime() = 0;
    virtual const char *GetIPCountry() = 0;
    virtual int8_t GetImageSize( int32_t, uint32_t *, uint32_t * ) = 0;
    virtual int8_t GetImageRGBA( int32_t, uint8_t *, int32_t ) = 0;
    virtual int8_t GetCSERIPPort( uint32_t *, uint16_t * ) = 0;
    virtual uint8_t GetCurrentBatteryPower() = 0;
    virtual uint32_t GetAppID() = 0;
};

/* MSVC orders overloaded virtuals (GetStat/SetStat, slots 1-4) differently from
 * mingw's Itanium layout, so those slots are placeholders and never called;
 * every slot used below is at the same index in both layouts. */
struct ISteamUserStats012
{
    virtual int8_t RequestCurrentStats() = 0;
    virtual void slot1() = 0;
    virtual void slot2() = 0;
    virtual void slot3() = 0;
    virtual void slot4() = 0;
    virtual void slot5() = 0;
    virtual int8_t GetAchievement( const char *, int8_t * ) = 0;
    virtual int8_t SetAchievement( const char * ) = 0;
    virtual int8_t ClearAchievement( const char * ) = 0;
    virtual int8_t GetAchievementAndUnlockTime( const char *, int8_t *, uint32_t * ) = 0;
    virtual int8_t StoreStats() = 0;
    virtual int32_t GetAchievementIcon( const char * ) = 0;
    virtual const char *GetAchievementDisplayAttribute( const char *, const char * ) = 0;
    virtual int8_t IndicateAchievementProgress( const char *, uint32_t, uint32_t ) = 0;
    virtual uint32_t GetNumAchievements() = 0;
    virtual const char *GetAchievementName( uint32_t ) = 0;
};

#pragma pack( push, 8 ) /* Steamworks' Windows callback packing */
struct CallbackMsg_t { int32_t user; int32_t id; uint8_t *param; int32_t size; };
/* CSteamID is pack(1) in steamclientpublic.h, so it sits at offset 12: 20 bytes in all */
struct UserStatsReceived_t { uint64_t game_id; int32_t result; uint32_t steam_id_lo, steam_id_hi; }; /* 1101 */
struct UserStatsStored_t { uint64_t game_id; int32_t result; };                         /* 1102 */
#pragma pack( pop )

typedef void *(*create_interface_fn)( const char *, int * );
typedef int8_t (*bgetcallback_fn)( int32_t, CallbackMsg_t *, int32_t * );
typedef int8_t (*freelastcallback_fn)( int32_t );

static bgetcallback_fn p_BGetCallback;
static freelastcallback_fn p_FreeLastCallback;

/* pump callbacks on the pipe until one with this id arrives; copy it out */
static bool wait_callback( int32_t pipe, int32_t id, void *out, size_t size, DWORD timeout_ms )
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms)
    {
        CallbackMsg_t msg;
        int32_t dummy;
        while (p_BGetCallback( pipe, &msg, &dummy ))
        {
            bool hit = msg.id == id;
            printf( "     callback %d, %d bytes\n", msg.id, msg.size );
            if (hit) memcpy( out, msg.param, (size_t)msg.size < size ? (size_t)msg.size : size );
            p_FreeLastCallback( pipe );
            if (hit) return true;
        }
        Sleep( 50 );
    }
    return false;
}

static int stats_test( ISteamClient017 *client, int32_t pipe, int32_t user )
{
    const char *ach = "ACH_WIN_ONE_GAME";
    int8_t got = -1;
    auto *stats = (ISteamUserStats012 *)client->GetISteamUserStats( user, pipe, "STEAMUSERSTATS_INTERFACE_VERSION012" );
    if (!stats) { printf( "FAIL GetISteamUserStats(012)\n" ); return 1; }

    if (!stats->RequestCurrentStats()) { printf( "FAIL RequestCurrentStats\n" ); return 1; }
    UserStatsReceived_t recv = {};
    if (!wait_callback( pipe, 1101, &recv, sizeof(recv), 15000 )) { printf( "FAIL no UserStatsReceived_t in 15 s\n" ); return 1; }
    printf( "%s UserStatsReceived_t: game %llu, result %d, user %llu\n", recv.result == 1 ? "ok  " : "FAIL",
            (unsigned long long)recv.game_id, recv.result,
            (unsigned long long)recv.steam_id_hi << 32 | recv.steam_id_lo );
    if (recv.result != 1) return 1;

    uint32_t n = stats->GetNumAchievements();
    printf( "%s %u achievements, first %s (\"%s\")\n", n ? "ok  " : "FAIL", n, n ? stats->GetAchievementName( 0 ) : "-",
            stats->GetAchievementDisplayAttribute( ach, "name" ) );
    stats->GetAchievement( ach, &got );
    printf( "     %s before: %d\n", ach, got );

    for (int pass = 0; pass < 2; pass++)
    {
        bool set = !pass;
        if (!(set ? stats->SetAchievement( ach ) : stats->ClearAchievement( ach )) || !stats->StoreStats())
        { printf( "FAIL %s/StoreStats\n", set ? "SetAchievement" : "ClearAchievement" ); return 1; }
        UserStatsStored_t stored = {};
        if (!wait_callback( pipe, 1102, &stored, sizeof(stored), 15000 )) { printf( "FAIL no UserStatsStored_t\n" ); return 1; }
        got = -1;
        stats->GetAchievement( ach, &got );
        printf( "%s %s + StoreStats: stored result %d, achieved now %d\n", stored.result == 1 && got == set ? "ok  " : "FAIL",
                set ? "SetAchievement" : "ClearAchievement", stored.result, got );
        if (stored.result != 1 || got != set) return 1;
    }
    return 0;
}

int main( int argc, char *argv[] )
{
    bool do_stats = argc > 1 && !strcmp( argv[1], "--stats" );
    const char *dll = sizeof(void *) == 8 ? "steamclient64.dll" : "steamclient.dll";
    if (!getenv( "SteamAppId" )) SetEnvironmentVariableA( "SteamAppId", "480" ); /* Spacewar */
    HMODULE mod = LoadLibraryA( dll );
    if (!mod) { printf( "FAIL LoadLibrary(%s) error %lu\n", dll, GetLastError() ); return 2; }
    auto create = (create_interface_fn)GetProcAddress( mod, "CreateInterface" );
    if (!create) { printf( "FAIL no CreateInterface\n" ); return 2; }
    p_BGetCallback = (bgetcallback_fn)GetProcAddress( mod, "Steam_BGetCallback" );
    p_FreeLastCallback = (freelastcallback_fn)GetProcAddress( mod, "Steam_FreeLastCallback" );

    int rc = 0;
    auto *client = (ISteamClient017 *)create( "SteamClient017", &rc );
    if (!client || client == (void *)0x51DEC0DE)
    { printf( "FAIL CreateInterface(SteamClient017) = %p, rc %d\n", (void *)client, rc ); return 1; }
    printf( "ok   CreateInterface(SteamClient017)\n" );

    int32_t pipe = client->CreateSteamPipe();
    int32_t user = pipe ? client->ConnectToGlobalUser( pipe ) : 0;
    printf( "%s pipe %d, user %d\n", pipe && user ? "ok  " : "FAIL", pipe, user );
    if (!pipe || !user) return 1;

    auto *utils = (ISteamUtils009 *)client->GetISteamUtils( pipe, "SteamUtils009" );
    if (!utils) { printf( "FAIL GetISteamUtils(SteamUtils009)\n" ); return 1; }
    printf( "ok   ISteamUtils009: app id %u, universe %u, IP country %s, server time %u\n",
            utils->GetAppID(), utils->GetConnectedUniverse(), utils->GetIPCountry(), utils->GetServerRealTime() );

    if (do_stats && stats_test( client, pipe, user )) return 1;

    client->ReleaseUser( pipe, user );
    client->BReleaseSteamPipe( pipe );
    printf( "PASS\n" );
    return 0;
}
