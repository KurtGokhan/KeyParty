# KeyParty

A **key-smashing game for kids**, built on zero-native (Zig native shell +
Next.js web UI). Every key paints a different burst of color and plays a
different sound. While playing it runs locked down in kiosk mode so a child can
mash the whole keyboard without quitting the game, switching apps, or triggering
anything in the operating system.

It ships as a desktop app for **macOS** and **Windows** (both lock the keyboard
the same way), plus a browser version that plays the same but can't lock the
keyboard.

## Play

```sh
zig build run
```

The app opens in a small **menu** window with **Start** and **Quit** — and, on
macOS, the one-time **Accessibility** setup needed for full keyboard locking
(Windows needs no extra permission). Press **Start** to enter kiosk mode: the
window takes over the **whole** screen — menu bar and dock on macOS, the taskbar
on Windows — and the keyboard is locked. Smash any key, or click and drag with
the mouse:

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

## Accessibility permission (macOS only, for full key blocking)

To swallow **every** system shortcut — including the ones the window server
handles below any normal app (Spotlight, the screenshot keys, the power/sleep
chord, media keys) — the game installs a global **CGEventTap**, which macOS only
allows once the app is trusted for Accessibility. (Windows needs no such
permission — its low-level keyboard hook works as soon as the app starts.)

The **menu** is where this is handled. If KeyParty isn't trusted yet, the menu
shows a **Grant Accessibility Access…** button that opens the system prompt and
jumps straight to **System Settings → Privacy & Security → Accessibility**. Turn
KeyParty on there and come back — the menu polls for the grant and flips to
"Keyboard lock ready" on its own, no restart needed. You can still press Start
without it: the game falls back to an in-app monitor that blocks every app/menu
shortcut (just not the window-server-level ones) and upgrades to the full tap the
moment the grant lands.

## How the lockdown works (macOS)

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

## How the lockdown works (Windows)

Windows uses a patched copy of zero-native's WebView2 host,
[`native/webview2_host.cpp`](native/webview2_host.cpp). The upstream host only
opened a bare window, so this copy also adds the main-window WebView2 (serving the
bundled frontend), the same `window.zero` / `window.keyparty` bridge as macOS, and
the kiosk lockdown. The **shared web UI is unchanged** — it sees the same bridge
events on both platforms.

On **Start** it:

- makes the menu window borderless + topmost and resizes it to cover the whole
  monitor, the taskbar included;
- installs a global **low-level keyboard hook** (`WH_KEYBOARD_LL`) that swallows
  every key — Win, Alt+Tab, Alt+Esc, Alt+F4, Ctrl+Esc, F5/refresh, and the rest —
  and forwards each one to the web UI as a synthetic `key` event, so it still
  produces an on-screen effect while never reaching another app or the OS;
- detects the grown-up **Control + Alt + Shift + Q** chord and returns to the menu.

The one shortcut Windows will not let any app intercept is **Ctrl + Alt + Del**
(the Secure Attention Sequence) — blocking it needs the OS Assigned Access / kiosk
policy, which is outside the app.

The packaged app loads the **WebView2** runtime (the Evergreen runtime ships with
Windows 11 and current Windows 10) via `WebView2Loader.dll`, which is bundled next
to the executable.

### Development mode

Run with the screen takeover and keyboard lock disabled for development:

```sh
KEYPARTY_NO_KIOSK=1 zig build run
```

The menu and Start/Quit still work; pressing Start just plays in the normal
window (no full-screen, no keyboard grab), and the **Control + Option + Shift + Q**
chord still returns to the menu — handled by the DOM key path in the web UI.

## Setup

The zero-native framework is a normal dependency, so install it (and the release
tooling) from the repo root, then let the build pull in the frontend deps:

```sh
npm install   # installs node_modules/zero-native + its CLI, and Changesets
```

`zig build dev`, `zig build run`, and `zig build package` also install the
frontend deps automatically (`npm install --prefix frontend`).

The build reads the framework from `node_modules/zero-native` (anchored at the
repo root) by default — override with `-Dzero-native-path=/path/to/zero-native`
to use a different checkout. The patched native hosts
([`native/appkit_host.m`](native/appkit_host.m),
[`native/webview2_host.cpp`](native/webview2_host.cpp)) ship in this repo, so
only the rest of the framework comes from that path.

`zig build dev` and `zig build package` shell out to the `zero-native` CLI, which
`npm install` puts in `node_modules/.bin`. Run those via `npx zig build …` (or
add `node_modules/.bin` to your `PATH`) so the CLI resolves — or install it
globally with `npm i -g zero-native@0.2.0`.

On **Windows**, the kiosk host ([`native/webview2_host.cpp`](native/webview2_host.cpp))
is compiled with **MSVC's `cl.exe`**, not Zig's bundled clang — the WebView2/WRL
(`wrl.h`) headers are MSVC-flavored and Zig's libc++ can't target the MSVC C++
ABI. Build from an *x64 Native Tools Command Prompt* (which puts `cl.exe` and the
Windows SDK on `PATH`/`%INCLUDE%`), passing only the WebView2 SDK headers:

```sh
zig build run -Dtarget=x86_64-windows-msvc ^
  -Dwebview2-include="C:\path\to\Microsoft.Web.WebView2\build\native\include"
```

`cl.exe` resolves the Windows SDK, `winrt` (`wrl.h`), and STL headers from
`%INCLUDE%` on its own; `-Dwinrt-include=...` is still accepted but only needed if
your `%INCLUDE%` somehow lacks the winrt folder. Omitting `-Dwebview2-include`
makes the host compile to a blank-window stub (it says so at launch). `zig build
run` also needs `WebView2Loader.dll` (from the WebView2 SDK's `build/native/x64/`)
findable at runtime — drop it in the repo root, which the run's working directory
searches. The WebView2 Evergreen runtime must be installed too (it is on Windows
11 / current Windows 10).

### Testing a Windows build without a local toolchain

Don't want to install MSVC? Run the **Windows build (test)** workflow
([`.github/workflows/windows-build.yml`](.github/workflows/windows-build.yml))
from the repo's **Actions** tab — or just push a change under `native/`,
`build.zig`, `app.zon`, or `frontend/` and it runs automatically. Download the
**keyparty-windows** artifact, unzip it, and run **`bin\keyparty.exe`** — the
frontend ships alongside it in `resources\frontend\out`, and `WebView2Loader.dll`
sits next to the exe in `bin\`. No release needed.

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

## Releasing

Versioning and releases run on [Changesets](https://github.com/changesets/changesets).

1. With each change worth shipping, add a changeset and commit it:
   ```sh
   npx changeset
   ```
2. On push to `main`, the [Release workflow](.github/workflows/release.yml) opens
   (or updates) a **"Version Packages"** PR. Merging it bumps the version,
   updates `CHANGELOG.md`, syncs that version into `app.zon`, `build.zig.zon`,
   and `frontend/package.json` (via [`scripts/sync-version.mjs`](scripts/sync-version.mjs)),
   tags the release (`keyparty@x.y.z`), and creates a GitHub Release.
3. The same workflow then builds the apps (`zig build package`, with the
   `zero-native` framework installed from npm) on two runners and uploads both to
   the release:
   - **macOS** — `keyparty-<version>-macos.zip` (a `.app` bundle). Unsigned, so on
     first launch users right-click → Open (or clear the quarantine flag).
   - **Windows** — `keyparty-<version>-windows.zip` (the `.exe`, the bundled
     frontend, and `WebView2Loader.dll`). Built against the WebView2 SDK headers
     (restored from NuGet) with the winrt headers from the MSVC dev environment.
     Unsigned, so SmartScreen shows a "More info → Run anyway" prompt on first
     launch; the WebView2 Evergreen runtime must be present (it is on Windows 11
     and current Windows 10).

One-time repo setup:

- **Settings → Pages → Source: GitHub Actions** (for the web build).
- **Settings → Actions → General → Workflow permissions:** allow GitHub Actions
  to **create and approve pull requests** (so the version PR can be opened).
