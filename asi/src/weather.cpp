#include "weather.h"
#include "archive.h"
#include "log.h"
#include "menu.h"

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "MinHook.h"

// ---------------------------------------------------------------------------
// The HUD's weather (the icon next to the clock) comes from rva 17FA700
// (actor, u32* flags, bool*): a bit per weather - 0 rainy, 1 snowy, 2 windy,
// 3 fog, 4 cloudy, 5 sunny (the names at rva 5912E88); the HUD shows the
// lowest bit set. The hook keeps the flags of every call.
//
// The weather itself: each tick the climate manager composes its weather
// layers (the weather sequences) into one WeatherConstant (rva 3DC0340, from
// its update rva 3DC4730, into [[manager+0x60]+0x18]): +0x12C precipitation, +0x130 cloudiness,
// +0x134 humidity, +0x168 snow, +0x16C rain. The rain and snow effects (rva
// 3DC3A10 / 3DC3AC0) and the HUD read it. After the game composes it, the
// season's weather goes in: spells of snow (winter) or rain, as often as the
// season has them; the game's own rain turns to snow in winter and is rarer
// in summer.
// ---------------------------------------------------------------------------

const wchar_t* const WEATHER_NAMES[WEATHER_UNKNOWN + 1] = { L"Rainy", L"Snowy", L"Windy", L"Fog", L"Cloudy", L"Sunny", L"Not known yet" };

static const uintptr_t WEATHER_QUERY = 0x17FA700;
static const unsigned char QUERY_START[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                             0x48, 0x83, 0xEC, 0x60 };

// The compose starts with a VEX instruction the hook library cannot move: the
// climate manager's update that calls it is hooked instead (rva 3DC4730:
// compose, then the effects' bookkeeping); the season's weather goes in after.
static const uintptr_t WEATHER_UPDATE = 0x3DC4730;
static const unsigned char UPDATE_START[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x50, 0xC5, 0xF8, 0x29, 0x74,
                                              0x24, 0x40 };

static const uintptr_t PRECIPITATION = 0x12C, CLOUDINESS = 0x130, HUMIDITY = 0x134, SNOW = 0x168, RAIN = 0x16C;

// The composed atmosphere ([[manager+0x60]+0x20]): +0xA8 the cloud cover the
// sky shows (the game's about 0.45; 1 is overcast - found by trying).
static const uintptr_t CLOUD_COVER = 0xA8;
static const float CLEAR_COVER = 0.25f;

typedef void (*QueryFn)(uintptr_t, uint32_t*, uint8_t*);
typedef void (*UpdateFn)(uintptr_t, float);
static QueryFn g_query = NULL;
static UpdateFn g_update = NULL;
static volatile LONG g_flags = -1;

// Each season's spells, in game hours: how often one comes, how long, how strong.
struct SeasonWeather
{
    float chance;               // of a spell at each roll
    float shortest, longest;    // game hours
    float weakest, strongest;
};

static const SeasonWeather SEASON_WEATHER[SEASON_COUNT] = {
    { 0.30f, 2.0f, 5.0f, 0.3f, 0.8f },     // spring: some rain
    { 0.08f, 1.0f, 3.0f, 0.3f, 0.7f },     // summer: rain now and then
    { 0.50f, 3.0f, 6.0f, 0.4f, 1.0f },     // autumn: rain often
    { 0.60f, 3.0f, 8.0f, 0.5f, 1.0f },     // winter: snow often
};

static const float DRY_SHORTEST = 4.0f, DRY_LONGEST = 8.0f;   // game hours between rolls when it is dry
static const float RAMP = 4.0f;                                // strength a game hour (a quarter hour to come)
static const float NATIVE_SUMMER_RAIN = 0.25f;                 // what is left of the game's own rain in summer

// The snow's depth, in game hours: deepest after DEPTH_BUILD of heavy snow;
// after DEPTH_KEEP without snow it melts over DEPTH_MELT (DEPTH_THAW after winter).
static const float DEPTH_BUILD = 5.0f, DEPTH_KEEP = 5.0f, DEPTH_MELT = 10.0f, DEPTH_THAW = 3.0f;

static volatile LONG g_force = WEATHER_FORCE_NONE;
static volatile float g_amount = 0;          // the spell's strength now (0..1), as it comes and goes
static volatile float g_target = 0;
static volatile LONG g_spell = 0;
static double g_clock = 0;                   // game hours since the mod started counting
static double g_until = 0.5;                 // when the spell (or the dry time) ends
static double g_lastSnow = 0;
static volatile float g_depth = 0;           // the snow's depth (0..1)
static volatile float g_hour = -1;           // the game's hour, read in the climate update
static DWORD g_ticking = 0;                  // when the clock last moved on as the game runs
static char g_ini[MAX_PATH];

// The snow's depth by the game's time (day * 24 + hour): the game's saves do
// not keep it, so a world that loads gets the depth last recorded at its
// time, and how long it had been dry then (Seasons\snow_depth.txt, a line a
// quarter game hour; the latest play of those hours counts).
struct DepthAt
{
    double time;
    float depth;
    float dry;                  // game hours since it last snowed (0: not known)
};

static std::vector<DepthAt> g_history;      // as written, the latest last
static char g_historyPath[MAX_PATH];
static const double RECORD_EVERY = 0.25;    // game hours
static const size_t HISTORY_MOST = 60000, HISTORY_KEEP = 40000;    // about 400 game days kept
static const float DRY_MOST = 99.0f;

static float Depth01(float d) { return d < 0 ? 0 : d > 1 ? 1 : d; }

static void WriteLine(FILE* f, const DepthAt& d)
{
    fprintf(f, "%.3f %.3f %.2f\n", d.time, d.depth, d.dry);
}

// A line a record; a damaged line is skipped.
static void LoadHistory()
{
    FILE* f = NULL;

    if (!g_historyPath[0] || fopen_s(&f, g_historyPath, "r") != 0 || !f)
        return;

    char line[64];

    while (fgets(line, sizeof(line), f))
    {
        DepthAt d = { 0, 0, 0 };
        int n = sscanf_s(line, "%lf %f %f", &d.time, &d.depth, &d.dry);

        if (n >= 2 && d.time >= 0)
        {
            d.depth = Depth01(d.depth);
            d.dry = n < 3 || d.dry < 0 ? 0 : d.dry > DRY_MOST ? DRY_MOST : d.dry;
            g_history.push_back(d);
        }
    }

    fclose(f);
}

static void Record(double time, float depth, float dry)
{
    DepthAt d = { time, depth, dry < 0 ? 0 : dry > DRY_MOST ? DRY_MOST : dry };
    g_history.push_back(d);

    if (!g_historyPath[0])
        return;

    FILE* f = NULL;

    if (g_history.size() <= HISTORY_MOST)
    {
        if (fopen_s(&f, g_historyPath, "a") == 0 && f)
        {
            WriteLine(f, d);
            fclose(f);
        }

        return;
    }

    // Too long: the latest kept, written anew and put in place.
    g_history.erase(g_history.begin(), g_history.end() - HISTORY_KEEP);
    char fresh[MAX_PATH];

    if (strlen(g_historyPath) + 5 >= MAX_PATH)
        return;

    sprintf_s(fresh, "%s.new", g_historyPath);

    if (fopen_s(&f, fresh, "w") != 0 || !f)
        return;

    for (const DepthAt& h : g_history)
        WriteLine(f, h);

    fclose(f);
    MoveFileExA(fresh, g_historyPath, MOVEFILE_REPLACE_EXISTING);
}

// The record last written for a time (up to a game hour before it; the
// world's own time is read a moment after it loads).
static const DepthAt* DepthAtTime(double time)
{
    for (size_t i = g_history.size(); i-- > 0;)
        if (g_history[i].time <= time + 0.02 && time - g_history[i].time <= 1.0)
            return &g_history[i];

    return NULL;
}

static void HookQuery(uintptr_t actor, uint32_t* flags, uint8_t* indoors)
{
    g_query(actor, flags, indoors);

    if (flags)
    {
        LONG was = InterlockedExchange(&g_flags, (LONG)*flags);

        if (was != (LONG)*flags)
        {
            Weather w = WeatherNow();
            Log("weather: %S (flags %X)", WEATHER_NAMES[w], *flags);
        }
    }
}

static float Max(float a, float b) { return a > b ? a : b; }

static void Season_(uintptr_t manager)
{
    __try
    {
        uintptr_t layers = *(uintptr_t*)(manager + 0x60);

        if (!layers)
            return;

        float* w = *(float**)(layers + 0x18);
        uint8_t* atmosphere = *(uint8_t**)(layers + 0x20);

        if (!w)
            return;

        float* cover = atmosphere ? (float*)(atmosphere + CLOUD_COVER) : NULL;

        float* precipitation = (float*)((uint8_t*)w + PRECIPITATION);
        float* cloud = (float*)((uint8_t*)w + CLOUDINESS);
        float* humidity = (float*)((uint8_t*)w + HUMIDITY);
        float* snow = (float*)((uint8_t*)w + SNOW);
        float* rain = (float*)((uint8_t*)w + RAIN);
        Season season = ArchiveSeason();
        LONG force = g_force;

        if (force == WEATHER_FORCE_CLEAR)
        {
            *precipitation = *snow = *rain = 0;

            if (cover && *cover > CLEAR_COVER)
                *cover = CLEAR_COVER;

            return;
        }

        // The game's own rain: snow in winter, rarer in summer.
        if (season == WINTER)
        {
            *snow = Max(*snow, *rain);
            *rain = 0;
        }
        else if (season == SUMMER)
            *rain *= NATIVE_SUMMER_RAIN;

        float amount = force == WEATHER_FORCE_RAIN || force == WEATHER_FORCE_SNOW ? 1.0f : g_amount;

        if (amount <= 0.001f)
            return;

        bool snowing = force == WEATHER_FORCE_SNOW || (force != WEATHER_FORCE_RAIN && season == WINTER);

        if (snowing)
            *snow = Max(*snow, amount);
        else
            *rain = Max(*rain, amount);

        *precipitation = Max(*precipitation, amount);
        *humidity = Max(*humidity, amount);
        *cloud = Max(*cloud, 0.5f + 0.5f * amount);

        // Overcast as the spell strengthens.
        if (cover)
            *cover = Max(*cover, *cover + (1.0f - *cover) * (amount < 0.7f ? amount / 0.7f : 1.0f));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// The game's hour: the climate manager's clock ([manager+0xE8], virtual
// +0x100), as the climate code asks it (rva 3DC1070).
typedef float (*HourFn)(uintptr_t);

static void ReadHour(uintptr_t manager)
{
    __try
    {
        uintptr_t clock = *(uintptr_t*)(manager + 0xE8);

        if (clock)
        {
            HourFn hour = (HourFn)(*(uintptr_t**)clock)[0x100 / 8];
            float h = hour(clock);

            if (h >= 0 && h <= 24.0f)
                g_hour = h;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void HookUpdate(uintptr_t manager, float dt)
{
    g_update(manager, dt);
    Season_(manager);
    ReadHour(manager);
}

static float Between(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

static void KeepDepth(float before)
{
    if ((int)(before * 10) != (int)(g_depth * 10) && g_ini[0])
    {
        char v[16];
        sprintf_s(v, "%.2f", g_depth);
        WritePrivateProfileStringA("Seasons", "snow_depth", v, g_ini);
        Log("weather: snow depth %d%%", (int)(g_depth * 100 + 0.5f));
    }
}

// A step of game time (a quarter hour at most): the spells, their strength
// coming and going, the snow's depth.
static void Step(double hours, bool live)
{
    g_clock += hours;
    Season season = ArchiveSeason();

    if (g_clock >= g_until)
    {
        const SeasonWeather& sw = SEASON_WEATHER[season];

        if (!g_spell && Between(0.0f, 1.0f) < sw.chance)
        {
            float length = Between(sw.shortest, sw.longest);
            g_target = Between(sw.weakest, sw.strongest);
            g_until = g_clock + length;
            g_spell = 1;
            Log("weather: %s for %.1f game hours (strength %.0f%%)", season == WINTER ? "snow" : "rain", length, g_target * 100);
        }
        else
        {
            float length = Between(DRY_SHORTEST, DRY_LONGEST);

            if (g_spell)
                Log("weather: the %s stops (dry for %.1f game hours at least)", season == WINTER ? "snow" : "rain", length);

            g_until = g_clock + length;
            g_target = 0;
            g_spell = 0;
        }
    }

    float a = g_amount, t = g_target, step = (float)(RAMP * hours);
    g_amount = a < t ? (a + step > t ? t : a + step) : (a - step < t ? t : a - step);

    // The depth: snow (the season's, a forced one, or the game's own).
    LONG force = g_force;
    float snowing = force == WEATHER_FORCE_SNOW ? 1.0f : force == WEATHER_FORCE_NONE && season == WINTER ? g_amount : 0.0f;

    // The game's own snow, by the HUD - only while the game runs (in skipped
    // time the HUD still shows what was there before).
    if (live && season == WINTER && force != WEATHER_FORCE_CLEAR && WeatherNow() == SNOWY && snowing < 0.6f)
        snowing = 0.6f;

    float before = g_depth, depth = g_depth;

    if (snowing > 0.05f)
    {
        depth += (float)(hours / DEPTH_BUILD) * (snowing > 0.4f ? snowing : 0.4f);
        g_lastSnow = g_clock;
    }
    else if (season != WINTER)
        depth -= (float)(hours / DEPTH_THAW);
    else if (g_clock - g_lastSnow > DEPTH_KEEP)
        depth -= (float)(hours / DEPTH_MELT);

    g_depth = depth < 0 ? 0 : depth > 1 ? 1 : depth;
    KeepDepth(before);
}

// Follows the game's hour; a paused game stops the weather too. A jump of the
// clock is either time slept or waited through (played through in steps) or
// a world loading (a loading screen's clock goes to 0.00, then to the
// world's hour). Which one is known once the world's time is: the day the
// HUD shows standing for 2 seconds and the clock running again (not in the
// main menu or a loading screen). A world that loads - the first, or one at
// a time the clock did not get to - takes the depth it had then.
static DWORD WINAPI DirectorThread(LPVOID)
{
    srand(GetTickCount());
    float last = -1;
    int day = 0;
    DWORD dayAt = 0, jumpAt = 0;
    bool loaded = true;
    bool dip = false;           // the clock jumped to 0.00 and has not run since
    double lastTime = -1;       // the world's time last known (day * 24 + hour)
    double since = 0;           // game hours the clock ran on since
    double skipped = 0;         // game hours it jumped since
    double recorded = -1;
    float recordedDepth = -1;

    for (;;)
    {
        Sleep(250);
        float hour = g_hour;

        if (hour < 0)
            continue;

        if (last < 0)
        {
            last = hour;
            Log("weather: the game's clock at %.2f", hour);
            continue;
        }

        double passed = hour - last;

        if (passed < 0)
            passed += 24.0;     // past midnight (or a sleep over it)

        last = hour;
        DWORD now = GetTickCount();

        if (passed <= 0.5)
        {
            if (passed > 0)
            {
                // The clock runs again after a jump that stood: the HUD's
                // day may follow from here.
                if (skipped > 0 && now - g_ticking > 2000)
                    jumpAt = now;

                g_ticking = now;
                dip = false;
            }

            for (double left = passed; left > 0; left -= 0.25)
                Step(left < 0.25 ? left : 0.25, true);

            since += passed;
        }
        else
        {
            Log("weather: the clock jumped %.1f game hours to %.2f", passed, hour);

            // 0.00 and then the world's hour, with no time between: a
            // loading screen (a sleep is one jump, and the clock runs after).
            if (dip && !loaded)
            {
                Log("weather: a loading screen - a world loaded");
                loaded = true;
            }

            dip = hour < 0.01f;
            skipped += passed;
            dayAt = jumpAt = now;       // the HUD's day may follow a moment later
        }

        int shown = GameDay();

        if (shown != day)
        {
            day = shown;
            dayAt = now;
        }

        // Just after midnight the day may change a moment after the hour.
        if (day <= 0 || now - dayAt < 2000 || !g_ticking || now - g_ticking > 2000 || hour < 0.25f)
            continue;

        double time = day * 24.0 + hour;
        double expected = lastTime + since + skipped;

        // No sleep skips a whole day.
        if (!loaded && skipped >= 24.0)
        {
            Log("weather: the clock jumped %.1f game hours in all - a world loaded", skipped);
            loaded = true;
        }

        // A day behind after a jump past midnight: the HUD's day not turned
        // yet (waited for up to 10 seconds).
        if (!loaded && lastTime >= 0 && fabs(time + 24.0 - expected) <= 1.0 && now - jumpAt < 10000)
            continue;

        if (!loaded && lastTime >= 0 && fabs(time - expected) > 1.0)
        {
            Log("weather: the game's time went from day %d %.2f to day %d %.2f (a world loaded)", (int)(lastTime / 24), fmod(lastTime, 24.0),
                day, hour);
            loaded = true;
        }

        if (loaded)
        {
            float before = g_depth;
            const DepthAt* then = DepthAtTime(time);

            // How long it had been dry then (not known: the depth holds a while).
            g_lastSnow = g_clock;

            if (then)
            {
                g_depth = then->depth;
                g_lastSnow = g_clock - then->dry;
                Log("weather: day %d %.2f - snow depth %d%%, dry for %.1f game hours (as it was then)", day, hour,
                    (int)(then->depth * 100 + 0.5f), then->dry);
            }
            else if (!g_history.empty())
            {
                g_depth = 0;
                Log("weather: day %d %.2f - no snow depth known for that time: none", day, hour);
            }
            else
                Log("weather: day %d %.2f - snow depth %d%% (nothing recorded yet)", day, hour, (int)(g_depth * 100 + 0.5f));

            KeepDepth(before);
            recorded = -1;
        }
        else if (skipped > 0)
        {
            // Slept or waited through.
            for (double left = skipped; left > 0; left -= 0.25)
                Step(left < 0.25 ? left : 0.25, false);

            Log("weather: %.1f game hours went by (day %d %.2f), snow depth %d%%", skipped, day, hour, (int)(g_depth * 100 + 0.5f));
        }

        loaded = false;
        lastTime = time;
        since = skipped = 0;

        if (recorded < 0 || fabs(time - recorded) >= RECORD_EVERY || fabsf(g_depth - recordedDepth) >= 0.02f)
        {
            Record(time, g_depth, (float)(g_clock - g_lastSnow));
            recorded = time;
            recordedDepth = g_depth;
        }

    }
}

// Hooked once the game runs (its code is checked first: another version
// might have something else there).
static DWORD WINAPI HookThread(LPVOID)
{
    uint8_t* module = (uint8_t*)GetModuleHandleW(NULL);
    uint8_t* query = module + WEATHER_QUERY;
    uint8_t* update = module + WEATHER_UPDATE;

    for (int tries = 0; tries < 60; ++tries)
    {
        Sleep(2000);

        if (memcmp(query, QUERY_START, sizeof(QUERY_START)) != 0)
            continue;

        if (MH_CreateHook(query, (void*)&HookQuery, (void**)&g_query) != MH_OK || MH_EnableHook(query) != MH_OK)
            Log("weather: could not hook the HUD's weather query");
        else
            Log("weather: watching the HUD's weather");

        MH_STATUS st = MH_UNKNOWN;

        if (memcmp(update, UPDATE_START, sizeof(UPDATE_START)) != 0)
            Log("weather: the game's weather update was not found (another game version?) - the season's weather is off");
        else if ((st = MH_CreateHook(update, (void*)&HookUpdate, (void**)&g_update)) != MH_OK || (st = MH_EnableHook(update)) != MH_OK)
            Log("weather: could not hook the game's weather update (%d)", (int)st);
        else
        {
            Log("weather: the season's weather is on");
            CloseHandle(CreateThread(NULL, 0, DirectorThread, NULL, 0, NULL));
        }

        return 0;
    }

    Log("weather: the HUD's weather query was not found (another game version?)");
    return 0;
}

void WeatherInit(const char* ini)
{
    strcpy_s(g_ini, ini);
    char v[16];
    GetPrivateProfileStringA("Seasons", "snow_depth", "0", v, sizeof(v), ini);
    float depth = (float)atof(v);
    g_depth = depth < 0 ? 0 : depth > 1 ? 1 : depth;

    // Next to the ini (none if the path would be too long).
    char folder[MAX_PATH];
    strcpy_s(folder, ini);
    char* slash = strrchr(folder, '\\');
    *(slash ? slash : folder) = 0;

    g_historyPath[0] = 0;

    if (strlen(folder) + 16 < MAX_PATH)
        sprintf_s(g_historyPath, "%s\\snow_depth.txt", folder);

    LoadHistory();
    Log("weather: snow depth %d%%, %zu times of it recorded", (int)(g_depth * 100 + 0.5f), g_history.size());
    CloseHandle(CreateThread(NULL, 0, HookThread, NULL, 0, NULL));
}

Weather WeatherNow()
{
    LONG f = g_flags;

    if (f < 0)
        return WEATHER_UNKNOWN;

    for (int i = 0; i < WEATHER_UNKNOWN; ++i)
        if (f & (1 << i))
            return (Weather)i;

    return WEATHER_UNKNOWN;
}

void WeatherForce(int force)
{
    InterlockedExchange(&g_force, force);
    Log("weather: %s", force == WEATHER_FORCE_CLEAR ? "clear (test)" : force == WEATHER_FORCE_RAIN ? "rain (test)" :
                       force == WEATHER_FORCE_SNOW ? "snow (test)" : "the season's");
}

int WeatherForced() { return g_force; }

int WeatherSpellPercent() { return (int)(g_amount * 100 + 0.5f); }

float WeatherHoursLeft()
{
    double left = g_until - g_clock;
    return left > 0 ? (float)left : 0;
}

float WeatherSnowDepth() { return g_depth; }

bool WeatherSpell() { return g_spell != 0; }
