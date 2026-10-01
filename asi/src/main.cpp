// Seasons by Khione
//
// Spring, summer, autumn and winter for Crimson Desert.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "archive.h"
#include "climate.h"
#include "hotkeys.h"
#include "weather.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"

static HMODULE g_module = NULL;
static char g_ini[MAX_PATH];

// The overlay is set up outside the loader lock: Windows' dxgi.dll could not
// make a factory while the plugin was being loaded.
static DWORD WINAPI OverlayThread(LPVOID)
{
    OverlayEarlyInit();
    MenuStart(g_ini);
    return 0;
}

// The ASI loader is also picked up by helper programs in the game folder.
static bool IsGameProcess()
{
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return _stricmp(name, "CrimsonDesert.exe") == 0;
}

static void Start()
{
    char folder[MAX_PATH];
    GetModuleFileNameA(g_module, folder, MAX_PATH);
    char* slash = strrchr(folder, '\\');

    if (slash)
        *slash = 0;

    char bin64[MAX_PATH];
    strcpy_s(bin64, folder);
    strcat_s(folder, "\\Seasons");
    CreateDirectoryA(folder, NULL);

    char log[MAX_PATH], ini[MAX_PATH];
    sprintf_s(log, "%s\\Seasons.log", folder);
    sprintf_s(ini, "%s\\Seasons.ini", folder);
    LogOpen(log);
    Log("Seasons 1.0.1");
    HotkeysLoad(bin64);

    char name[32] = { 0 };
    GetPrivateProfileStringA("Seasons", "season", "summer", name, sizeof(name), ini);
    Season start = SUMMER;

    for (int s = 0; s < SEASON_COUNT; ++s)
        if (_stricmp(name, SEASON_NAMES[s]) == 0)
            start = (Season)s;

    Log("starting in %s", SEASON_NAMES[start]);

    // Hooked right away: the game reads its file index early.
    ArchiveInit(start);
    ClimateStart(start, ini);
    WeatherInit(ini);
    strcpy_s(g_ini, ini);
    CloseHandle(CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL));

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);

        if (IsGameProcess())
            Start();
    }

    return TRUE;
}
