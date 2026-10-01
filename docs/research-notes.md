# Seasons - research notes

Goal: seasons in Crimson Desert (spring, summer, autumn, winter).

## Weather component (object/bin__/prefab/preset/weather.prefab)

WeatherComponent -> WeatherConstant:
- snow: _snowAmount, _snowRate, _snow, _iceRatio, _snowPuddleRate
- rain: _precipitation, _rain, _humidity, _cloudiness, _puddleRate, _rainDropletAmount/Rate
- wind: _windSpeed, _windDegree, _altitudeWindRatio
- climate: _climateTexture1/2, _climateSandTexture, _enableClimateTexture, _heightScaleMin/Max

AtmosphereConstant: _earthAxisTilt, _latitude, _sunDirX/Y, _moonDirX/Y, sun/moon size,
rayleigh/mie, ozone, height fog, cloud base density...

Weather sequences (sequencer/baseseq/contents/weather): sunny, cloudy, rainy (+desert, jungle),
heightfog, mie, wind, winddegree, lightning. No snow sequence.
Weather ring items (gimmick_equip_weather_change_sunny / rainy_hard) change weather at runtime.

## Tools
- tools/game_files.py: read game archive files (needs Crimson Browser Sharp's archive index)
- tools/memscan.py: read/write game memory from outside (research)

## Reflection binary format (.prefab / .palevel)
- Header: ff ff, u32 4, 8-byte hash, u32, u16 class count; then per class: u32 len+name, u16 field count,
  per field: u32 len+name, u32 len+type, u16 kind, u16 size, u32 flags (check__ bools: 0x800020).
  tools/palevel.py prints classes.
- Data: records FF x8, u32 self, u32 ?, then values; a record ends with the next object's header (class index).
- WeatherConstant tail: stored floats, u32, "01 0c 00" + 5-byte bitmap, "01 10" + 5-byte bitmap.
  In weather.prefab bitmap A bits 22/24/28 = _snowAmount 0.0836, _snowRate 2.0, _windDegree 0 (3 floats).
- rootlevel.palevel (0015) holds GlobalWeather (world weather): first stored WC float 0.5005 (snowAmount?).
- Climate texture channels (texture/climate_texture*.dds A, R, B) do NOT control snow (tested).

## Cold / snow channel (2026-09-29)
- terrain/global/climate_texture_1.dds (0015, DXT1) R == texture/climate_texture_1.dds (0002, BGRA) B (corr 0.98).
  Values: desert 0, most land 176-192, the light centre patch 240+ (matches the light patch in global_colormap)
  -> probably coldness; snow zone >= ~240. Test v0.3: texture/climate_texture_1 B = 250 everywhere.
- Earlier tests set the wrong channels (climate_texture A, climate_texture_1 R, climate_texture B).
- tools/textures.py now reads mipmapped "Partial" textures (header +0x20: stored sizes of the first 4 mips).
- Materials: ice.material / iceforobject*.material use _snowAmount, _snowRate, _sideSnowRate, _snowHeightTexture
  (snow_ground01_d), _snowColor -> snow cover on objects is a material feature. Shaders only as compiled cache.
- Backup test "Seasons Test B" (build/): texture/climate_texture_1 B=250 + terrain climate_texture_1 R=250
  (tools/dxt1_channel.py edits DXT1 block endpoints).
- RESULT test A (2026-09-29): texture/climate_texture_1 B=250 -> cold works: frost screen effect, snow on logs,
  frosted armour; ground still green. Next: B (terrain climate_texture_1 R too).
- BUG found: textures.py cleared the header's stored-size fields (+0x20); the game needs them (= mip sizes when
  plain). Variant B failed probably because of that (whole climate off). Fixed; B and B0 rebuilt from the
  original bytes. B0 = A + untouched terrain climate texture (diagnostic).
- terraindetailmapinfonew.xml: 255 ground materials (stone/gravel/grass/sand/desert/mud) - NO snow material.
  Snow zone tiles use ordinary stone/gravel -> ground snow is dynamic (climate), not painted.
- RESULT B (fixed header): frost + object snow, ground still green. With A/B active the real snow zone LOST
  its deep snow -> the channel is more like a climate zone value (plateaus 242/243/246 in the cold patch,
  176-192 ordinary land, 0 red desert). 250 = frost zone, not deep snow. Test v0.5: 243 everywhere.
- 243 (texture/ only): frost + object snow, no ground snow. 165 everywhere: nothing (warm).
  Hypothesis: ground reads the terrain copy; deep-snow band ~242-246 (250 too high). Test v0.7: 243 in both.
- 243 in both textures: frost gone too -> results depend on more than climate_texture_1.
- texture/climate_texture_2.dds (R10G10B10A2, only B used, 0..898): a HEIGHT map of the playable square
  (x 64..268, y 178..383 of 512; 188 outside). Hypothesis: ground snow = cold (tex1 B) + high (tex2 B,
  scaled by _heightScaleMin/Max). Test v0.8: tex1 B=243 + tex2 B=898 everywhere.
- KEY: highest mountains (tex2 height >= 500) have tex1 B = 25 (and 0, 40, 50, 60, 70) -> the black area with
  grey rings is the SNOW MOUNTAINS (not desert). 243/250 only gave edge frost. Test v0.9: tex1 B = 25 + tex2 max.
- Scan of all 71312 .palevel/.prefab files: weather objects only in rootlevel.palevel (GlobalWeather) and
  presets (weather*.prefab, weatherpostprocess/*). NO regional weather zones -> regions come from the
  climate textures only.
- v0.9 (tex1 B=25 + tex2 max): frost + log snow only. Objects react to any cold value; ground probably reads
  the terrain copy (0015 terrain/global/climate_texture_1 R). v1.0: both copies 25 + tex2 max.
- DMM CANNOT re-encode climate_texture_2 (R10G10B10A2): "regen failed ... may render white/flat" -> v0.8/v0.9
  broke the height map (that's why the snow zone lost its snow). Don't ship tex2 through DMM as loose DDS.
  v1.0 now: tex1 B=25 + terrain copy R=25 only.
- v1.1 DIAGNOSTIC: unchanged climate_texture_1 through DMM -> frost everywhere + snow zone loses snow.
  => DMM re-encodes single-mip class-0x04 textures (to ~BC3 + mips, 349680 bytes) and garbles the zone
  values. ALL earlier value tests were really tests of that conversion.
  DMM keeps multi-mip textures native -> tools/climate_dds.py writes BGRA8 with a full mip chain
  (nearest downsampling). v1.2 = unchanged + mips (diagnostic).
- v1.2 (unchanged + mips): game exactly vanilla -> delivery path clean.
- Snow mountain pixel (BGRA): (25, 127, 63, 255) (also R 60/61). v1.3: whole world = that pixel.

## WINTER WORKS (2026-09-30)
v1.3: texture/climate_texture_1.dds (0002) = (B25, G127, R63, A255) everywhere, BGRA8 with full mips
(tools/climate_dds.py) -> snow on the ground, objects, frost. The terrain copy was left vanilla.
Keys: deliver climate textures multi-mip (DMM re-encodes single-mip ones); snow zone pixel from the
high mountains (climate_texture_2 height >= 500).
- v1.3 screenshot: desert fully snowed in (deep snow, frost). In-game map matches the colour map's red area
  -> red area = desert. Pixels: land (B~190,G127,R3), desert (B0,G127,R60-63|3), snow pixel (25,127,63).
- v1.4: snow pixel everywhere except the desert (mask: reddish in global_colormap).
- v1.4 (colour-map mask): desert red stays clear, snow outside; hard seam at the edge (user: ok); parts of the
  desert (Tashkalp valley) got snow. v1.5: mask = climate B 0-120 | 240-246 | colour-map red
  (research/climate/desert_mask.npy).
- Crash in Tashkalp (v1.5): dump shows EquipEverything.asi + CombatEvolved.asi (Aero's beta) on the stack,
  crash at game rva 17B16E4 (write via garbage out-pointer). Without Aero's mod: no crash so far.
- v1.5: Tashkalp still snowy -> its zone is 165 (warm; 165 everywhere gave no frost). v1.6: mask + B==165.
- v1.6 (+ zone 165): Tashkalp still snowy -> Tashkalp is ordinary land (187-192) inside the desert.
  global_regionmap values are NOT region table indices (65 scattered). v1.7: mask closed (dilate/erode 6)
  and holes enclosed by desert filled (5354 px, the land between desert, white desert and zone 165).
- Locator v1.8 (winter x 64-165 of the playable square): user's camp snowy, Tashkalp clear -> Tashkalp in
  x 166-268. v1.9: top right quarter (x 166-268, y 178-280).
- ORIENTATION (locator v1.8 + world map): climate texture = map orientation (north up, west left); the
  playable map = the square x 64-268, y 178-383 (where climate_texture_2 has heights). Pailune snow mountains
  (west) = dark area with rings (B 25 etc.); Crimson Desert (NE) = the white patch (B 242-253) + halo.
  Tashkalp ~ texel (196,229), B 243/250+. v2.0: mask = desert polygon traced on the world map | B >= 200.
- v2.0 WINTER OK (user 2026-09-30): snow everywhere, Crimson Desert clear. Saved as seasons/winter/.
  Rebuild: python tools/climate_dds.py <out> --except-desert B=25 G=127 R=63 (mask seasons/desert_mask.npy
  = research/climate/desert_mask.npy).

## Autumn research (2026-09-30)
- Vegetation materials (treeleaf, grassleaf...) are ~165-byte shells: leaf colour = plant textures (+ shader).
- Test v3.0: global_colormap (DXT5 2048, 12 mips) + global_extra_region_tintcolormap (DXT1 2048, 12 mips)
  hue-shifted green -> gold/orange/rust (tools/autumn_tint.py edits block endpoints, all mips).
  Question: do grass / trees follow the colour map, or only the ground?
- v3.0 result: grass AND tree leaves follow the colour map (visibly yellower/olive) -> colour maps drive
  vegetation tint. v3.1: stronger (hues 8-42, saturation +80%, brightness +18%).
- v3.1 AUTUMN OK (user screenshots 2026-09-30): deciduous leaves gold/yellow, grass dry gold, conifers stay
  green. Mostly yellow (map colour blends with the leaves' own green); push hues towards red for more orange.
  Saved as seasons/autumn/ (global_colormap.dds, global_extra_region_tintcolormap.dds, 0015 terrain/global).
- Distance stayed green in v3.1 -> terrain colour tiles (terrain/color/terrain_x_y_color_c.dds, 785 x DXT1 512)
  are the far look. v3.2: all tiles tinted too (tools/autumn_tiles.py), 141 MB.
- v3.2 result: distance follows, but far grass LOD stays green until close (own colour, not tinted).
- Vegetation system = top-level `tree/` (0001): 1467 .pat plants (637 weed, 465 tree, 202 bush), tree/texture
  atlases (837 _color DXT1 512 + 832 _subsurface), tree/impostor (1586 _impostor_color DXT1 1024, 1.1 GB).
  Trees are NOT in .pami files. tools/autumn_plants.py tints leaf/grass textures, keeps evergreens, palms,
  desert/sea/abyss plants, crops, moss, bark. v3.3: 961 textures (295 MB), colour maps vanilla.
- v3.3 result (user screenshots): deciduous trees rust/gold, grass amber, conifers + flowers unchanged -
  "looks good"; distance still green. v3.4: + 902 impostor colour images (630 MB) + 785 terrain tiles;
  global colour map vanilla (it tints near plants a second time). Total ~1 GB.
- v3.4 result: distance works (far forests brown/gold). Bright green patches left ON THE GROUND = terrain grass
  ground textures (object/texture cd_bg_ground_grass_*, pa_terrain_grass_*: 95 DXT1 colour textures).
  v3.5: + those (tools/autumn_ground.py).

## In-game date (2026-09-30)
The HUD (next to the minimap) shows "Day 76 Sat 12:06 PM": the game counts days and
weekdays. Earlier conclusion "no calendar, only a clock" was wrong. A season switcher
(ASI) could use this day count, e.g. 4 seasons of N days.

## Spring test 1.0
SEASON=spring: greens pulled 60% towards hue 78-108, +20% saturation, +15% brightness.
Plants + impostors + tiles + ground grass (2012 + 785 + 95 textures, 1.2 GB).

## Release 1.0 (2026-09-30)
build/release: "Seasons - Winter" (0002 climate_texture_1), "Seasons - Spring" (spring 1.1),
"Seasons - Autumn" (autumn 3.6), each a DMM mod + zip. One season at a time; summer = all off.

## Snow and deep snow (2026-10-01)
- climate_texture_1 is read 3 times, all at game start (2 whole-file, 1 mip0). Never again
  (not on save load, not on fast travel). The game builds a snow layer from it at start
  (deep snow, whole areas with seams); no CPU copy of the pixels is kept (only freed buffers).
- Live: the GPU texture (format 27, 1 mip) can be replaced -> snow cover everywhere, but no
  deep snow. Deep snow needs the game to read winter's climate at start (ASI deep_snow=1).
- Upload-buffer dumps taken at CopyTextureRegion time are incomplete (buffer still being
  filled) - proven with a summer control. Don't use them to judge decoding.
- The game's LZ4 blocks: literal runs <= ~1.8 KB, copies <= ~60 KB, near copies <= ~1 KB.

## Live climate (2026-10-01, v0.19: user OK - deep snow + cold appeared live)
- PAClimateTexture (vtable rva 5AD1A10): +0x18 texels (u32), +0x20 texel COUNT (262144 = 512x512, 1 MB),
  +0x30/+0x34 80 m per texel, +0x38/+0x3C 512x512. = climate_texture_1.dds flipped vertically,
  bytes A,R,G,B (u32 = A | R<<8 | G<<16 | B<<24). Earlier "256x256 / 262144 bytes" was wrong: research
  guarded and wrote only the first quarter (south edge, outside the playable area) -> "0 reads", "no effect".
- 2 maps in memory (weather manager +0x48, climate manager +0xD8; chain [146D69198]+0x40 -> +0xF68 -> +0xD8).
  Memory scan for them: ~0.5 s.
- Sampler rva 269F080(map, &xz, out[4]) bilinear: out0 = clamp((2B/255-1)*50, -50, 50) = day's top
  temperature (B 25 = -40 C, B 190 = +24.5 C); out1 = G/255; out2 = clamp(R*0.25, 0, 60); out3 = A/255*360.
- GetClimateAt rva 3DC0FE0 -> {top, temperature, G, R', A'} (default {0, 20, 0.5, 0, 0}); temperature =
  night + f(hour)*(top-night), night = top - ((1-G/255)*18+7), f = 1 at 11-13 h (rva 269EFE0); hour from
  [mgr+0xE8] vcall +0x100.
- Writing the season's map live -> temperature and deep snow change at once (no reload).
- Vanilla R: ~60 in the snowy north-west, 85-106 in the desert box, ~3 elsewhere (not snow).
- SUMMER SNOW BUG (fixed 0.25): with the mod, a summer start had snow in patches near the camp (none
  without the mod; gone after winter -> summer). The game's texture loader (GPU path, mip0 read of 395330
  stored bytes) decodes our LZ4-packed climate copy wrongly from ~100 KB on; the CPU maps (whole-file
  reads) decode fine. Fix: the climate's summer entries stay the game's own (uncompressed) file. The
  "seams" at winter starts with deep_snow=1 were probably the same bug, not a load-time snow layer.
- Snow settings (globals, cvars): 6D63848 = -5 (snow starts), 6D63898 = -20 (snow full), 6D638E8 = -20
  (deep snow: rva 3DC0C50 sets climate manager +0x120 when the 3x3 min midday temp < it), 6CA70D8 = -6.5,
  6D63938 = -30. Deep snow seen at top temp -25, not -20; it and the temperature meter both follow the
  climate manager's map (+0xD8), not the weather manager's.

## Weather (2026-10-01)
- HUD weather: rva 17FA700(actor, u32* flags, bool*) -> player object +0x178 flags; bit 0 rainy, 1 snowy,
  2 windy, 3 fog, 4 cloudy, 5 sunny (names rva 5912E88); HUD shows the lowest bit. Seasons weather.cpp hooks it.
- GlobalWeather WeatherConstant live block (climate mgr +0x90 -> +0x70 -0x28 -> +0x40): +0x120 heightScaleMin,
  +0x124 heightScaleMax, +0x128 enable, +0x12C precipitation, +0x130 cloudiness, +0x134 humidity, +0x138 windSpeed,
  +0x13C puddleRate (wetness, rose during rain), +0x140 snowPuddleRate, ... +0x168 _snow, +0x16C _rain.
  Writing precipitation there: _snow follows (smoothed) but no visible snowfall; the weather ring's rain kept
  precipitation 0 -> not the visible weather.
- Weather ring: buff ChangeWeather -> client sets that block's +0x12C..0x138 via climate mgr +0x28 iface;
  server part obfuscated. Ring = global stage sequencer WeatherRing_Rainy -> stageseq/item/cd_seq_item_weather_rainy_0001
  (loops 5x). Its rain stays rain even at -40 C.
- Natural weather: sequencer/baseseq/contents/weather/cd_seq_contents_weather_{sunny,cloudy,rainy,wind,fog,...}
  (loop counts 45 sunny, 30 cloudy); WeatherComponent tracks. rootlevel GlobalWeather has effects rain, heavyrain,
  snow, heavysnow, snowdust, lightning, desertdust.
- gameglobaleffectinfo: global effects incl. Weather_Snow_0001/2, Weather_Rainy_0001-3, Weather_Sunny_0001/2,
  Weather_NotRainy_0001, DefaultWeather (fields: precipitation, cloudiness, humidity, snowAmount, ...).
- globalgameevent: Drought/HeatWave/ColdWave/HeavySnow/Prolonged_Rainy_Season/Storm_* (region news events?).
- Snow settings: SNOW_START 6D63848 (-5), SNOW_FULL 6D63898 (-20) - moving them did not turn the ring's rain to snow.
- 0.29: logs reads of the weather sequence files (0014) to see which weather the game picks and when.
- WEATHER CONTROL FOUND (2026-10-01, via the Crimson Weather mod's hook list, mod 632): the climate manager
  composes its weather layers each tick (rva 3DC0340 "WeatherCompose", rcx = climate mgr, xmm1 = dt) into
  W = [[mgr+0x60]+0x18] (a WeatherConstant; NOT the +0x90 chain block). W+0x168 snow, +0x16C rain are read by
  GetSnowIntensity / GetRainIntensity (rva 3DC3AC0 / 3DC3A10, altitude-faded unless mgr+0x31) -> effects + HUD.
  Holding W+0x168 = 1 (and +0x130 cloud) -> snowfall, HUD "Snowy" (user confirmed).
  0.32: compose hook writes the season's spells after the game composes.
