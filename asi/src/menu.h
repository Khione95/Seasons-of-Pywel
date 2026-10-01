#pragma once

// The Control Menu (F5): a panel over the game with the day and a row
// per setting. ini: where the chosen season is kept for the next start.
void MenuStart(const char* ini);

// The day shown next to the minimap (0 when not known yet).
int GameDay();

// The calendar: days a season has, and the day of the season it is (1..; 0
// when the day is not known yet).
int DaysPerSeason();
int DayOfSeason();
