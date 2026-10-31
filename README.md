# Lembas for Windows

A Middle-earth theme for Windows 11 that follows the sun, in four looks:

| Look | When | Feel |
|---|---|---|
| **Shire Morning** | sunrise → late afternoon | light: sage and green ink, the Shire hills |
| **Red Book** | late afternoon → sunset | light: parchment and red ink |
| **Starlit Night** | sunset → 11pm, 4am → sunrise | dark: midnight and gold, stars and the moon |
| **Elven Silver** | 11pm → 4am | dark: black-blue and silver |

The looks change at the same minute as the Lembas phone launcher and watch face (same sunrise and sunset sums, from your time zone's main city). Each look sets the wallpaper, lock screen, light/dark mode and accent colour, and recolours the taskbar, Start menu, notification centre, File Explorer, Settings, window borders and the copy window. There's also a Lembas icon set (parchment folio folders, chest drives, a round-door This PC, a palantír for the network, a barrel Recycle Bin), cursors with the One Ring as the busy cursor, a harp-and-bell sound scheme and Windows Terminal colours. Text stays in the normal Windows font; the Lembas typefaces are used for headings and the clock.

**Lembas Desktop** brings the phone's home screen to the desktop. Each look is a wide panorama drawn for PC screens (not a stretched phone picture): the Shire with its round green door, a map by the fire, a lakeside cabin under the stars, and Rivendell's silver trees and waterfalls. They move gently behind your icons: clouds and chimney smoke, glints on the river and lake, a flickering campfire and a swaying compass needle, twinkling stars, a shooting star and the real moon phase, lanterns, mist and falling water. The scenes change with the **seasons** (blossom in spring, autumn leaves, snow and a bare tree in winter, flipped for the southern hemisphere) and, if you like, with the **weather** outside (rain, snow, fog or heavy cloud). On ultrawide and multi-monitor setups the panorama widens to fill each screen instead of cropping. The clock sits in the corner: the day, the time, the hobbit mealtime and the Shire date. **Click it** for a small panel with the date, today's sunrise and sunset, when the look changes next and the week ahead.

It's a small native program written for Lembas (`desktop/LembasDesktop.cpp`: Direct2D, no browser, no other tools). It's built to be easy on the battery:

- The still parts of each scene are pre-rendered in layers, so it only redraws the few small things that move (usually under 5% of the screen).
- It draws 24 frames a second on mains power and 12 on battery.
- It stops completely when windows cover the desktop, the screen is off or locked, Battery saver is on, or Windows' *Animation effects* are off. The clock still turns over each minute.

The weather comes from [Open-Meteo](https://open-meteo.com) (free, no account), asked about your time zone's main city at most once an hour; nothing else is sent. Turn it off in the tray menu and nothing is fetched.

The **sign-in screen** matches too: it shows the look's picture clearly instead of a blurred one (the setting is put back when you uninstall).

The **Lembas icon by the clock** (in the system tray) changes the look, the desktop clock (**Modern**, in the same type as the phone launcher and watch face; **Classic**; or off), the moving scene, the weather and the mealtimes, and has Preview, Check for updates and Uninstall. Start > Lembas > Change look does the same from a menu.

## Install: one click

1. Download **[Lembas-for-Windows.zip](https://github.com/gbesh/lembas-windows/releases/latest/download/Lembas-for-Windows.zip)** and extract it.
2. Double-click **Install Lembas** and say Yes when Windows asks for permission (once).

It installs [Windhawk](https://windhawk.net) 2 and its mods silently, plus a small *Lembas Sun* task that switches looks with the sun. It updates itself once a day. Start > Lembas has **Preview the four looks**, **Change look**, **Update** and **Uninstall** (one click; it puts everything back).

See [README.txt](README.txt) for details.

## Building

Every push to `main` runs `.github/workflows/build.yml`, which renders the wallpapers and the desktop's still layers (`scenes/`), cross-compiles Lembas Desktop for ARM64 and x64 (llvm-mingw), makes the cursors, sounds and icons, assembles the kit (`tools/build_kit.py`, looks in `tools/palettes.py`, Windhawk styles in `windhawk/`) and publishes a release.

Fonts: SIL Open Font License (Cinzel, Cormorant Garamond, Alegreya, IM Fell English, Fraunces, Alegreya Sans). An unofficial fan project, not affiliated with the Tolkien Estate or any film studio.
