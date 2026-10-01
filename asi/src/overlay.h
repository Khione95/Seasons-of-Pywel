#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

// Draws the menu into the game's own frames (from Character Creator's
// overlay). The game's DirectX 12 back buffer is wrapped for Direct2D right
// before each frame is presented; HDR screens get the menu converted.
//
// Keyboard input reaches the menu through the game window's message loop;
// while the menu is open those keys are kept from the game.

struct OverlayDrawContext
{
    ID2D1DeviceContext* dc;
    IDWriteFactory* write;
    float width;
    float height;
    unsigned generation;    // changes when the drawing objects were made anew:
                            // brushes and fonts made before are invalid
};

typedef void (*OverlayDrawFn)(const OverlayDrawContext& ctx);

// Called for every key press while the menu is open (virtual-key code).
typedef void (*OverlayKeyFn)(int vk);

// Call as early as possible: watches for the game creating its swap chain.
bool OverlayEarlyInit();
bool OverlayInit();

void OverlaySetCallbacks(OverlayDrawFn draw, OverlayKeyFn key);
void OverlaySetVisible(bool visible);
bool OverlayVisible();
void OverlaySetDrawing(bool drawing);
