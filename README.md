# KeyParty

A **key-smashing game for kids**, built on zero-native (Zig native shell +
Next.js web UI). Every key paints a different burst of color and plays a
different sound. While playing it runs locked down in kiosk mode so a child can
mash the whole keyboard without quitting the game, switching apps, or triggering
anything in the operating system.

## Play

```sh
zig build run
```

The app opens in a small **menu** window with **Start** and **Quit** — and, on
macOS, the one-time **Accessibility** setup needed for full keyboard locking.
Press **Start** to enter kiosk mode: the window takes over the **whole** screen,
menu bar and dock included, and the keyboard is locked. Smash any key, or click
and drag with the mouse:

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

### Leaving the game (grown-ups only)

While playing there is no quit button and no menu shortcut. The only way back is
the four-key chord:

> **Control + Option + Shift + Q**

It returns to the **menu** (it does not quit the app) — from there a grown-up can
press **Quit**, or **Start** again. Every other key combination — ⌘Q, ⌘Tab,
⌘Space (Spotlight), ⌘H, ⌘⌥Esc, the screenshot shortcuts, even the power/sleep and
media keys — is swallowed before the system ever sees it, *and* still produces an
on-screen effect. When a combo that would normally poke the OS is pressed, the
game also briefly flashes a small hint showing the chord.

## Accessibility permission (for full key blocking)

To swallow **every** system shortcut — including the ones the window server
handles below any normal app (Spotlight, the screenshot keys, the power/sleep
chord, media keys) — the game installs a global **CGEventTap**, which macOS only
allows once the app is trusted for Accessibility.

The **menu** is where this is handled. If KeyParty isn't trusted yet, the menu
shows a **Grant Accessibility Access…** button that opens the system prompt and
jumps straight to **System Settings → Privacy & Security → Accessibility**. Turn
KeyParty on there and come back — the menu polls for the grant and flips to
"Keyboard lock ready" on its own, no restart needed. You can still press Start
without it: the game falls back to an in-app monitor that blocks every app/menu
shortcut (just not the window-server-level ones) and upgrades to the full tap the
moment the grant lands.

## How the lockdown works

The kiosk behavior lives in a patched copy of the macOS native host,
[`native/appkit_host.m`](native/appkit_host.m) (vendored from zero-native and
wired up in [`build.zig`](build.zig)). The app opens at a small, ordinary titled
window (the menu). The web UI drives the transitions over a tiny one-way bridge
(`window.keyparty.{start,quit,requestAccessibility,checkAccessibility}`); status
flows back as `keyparty:*` events on the existing `window.zero` event bus.

On **Start** (`-enterKiosk`) it:

- turns the menu window borderless and resizes it to cover the **entire** screen,
  then raises it to `CGShieldingWindowLevel()` so it sits *above* the menu bar and
  dock (the level screen savers use), non-movable;
- sets `NSApplicationPresentationOptions` to hide the dock and menu bar and to
  disable process switching (⌘Tab / Mission Control), force-quit (⌘⌥Esc), the
  power-key session dialog, and app hiding (⌘H);
- installs a **global CGEventTap** at the HID level that swallows every key-down,
  key-up, and modifier change, plus the system-defined power/sleep/media events,
  and forwards each one to the web UI so it still produces an effect — nothing a
  child presses reaches another app or the OS;
- falls back to an in-app key monitor when Accessibility hasn't been granted yet,
  and auto-upgrades to the tap once it is.

On the **Control + Option + Shift + Q** chord (`-exitKioskToMenu`) it reverses all
of that — removes the tap, restores the dock/menu bar, and shrinks the window back
to the centred menu — and emits `keyparty:menu` so the UI shows the menu again.
The menu's Quit button (`-handleKeyPartyControlCommand:`) is what shuts the app
down. The menu's Quit / Close / Hide key-equivalents are also stripped as another
layer of defense.

The game UI (the menu, canvas, effects, sounds, pointer splashes, custom cursor,
the on-screen hint) is in [`frontend/app/page.tsx`](frontend/app/page.tsx). It
consumes the native `key` events the tap forwards, and also handles plain DOM
keys (for development / browser use), calling `preventDefault()` so the web layer
never scrolls, quick-finds, or moves focus.

### Development mode

Run with the screen takeover and keyboard lock disabled for development:

```sh
KEYPARTY_NO_KIOSK=1 zig build run
```

The menu and Start/Quit still work; pressing Start just plays in the normal
window (no full-screen, no keyboard grab), and the **Control + Option + Shift + Q**
chord still returns to the menu — handled by the DOM key path in the web UI.

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
