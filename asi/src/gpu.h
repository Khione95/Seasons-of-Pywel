#pragma once

#include "archive.h"

struct ID3D12CommandQueue;

// Textures the game loads only once (the climate texture: winter's snow) are
// changed on the graphics card itself: the game's upload of them is recognised
// by their pixels, and a season change uploads the season's version.

// The game's queue its swap chain presents with (from the overlay): hooks the
// graphics card's copy and barrier commands.
void GpuInit(ID3D12CommandQueue* queue);

// After a season change: uploads the season's version of those textures.
void GpuSeasonChanged(Season season);

// A season file's mip, unpacked (its pixels as the file has them).
#include <stdint.h>
#include <vector>
bool SeasonTextureMip(const char* group, const char* path, Season season, uint32_t mip, std::vector<uint8_t>* out, uint32_t* width, uint32_t* height);
