/*
 * steamclient_probe: check, without Wine, that the native macOS
 * steamclient.dylib can be loaded the way lsteamclient's unix side loads it
 * (steam/lsteamclient/unixlib.cpp, steamclient_init / find_steamclient_dylib),
 * and optionally that it connects to the running Steam client.
 *
 * Build and run on the Mac:
 *   clang++ -std=c++17 -o steamclient_probe steam/tests/steamclient_probe.cpp
 *   ./steamclient_probe                 # dlopen + the 7 entry points
 *   ./steamclient_probe --connect       # also pipe + user + ISteamUtils (Steam must be running)
 *   ./steamclient_probe /path/to/steamclient.dylib
 *
 * It uses the same search order as find_steamclient_dylib(), honouring
 * MPROTON_STEAMCLIENT_PATH and STEAM_COMPAT_CLIENT_INSTALL_PATH.
 */

#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* vtable prefixes of the SDK interfaces, as in unix_private_generated.h */
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

static bool readable( const char *path )
{
    return !access( path, R_OK );
}

static void find_steamclient_dylib( char *path )
{
    static const char bundle_dir[] = "Steam.AppBundle/Steam/Contents/MacOS";
    const char *env;

    if ((env = getenv( "MPROTON_STEAMCLIENT_PATH" )) && *env)
    {
        snprintf( path, PATH_MAX, "%s", env );
        return;
    }
    if ((env = getenv( "STEAM_COMPAT_CLIENT_INSTALL_PATH" )) && *env)
    {
        snprintf( path, PATH_MAX, "%s/steamclient.dylib", env );
        if (readable( path )) return;
        snprintf( path, PATH_MAX, "%s/%s/steamclient.dylib", env, bundle_dir );
        if (readable( path )) return;
    }
    if ((env = getenv( "HOME" )) && *env)
    {
        snprintf( path, PATH_MAX, "%s/Library/Application Support/Steam/%s/steamclient.dylib", env, bundle_dir );
        if (readable( path )) return;
    }
    strcpy( path, "steamclient.dylib" );
}

int main( int argc, char *argv[] )
{
    static const char *const entry_points[] =
    {
        "CreateInterface", "Steam_BGetCallback", "Steam_GetAPICallResult", "Steam_FreeLastCallback",
        "Steam_ReleaseThreadLocalMemory",
    };
    /* not exported by the macOS steamclient.dylib; lsteamclient falls back without them */
    static const char *const optional_entry_points[] =
    {
        "Steam_IsKnownInterface", "Steam_NotifyMissingInterface",
    };
    char path[PATH_MAX];
    bool connect = false;
    int i, missing = 0;
    void *lib;

    path[0] = 0;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp( argv[i], "--connect" )) connect = true;
        else snprintf( path, sizeof(path), "%s", argv[i] );
    }
    if (!path[0]) find_steamclient_dylib( path );

    printf( "steamclient: %s\n", path );
    if (!(lib = dlopen( path, RTLD_NOW )))
    {
        printf( "FAIL dlopen: %s\n", dlerror() );
        return 1;
    }
    printf( "ok   dlopen\n" );

    for (const char *name : entry_points)
    {
        void *p = dlsym( lib, name );
        printf( "%s %s\n", p ? "ok  " : "FAIL", name );
        missing += !p;
    }
    for (const char *name : optional_entry_points)
        printf( "%s %s (optional)\n", dlsym( lib, name ) ? "ok  " : "--  ", name );
    if (missing || !connect) return missing ? 1 : 0;

    if (!getenv( "SteamAppId" )) setenv( "SteamAppId", "480", 1 ); /* Spacewar */
    auto create_interface = (void *(*)( const char *, int * ))dlsym( lib, "CreateInterface" );
    int error = 0;
    auto client = (ISteamClient017 *)create_interface( "SteamClient017", &error );
    if (!client)
    {
        printf( "FAIL CreateInterface(SteamClient017), error %d\n", error );
        return 1;
    }
    int32_t pipe = client->CreateSteamPipe();
    int32_t user = pipe ? client->ConnectToGlobalUser( pipe ) : 0;
    printf( "%s pipe %d, user %d\n", pipe && user ? "ok  " : "FAIL", pipe, user );
    if (!pipe || !user) return 1;

    auto utils = (ISteamUtils009 *)client->GetISteamUtils( pipe, "SteamUtils009" );
    if (utils)
        printf( "ok   ISteamUtils009: app id %u, universe %u, IP country %s\n", utils->GetAppID(),
                utils->GetConnectedUniverse(), utils->GetIPCountry() );
    else
        printf( "FAIL GetISteamUtils(SteamUtils009)\n" );
    client->BReleaseSteamPipe( pipe );
    return utils ? 0 : 1;
}
