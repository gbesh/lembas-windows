LEMBAS FOR WINDOWS
One small bite is enough.
======================================================================

Your whole PC in four Middle-earth looks that follow the sun, like the Lembas phone launcher:

  Shire Morning   sunrise -> midday      light   sage and green ink, the Shire hills
  Red Book        midday  -> sunset      light   parchment and red ink
  Starlit Night   sunset  -> 11pm        dark    midnight and gold, stars and the moon
  Elven Silver    11pm    -> 4am         dark    black-blue and silver
  (Starlit Night again from 4am until sunrise)


INSTALL: one click
----------------------------------------------------------------------
  1. Unzip this folder (right-click > Extract All).
  2. Double-click  "Install Lembas".  Say Yes when Windows asks for permission (once).

  That's all. It takes a minute or two and installs everything below, including Windhawk
  (the free tool that restyles the taskbar, Start and File Explorer) and its mods.
  If Windows warns about the file: More info > Run anyway.

  Afterwards, in Start > Lembas:
    Preview the four looks   see all four now, 25 seconds each
    Change look              follow the sun (default), or keep one look all the time
    Update                   download and install the newest Lembas over this one
    Uninstall                remove everything and put your PC back as it was

  Running "Install Lembas" again is always safe: it repairs or updates in place.


UNINSTALL: one click
----------------------------------------------------------------------
  Start > Lembas > Uninstall  (or double-click "Uninstall Lembas" in this folder).
  It removes the Lembas mods, the sun task, the fonts, icons and shortcuts, puts back your own
  cursors, sounds, colours and wallpaper, and removes Windhawk too if Lembas installed it and
  you have no other mods in it.


WHAT CHANGES
----------------------------------------------------------------------
  Wallpaper and lock screen   a 4K scene for each look
  Desktop (Lembas Desktop)    the scene moves gently behind your icons, with the time, the Shire
                              date, the Quenya hour and the hobbit mealtime
  Light / dark mode           light by day, dark by night
  Accent colour               green, red, gold or silver with the look (selection, toggles, title bars)
  Taskbar                     tinted glass with a star-in-a-ring Start button
  Clock                       Shire weekday names: Highday, Sterday, Trewsday...
  Start menu                  glass panels, Cinzel headings
  Notifications, calendar     the same glass, Cinzel headings
  File Explorer               tinted tabs, address bar and toolbar, the look's scene behind the
                              Home page, gold or silver drive bars
  Icons                       folders are parchment folios (a quill on Documents, a harp on Music,
                              a mountain on Pictures...), drives iron-bound chests, USB sticks
                              leaf-wrapped parcels, This PC a round green door, the network a
                              palantir, the Recycle Bin a barrel that fills with scrolls
  Settings app                tinted cards with icons in the look's colour
  Window edges                a thin line in the look's colour
  Copy window                 progress ring in the look's colour
  Cursors                     parchment and ink; the busy cursor is the One Ring
  Sounds                      harp plucks, small bells, a low horn for errors
  Windows Terminal            Red Book (light) and Starlit Night (dark) colour schemes

  Text everywhere stays in the normal Windows font so it's easy to read; the Lembas typefaces
  are used for headings and the clock.

  Optional: Chrome / Edge themes are in  %LOCALAPPDATA%\Lembas\Browser  (browsers don't allow
  installing them automatically: chrome://extensions > Developer mode > Load unpacked).


GOOD TO KNOW
----------------------------------------------------------------------
 - Sunrise and sunset come from your time zone's main city. To use your own location, run
   Install Lembas from a terminal with  -Latitude <yours> -Longitude <yours>.
 - The sun task checks every 10 minutes, at sign-in and when you unlock, and does nothing
   unless the look should change.
 - Windhawk 2 is an early (alpha) version; it updates itself. Each Lembas mod can be switched
   off in Windhawk if you don't want that part.
 - On ARM PCs (Snapdragon), File Explorer's file list and sidebar stay plain black or white:
   the mod that tints them doesn't support ARM yet. Everything else works.
 - Lembas Desktop is easy on the battery. Only the small moving parts are redrawn, at 24 frames
   a second on mains power and 15 on battery. It stops while windows cover the desktop, the
   screen is off or locked, Battery saver is on, or Animation effects are off in Settings.
   Start > Lembas > Change look turns it (or just its clock) off. Each minute it logs its CPU use
   to desktop.log.
 - Logs: %LOCALAPPDATA%\Lembas\install.log, sun.log and desktop.log.


CREDITS
----------------------------------------------------------------------
 Art, icons, cursors and sounds: made for Lembas.
 Fonts (SIL Open Font License, licences in the Fonts folder): Cinzel (Natanael Gama),
   Cormorant Garamond (Christian Thalmann), Alegreya (Huerta Tipografica), IM Fell English
   (Igino Marini), Fraunces (Undercase Type), Alegreya Sans (Huerta Tipografica).
 Windhawk and its mods: Ramen Software and the Windhawk mod authors.
 Names and the Shire calendar come from J.R.R. Tolkien's legendarium.
 An unofficial fan project, not affiliated with the Tolkien Estate or any film studio.
