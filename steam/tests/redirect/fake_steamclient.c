/* A stand-in for Valve's steamclient(64).dll: same export names, every function
 * returns a sentinel. Wine's loader (Proton commit c177a34d784) overwrites these
 * exports with jumps to lsteamclient.dll, so a caller that gets the sentinel back
 * was NOT redirected. Functions are 16-byte aligned so the 12-byte x86_64 jump
 * stub Wine writes over each one stays inside its own slot. */
#define SENTINEL ((void *)0x51DEC0DE)
#define EXPORT __declspec(dllexport) __attribute__((aligned(16), noinline))
EXPORT void *CreateInterface(const char *name, int *ret) { (void)name; if (ret) *ret = 0x51DE; return SENTINEL; }
EXPORT int Steam_BGetCallback(int pipe, void *msg, int *call) { (void)pipe; (void)msg; (void)call; return 0x51DE; }
EXPORT int Steam_FreeLastCallback(int pipe) { (void)pipe; return 0x51DE; }
EXPORT int Steam_GetAPICallResult(int pipe, unsigned long long call, void *cb, int cb_size, int expected, int *failed) { (void)pipe; (void)call; (void)cb; (void)cb_size; (void)expected; (void)failed; return 0x51DE; }
EXPORT int Steam_IsKnownInterface(const char *version) { (void)version; return 0x51DE; }
EXPORT void Steam_NotifyMissingInterface(int pipe, const char *version) { (void)pipe; (void)version; }
EXPORT int Steam_ReleaseThreadLocalMemory(int thread_exit) { (void)thread_exit; return 0x51DE; }
