#include "menu.h"
#include "archive.h"
#include "climate.h"
#include "weather.h"
#include "log.h"
#include "overlay.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

// ---------------------------------------------------------------------------
// The Control Menu (F5), drawn like Character Creator's: a panel on the right
// with rows to choose with the arrows.
// ---------------------------------------------------------------------------

// The day the HUD shows: a u32 in the game's data (it went 76 -> 77 with the
// HUD; research/notes.md). Unconfirmed across game versions.
static const uintptr_t DAY_RVA = 0x6A6D218;

static char g_ini[MAX_PATH];
static volatile LONG g_auto = 1;         // the season by the day (or fixed)
static SRWLOCK g_lock = SRWLOCK_INIT;

enum Row { ROW_MODE, ROW_LENGTH, ROW_SEASON, ROW_WEATHER, ROW_COLD, ROW_DEEP, ROW_DAY, ROW_COUNT };

// Test: the calendar on the first day of a season (the year starts in
// summer: summer, autumn, winter, spring).
static const wchar_t* const DAY_LABELS[] = { L"Game's", L"Summer", L"Autumn", L"Winter", L"Spring" };
static const int DAY_COUNT = 5;
static int g_dayChoice = 0;

// Test: the weather forced (Season: the season's).
static const wchar_t* const WEATHER_LABELS[] = { L"Season", L"Clear", L"Rain", L"Snow" };
static const int WEATHER_FORCES[] = { WEATHER_FORCE_NONE, WEATHER_FORCE_CLEAR, WEATHER_FORCE_RAIN, WEATHER_FORCE_SNOW };
static const int WEATHER_COUNT = 4;

// The test rows (weather, top temperature, deep snow) only with tests=1 in
// the ini: players get the calendar and the season.
static bool g_tests = false;

static bool Shown(int row)
{
    return g_tests || (row != ROW_WEATHER && row != ROW_COLD && row != ROW_DEEP && row != ROW_DAY);
}

// By the day, the season and the weather follow the calendar: locked.
static bool Locked(int row)
{
    return g_auto && (row == ROW_SEASON || row == ROW_WEATHER);
}

static bool Selectable(int row) { return Shown(row) && !Locked(row); }

static int WeatherIndex()
{
    for (int i = 0; i < WEATHER_COUNT; ++i)
        if (WEATHER_FORCES[i] == WeatherForced())
            return i;

    return 0;
}

// Test: the day's top temperature everywhere, live.
static const wchar_t* const COLD_LABELS[] = { L"Season", L"-40", L"-35", L"-30", L"-25", L"-20", L"-10", L"0", L"+5" };
static const int COLD_DEGREES[] = { CLIMATE_TEST_NONE, -40, -35, -30, -25, -20, -10, 0, 5 };
static const int COLD_COUNT = 9;

static int ColdIndex()
{
    for (int i = 0; i < COLD_COUNT; ++i)
        if (COLD_DEGREES[i] == ClimateTestDegrees())
            return i;

    return 0;
}

// Test: how deep the snow is (Snowfall: as the snowfall made it).
static const wchar_t* const DEEP_LABELS[] = { L"Snowfall", L"None", L"Half", L"Deepest" };
static const int DEEP_PERCENT[] = { -1, 0, 50, 100 };
static const int DEEP_COUNT = 4;

static int DeepIndex()
{
    for (int i = 0; i < DEEP_COUNT; ++i)
        if (DEEP_PERCENT[i] == ClimateDeepTestPercent())
            return i;

    return 0;
}

static const wchar_t* const SEASON_LABELS[SEASON_COUNT] = { L"Spring", L"Summer", L"Autumn", L"Winter" };
static const wchar_t* const MODE_LABELS[] = { L"By the day", L"Fixed" };
static const wchar_t* const LENGTH_LABELS[] = { L"3 days", L"7 days", L"14 days", L"30 days" };
static const int LENGTHS[] = { 3, 7, 14, 30 };
static const int LENGTH_COUNT = 4;

static int g_row = ROW_SEASON;
static int g_season = SUMMER;       // chosen in the menu (applied with Enter)

// The calendar: each season lasts g_days in-game days (day 1 starts spring),
// and in "by the day" mode the season follows the day the game shows.
static volatile LONG g_days = 30;

// The year starts in summer, as the game does.
static Season SeasonOfDay(int day)
{
    static const Season YEAR[SEASON_COUNT] = { SUMMER, AUTUMN, WINTER, SPRING };
    return YEAR[((day - 1) / g_days) % SEASON_COUNT];
}

static int NextSeasonDay(int day)
{
    return ((day - 1) / g_days + 1) * g_days + 1;
}

static int LengthIndex()
{
    for (int i = 0; i < LENGTH_COUNT; ++i)
        if (LENGTHS[i] == g_days)
            return i;

    return LENGTH_COUNT - 1;
}

static void SaveSettings()
{
    char v[16];
    WritePrivateProfileStringA("Seasons", "mode", g_auto ? "day" : "fixed", g_ini);
    sprintf_s(v, "%ld", g_days);
    WritePrivateProfileStringA("Seasons", "days_per_season", v, g_ini);
}
static std::wstring g_notice;
static DWORD g_noticeAt = 0;

int DaysPerSeason() { return g_days; }



// Test: the calendar shifted to a season's first day (0: the game's day).
static volatile LONG g_dayShift = 0;

static int CalendarDay()
{
    int day = GameDay();
    return day > 0 ? day + g_dayShift : 0;
}

int DayOfSeason()
{
    int day = CalendarDay();
    return day > 0 ? (day - 1) % g_days + 1 : 0;
}

int GameDay()
{
    uint32_t day = 0;

    __try
    {
        day = *(uint32_t*)((uintptr_t)GetModuleHandleW(NULL) + DAY_RVA);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return day < 100000 ? (int)day : 0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

struct Style
{
    bool ready;
    IDWriteTextFormat* title;
    IDWriteTextFormat* body;
    IDWriteTextFormat* caption;
    IDWriteTextFormat* big;
    ID2D1SolidColorBrush* gold;
    ID2D1SolidColorBrush* text;
    ID2D1SolidColorBrush* dim;
    ID2D1SolidColorBrush* line;
    ID2D1SolidColorBrush* tabOff;
    ID2D1SolidColorBrush* tabOn;
    ID2D1LinearGradientBrush* panel;
    float scale;
};

static Style g_style = {};

template <typename T> static void Release(T*& p)
{
    if (p)
    {
        p->Release();
        p = NULL;
    }
}

static IDWriteTextFormat* Font(IDWriteFactory* write, float size, DWRITE_TEXT_ALIGNMENT align)
{
    IDWriteTextFormat* f = NULL;
    write->CreateTextFormat(L"Georgia", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);

    if (f)
    {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    return f;
}

static void MakeStyle(const OverlayDrawContext& ctx)
{
    float s = ctx.height / 1080.0f;
    ID2D1DeviceContext* dc = ctx.dc;
    Style& st = g_style;

    st.scale = s;
    st.title = Font(ctx.write, 26 * s, DWRITE_TEXT_ALIGNMENT_CENTER);
    st.body = Font(ctx.write, 20 * s, DWRITE_TEXT_ALIGNMENT_LEADING);
    st.caption = Font(ctx.write, 17 * s, DWRITE_TEXT_ALIGNMENT_CENTER);
    st.big = Font(ctx.write, 22 * s, DWRITE_TEXT_ALIGNMENT_CENTER);

    dc->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.76f, 0.45f), &st.gold);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.91f, 0.87f), &st.text);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.60f, 0.56f), &st.dim);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.47f, 0.32f, 0.8f), &st.line);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.13f, 0.09f, 0.85f), &st.tabOff);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.39f, 0.15f, 0.95f), &st.tabOn);

    // Panel fades in from the left like the game's own side menus.
    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.0f) },
        { 0.12f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.82f) },
        { 1.0f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.9f) } };
    ID2D1GradientStopCollection* collection = NULL;
    dc->CreateGradientStopCollection(stops, 3, &collection);

    if (collection)
    {
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)),
            collection, &st.panel);
        collection->Release();
    }

    st.ready = st.title && st.body && st.caption && st.big && st.gold && st.text && st.dim && st.line &&
        st.tabOff && st.tabOn && st.panel;
}

static void ForgetDrawing()
{
    Style& st = g_style;
    Release(st.title); Release(st.body); Release(st.caption); Release(st.big);
    Release(st.gold); Release(st.text); Release(st.dim); Release(st.line);
    Release(st.tabOff); Release(st.tabOn); Release(st.panel);
    st = {};
}

static void Text(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b)
{
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// A row of choices: label on the left, the options as tabs.
static void Choices(ID2D1DeviceContext* dc, float y, const wchar_t* label, const wchar_t* const* options, int count,
                    int chosen, int active, bool on, float gx0, float gx1, bool locked = false)
{
    Style& st = g_style;
    float s = st.scale;
    Text(dc, on ? std::wstring(L"[ ") + label + L" ]" : label, st.body, D2D1::RectF(gx0, y, gx0 + 160 * s, y + 40 * s),
         on ? st.gold : locked ? st.dim : st.text);

    float x = gx0 + 170 * s, gap = 4 * s, w = (gx1 - x - gap * (count - 1)) / count;

    for (int i = 0; i < count; ++i)
    {
        D2D1_RECT_F r = D2D1::RectF(x + i * (w + gap), y, x + i * (w + gap) + w, y + 40 * s);
        dc->FillRectangle(r, i == active ? st.tabOn : st.tabOff);

        if (on && i == chosen)
            dc->DrawRectangle(r, st.gold, 2.0f * s);

        Text(dc, options[i], st.caption, r, locked ? (i == active ? st.text : st.dim) : i == active ? st.gold : (i == chosen ? st.text : st.dim));
    }
}

static void MenuDraw(const OverlayDrawContext& ctx)
{
    static unsigned generation = 0;

    if (ctx.generation != generation)
    {
        if (generation)
            ForgetDrawing();

        generation = ctx.generation;
    }

    if (!g_style.ready)
    {
        MakeStyle(ctx);

        if (!g_style.ready)
            return;
    }

    AcquireSRWLockExclusive(&g_lock);
    ID2D1DeviceContext* dc = ctx.dc;
    Style& st = g_style;
    float s = st.scale;

    // Panel on the right, like Character Creator's.
    float x0 = ctx.width - 624 * s, x1 = ctx.width - 24 * s;
    float y0 = 50 * s, y1 = y0 + (g_tests ? 880 : 640) * s;
    float fadeX = x0 - 90 * s;
    st.panel->SetStartPoint(D2D1::Point2F(fadeX, 0));
    st.panel->SetEndPoint(D2D1::Point2F(x1, 0));
    dc->FillRectangle(D2D1::RectF(fadeX, y0, x1, y1), st.panel);
    dc->DrawLine(D2D1::Point2F(fadeX + 70 * s, y0), D2D1::Point2F(x1, y0), st.line, 1.2f * s);
    dc->DrawLine(D2D1::Point2F(fadeX + 70 * s, y1), D2D1::Point2F(x1, y1), st.line, 1.2f * s);

    float gx0 = x0 + 24 * s, gx1 = x1 - 24 * s;
    Text(dc, L"Seasons  |  Control Menu", st.title, D2D1::RectF(x0, y0 + 18 * s, x1, y0 + 70 * s), st.gold);

    wchar_t info[160];
    int day = CalendarDay();
    Season now = ArchiveSeason();

    if (day)
        swprintf_s(info, L"Day %d    Season now: %s", day, SEASON_LABELS[now]);
    else
        swprintf_s(info, L"Day not known yet    Season now: %s", SEASON_LABELS[now]);

    Text(dc, info, st.big, D2D1::RectF(gx0, y0 + 80 * s, gx1, y0 + 116 * s), st.text);
    Text(dc, L"Up / Down: choose    Left / Right: change    Enter: apply the season", st.caption,
         D2D1::RectF(gx0, y0 + 118 * s, gx1, y0 + 146 * s), st.dim);

    float row = y0 + 170 * s;
    Choices(dc, row, L"Seasons", MODE_LABELS, 2, g_auto ? 0 : 1, g_auto ? 0 : 1, g_row == ROW_MODE, gx0, gx1);
    Choices(dc, row + 60 * s, L"Length", LENGTH_LABELS, LENGTH_COUNT, LengthIndex(), LengthIndex(), g_row == ROW_LENGTH, gx0, gx1);
    Choices(dc, row + 120 * s, L"Season", SEASON_LABELS, SEASON_COUNT, g_season, now, g_row == ROW_SEASON, gx0, gx1, Locked(ROW_SEASON));
    float next = row + 180 * s;

    if (g_tests)
    {
        Choices(dc, next, L"Weather", WEATHER_LABELS, WEATHER_COUNT, WeatherIndex(), WeatherIndex(), g_row == ROW_WEATHER, gx0, gx1,
                Locked(ROW_WEATHER));
        Choices(dc, next + 60 * s, L"Top temp.", COLD_LABELS, COLD_COUNT, ColdIndex(), ColdIndex(), g_row == ROW_COLD, gx0, gx1);
        Choices(dc, next + 120 * s, L"Deep snow", DEEP_LABELS, DEEP_COUNT, DeepIndex(), DeepIndex(), g_row == ROW_DEEP, gx0, gx1);
        Choices(dc, next + 180 * s, L"Calendar", DAY_LABELS, DAY_COUNT, g_dayChoice, g_dayChoice, g_row == ROW_DAY, gx0, gx1);
        next += 240 * s;
    }

    wchar_t weather[200];

    if (WeatherSpell())
        swprintf_s(weather, L"Weather now: %s    %s %d%%, %.0f h left    Snow depth: %d%%", WEATHER_NAMES[WeatherNow()],
                   now == WINTER ? L"Snowfall" : L"Rain", WeatherSpellPercent(), WeatherHoursLeft(), ClimateDeepPercent());
    else
        swprintf_s(weather, L"Weather now: %s    Dry, next roll in %.0f h    Snow depth: %d%%", WEATHER_NAMES[WeatherNow()],
                   WeatherHoursLeft(), ClimateDeepPercent());

    Text(dc, weather, st.body, D2D1::RectF(gx0, next, gx1, next + 40 * s), st.text);
    row = next - 360 * s;       // the texts below follow

    wchar_t plan[200];

    if (!g_auto)
        swprintf_s(plan, L"Fixed: the season stays %s until you change it.", SEASON_LABELS[now]);
    else if (day)
        swprintf_s(plan, L"%s begins on day %d (each season: %ld days).", SEASON_LABELS[SeasonOfDay(NextSeasonDay(day))],
                   NextSeasonDay(day), g_days);
    else
        swprintf_s(plan, L"The season follows the day once a game is loaded (each season: %ld days).", g_days);

    Text(dc, plan, st.caption, D2D1::RectF(gx0, row + 420 * s, gx1, row + 450 * s), st.text);
    Text(dc, L"Snow comes and goes at once; plants, grass and far terrain change as the game loads them "
             L"again (a few seconds, or after travelling).",
         st.caption, D2D1::RectF(gx0, row + 452 * s, gx1, row + 512 * s), st.dim);

    if (ArchiveSwitching())
        Text(dc, L"Changing the season...", st.big, D2D1::RectF(gx0, row + 520 * s, gx1, row + 556 * s), st.gold);
    else if (!g_notice.empty() && GetTickCount() - g_noticeAt < 4000)
        Text(dc, g_notice, st.big, D2D1::RectF(gx0, row + 520 * s, gx1, row + 556 * s), st.gold);

    float footerTop = y1 - 56 * s;
    dc->DrawLine(D2D1::Point2F(gx0, footerTop), D2D1::Point2F(gx1, footerTop), st.line, 1.0f * s);
    Text(dc, L"[F5] Close", st.big, D2D1::RectF(x0, footerTop + 8 * s, x1, y1 - 8 * s), st.text);

    ReleaseSRWLockExclusive(&g_lock);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

static void SetSeason(Season season)
{
    ArchiveSetSeason(season);
    WritePrivateProfileStringA("Seasons", "season", SEASON_NAMES[season], g_ini);
}

// Choosing a season by hand fixes it (the calendar no longer changes it).
static void Apply()
{
    if (g_row == ROW_SEASON)
    {
        InterlockedExchange(&g_auto, 0);
        SaveSettings();
        SetSeason((Season)g_season);
        g_notice = std::wstring(SEASON_LABELS[g_season]) + L" (fixed)";
        g_noticeAt = GetTickCount();
    }
}

static void Change(int step)
{
    switch (g_row)
    {
    case ROW_MODE:
        InterlockedExchange(&g_auto, g_auto ? 0 : 1);
        SaveSettings();

        if (g_auto)
            WeatherForce(WEATHER_FORCE_NONE);       // the weather follows the season again

        break;

    case ROW_DAY:
    {
        g_dayChoice = (g_dayChoice + step + DAY_COUNT) % DAY_COUNT;
        int game = GameDay();
        int want = (g_dayChoice - 1) * g_days + 1;     // the season's first day in the year
        LONG shift = !g_dayChoice || !game ? 0 : want - (game - 1) % (4 * g_days) - 1;
        InterlockedExchange(&g_dayShift, shift);
        g_notice = g_dayChoice ? std::wstring(L"Calendar: ") + DAY_LABELS[g_dayChoice] + L" (test)" : L"Calendar: the game's day";
        g_noticeAt = GetTickCount();
        Log("menu: calendar test %ls (shift %ld)", DAY_LABELS[g_dayChoice], shift);
        break;
    }

    case ROW_LENGTH:
        InterlockedExchange(&g_days, LENGTHS[(LengthIndex() + step + LENGTH_COUNT) % LENGTH_COUNT]);
        SaveSettings();
        break;

    case ROW_SEASON:
        g_season = (g_season + step + SEASON_COUNT) % SEASON_COUNT;
        break;

    case ROW_WEATHER:
    {
        int i = (WeatherIndex() + step + WEATHER_COUNT) % WEATHER_COUNT;
        WeatherForce(WEATHER_FORCES[i]);
        g_notice = i ? std::wstring(L"Weather: ") + WEATHER_LABELS[i] + L" (test)" : L"The season's weather";
        g_noticeAt = GetTickCount();
        break;
    }

    case ROW_DEEP:
    {
        int i = (DeepIndex() + step + DEEP_COUNT) % DEEP_COUNT;
        ClimateDeepTest(DEEP_PERCENT[i]);
        g_notice = i ? std::wstring(L"Deep snow: ") + DEEP_LABELS[i] : L"Deep snow follows the snowfall";
        g_noticeAt = GetTickCount();
        Log("menu: deep snow test %ls", DEEP_LABELS[i]);
        break;
    }

    case ROW_COLD:
    {
        int i = (ColdIndex() + step + COLD_COUNT) % COLD_COUNT;
        ClimateTest(COLD_DEGREES[i]);
        g_notice = i ? std::wstring(L"Top temperature everywhere: ") + COLD_LABELS[i] + L" degrees" : L"The season's temperatures";
        g_noticeAt = GetTickCount();
        Log("menu: top temperature test %ls", COLD_LABELS[i]);
        break;
    }
    }
}

static void MenuKey(int vk)
{
    AcquireSRWLockExclusive(&g_lock);

    switch (vk)
    {
    case VK_UP:
        do g_row = (g_row + ROW_COUNT - 1) % ROW_COUNT; while (!Selectable(g_row));
        break;

    case VK_DOWN:
        do g_row = (g_row + 1) % ROW_COUNT; while (!Selectable(g_row));
        break;
    case VK_LEFT:   Change(-1); break;
    case VK_RIGHT:  Change(1); break;
    case VK_RETURN: case VK_SPACE:
        Apply();
        break;
    case VK_ESCAPE:
        OverlaySetVisible(false);
        break;
    }

    ReleaseSRWLockExclusive(&g_lock);
}

// F5 opens and closes the menu (checked from a thread: the game may not pass
// the key on while the menu is closed).
static DWORD WINAPI KeyThread(LPVOID)
{
    bool was = false;

    for (;;)
    {
        Sleep(30);
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F5) & 0x8000);

        if (down && !was)
        {
            bool open = !OverlayVisible();

            if (open)
            {
                AcquireSRWLockExclusive(&g_lock);
                g_season = ArchiveSeason();
                ReleaseSRWLockExclusive(&g_lock);
            }

            OverlaySetVisible(open);
            Log("menu %s", open ? "opened" : "closed");
        }

        was = down;
    }
}

// The calendar: every two seconds, the season of the day the game shows (once
// it has shown the same day twice: loading a game changes it).
static DWORD WINAPI CalendarThread(LPVOID)
{
    int last = 0;

    for (;;)
    {
        Sleep(2000);
        int day = CalendarDay();
        bool steady = day > 0 && day == last;
        last = day;

        if (!g_auto || !steady || ArchiveSwitching())
            continue;

        Season season = SeasonOfDay(day);

        if (season != ArchiveSeason())
        {
            Log("calendar: day %d is in %s (%ld days a season)", day, SEASON_NAMES[season], g_days);
            SetSeason(season);
        }
    }
}

void MenuStart(const char* ini)
{
    strcpy_s(g_ini, ini);

    char mode[16] = { 0 };
    GetPrivateProfileStringA("Seasons", "mode", "day", mode, sizeof(mode), ini);
    g_auto = _stricmp(mode, "fixed") != 0;
    g_tests = GetPrivateProfileIntA("Seasons", "tests", 0, ini) != 0;
    int days = (int)GetPrivateProfileIntA("Seasons", "days_per_season", 30, ini);
    g_days = days >= 1 && days <= 365 ? days : 30;
    SaveSettings();
    Log("calendar: %s, %ld days a season", g_auto ? "by the day" : "fixed season", g_days);

    OverlaySetCallbacks(MenuDraw, MenuKey);
    OverlayInit();
    CloseHandle(CreateThread(NULL, 0, KeyThread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, CalendarThread, NULL, 0, NULL));
}
