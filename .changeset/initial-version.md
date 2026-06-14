---
"keyparty": major
---

Initial release of KeyParty — a full-screen, kid-proof key-smashing toy where every key, click, and drag bursts into color and sound, and nothing a child mashes can quit the game or escape to the OS.

**Play**

- Opens to a menu with **Start** and **Quit**, plus the macOS Accessibility setup needed for full keyboard locking.
- Start drops into full-screen kiosk mode; the grown-up chord **Control + Option + Shift + Q** returns to the menu (it never just quits mid-play).
- Every key paints a unique splash and plays a sound:
  - **Letters** — a glowing letter + colored burst, one musical note each
  - **Numbers** — a bouncing digit + star burst
  - **Space** — a rainbow firework, a boom, and a screen shake
  - **Enter** — an expanding rainbow ripple + a little chord
  - **Arrows** — a shooting stream of triangles + a directional swoop
  - **Backspace, Delete, Tab, Escape, punctuation** — their own shapes and tones
  - **Modifiers** (⇧ Shift, ⌃ Control, ⌥ Option, ⌘ Command, Caps Lock, Fn) each get their own color, symbol, and chime
- Holding several keys streams effects from all of them at once; clicks and drags paint splashes and trails behind a glowing star cursor, with a running tally of every smash.

**macOS kiosk lockdown**

- Covers the entire screen — above the menu bar and dock, with no title bar or window buttons.
- A global event tap swallows every shortcut (⌘Q, ⌘Tab, Spotlight, the screenshot keys, power/sleep, media keys) and still turns each press into an on-screen effect.
- Hides the dock and menu bar and disables app switching, force-quit, and app hiding. Until Accessibility is granted it falls back to an in-app key monitor, then upgrades to the full system-wide tap automatically — no restart needed.

**Web**

- Also playable in the browser. The web menu notes that the desktop app plays best (a browser can't lock the keyboard), hides Quit, and links to the download.
