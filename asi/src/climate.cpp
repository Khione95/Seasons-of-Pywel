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
// snows (DEEP_SNOW rising from DEEP_SNOW_LEAST to DEEP_SNOW_MOST) and melts
// after a while without snow; the deeper the snow, the colder winter gets
// (COLDER_AT_FULL_DEPTH), and the line goes down as much.
// ---------------------------------------------------------------------------

static const uintptr_t CLIMATE_VTABLE = 0x5AD1A10;
static const uint32_t MAP_SIZE = 512;
static const uintptr_t SNOW_START = 0x6D63848, SNOW_FULL = 0x6D63898, DEEP_SNOW = 0x6D638E8, DEEP_SNOW_FULL = 0x6D63938;
static const float DEEP_SNOW_NONE = -45.0f;     // below winter's climate texture (-40): no deep snow

// Each season's days and nights against the game's own (summer is hot).
static const float SHIFT[SEASON_COUNT] = { -6.0f, 4.0f, -8.0f, -24.0f };   // spring, summer, autumn, winter
// DEEP_SNOW as the snow deepens: the game's (-20, the snow mountains only)
// with none, then from DEEP_SNOW_LEAST to DEEP_SNOW_MOST - the middle lands'
// winter days with winter LINE_WINTER degrees against the game's - so it
// spreads from the colder places. Where winter is colder (SHIFT, the snow's
// depth) the line goes down as much (SnowLine), so it spreads the same.
static const float DEEP_SNOW_LEAST = 0.0f, DEEP_SNOW_MOST = 6.0f, LINE_WINTER = -22.0f;
// Winter's days and nights this much colder at full depth (not in the desert).
static const float COLDER_AT_FULL_DEPTH = 6.0f;
// The game hurts the cold from about -40 degrees (tested without cold gear):
// no season makes a night colder than this where the game's own are warmer
// (guards froze to death in chilly forts); colder places keep the game's own.
static const float NIGHT_FLOOR = -30.0f;

static std::vector<uint32_t> g_map[SEASON_COUNT];   // in the game's layout; empty: none
static std::vector<uintptr_t> g_objects;
static volatile LONG g_target = SUMMER;
static HANDLE g_wake = NULL;
static char g_ini[MAX_PATH];

// The climate map's R is the wind (R * 0.25; the game's own text for the
// texture says so, about 60 in the snowy north): every season keeps the
// land's own.
static std::vector<uint8_t> g_wintry;              // 1: winter's texel (0: the desert, as in summer)
static std::vector<uint32_t> g_lineMap;            // winter's days at LINE_WINTER (the top byte only)
static std::vector<uint8_t> g_least;               // winter's coldest day top (byte) for NIGHT_FLOOR
static std::vector<uint32_t> g_deepMap;
static int g_deepMapPercent = -1;

static volatile LONG g_test = CLIMATE_TEST_NONE;   // the day's top temperature everywhere (test)
static std::vector<uint32_t> g_testMap;

static volatile LONG g_deepOn = 1;                 // deep snow on (the player's choice)
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

// The coldest midday around the camera in a map, as the game's deep snow
// test takes it (rva 3DC0C50: 3x3 texels of 80 m); wintry: that texel is
// winter's (g_wintry).
static bool ColdestAround(const std::vector<uint32_t>& m, float* coldest, bool* wintry)
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
                size_t i = (size_t)r * MAP_SIZE + c;
                float t = Top(m[i]);

                if (t < least)
                {
                    least = t;
                    *wintry = i < g_wintry.size() && g_wintry[i];
                }
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
// the winter texture, when it is not. land: the map the line was set for
// (g_lineMap); percent: the snow's depth; colder: how much colder winter's
// texels are in the map the game gets (SHIFT, Deepened). Where the coldest
// texel around is winter's, the line goes down as much, so the game's own
// test takes the same places.
static void SnowLine(Season season, const std::vector<uint32_t>& land, int percent, float colder)
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

    float deep = percent / 100.0f;
    float line = percent <= 0 ? g_gameDeep : DEEP_SNOW_LEAST + (DEEP_SNOW_MOST - DEEP_SNOW_LEAST) * deep;
    float wantDeep = line, wantDeepFull = percent <= 0 ? g_gameDeepFull : line - 10.0f;

    if (season == WINTER)
    {
        float coldest;
        bool wintry = false;
        bool due = percent > 0 && ColdestAround(land, &coldest, &wintry) && coldest < line;

        if (!due)
        {
            wantDeep = DEEP_SNOW_NONE;
            wantDeepFull = DEEP_SNOW_NONE - 5.0f;
        }
        else if (wintry)
        {
            wantDeep = line - colder;
            wantDeepFull = wantDeep - 10.0f;
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

// Day to night in a texel (G; rva 3DC0FE0): the night is this much colder.
static float Range(uint32_t texel)
{
    return (1.0f - ((texel >> 16) & 0xFF) / 255.0f) * 18.0f + 7.0f;
}

// The texel's top byte for a day's top temperature, rounded up (a floor).
static uint32_t TopByteUp(float degrees)
{
    int b = (int)ceilf((degrees / 50.0f + 1.0f) / 2.0f * 255.0f - 0.001f);
    return (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

// The coldest day top byte a season may give a texel (rest: its G): its
// night no colder than NIGHT_FLOOR.
static uint32_t Least(uint32_t rest)
{
    return TopByteUp(NIGHT_FLOOR + Range(rest));
}

// The game blends the 2x2 texels around a spot (rva 269F080): where its own
// nights are colder than NIGHT_FLOOR, that texel and the ones next to it
// keep the game's own temperatures, so the blend there is the game's own.
static std::vector<uint8_t> GameCold(const std::vector<uint32_t>& game)
{
    std::vector<uint8_t> cold(game.size(), 0);

    for (int r = 0; r < (int)MAP_SIZE; ++r)
        for (int c = 0; c < (int)MAP_SIZE; ++c)
        {
            uint32_t t = game[(size_t)r * MAP_SIZE + c];

            if (Top(t) - Range(t) >= NIGHT_FLOOR)
                continue;

            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc)
                    if (r + dr >= 0 && c + dc >= 0 && r + dr < (int)MAP_SIZE && c + dc < (int)MAP_SIZE)
                        cold[(size_t)(r + dr) * MAP_SIZE + c + dc] = 1;
        }

    return cold;
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
// as in summer - but not where summer's is the winter texture's snow already
// (-40 degrees, patches in the snow mountains).
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

    size_t kept = 0, floored[SEASON_COUNT] = {}, own = 0;
    std::vector<uint8_t> cold = game.size() == (size_t)MAP_SIZE * MAP_SIZE ? GameCold(game) : std::vector<uint8_t>(game.size(), 0);
    g_least.assign(game.size(), 0);
    g_wintry.assign(game.size(), 0);
    g_lineMap.assign(game.size(), 0);

    for (int s = 0; s < SEASON_COUNT; ++s)
    {
        std::vector<uint32_t>& m = g_map[s];
        m.resize(game.size());

        for (size_t i = 0; i < game.size(); ++i)
        {
            uint32_t rest = game[i] & 0x00FFFFFF;
            float shift = SHIFT[s];
            bool warm = false;

            if (s == WINTER && haveWinter)
            {
                if (winter[i] == game[i] && Top(game[i]) > -40.0f)
                {
                    shift = SHIFT[SUMMER];
                    warm = true;
                    ++kept;
                }
                else
                {
                    // The winter texture's G and A; R (the wind) stays the
                    // land's own. Where the game is that cold (GameCold),
                    // its own G too.
                    rest = (winter[i] & 0x00FF00FF) | (game[i] & 0x0000FF00);

                    if (cold[i])
                        rest = (rest & 0xFF00FFFF) | (game[i] & 0x00FF0000);
                    else
                        g_wintry[i] = 1;
                }
            }

            // Where the game is that cold, its own temperatures (GameCold).
            if (shift < 0 && cold[i])
            {
                shift = 0;
                own += s == WINTER;
            }

            uint32_t b = TopByte(Top(game[i]) + shift);

            if (shift < 0)
            {
                uint32_t least = Least(rest);

                if (b < least)
                {
                    b = least;
                    ++floored[s];
                }

                if (s == WINTER)
                    g_least[i] = (uint8_t)least;
            }

            m[i] = rest | (b << 24);

            // The line over the land's winter days; where the season keeps
            // the game's (the desert, the game's cold), over those.
            if (s == WINTER)
                g_lineMap[i] = (g_wintry[i] ? TopByte(Top(game[i]) + LINE_WINTER) : b) << 24;
        }
    }

    Log("climate: spring %+.0f, summer %+.0f, autumn %+.0f, winter %+.0f degrees against the game's (%zu texels warm in winter: the desert)",
        SHIFT[SPRING], SHIFT[SUMMER], SHIFT[AUTUMN], SHIFT[WINTER], kept);
    Log("climate: nights no colder than %.0f: %zu texels held there in spring, %zu in autumn, %zu in winter; %zu keep the game's own cold",
        NIGHT_FLOOR, floored[SPRING], floored[AUTUMN], floored[WINTER], own);
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

// How much colder winter is with the snow this deep (whole percents).
static float Colder(int percent)
{
    return COLDER_AT_FULL_DEPTH * (percent / 100.0f);
}

// Winter's map as deep as the snow (whole percents): the days and nights
// colder (Colder, down to NIGHT_FLOOR; the desert and the game's cold stay
// as they are).
static const std::vector<uint32_t>& Deepened(int percent)
{
    if (percent != g_deepMapPercent || g_deepMap.size() != g_map[WINTER].size())
    {
        g_deepMap = g_map[WINTER];
        float colder = Colder(percent);

        for (size_t i = 0; i < g_deepMap.size() && i < g_wintry.size() && i < g_least.size() && colder > 0; ++i)
            if (g_wintry[i])
            {
                uint32_t t = g_deepMap[i], b = TopByte(Top(t) - colder);
                g_deepMap[i] = (t & 0x00FFFFFF) | ((b > g_least[i] ? b : g_least[i]) << 24);
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
        float deep = deepTest >= 0 ? deepTest / 100.0f : g_deepOn ? WeatherSnowDepth() : 0.0f;
        int percent = (int)(deep * 100 + 0.5f);
        InterlockedExchange(&g_deepShown, percent);

        const std::vector<uint32_t>* pick = &g_map[target];

        if (pick->empty())
            continue;

        // How much colder winter's texels are than the line was set for:
        // the season's (SHIFT against LINE_WINTER) and the snow's.
        float colder = 0;

        if (target == WINTER)
        {
            pick = &Deepened(percent);
            colder = LINE_WINTER - SHIFT[WINTER] + Colder(percent);
        }

        LONG test = g_test;

        if (test != CLIMATE_TEST_NONE)
        {
            colder = 0;     // the test's temperature everywhere, not the season's

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

        // The line over winter's days as it was set for (g_lineMap).
        bool setFor = target == WINTER && test == CLIMATE_TEST_NONE && g_lineMap.size() == m.size();

        if (!g_objects.empty())
            SnowLine(target, setFor ? g_lineMap : m, percent, setFor ? colder : 0.0f);

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
    char deep[8] = { 0 };
    GetPrivateProfileStringA("Seasons", "deep_snow", "default", deep, sizeof(deep), ini);
    g_deepOn = _stricmp(deep, "off") != 0 && _stricmp(deep, "0") != 0 && _stricmp(deep, "no") != 0 && _stricmp(deep, "false") != 0;
    Log("climate: deep snow %s", g_deepOn ? "default" : "off");
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

void ClimateSetDeepSnow(bool on)
{
    InterlockedExchange(&g_deepOn, on ? 1 : 0);

    if (g_wake)
        SetEvent(g_wake);
}

bool ClimateDeepSnow() { return g_deepOn != 0; }
