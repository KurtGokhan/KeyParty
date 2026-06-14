# KeyParty

A full-screen **key-smashing game for kids**, built on zero-native (Zig native
shell + Next.js web UI). Every key paints a different burst of color and plays a
different sound. The window runs locked down in kiosk mode so a child can mash
the whole keyboard without quitting the game, switching apps, or triggering
anything in the operating system.

## Play

```sh
zig build run
```

The game launches full-screen — it covers the **whole** screen, menu bar and
dock included. Smash any key, or click and drag with the mouse:

- **Letters** — glowing letter + colored burst, a musical note per letter
- **Numbers** — bouncing digit + star burst
- **Space** — rainbow firework, a boom, and a screen shake
- **Enter** — expanding rainbow ripple + a little chord
- **Arrows** — a shooting stream of triangles + a swoop
- **Backspace / Delete / Tab / Escape / punctuation** — their own shapes and tones
- **Modifiers** — ⇧ Shift, ⌃ Control, ⌥ Option, ⌘ Command, Caps Lock and Fn each
  get their own color, symbol, and chime. Pressing them (alone or as part of a
  chord) still makes something happen instead of doing nothing.
- **Clicks & drags** — paint colored splashes and trails at the pointer. The
  cursor is a friendly glowing star you can always see.

### Quitting (grown-ups only)

There is no quit button and no menu shortcut. The only way out is the
four-key chord:

> **Control + Option + Shift + Q**

Every other key combination — ⌘Q, ⌘Tab, ⌘Space (Spotlight), ⌘H, ⌘⌥Esc, the
screenshot shortcuts, even the power/sleep and media keys — is swallowed before
the system ever sees it, *and* still produces an on-screen effect. When a combo
that would normally poke the OS is pressed, the game also briefly flashes a
small hint showing the quit chord.

## Accessibility permission (for full key blocking)

To swallow **every** system shortcut — including the ones the window server
handles below any normal app (Spotlight, the screenshot keys, the power/sleep
chord, media keys) — the game installs a global **CGEventTap**, which macOS only
allows once the app is trusted for Accessibility.

The first time you run it, macOS prompts you to grant Accessibility in
**System Settings → Privacy & Security → Accessibility**. Turn KeyParty on there.
You do **not** need to restart — the game polls for the grant and upgrades to the
full system-wide block the moment you flip the switch. Until then it falls back
to an in-app monitor that still blocks every app/menu shortcut (just not the
window-server-level ones).

## How the lockdown works

The kiosk behavior lives in a patched copy of the macOS native host,
[`native/appkit_host.m`](native/appkit_host.m) (vendored from zero-native and
wired up in [`build.zig`](build.zig)). It:

- creates the main window covering the **entire** screen — no title bar or window
  buttons, non-movable — and raises it to `CGShieldingWindowLevel()` so it sits
  *above* the menu bar and dock (the level screen savers use);
- sets `NSApplicationPresentationOptions` to hide the dock and menu bar and to
  disable process switching (⌘Tab / Mission Control), force-quit (⌘⌥Esc), the
  power-key session dialog, and app hiding (⌘H);
- installs a **global CGEventTap** at the HID level that swallows every key-down,
  key-up, and modifier change, plus the system-defined power/sleep/media events,
  and forwards each one to the web UI so it still produces an effect — nothing a
  child presses reaches another app or the OS;
- falls back to an in-app key monitor when Accessibility hasn't been granted yet,
  and auto-upgrades to the tap once it is;
- strips the menu's Quit / Close / Hide key-equivalents as another layer of
  defense;
- recognizes the **Control + Option + Shift + Q** quit chord and shuts down
  gracefully.

The game UI (canvas, effects, sounds, pointer splashes, custom cursor, the
on-screen hint) is in [`frontend/app/page.tsx`](frontend/app/page.tsx). It
consumes the native `key` events the tap forwards, and also handles plain DOM
keys (for development / browser use), calling `preventDefault()` so the web layer
never scrolls, quick-finds, or moves focus.

### Development mode

Run in a normal resizable window (no screen takeover) for development:

```sh
KEYPARTY_NO_KIOSK=1 zig build run
```

The quit chord still works; the window also keeps a close button in this mode.

## Setup

`zig build dev`, `zig build run`, and `zig build package` install frontend
dependencies automatically. To install them explicitly:

```sh
npm install --prefix frontend
```

The build defaults to this zero-native framework path:

```text
/Users/gokhankurt/.vite-plus/js_runtime/node/24.16.0/lib/node_modules/zero-native
```

Override it with `-Dzero-native-path=/path/to/zero-native` if you move this app.
(The patched `native/appkit_host.m` ships in this repo, so only the rest of the
framework is read from that path.)

## Commands

```sh
zig build run       # build the frontend + launch the game
zig build dev       # frontend dev server + native shell
zig build test      # run tests
zig build package   # create a distributable .app
zero-native doctor --manifest app.zon
```

Diagnostics:

- Set `ZERO_NATIVE_LOG_DIR` to override the log directory during development.
- Set `ZERO_NATIVE_LOG_FORMAT=text|jsonl` to choose the persistent log format.
