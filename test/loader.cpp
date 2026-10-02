// dlopen smoke test: load the built plugin, confirm all FIVE exports the
// Linux loader requires (it silently skips a plugin missing any — incl.
// isUnicode) and the menu ABI, without needing the Nextpad++ host.
//
// Usage: loader <path to Finder.so>

#include "plugin.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <so>\n", argv[0]); return 2; }
    // RTLD_LAZY | RTLD_LOCAL — exactly what the host's load_plugin() uses.
    void *h = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!h) { printf("dlopen FAIL: %s\n", dlerror()); return 1; }

    auto pGetName  = (const char *(*)())dlsym(h, "getName");
    auto pGetFuncs = (FuncItem * (*)(int *)) dlsym(h, "getFuncsArray");
    auto pSetInfo  = (void (*)(NppData))dlsym(h, "setInfo");
    auto pNotify   = (void (*)(void *))dlsym(h, "beNotified");
    auto pMsgProc  = (long (*)(unsigned int, unsigned long, long))dlsym(h, "messageProc");
    auto pIsUni    = (int (*)())dlsym(h, "isUnicode");
    if (!pGetName || !pGetFuncs || !pSetInfo || !pNotify || !pMsgProc || !pIsUni) {
        printf("dlsym FAIL (missing export)\n");
        return 1;
    }

    // setInfo() queries the config dir — supply a stub hostMsg, never NULL.
    NppData nd = {};
    nd.hostMsg = [](unsigned int, unsigned long, long) -> long { return 0; };
    pSetInfo(nd);

    printf("getName: '%s'\n", pGetName());
    int n = 0;
    FuncItem *f = pGetFuncs(&n);
    printf("nbFunc: %d\n", n);
    for (int i = 0; i < n; ++i)
        printf("  [%2d] '%s'\n", i, f[i].itemName);
    printf("sizeof(FuncItem): %zu (host expects 80)\n", sizeof(FuncItem));
    printf("isUnicode: %d\n", pIsUni());

    bool ok = (n == 3) && (sizeof(FuncItem) == 80) && pIsUni() == 1 &&
              !strcmp(pGetName(), "Finder") &&
              !strcmp(f[0].itemName, "Toggle Finder Panel") &&
              !strcmp(f[1].itemName, "Locate Current File in Finder Panel") &&
              !strcmp(f[2].itemName, "Reveal Current File in File Manager") &&
              f[0].pFunc && f[1].pFunc && f[2].pFunc;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
