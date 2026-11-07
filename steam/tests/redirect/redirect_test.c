/* Loads steamclient(64).dll from its own directory and calls CreateInterface.
 * PASS: the call was redirected to lsteamclient (no sentinel back).
 * FAIL: the fake's own code ran (sentinel 0x51DEC0DE back).
 * Run with WINEDEBUG=+steamclient to see lsteamclient's "trying to create" trace. */
#include <windows.h>
#include <stdio.h>
typedef void *(*create_interface_fn)(const char *, int *);
int main(void)
{
    const char *dll = sizeof(void *) == 8 ? "steamclient64.dll" : "steamclient.dll";
    HMODULE mod = LoadLibraryA(dll);
    create_interface_fn create;
    void *ret;
    int rc = -1;

    if (!mod) { printf("FAIL: LoadLibrary(%s) error %lu\n", dll, GetLastError()); return 2; }
    if (!(create = (create_interface_fn)GetProcAddress(mod, "CreateInterface")))
    { printf("FAIL: no CreateInterface in %s\n", dll); return 2; }
    printf("%s at %p, CreateInterface at %p, first bytes:", dll, (void *)mod, (void *)create);
    for (int i = 0; i < 12; i++) printf(" %02x", ((unsigned char *)create)[i]);
    printf("\n");
    fflush(stdout);
    ret = create("SteamClient_mproton_redirect_test", &rc);
    if (ret == (void *)0x51DEC0DE)
    { printf("FAIL: not redirected, the fake steamclient's own code ran (rc %#x)\n", rc); return 1; }
    printf("PASS: redirected to lsteamclient (CreateInterface returned %p, rc %d)\n", ret, rc);
    return 0;
}
