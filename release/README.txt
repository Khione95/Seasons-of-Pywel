Seasons of Pywel 1.0 for Crimson Desert
by Khione

Four seasons that follow the in-game day, changing live while you play.

SEASONS
- The year starts in summer, as the game does: by default each season lasts
  30 in-game days (days 1-30 summer, 31-60 autumn, 61-90 winter, 91-120
  spring, then summer again).
- Spring: fresh green leaves and grass.
- Summer: the game's own look, a few degrees hotter.
- Autumn: golden and orange trees, grass and terrain, near and far.
- Winter: snow on the ground, the trees and objects, cold that the
  temperature meter shows, and snowfall.

WEATHER
- Winter snows often; autumn rains often; spring has some rain; summer
  only rarely. Clouds come with the rain and snow.
- Deep snow builds up while it snows (the snow depth in the menu) and
  melts after a while without snow, and soon after winter ends.
- Weather follows the game's clock: sleeping or waiting moves it on too.

CONTROL MENU (F5)
- Seasons: By the day (the season follows the calendar) or Fixed (you
  pick the season).
- Length: 3, 7, 14 or 30 days a season.
- Season: in Fixed mode, choose a season and press Enter.
- The menu also shows the day, the season, the weather and the snow depth.

INSTALL
With DMM: drop the zip on DMM and turn Seasons on in its ASI tab. DMM puts
Seasons.asi and the Seasons data folder into the game's bin64 (and has its
own ASI loader).

By hand:
1. You need an ASI loader: Ultimate ASI Loader as winmm.dll in bin64.
2. From the zip's Seasons folder, copy Seasons.asi and the Seasons folder
   into your Crimson Desert\bin64 folder, so that you have
   bin64\Seasons.asi and bin64\Seasons\data.
3. Start the game.

The mod changes no game files on disk: the season textures are served from
bin64\Seasons\data while the game runs. Settings are kept in
bin64\Seasons\Seasons.ini, a log in bin64\Seasons\Seasons.log.

UNINSTALL
With DMM: delete Seasons in the ASI tab. By hand: delete bin64\Seasons.asi
and the folder bin64\Seasons.

COMPATIBILITY
- Works with and without ReShade / RenoDX (HDR included).
- Mods that replace the same tree, grass or terrain colour textures may
  look mixed in spring, autumn and winter.
- Other weather mods may fight with the seasons' weather.
- A game update may move what the mod uses; it then turns those parts off
  (see the log) until it is updated.
