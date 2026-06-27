# keyparty

## 1.1.0

### Minor Changes

- 5fe9a42: Reworked the menu into a full-screen, kiosk-native layout:

  - The menu now opens full-screen — a panel anchored to the bottom with a "Click
    to start" prompt over the canvas. Clicking anywhere outside the menu starts the
    game. On short screens the menu is centered like a modal and the prompt is hidden.
  - Starting collapses the menu into a thin bar at the bottom holding all the
    actions (Background, Unlock mouse, Menu, Quit) plus a Hide button. Hiding leaves
    a clean play surface; the menu chord brings the bar back.
  - The grown-up chord is now a "menu chord": Control+Option+Shift+Q brings the bar
    back when hidden, and leaves the game (back to the start menu) when it's showing.
  - New "Unlock mouse" action: the mouse reaches background apps while the keyboard
    stays locked to the game, with the bar itself staying clickable to re-lock.
  - The panel/bar collapse, hide, and reappear with animation.

## 1.0.3

### Patch Changes

- e0060a5: More boom and ripple love for keys and mouse:

  - Enter and Escape now trigger the screen shake and bassy boom
  - Right click triggers the screen shake and bassy boom
  - Left click sends out a blur ripple in transparent mode

- 232f31f: Simpler, icon-forward quit hint:

  - Drops the "Grown-ups…" wording — everyone can quit
  - In-game and menu hints now read as a compact "keys → 🚪 QUIT" statement
  - Chord keys show their glyph icon alongside the name (⌃ Control, ⌥ Option, ⇧ Shift)
  - Adds a ⚙️ icon to the Grant Accessibility Access button

- ee88b4d: A few fixes and improvements:

  - Confines mouse in the playable area
  - Adds boom effect to more keys
  - Prevents opening dictation on MacOS

## 1.0.2

### Patch Changes

- f81d4e5: Give the macOS **.dmg** a branded install window instead of the bare default Finder folder: a light party-gradient backdrop with a "drag the keycap into your Applications folder" arrow, the app and Applications icons positioned over it, and the app's own icon as the disk's volume icon.

## 1.0.1

### Patch Changes

- c08a03c: Name the downloadable builds **KeyParty.exe** and **KeyParty.dmg** (was `keyparty-windows.exe` / `keyparty-macos.dmg`). The in-app download links and the stable `releases/latest/download/…` URLs are updated to match.
- 70e8711: Open the menu in the see-through **transparent** backdrop by default instead of the solid deep-purple chrome. The web UI and both native shells (macOS / Windows) now start in transparent mode; the grown-up can still cycle back to solid/blurry from the menu's Background toggle.

## 1.0.0

### Major Changes

- 9112503: Initial release of KeyParty — a full-screen, kid-proof key-smashing toy where every key, click, and drag bursts into color and sound, and nothing a child mashes can quit the game or escape to the OS. Ships as a desktop app for **macOS** and **Windows**, plus a browser version.

  **Play**

  - Opens to a menu with **Start** and **Quit**, plus (on macOS) the Accessibility setup needed for full keyboard locking.
  - Start drops into full-screen kiosk mode; the grown-up chord **Control + Option/Alt + Shift + Q** returns to the menu (it never just quits mid-play).
  - Every key paints a unique splash and plays a sound:
    - **Letters** — a glowing letter + colored burst, one musical note each
    - **Numbers** — a bouncing digit + star burst
    - **Space** — a rainbow firework, a boom, and a screen shake
    - **Enter** — an expanding rainbow ripple + a little chord
    - **Arrows** — a shooting stream of triangles + a directional swoop
    - **Backspace, Delete, Tab, Escape, punctuation** — their own shapes and tones
    - **Modifiers** (⇧ Shift, ⌃ Control, ⌥ Option/Alt, ⌘ Command/Win, Caps Lock, Fn) each get their own color, symbol, and chime
  - Holding several keys streams effects from all of them at once; clicks and drags paint splashes and trails behind a glowing star cursor, with a running tally of every smash.

  **macOS kiosk lockdown**

  - Covers the entire screen — above the menu bar and dock, with no title bar or window buttons.
  - A global event tap swallows every shortcut (⌘Q, ⌘Tab, Spotlight, the screenshot keys, power/sleep, media keys) and still turns each press into an on-screen effect.
  - Hides the dock and menu bar and disables app switching, force-quit, and app hiding. Until Accessibility is granted it falls back to an in-app key monitor, then upgrades to the full system-wide tap automatically — no restart needed.

  **Windows kiosk lockdown**

  - Covers the whole monitor — borderless, topmost, taskbar included.
  - A global low-level keyboard hook swallows every shortcut (Win, Alt+Tab, Alt+Esc, Alt+F4, Ctrl+Esc, F5/refresh, …) and still turns each press into an on-screen effect. No special permission is needed. The grown-up **Control + Alt + Shift + Q** chord returns to the menu.
  - Built on a WebView2 host with the same bridge as macOS, so the game UI behaves identically. (Ctrl+Alt+Del is the one sequence Windows reserves for itself.)
  - Ships as a single self-contained `keyparty.exe` — the frontend is embedded in the executable and the WebView2 loader is static-linked, so there's no DLL or assets folder to ship (only the WebView2 Evergreen runtime, which is already on Windows 11 / current Windows 10).

  **Web**

  - Also playable in the browser. The web menu notes that the desktop app plays best (a browser can't lock the keyboard), hides Quit, and links to the download.
