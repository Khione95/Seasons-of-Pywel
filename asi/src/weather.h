#pragma once

// The weather where the player is, as the HUD shows it.
enum Weather { RAINY, SNOWY, WINDY, FOG, CLOUDY, SUNNY, WEATHER_UNKNOWN };

extern const wchar_t* const WEATHER_NAMES[WEATHER_UNKNOWN + 1];

// Hooks the HUD's weather and the game's weather compose (the season's
// weather: snow in winter, rain as often as the season has it).
void WeatherInit(const char* ini);
Weather WeatherNow();

// Test: the weather forced (or the season's).
enum WeatherForceMode { WEATHER_FORCE_NONE = -1, WEATHER_FORCE_CLEAR, WEATHER_FORCE_RAIN, WEATHER_FORCE_SNOW };
void WeatherForce(int force);
int WeatherForced();

// The season's spell: strength now (percent), game hours left (of it, or of
// the dry time before the next roll).
bool WeatherSpell();
int WeatherSpellPercent();
float WeatherHoursLeft();

// The snow's depth (0..1): deeper while it snows in winter, melting after a
// while without snow (on the game's clock).
float WeatherSnowDepth();
