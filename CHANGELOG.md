# keyparty

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
