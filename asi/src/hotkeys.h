#pragma once

// The Control Menu's key, from bin64\Seasons.ini (written with the default,
// F5, on the first start - where DMM's plugin config editor finds it).
void HotkeysLoad(const char* bin64);
bool HotkeyDown();
const wchar_t* HotkeyName();
