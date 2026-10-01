#include "climate.h"
#include "gpu.h"
#include "log.h"
#include "weather.h"

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

// ---------------------------------------------------------------------------
// The climate map (PAClimateTexture, vtable rva 5AD1A10): +0x18 texels, +0x20
// their count, +0x30/+0x34 metres per texel (80), +0x38/+0x3C width and height
// (512). It is texture/climate_texture_1.dds upside down, a u32 per texel with
// the bytes A,R,G,B. The game makes two (the weather manager's, the climate
// manager's at +0xD8) and samples them as it runs (rva 269F080): the top byte
// is the day's top temperature, -50..50 degrees for 0..255, the night is up
// to 25 degrees colder (G). The temperature meter and deep snow follow the
// climate manager's. Each map gets the season's texels, checked every 2
// seconds (the game makes them again when a world loads).
//
// Snow (settings in the game's data, read live):
//   SNOW_START / SNOW_FULL (-5 / -20): the game's, left as they are
//   DEEP_SNOW / DEEP_SNOW_FULL (-20 / -30): deep snow near the camera when the
//                    midday there is below it (rva 3DC0C50 -> climate manager
//                    +0x120), and on the graphics card where the climate
//                    texture is (with the walking-in-deep-snow effect)
// Each season has its own temperatures (SHIFT). Deep snow builds up while it
// snows (DEEP_SNOW rising to +6) and melts after a while without snow.
// ---------------------------------------------------------------------------

static const uintptr_t CLIMATE_VTABLE = 0x5AD1A10;
static const uint32_t MAP_SIZE = 512;
static const uintptr_t SNOW_START = 0x6D63848, SNOW_FULL = 0x6D63898, DEEP_SNOW = 0x6D638E8, DEEP_SNOW_FULL = 0x6D63938;
static const float DEEP_SNOW_NONE = -45.0f;     // below winter's climate texture (-40): no deep snow

// Each season's days and nights against the game's own (summer is hot).
static const float SHIFT[SEASON_COUNT] = { -6.0f, 4.0f, -8.0f, -22.0f };   // spring, summer, autumn, winter
// DEEP_SNOW as the snow deepens: the game's (-20, the snow mountains only)
// with none, then from DEEP_SNOW_LEAST to DEEP_SNOW_MOST - the winter's day
// temperatures in the middle lands - so it spreads from the colder places.
static const float DEEP_SNOW_LEAST = 0.0f, DEEP_SNOW_MOST = 6.0f;

static std::vector<uint32_t> g_map[SEASON_COUNT];   // in the game's layout; empty: none
static std::vector<uintptr_t> g_objects;
static volatile LONG g_target = SUMMER;
static HANDLE g_wake = NULL;
static char g_ini[MAX_PATH];

// The climate map's R: the ground cover the game wades through (snow in the
// north, sand in the desert; 3 on the land). Winter keeps the land's and
// deepens it with the snow, up to the winter texture's (the snow mountains').
static std::vector<uint8_t> g_winterCover;         // the winter texture's R (0: none)
static std::vector<uint32_t> g_deepMap;
static int g_deepMapPercent = -1;

static volatile LONG g_test = CLIMATE_TEST_NONE;   // the day's top temperature everywhere (test)
static std::vector<uint32_t> g_testMap;

static volatile LONG g_deepTest = -1;               // test: -1 the snowfall's, else percent
static volatile LONG g_deepShown = 0;               // percent, for the menu

static float g_gameStart = 0, g_gameFull = 0, g_gameDeep = 0, g_gameDeepFull = 0;
static bool g_snowRead = false;

static float* Global(uintptr_t rva) { return (float*)((uintptr_t)GetModuleHandleW(NULL) + rva); }

// The climate manager ([146D69198] +0x40 -> +0xF68, rva 73E4DB) and the
// camera it keeps (+0x4001C8: x, y, z).
static const uintptr_t CLIMATE_ROOT = 0x6D69198;

static bool Camera(float* x, float* z)
{
    __try
    {
        uintptr_t root = *(uintptr_t*)((uintptr_t)GetModuleHandleW(NULL) + CLIMATE_ROOT);
        uintptr_t a = root ? *(uintptr_t*)(root + 0x40) : 0;
        uintptr_t mgr = a ? *(uintptr_t*)(a + 0xF68) : 0;

        if (!mgr)
            return false;

        *x = *(float*)(mgr + 0x4001C8);
        *z = *(float*)(mgr + 0x4001D0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static float Top(uint32_t texel);

// The coldest midday around the camera in the map the game gets, as the
// game's deep snow test takes it (rva 3DC0C50: 3x3 texels of 80 m).
static bool ColdestAround(const std::vector<uint32_t>& m, float* coldest)
{
    float x, z;

    if (m.size() != (size_t)MAP_SIZE * MAP_SIZE || !Camera(&x, &z))
        return false;

    int col = (int)(x / 80.0f) + (int)MAP_SIZE / 2, row = (int)(z / 80.0f) + (int)MAP_SIZE / 2;
    float least = 1000;

    for (int r = row - 1; r <= row + 1; ++r)
        for (int c = col - 1; c <= col + 1; ++c)
            if (r >= 0 && c >= 0 && r < (int)MAP_SIZE && c < (int)MAP_SIZE)
            {
                float t = Top(m[(size_t)r * MAP_SIZE + c]);
                least = t < least ? t : least;
            }

    *coldest = least;
    return least < 1000;
}

// Deep snow. The game shows it near the camera when the midday there is
// below DEEP_SNOW (the climate maps), and the graphics card puts it - and the
// walking-in-deep-snow effect - where the climate texture is below DEEP_SNOW
// (and full below DEEP_SNOW_FULL). Winter's climate texture is -40 degrees
// everywhere (its snow cover): with the game's line (-20) every place off
// the roads waded in deep snow. So in winter the line is either where the
// snow's depth puts it, when deep snow is due near the camera, or below
// the winter texture, when it is not.
static void SnowLine(Season season, const std::vector<uint32_t>& m)
{
    if (!g_snowRead)
    {
        g_gameStart = *Global(SNOW_START);
        g_gameFull = *Global(SNOW_FULL);
        g_gameDeep = *Global(DEEP_SNOW);
        g_gameDeepFull = *Global(DEEP_SNOW_FULL);
        g_snowRead = true;
        Log("climate: the game's snow temperatures %.1f / %.1f, deep snow below %.1f / %.1f", g_gameStart, g_gameFull, g_gameDeep,
            g_gameDeepFull);
    }

    LONG test = g_deepTest;
    float deep = test >= 0 ? test / 100.0f : WeatherSnowDepth();
    float line = deep <= 0 ? g_gameDeep : DEEP_SNOW_LEAST + (DEEP_SNOW_MOST - DEEP_SNOW_LEAST) * deep;
    float wantDeep = line, wantDeepFull = deep <= 0 ? g_gameDeepFull : line - 10.0f;

    if (season == WINTER)
    {
        float coldest;
        bool due = deep > 0 && ColdestAround(m, &coldest) && coldest < line;

        if (!due)
        {
            wantDeep = DEEP_SNOW_NONE;
            wantDeepFull = DEEP_SNOW_NONE - 5.0f;
        }
    }

    if (*Global(SNOW_START) != g_gameStart || *Global(SNOW_FULL) != g_gameFull || *Global(DEEP_SNOW) != wantDeep ||
        *Global(DEEP_SNOW_FULL) != wantDeepFull)
    {
        bool quiet = fabsf(*Global(DEEP_SNOW) - wantDeep) < 1.0f && *Global(SNOW_START) == g_gameStart;
        *Global(SNOW_START) = g_gameStart;
        *Global(SNOW_FULL) = g_gameFull;
        *Global(DEEP_SNOW) = wantDeep;
        *Global(DEEP_SNOW_FULL) = wantDeepFull;

        if (!quiet)
            Log("climate: deep snow below %.1f (full below %.1f)%s", wantDeep, wantDeepFull,
                wantDeep == DEEP_SNOW_NONE ? " - none here" : "");
    }
}

static float Top(uint32_t texel)
{
    return ((texel >> 24) / 255.0f * 2.0f - 1.0f) * 50.0f;
}

// The texel's top byte for a day's top temperature.
static uint32_t TopByte(float degrees)
{
    int b = (int)((degrees / 50.0f + 1.0f) / 2.0f * 255.0f + 0.5f);
    return (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

// A season's climate texture in the game's layout.
static bool Load(Season season, std::vector<uint32_t>* m)
{
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;

    if (!SeasonTextureMip("0002", "texture/climate_texture_1.dds", season, 0, &px, &w, &h) || w != MAP_SIZE || h != MAP_SIZE ||
        px.size() != (size_t)MAP_SIZE * MAP_SIZE * 4)
        return false;

    m->resize((size_t)MAP_SIZE * MAP_SIZE);

    for (uint32_t y = 0; y < MAP_SIZE; ++y)
        for (uint32_t x = 0; x < MAP_SIZE; ++x)
        {
            const uint8_t* p = &px[((size_t)(MAP_SIZE - 1 - y) * MAP_SIZE + x) * 4];   // B,G,R,A
            (*m)[(size_t)y * MAP_SIZE + x] = (uint32_t)p[3] | ((uint32_t)p[2] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24);
        }

    return true;
}

// Each season: the game's own (summer's climate texture) SHIFT degrees warmer
// or colder. Winter takes the winter texture's other channels (the snow
// mountains'); where the winter texture keeps summer's (the desert), it stays
// as in summer.
static void Build()
{
    std::vector<uint32_t> game, winter;

    if (!Load(SUMMER, &game))
    {
        Log("climate: no summer climate map in the season data");
        return;
    }

    bool haveWinter = Load(WINTER, &winter);

    if (!haveWinter)
        Log("climate: no winter climate map in the season data");

    size_t kept = 0;
    g_winterCover.assign(game.size(), 0);

    for (int s = 0; s < SEASON_COUNT; ++s)
    {
        std::vector<uint32_t>& m = g_map[s];
        m.resize(game.size());

        for (size_t i = 0; i < game.size(); ++i)
        {
            uint32_t rest = game[i] & 0x00FFFFFF;
            float shift = SHIFT[s];

            if (s == WINTER && haveWinter)
            {
                if (winter[i] == game[i])
                {
                    shift = SHIFT[SUMMER];
                    ++kept;
                }
                else
                {
                    // The winter texture's G and A; R (the ground cover the
                    // game wades through, deep in the snowy north) stays the
                    // land's own and deepens with the snow (Deepened).
                    rest = (winter[i] & 0x00FF00FF) | (game[i] & 0x0000FF00);
                    g_winterCover[i] = (uint8_t)((winter[i] >> 8) & 0xFF);
                }
            }

            m[i] = rest | (TopByte(Top(game[i]) + shift) << 24);
        }
    }

    Log("climate: spring %+.0f, summer %+.0f, autumn %+.0f, winter %+.0f degrees against the game's (%zu texels warm in winter: the desert)",
        SHIFT[SPRING], SHIFT[SUMMER], SHIFT[AUTUMN], SHIFT[WINTER], kept);
}

static bool IsMap(uintptr_t o, uintptr_t vtable)
{
    __try
    {
        return *(uintptr_t*)o == vtable && *(uintptr_t*)(o + 0x18) > 0x10000 && *(uint32_t*)(o + 0x20) == MAP_SIZE * MAP_SIZE &&
               *(uint32_t*)(o + 0x38) == MAP_SIZE && *(uint32_t*)(o + 0x3C) == MAP_SIZE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The climate maps in one stretch of memory.
static size_t ScanRegion(const uint8_t* base, size_t size, uintptr_t vtable, uintptr_t* out, size_t max)
{
    size_t n = 0;

    __try
    {
        for (size_t i = 0; i + 0x40 <= size && n < max; i += 8)
            if (*(const uintptr_t*)(base + i) == vtable && IsMap((uintptr_t)(base + i), vtable))
                out[n++] = (uintptr_t)(base + i);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return n;
}

// Every climate map in the game's memory.
static std::vector<uintptr_t> Find()
{
    uintptr_t vtable = (uintptr_t)GetModuleHandleW(NULL) + CLIMATE_VTABLE;
    std::vector<uintptr_t> found;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* addr = NULL;
    uintptr_t hits[16];

    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        addr = base + size;

        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE)
            continue;

        size_t n = ScanRegion(base, size, vtable, hits, 16);
        found.insert(found.end(), hits, hits + n);
    }

    return found;
}

// Gives a map the season's texels; true when it had others.
static bool Bring(uintptr_t o, const uint32_t* m, size_t count)
{
    __try
    {
        uint32_t* t = *(uint32_t**)(o + 0x18);

        if (memcmp(t, m, count * 4) == 0)
            return false;

        memcpy(t, m, count * 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Winter's map with the ground cover as deep as the snow (whole percents).
static const std::vector<uint32_t>& Deepened(float deep)
{
    int percent = (int)(deep * 100 + 0.5f);

    if (percent != g_deepMapPercent || g_deepMap.size() != g_map[WINTER].size())
    {
        g_deepMap = g_map[WINTER];

        for (size_t i = 0; i < g_deepMap.size() && i < g_winterCover.size(); ++i)
        {
            uint32_t land = (g_deepMap[i] >> 8) & 0xFF, snow = g_winterCover[i];

            if (snow > land)
            {
                uint32_t r = land + (snow - land) * (uint32_t)percent / 100;
                g_deepMap[i] = (g_deepMap[i] & 0xFFFF00FF) | (r << 8);
            }
        }

        g_deepMapPercent = percent;
    }

    return g_deepMap;
}

static DWORD WINAPI ClimateThread(LPVOID)
{
    Build();
    uintptr_t vtable = (uintptr_t)GetModuleHandleW(NULL) + CLIMATE_VTABLE;
    DWORD lastFind = 0;

    for (;;)
    {
        WaitForSingleObject(g_wake, 2000);
        Season target = (Season)g_target;
        LONG deepTest = g_deepTest;
        InterlockedExchange(&g_deepShown, deepTest >= 0 ? deepTest : (LONG)(WeatherSnowDepth() * 100 + 0.5f));

        const std::vector<uint32_t>* pick = &g_map[target];

        if (pick->empty())
            continue;

        if (target == WINTER)
            pick = &Deepened(deepTest >= 0 ? deepTest / 100.0f : WeatherSnowDepth());

        LONG test = g_test;

        if (test != CLIMATE_TEST_NONE)
        {
            g_testMap = *pick;

            for (uint32_t& t : g_testMap)
                t = (t & 0x00FFFFFF) | (TopByte((float)test) << 24);

            pick = &g_testMap;
        }

        const std::vector<uint32_t>& m = *pick;

        // A map gone (a world unloaded): look again (every 10 seconds while
        // there is none); and every 30 seconds, for new ones.
        bool stale = false;

        for (uintptr_t o : g_objects)
            stale = stale || !IsMap(o, vtable);

        DWORD since = GetTickCount() - lastFind;

        if (!lastFind || stale || since > 30000 || (g_objects.empty() && since > 10000))
        {
            DWORD t0 = GetTickCount();
            std::vector<uintptr_t> found = Find();
            lastFind = GetTickCount();

            if (found != g_objects)
            {
                g_objects = found;
                Log("climate: %zu climate maps (looked for %lu ms)", g_objects.size(), lastFind - t0);
            }
        }

        if (!g_objects.empty())
            SnowLine(target, m);

        int changed = 0;

        for (uintptr_t o : g_objects)
            changed += Bring(o, m.data(), m.size()) ? 1 : 0;

        if (changed && test != CLIMATE_TEST_NONE)
            Log("climate: %d climate maps now %s's at %ld degrees (test)", changed, SEASON_NAMES[target], test);
        else if (changed)
            Log("climate: %d climate maps now %s's", changed, SEASON_NAMES[target]);
    }
}

void ClimateStart(Season start, const char* ini)
{
    g_target = start;
    strcpy_s(g_ini, ini);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, ClimateThread, NULL, 0, NULL));
}

void ClimateSeasonChanged(Season season)
{
    InterlockedExchange(&g_target, season);

    if (g_wake)
        SetEvent(g_wake);
}

void ClimateTest(int degrees)
{
    InterlockedExchange(&g_test, degrees);

    if (g_wake)
        SetEvent(g_wake);
}

int ClimateTestDegrees() { return g_test; }

void ClimateDeepTest(int percent)
{
    InterlockedExchange(&g_deepTest, percent);

    if (g_wake)
        SetEvent(g_wake);
}

int ClimateDeepTestPercent() { return g_deepTest; }

int ClimateDeepPercent() { return g_deepShown; }
