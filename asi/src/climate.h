#pragma once

#include "archive.h"

// The game's climate map on the processor's side: the temperature and the
// snow where the player is come from it while the game runs. It follows the
// season (winter's is cold), live; in winter deep snow builds up while it
// snows. ini: where the snow's depth is kept.
void ClimateStart(Season start, const char* ini);
void ClimateSeasonChanged(Season season);

// How deep the snow is (percent).
int ClimateDeepPercent();

// Deep snow on or off (the player's choice, ini deep_snow=default/off). Off: no
// deep snow in winter, nor the cold that comes with it.
void ClimateSetDeepSnow(bool on);
bool ClimateDeepSnow();

// Test: the day's top temperature everywhere (CLIMATE_TEST_NONE: the season's).
static const int CLIMATE_TEST_NONE = -1000;
void ClimateTest(int degrees);
int ClimateTestDegrees();

// Test: the snow's depth (percent; -1: the snowfall's).
void ClimateDeepTest(int percent);
int ClimateDeepTestPercent();
