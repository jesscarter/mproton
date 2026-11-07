// sentinel: not a test. run.sh starts it before the concurrent phase so the wineserver cannot
// exit between programs, then creates C:\ladder-stop to end it. Proves "one wineserver
// lifetime": if the sentinel is still running when the phase ends, every program in the phase
// shared its server.
//
// Usage: sentinel.exe [STOPFILE] [MAX_SECONDS]   (defaults C:\ladder-stop, 3600)
#include "ladder.h"

int main(int argc, char **argv) {
    const char *stop = argc > 1 ? argv[1] : "C:\\ladder-stop";
    DWORD max_s = argc > 2 ? (DWORD)atoi(argv[2]) : 3600;
    g_prog = "sentinel";
    InitializeCriticalSection(&g_report_lock);
    info("start", "pid %lu, waiting for %s", GetCurrentProcessId(), stop);
    double t0 = now_ms();
    while (GetFileAttributesA(stop) == INVALID_FILE_ATTRIBUTES) {
        if (now_ms() - t0 > max_s * 1000.0) { fail("stop", "no stop file after %lus", max_s); return ladder_done(); }
        Sleep(200);
    }
    pass("stop", "stop file seen after %.1fs", (now_ms() - t0) / 1000);
    return ladder_done();
}
