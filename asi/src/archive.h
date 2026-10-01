#pragma once

// The game's file index (<group>/0.pamt) and texture size table
// (meta/0.pathc), patched as the game reads them and later in memory: each
// season has its own copies of the files it changes, named <name>__<season>.<ext>
// (leaf_oak_02_color__autumn.dds); a season points the original file's
// entries at its copies, summer points them back.

enum Season { SPRING, SUMMER, AUTUMN, WINTER, SEASON_COUNT };

extern const char* const SEASON_NAMES[SEASON_COUNT];

// Hooks the game's file opening and reading. start: the season the game starts in.
bool ArchiveInit(Season start);

// Switches the season live: textures change as the game streams them in again.
// Some (the climate texture: winter's snow) are only read when the game starts.
void ArchiveSetSeason(Season season);

// A season change still being applied (in the background).
bool ArchiveSwitching();

Season ArchiveSeason();

// How many files each season changes (for the menu).
int ArchiveSeasonFiles(Season season);

// The season data's stored bytes of a file in a season (as the game packs it).
#include <stdint.h>
#include <vector>
bool ArchiveSeasonFileBytes(const char* group, const char* path, Season season, std::vector<uint8_t>* out);

// Every file of the season data: where its stored bytes are.
#include <string>
struct SeasonFileRef
{
    std::string group, path;
    Season season;
    std::wstring dat;
    uint64_t offset;
    uint32_t stored, size;
};

std::vector<SeasonFileRef> ArchiveListSeasonFiles();
