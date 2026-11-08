/*
 * Stand-in for Valve's Windows steamclient.dll / steamclient64.dll on macOS.
 *
 * Proton copies Valve's real ones from Steam's legacycompat/ directory into
 * the prefix (C:\Program Files (x86)\Steam), where the registry's
 * SteamClientDll[64] points games' steam_api(64).dll. The macOS Steam client
 * has no legacycompat/ and no Windows binaries, so without this file
 * steam_api fails to load steamclient and the game runs without Steam.
 *
 * Only the export names matter: when Wine loads a module called
 * steamclient(64).dll, its loader (Proton c177a34d784, mproton's arm64 fix in
 * patch 0727) overwrites every export that lsteamclient.dll also exports with
 * a jump to lsteamclient. The bodies below only run if that redirect did not
 * happen, and then fail cleanly (NULL / false) instead of crashing.
 *
 * The exports are the cdecl (non-stub) entries of lsteamclient.spec. Functions
 * are 16-byte aligned so the 12-byte x86_64 jump Wine writes over each one
 * stays inside its own slot. No CRT, no imports.
 */
#define EXPORT __declspec(dllexport) __attribute__((aligned(16), noinline, used))
typedef unsigned long long u64;

EXPORT void Breakpad_SteamMiniDumpInit(unsigned int a, const char *b, const char *c) { (void)a; (void)b; (void)c; }
EXPORT void Breakpad_SteamSetAppID(unsigned int a) { (void)a; }
EXPORT void Breakpad_SteamSetSteamID(u64 a) { (void)a; }
EXPORT void Breakpad_SteamWriteMiniDumpSetComment(const char *a) { (void)a; }
EXPORT void Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(int a, int b) { (void)a; (void)b; }
EXPORT void *CreateInterface(const char *name, int *ret) { (void)name; if (ret) *ret = 1; return 0; }
EXPORT char Steam_BGetCallback(int pipe, void *msg, int *call) { (void)pipe; (void)msg; (void)call; return 0; }
EXPORT char Steam_FreeLastCallback(int pipe) { (void)pipe; return 0; }
EXPORT char Steam_GetAPICallResult(int pipe, u64 call, void *cb, int size, int expected, char *failed)
{ (void)pipe; (void)call; (void)cb; (void)size; (void)expected; if (failed) *failed = 1; return 0; }
EXPORT char Steam_IsKnownInterface(const char *version) { (void)version; return 0; }
EXPORT void Steam_NotifyMissingInterface(int pipe, const char *version) { (void)pipe; (void)version; }
EXPORT void Steam_ReleaseThreadLocalMemory(int thread_exit) { (void)thread_exit; }

int __stdcall DllMainCRTStartup(void *inst, unsigned int reason, void *reserved) { (void)inst; (void)reason; (void)reserved; return 1; }
