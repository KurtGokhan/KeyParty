"use client";

import { useEffect, useRef, useState } from "react";
import { trackEvent } from "./analytics";

/* ------------------------------------------------------------------ *
 * KeyParty — a key-smashing game for kids.
 *
 * The app opens at a small "menu" (Start / Quit / accessibility setup).
 * Pressing Start asks the native shell (see native/appkit_host.m) to enter
 * kiosk mode: full-screen, with a global event tap that swallows every OS
 * shortcut so nothing the child presses can quit the game, switch apps, or
 * poke the operating system. Swallowed keys are forwarded back to this UI as
 * a native "key" event, so even Cmd/Ctrl/Option chords and lone modifiers
 * still make something happen on screen. Clicks and drags paint too.
 *
 * Every key makes a different splash of color and a different sound. The
 * grown-up chord — Control + Option + Shift + Q — leaves the game and returns
 * to the menu (the native tap drives it in kiosk; the DOM path below handles
 * it in plain-browser / KEYPARTY_NO_KIOSK dev mode).
 * ------------------------------------------------------------------ */

type Mode = "menu" | "playing";

type AccessibilityStatus = { trusted: boolean; kioskEnabled: boolean };

/** The window.keyparty shim the native shell injects (absent in a plain browser). */
// macOS window background: "solid" = opaque chrome, "blurry" = desktop shows
// through and is blurred in CSS, "transparent" = raw desktop shows through sharp.
type BackdropMode = "solid" | "blurry" | "transparent";

type KeyPartyBridge = {
  start?: () => void;
  quit?: () => void;
  requestAccessibility?: () => void;
  checkAccessibility?: () => void;
  // macOS only: switch the window background between solid / clear / blurred.
  setBackdrop?: (mode: BackdropMode) => void;
};

const keyPartyBridge = (): KeyPartyBridge | undefined =>
  (window as unknown as { keyparty?: KeyPartyBridge }).keyparty;

// The GitHub repo and the stable "latest release" download URLs the release
// workflow publishes (version-less asset names). The desktop note and these
// links show only in the browser build, which can't lock the keyboard the way
// the native kiosk shell does.
const REPO_URL = "https://github.com/KurtGokhan/KeyParty";
const DOWNLOADS = {
  mac: `${REPO_URL}/releases/latest/download/keyparty-macos.dmg`,
  windows: `${REPO_URL}/releases/latest/download/keyparty-windows.exe`,
};

type DesktopOS = "mac" | "windows" | "other";

// Best-effort guess at the visitor's desktop OS so we can lead with the right
// download. Runs only in the browser (after mount) to avoid SSR/hydration skew.
const detectOS = (): DesktopOS => {
  if (typeof navigator === "undefined") return "other";
  const ua = navigator.userAgent;
  if (/Macintosh|Mac OS X/.test(ua)) return "mac";
  if (/Windows/.test(ua)) return "windows";
  return "other";
};

// Set by the GitHub Pages deploy workflow; unset in the native app build. Lets
// the web build show the "download the app" note and hide Quit deterministically
// (no first-paint flicker), while still falling back to runtime shell detection.
const IS_WEB_BUILD = process.env.NEXT_PUBLIC_WEB_BUILD === "1";

type Shape = "circle" | "star" | "square" | "triangle" | "ring";

type Particle = {
  x: number;
  y: number;
  vx: number;
  vy: number;
  life: number;
  max: number;
  size: number;
  hue: number;
  light: number;
  shape: Shape;
  rot: number;
  spin: number;
  grav: number;
};

type Glyph = {
  text: string;
  x: number;
  y: number;
  life: number;
  max: number;
  hue: number;
  rot: number;
  vrot: number;
  scale: number;
  pop: number;
};

type Ring = {
  x: number;
  y: number;
  life: number;
  max: number;
  hue: number;
  rainbow: boolean;
  maxR: number;
};

/** A normalized key, fed from either the native tap or DOM keydown. */
type KeyInfo = {
  code: string;
  key: string;
  repeat: boolean;
  ctrl: boolean;
  alt: boolean;
  meta: boolean;
  shift: boolean;
};

type Category =
  | "letter"
  | "digit"
  | "space"
  | "enter"
  | "arrow"
  | "erase"
  | "tab"
  | "escape"
  | "modifier"
  | "other";

const PENTATONIC = [0, 2, 4, 7, 9]; // major pentatonic semitone offsets

const MODIFIER_CODES = new Set([
  "ShiftLeft",
  "ShiftRight",
  "ControlLeft",
  "ControlRight",
  "AltLeft",
  "AltRight",
  "MetaLeft",
  "MetaRight",
  "CapsLock",
  "Fn",
]);

// Per-modifier look + sound, keyed by the family in its code.
const MODIFIER_STYLE: Record<string, { hue: number; freq: number; sym: string }> = {
  Shift: { hue: 200, freq: 392.0, sym: "⇧" },
  Control: { hue: 135, freq: 329.63, sym: "⌃" },
  Alt: { hue: 32, freq: 293.66, sym: "⌥" },
  Meta: { hue: 300, freq: 440.0, sym: "⌘" },
  CapsLock: { hue: 50, freq: 523.25, sym: "⇪" },
  Fn: { hue: 330, freq: 246.94, sym: "fn" },
};

const modifierFamily = (code: string): string => {
  if (code.startsWith("Shift")) return "Shift";
  if (code.startsWith("Control")) return "Control";
  if (code.startsWith("Alt")) return "Alt";
  if (code.startsWith("Meta")) return "Meta";
  if (code === "CapsLock") return "CapsLock";
  if (code === "Fn") return "Fn";
  return "Shift";
};

export default function Home() {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const hintRef = useRef<HTMLDivElement>(null);
  const startRef = useRef<HTMLDivElement>(null);
  const counterRef = useRef<HTMLDivElement>(null);
  // Holds the rare DOM "blur ripple" effects (canvas can't blur the desktop).
  const fxRef = useRef<HTMLDivElement>(null);

  // "menu" shows the Start/Quit screen; "playing" runs the game. The game
  // effect reads modeRef (a ref, so it always sees the latest value without
  // re-running the effect); React renders the menu off the `mode` state.
  const [mode, setMode] = useState<Mode>("menu");
  const modeRef = useRef<Mode>("menu");
  // null = not yet determined (avoids flashing the wrong banner on first paint);
  // true = native shell present; false = plain browser build.
  const [hasNative, setHasNative] = useState<boolean | null>(null);
  const [accessibility, setAccessibility] = useState<AccessibilityStatus | null>(null);
  // The visitor's desktop OS, so the browser build leads with the right download.
  const [os, setOs] = useState<DesktopOS | null>(null);
  // Glass background: canBackdrop is true once the native shell exposes
  // setBackdrop (macOS); `backdrop` is the live mode the menu toggle cycles.
  // Defaults to "transparent" — the see-through desktop backdrop — to match the
  // native shells, which also open in transparent mode (see appkit_host.m /
  // webview2_host.cpp). The menu toggle cycles back to solid/blurry from here.
  const [canBackdrop, setCanBackdrop] = useState(false);
  const [backdrop, setBackdrop] = useState<BackdropMode>("transparent");
  // Imperative hooks into the game engine, published by the game effect.
  const engineRef = useRef<{ enterPlaying: () => void; returnToMenu: () => void } | null>(null);

  useEffect(() => {
    modeRef.current = mode;
  }, [mode]);

  // Detect the native shell and keep the Accessibility status fresh while the
  // menu is up, so "Grant Access…" flips to "ready" the moment the grown-up
  // toggles KeyParty on in System Settings (no restart needed).
  useEffect(() => {
    const kp = keyPartyBridge();
    const native = typeof kp !== "undefined";
    setHasNative(native);
    setOs(detectOS());
    setCanBackdrop(typeof kp?.setBackdrop === "function");
    if (!kp) return;
    kp.checkAccessibility?.();
    const id = window.setInterval(() => {
      if (modeRef.current === "menu") keyPartyBridge()?.checkAccessibility?.();
    }, 1500);
    return () => window.clearInterval(id);
  }, []);

  // Mirror the background mode onto <html> so the CSS can react (see globals.css:
  // kp-blurry / kp-transparent). It lives on the root element because <html>'s
  // background propagates to the whole viewport — clearing only <body> would
  // leave that root fill painting over the desktop.
  useEffect(() => {
    const root = document.documentElement;
    root.classList.toggle("kp-blurry", backdrop === "blurry");
    root.classList.toggle("kp-transparent", backdrop === "transparent");
    return () => root.classList.remove("kp-blurry", "kp-transparent");
  }, [backdrop]);

  // Cycle solid → blurry → transparent → solid. On Windows the WebView2 compositor
  // can't blur the desktop behind the transparent window (backdrop-filter has nothing
  // to sample there), so "blurry" would look identical to "transparent" — skip it and
  // cycle solid → transparent → solid. macOS keeps all three.
  const handleCycleBackdrop = () => {
    setBackdrop((m) => {
      const next: BackdropMode =
        m === "solid"
          ? os === "windows"
            ? "transparent"
            : "blurry"
          : m === "blurry"
            ? "transparent"
            : "solid";
      keyPartyBridge()?.setBackdrop?.(next);
      return next;
    });
  };

  const handleStart = () => {
    // Switch the UI to the game immediately (responsive), then ask the native
    // shell to lock down. In a plain browser there's no shell — the UI just plays.
    engineRef.current?.enterPlaying();
    keyPartyBridge()?.start?.();
  };

  const handleQuit = () => {
    const kp = keyPartyBridge();
    if (kp?.quit) kp.quit();
    else window.close();
  };

  const handleGrantAccess = () => {
    keyPartyBridge()?.requestAccessibility?.();
  };

  // The download + repo links only render in the web build, which is exactly
  // where GA is active — so these are the user actions worth measuring (the app
  // store of this kids' game is "did the visitor grab the desktop app?").
  const handleDownload = (target: "mac" | "windows") => () =>
    trackEvent("download_app", { os: target });

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    let width = 0;
    let height = 0;
    let dpr = 1;

    const resize = () => {
      // Cap the backing resolution at 1.5x. The effects are soft glows, so a
      // slightly lower resolution is imperceptible, but it cuts the per-frame
      // fill rate by ~45% vs dpr 2 (a big deal on a 4K-class display).
      dpr = Math.min(window.devicePixelRatio || 1, 1.5);
      width = window.innerWidth;
      height = window.innerHeight;
      canvas.width = Math.floor(width * dpr);
      canvas.height = Math.floor(height * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    };
    resize();
    window.addEventListener("resize", resize);

    /* ---------------------------- audio ---------------------------- */
    let audio: AudioContext | null = null;
    const ensureAudio = (): AudioContext | null => {
      try {
        if (!audio) {
          const Ctor =
            window.AudioContext ||
            (window as unknown as { webkitAudioContext: typeof AudioContext })
              .webkitAudioContext;
          audio = new Ctor();
        }
        if (audio.state === "suspended") void audio.resume();
        return audio;
      } catch {
        return null;
      }
    };

    const tone = (
      freq: number,
      opts: {
        type?: OscillatorType;
        dur?: number;
        gain?: number;
        glideTo?: number;
        delay?: number;
      } = {}
    ) => {
      const ac = ensureAudio();
      if (!ac) return;
      const { type = "sine", dur = 0.28, gain = 0.18, glideTo, delay = 0 } = opts;
      const t0 = ac.currentTime + delay;
      const osc = ac.createOscillator();
      const g = ac.createGain();
      osc.type = type;
      osc.frequency.setValueAtTime(freq, t0);
      if (glideTo) {
        osc.frequency.exponentialRampToValueAtTime(Math.max(40, glideTo), t0 + dur);
      }
      g.gain.setValueAtTime(0.0001, t0);
      g.gain.exponentialRampToValueAtTime(gain, t0 + 0.012);
      g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
      osc.connect(g);
      g.connect(ac.destination);
      osc.start(t0);
      osc.stop(t0 + dur + 0.03);
    };

    const boom = () => {
      const ac = ensureAudio();
      if (!ac) return;
      const t0 = ac.currentTime;
      const dur = 0.45;
      const buffer = ac.createBuffer(1, Math.floor(ac.sampleRate * dur), ac.sampleRate);
      const data = buffer.getChannelData(0);
      for (let i = 0; i < data.length; i++) {
        data[i] = (Math.random() * 2 - 1) * (1 - i / data.length);
      }
      const src = ac.createBufferSource();
      src.buffer = buffer;
      const lp = ac.createBiquadFilter();
      lp.type = "lowpass";
      lp.frequency.value = 700;
      const g = ac.createGain();
      g.gain.setValueAtTime(0.45, t0);
      g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
      src.connect(lp);
      lp.connect(g);
      g.connect(ac.destination);
      src.start(t0);
      src.stop(t0 + dur);
      tone(90, { type: "sine", dur: 0.4, gain: 0.3, glideTo: 45 });
    };

    const noteFreq = (index: number, baseOctave = 0) => {
      const step = PENTATONIC[((index % 5) + 5) % 5];
      const octave = baseOctave + Math.floor(index / 5);
      const semis = step + octave * 12;
      return 261.63 * Math.pow(2, semis / 12); // from middle C
    };

    /* --------------------------- effects --------------------------- */
    const particles: Particle[] = [];
    const glyphs: Glyph[] = [];
    const rings: Ring[] = [];
    const MAX_PARTICLES = 1000;
    let shake = 0;
    let count = 0;

    const rand = (a: number, b: number) => a + Math.random() * (b - a);
    const pick = <T,>(arr: T[]): T => arr[Math.floor(Math.random() * arr.length)];

    const burst = (
      x: number,
      y: number,
      n: number,
      hue: number,
      opts: {
        spread?: number;
        speed?: number;
        shapes?: Shape[];
        rainbow?: boolean;
        gravity?: boolean;
      } = {}
    ) => {
      const {
        spread = Math.PI * 2,
        speed = 5,
        shapes = ["circle"],
        rainbow = false,
        gravity = true,
      } = opts;
      for (let i = 0; i < n; i++) {
        const ang = rand(-spread / 2, spread / 2) - Math.PI / 2 + rand(-0.4, 0.4);
        const sp = rand(speed * 0.3, speed) * rand(0.6, 1.4);
        const max = rand(0.6, 1.3);
        particles.push({
          x,
          y,
          vx: Math.cos(ang) * sp * 60,
          vy: Math.sin(ang) * sp * 60,
          life: max,
          max,
          size: rand(6, 18),
          hue: rainbow ? rand(0, 360) : hue + rand(-22, 22),
          light: rand(55, 72),
          shape: pick(shapes),
          rot: rand(0, Math.PI * 2),
          spin: rand(-6, 6),
          grav: gravity ? rand(500, 900) : 0,
        });
      }
      if (particles.length > MAX_PARTICLES) {
        particles.splice(0, particles.length - MAX_PARTICLES);
      }
    };

    const addGlyphAt = (text: string, hue: number, x: number, y: number) => {
      glyphs.push({
        text,
        x,
        y,
        life: 1.1,
        max: 1.1,
        hue,
        rot: rand(-0.25, 0.25),
        vrot: rand(-1.2, 1.2),
        scale: Math.min(width, height) * rand(0.16, 0.26),
        pop: 0,
      });
      if (glyphs.length > 20) glyphs.shift();
    };

    // True only in "transparent" mode (the raw, sharp desktop showing through).
    // Blur ripples are skipped in "blurry" mode, where the whole desktop is
    // already blurred so a ripple would add nothing.
    const isTransparentMode = () =>
      document.documentElement.classList.contains("kp-transparent");

    // A rare "blur shockwave": an expanding circle whose backdrop-filter blurs the
    // desktop (and canvas) showing through within it, then fades. It has to be a
    // DOM element — a <canvas> can only paint pixels, it can't blur what's behind
    // the transparent web view. Self-removes when its animation ends.
    //
    // We grow the real width/height (NOT transform: scale) on purpose: WebKit
    // applies backdrop-filter in the element's local space and then scales the
    // result, so a scaled blur radius balloons with the scale factor. Sizing the
    // box directly keeps blur(Npx) at N px on screen the whole time. And we fade
    // through the blur radius rather than opacity, because WebKit drops the
    // backdrop-filter entirely once opacity < 1.
    const spawnBlurRipple = (x: number, y: number) => {
      const layer = fxRef.current;
      if (!layer) return;
      const el = document.createElement("div");
      el.className = "fx-blur-ripple";
      layer.appendChild(el);

      const maxD = Math.min(width, height) * 0.6; // final diameter
      const peak = 10; // px — blur radius at its strongest (tunable)
      const anim = el.animate(
        [
          {
            width: "0px",
            height: "0px",
            left: `${x}px`,
            top: `${y}px`,
            backdropFilter: "blur(0px)",
          },
          { backdropFilter: `blur(${peak}px)`, offset: 0.15 },
          { backdropFilter: `blur(${peak}px)`, offset: 0.7 },
          {
            width: `${maxD}px`,
            height: `${maxD}px`,
            left: `${x - maxD / 2}px`,
            top: `${y - maxD / 2}px`,
            backdropFilter: "blur(0px)",
          },
        ],
        { duration: 850, easing: "cubic-bezier(0.22, 0.61, 0.36, 1)", fill: "both" },
      );
      const done = () => el.remove();
      anim.onfinish = done;
      anim.oncancel = done;
    };

    const tally = () => {
      count += 1;
      if (counterRef.current) {
        counterRef.current.textContent = String(count);
        counterRef.current.classList.add("show");
      }
      if (startRef.current) startRef.current.classList.add("hidden");
    };

    /* ------------------------ key handling ------------------------- */
    const categorize = (info: KeyInfo): Category => {
      const c = info.code;
      if (MODIFIER_CODES.has(c)) return "modifier";
      if (/^Key[A-Z]$/.test(c)) return "letter";
      if (/^Digit[0-9]$/.test(c) || /^Numpad[0-9]$/.test(c)) return "digit";
      if (c === "Space") return "space";
      if (c === "Enter" || c === "NumpadEnter") return "enter";
      if (c.startsWith("Arrow")) return "arrow";
      if (c === "Backspace" || c === "Delete") return "erase";
      if (c === "Tab") return "tab";
      if (c === "Escape") return "escape";
      return "other";
    };

    const glyphFor = (info: KeyInfo, cat: Category): string => {
      switch (cat) {
        case "letter":
          return info.code.slice(3);
        case "digit":
          return info.code.replace(/\D/g, "");
        case "space":
          return "✦";
        case "enter":
          return "↵";
        case "erase":
          return "⌫";
        case "tab":
          return "⇥";
        case "escape":
          return "⎋";
        case "modifier":
          return MODIFIER_STYLE[modifierFamily(info.code)].sym;
        case "arrow":
          return (
            { ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→" }[
              info.code
            ] ?? "✦"
          );
        default:
          return info.key.length === 1
            ? info.key
            : pick(["★", "✿", "❀", "✧", "❤", "☆", "✺", "✸"]);
      }
    };

    let lastRepeatSound = 0;

    const doEffect = (info: KeyInfo, opts: { glyph?: boolean } = {}) => {
      const cat = categorize(info);
      const repeat = info.repeat;
      // The big letter normally only shows on the first press; while a key is
      // held the loop re-pops it at its own cadence via opts.glyph.
      const showGlyph = opts.glyph ?? !repeat;
      const now = performance.now();
      const cx = width / 2;
      const cy = height / 2;

      // Pick a focal point near the centre so glyph + burst line up.
      const fx = rand(width * 0.25, width * 0.75);
      const fy = rand(height * 0.32, height * 0.68);

      let hue = rand(0, 360);

      switch (cat) {
        case "letter": {
          const i = info.code.charCodeAt(3) - 65;
          hue = (i / 26) * 360;
          burst(fx, fy, repeat ? 12 : 34, hue, {
            speed: 6,
            shapes: ["circle", "ring"],
          });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(noteFreq(i, 0), { type: "triangle", dur: 0.3, gain: 0.16 });
            lastRepeatSound = now;
          }
          break;
        }
        case "digit": {
          const d = parseInt(info.code.replace(/\D/g, ""), 10);
          hue = 45 + d * 6;
          burst(fx, fy, repeat ? 12 : 30, hue, {
            speed: 6.5,
            shapes: ["star", "square"],
          });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(noteFreq(d, 1), { type: "square", dur: 0.22, gain: 0.12 });
            lastRepeatSound = now;
          }
          break;
        }
        case "space": {
          // Big firework + screen shake.
          burst(fx, fy, repeat ? 26 : 90, hue, {
            speed: 11,
            shapes: ["circle", "star", "ring", "triangle"],
            rainbow: true,
          });
          rings.push({
            x: fx,
            y: fy,
            life: 0.9,
            max: 0.9,
            hue,
            rainbow: true,
            maxR: Math.min(width, height) * 0.55,
          });
          shake = Math.min(shake + 16, 26);
          if (!repeat || now - lastRepeatSound > 110) {
            boom();
            lastRepeatSound = now;
          }
          break;
        }
        case "enter": {
          rings.push({
            x: cx,
            y: cy,
            life: 1.1,
            max: 1.1,
            hue,
            rainbow: true,
            maxR: Math.min(width, height) * 0.7,
          });
          burst(cx, cy, repeat ? 14 : 40, hue, {
            speed: 7,
            shapes: ["ring", "circle"],
            rainbow: true,
          });
          if (!repeat) {
            [0, 4, 7].forEach((s, k) =>
              tone(261.63 * Math.pow(2, s / 12) * 2, {
                type: "sawtooth",
                dur: 0.5,
                gain: 0.1,
                delay: k * 0.05,
              })
            );
          }
          break;
        }
        case "arrow": {
          const dir: Record<string, [number, number, number]> = {
            ArrowUp: [0, -1, 200],
            ArrowDown: [0, 1, 200],
            ArrowLeft: [-1, 0, 200],
            ArrowRight: [1, 0, 200],
          };
          const [dx, dy] = dir[info.code] ?? [0, -1, 200];
          hue = 190 + rand(-20, 40);
          for (let i = 0; i < (repeat ? 8 : 22); i++) {
            const max = rand(0.5, 0.9);
            particles.push({
              x: cx,
              y: cy,
              vx: dx * rand(400, 800) + rand(-60, 60),
              vy: dy * rand(400, 800) + rand(-60, 60),
              life: max,
              max,
              size: rand(8, 16),
              hue: hue + rand(-20, 20),
              light: 64,
              shape: "triangle",
              rot: Math.atan2(dy, dx),
              spin: 0,
              grav: 0,
            });
          }
          if (!repeat || now - lastRepeatSound > 70) {
            tone(330, { type: "sine", dur: 0.25, gain: 0.14, glideTo: dy < 0 ? 660 : 165 });
            lastRepeatSound = now;
          }
          break;
        }
        case "erase": {
          hue = 8 + rand(-8, 20);
          burst(fx, fy, repeat ? 10 : 26, hue, { speed: 5, shapes: ["square", "circle"] });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(420, { type: "sawtooth", dur: 0.3, gain: 0.13, glideTo: 120 });
            lastRepeatSound = now;
          }
          break;
        }
        case "tab": {
          hue = 280 + rand(-20, 30);
          burst(fx, fy, repeat ? 10 : 26, hue, { speed: 6, shapes: ["star", "ring"] });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(180, { type: "triangle", dur: 0.3, gain: 0.13, glideTo: 520 });
            lastRepeatSound = now;
          }
          break;
        }
        case "escape": {
          hue = 0 + rand(-10, 16);
          rings.push({
            x: cx,
            y: cy,
            life: 0.7,
            max: 0.7,
            hue,
            rainbow: false,
            maxR: Math.min(width, height) * 0.4,
          });
          burst(cx, cy, repeat ? 10 : 24, hue, { speed: 6, shapes: ["ring", "triangle"] });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(520, { type: "square", dur: 0.22, gain: 0.12, glideTo: 180 });
            lastRepeatSound = now;
          }
          break;
        }
        case "modifier": {
          // Lone modifier keys get their own colour, symbol, and chime —
          // pressing Shift/Ctrl/Option/Cmd is no longer a dead key.
          const style = MODIFIER_STYLE[modifierFamily(info.code)];
          hue = style.hue;
          burst(fx, fy, repeat ? 10 : 30, hue, {
            speed: 6,
            shapes: ["ring", "circle", "star"],
            gravity: false,
          });
          rings.push({
            x: fx,
            y: fy,
            life: 0.8,
            max: 0.8,
            hue,
            rainbow: false,
            maxR: Math.min(width, height) * 0.32,
          });
          if (!repeat || now - lastRepeatSound > 90) {
            tone(style.freq, { type: "sine", dur: 0.32, gain: 0.13, glideTo: style.freq * 1.5 });
            lastRepeatSound = now;
          }
          break;
        }
        default: {
          burst(fx, fy, repeat ? 12 : 30, hue, {
            speed: 7,
            shapes: ["circle", "star", "square", "triangle", "ring"],
            rainbow: true,
          });
          if (!repeat || now - lastRepeatSound > 70) {
            tone(rand(220, 700), {
              type: pick(["sine", "triangle", "square"]) as OscillatorType,
              dur: 0.25,
              gain: 0.13,
            });
            lastRepeatSound = now;
          }
        }
      }

      // Modifiers + the centre keys read better when the glyph sits at the burst.
      if (showGlyph) {
        if (cat === "enter" || cat === "arrow" || cat === "escape") {
          addGlyphAt(glyphFor(info, cat), hue, cx, cy);
        } else {
          addGlyphAt(glyphFor(info, cat), hue, fx, fy);
        }
      }

      // A fresh press in "transparent" mode also sends out a blur shockwave.
      // Never on key-repeat, so a held key doesn't machine-gun them.
      if (!repeat && isTransparentMode()) {
        spawnBlurRipple(fx, fy);
      }

      tally();
    };

    /* --------------------------- the hint -------------------------- */
    let hintTimer: number | undefined;
    const showHint = () => {
      const el = hintRef.current;
      if (!el) return;
      el.classList.add("show");
      if (hintTimer) window.clearTimeout(hintTimer);
      hintTimer = window.setTimeout(() => el.classList.remove("show"), 2600);
    };

    // A key is "special" — would normally poke the OS — if a command/control/
    // option modifier is held, or it is itself a modifier key. Those still get
    // an effect (above), and additionally flash the grown-up quit hint.
    const isSpecial = (info: KeyInfo) =>
      info.ctrl || info.alt || info.meta || MODIFIER_CODES.has(info.code) || info.code === "Escape";

    // Keys the game itself repeats while physically held. The OS only
    // auto-repeats the last key pressed, so to let a child hold several keys at
    // once we track every held key here and drive their repeats from the loop.
    // Modifiers and "Other" (punctuation, which all share one code) stay
    // one-shot.
    const heldKeys = new Map<string, KeyInfo>();
    const heldSince = new Map<string, number>();
    const heldGlyphSince = new Map<string, number>();
    const HOLD_INTERVAL = 90; // ms between particle repeats, per held key
    const GLYPH_INTERVAL = 260; // ms between letter re-pops, per held key
    const isHoldable = (info: KeyInfo) =>
      info.code !== "Other" && !MODIFIER_CODES.has(info.code);

    const handleKey = (info: KeyInfo) => {
      // Only the game consumes keys; in the menu they belong to the buttons.
      if (modeRef.current !== "playing") return;
      // Ignore OS auto-repeat — the loop drives repeats for ALL held keys, so a
      // single OS-repeating key would otherwise fire twice as fast as the rest.
      if (info.repeat) return;
      if (isHoldable(info)) {
        // Already held? Then this is an OS auto-repeat the HID tap didn't flag
        // as one. Ignore it: the loop already drives this key, so we don't want
        // to fire the (heavier) initial burst or re-pin its glyph again.
        if (heldKeys.has(info.code)) return;
        const now = performance.now();
        heldKeys.set(info.code, info);
        heldSince.set(info.code, now);
        heldGlyphSince.set(info.code, now);
      }
      doEffect(info);
      if (isSpecial(info)) showHint();
    };

    const releaseKey = (code: string) => {
      heldKeys.delete(code);
      heldSince.delete(code);
      heldGlyphSince.delete(code);
    };
    const releaseAllKeys = () => {
      heldKeys.clear();
      heldSince.clear();
      heldGlyphSince.clear();
    };

    // 1) Native path: the kiosk event tap swallows the real key and sends it
    //    here, so even blocked chords/modifiers reach the game.
    const onNativeKey = (detail: unknown) => {
      const d = (detail ?? {}) as Partial<KeyInfo>;
      handleKey({
        code: typeof d.code === "string" ? d.code : "Other",
        key: typeof d.key === "string" ? d.key : "",
        repeat: !!d.repeat,
        ctrl: !!d.ctrl,
        alt: !!d.alt,
        meta: !!d.meta,
        shift: !!d.shift,
      });
    };

    const zero = (
      window as unknown as {
        zero?: { on?: (n: string, cb: (detail: unknown) => void) => void };
      }
    ).zero;
    // The native tap forwards key releases as "keyup" so we can stop repeating
    // a key the moment it's let go.
    const onNativeKeyUp = (detail: unknown) => {
      const code = (detail as { code?: string })?.code;
      if (typeof code === "string") releaseKey(code);
    };
    zero?.on?.("key", onNativeKey);
    zero?.on?.("keyup", onNativeKeyUp);
    zero?.on?.("hint", showHint);
    const onNativeKeyEvent = (e: Event) => onNativeKey((e as CustomEvent).detail);
    const onNativeKeyUpEvent = (e: Event) => onNativeKeyUp((e as CustomEvent).detail);
    window.addEventListener("zero-native:key", onNativeKeyEvent as EventListener);
    window.addEventListener("zero-native:keyup", onNativeKeyUpEvent as EventListener);
    window.addEventListener("zero-native:hint", showHint as EventListener);

    // 2) Browser/dev path: when the tap is not active (plain browser, or
    //    KEYPARTY_NO_KIOSK=1), the keys reach the webview as DOM events.
    const onKeyDown = (e: KeyboardEvent) => {
      // In the menu, keys belong to the buttons (Tab/Enter/Space) — don't touch them.
      if (modeRef.current !== "playing") return;
      // The game owns every keystroke — no scrolling, quick-find, or focus moves.
      e.preventDefault();
      // Grown-up chord (browser / KEYPARTY_NO_KIOSK dev path, where there's no
      // native tap to catch it): Control + Option + Shift + Q returns to the menu.
      if (e.ctrlKey && e.altKey && e.shiftKey && !e.metaKey && e.code === "KeyQ") {
        engineRef.current?.returnToMenu();
        return;
      }
      handleKey({
        code: e.code,
        key: e.key,
        repeat: e.repeat,
        ctrl: e.ctrlKey,
        alt: e.altKey,
        meta: e.metaKey,
        shift: e.shiftKey,
      });
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (modeRef.current !== "playing") return;
      e.preventDefault();
      releaseKey(e.code);
    };
    window.addEventListener("keydown", onKeyDown, { passive: false });
    window.addEventListener("keyup", onKeyUp, { passive: false });
    // If focus is lost we may miss the key-up; drop all held keys so none get
    // stuck repeating. (In kiosk the window never loses focus, but dev mode can.)
    const onBlur = () => releaseAllKeys();
    const onVisibility = () => {
      if (document.hidden) releaseAllKeys();
    };
    window.addEventListener("blur", onBlur);
    document.addEventListener("visibilitychange", onVisibility);

    // Block pinch-zoom gestures, and the context menu while playing (so a child
    // can't right-click out of the game). On the menu screen — a grown-up screen,
    // native shell or not — let the right-click menu through, which also exposes
    // "Inspect Element" for debugging.
    const block = (e: Event) => {
      if (e.type === "contextmenu" && modeRef.current === "menu") {
        return;
      }
      e.preventDefault();
    };
    window.addEventListener("contextmenu", block);
    window.addEventListener("gesturestart", block);

    /* ------------------------ pointer effects ---------------------- */
    // Clicks and drags paint at the pointer, with a colour that rolls through
    // the rainbow so every splash differs.
    let pointerHue = rand(0, 360);
    const pointer = { x: width / 2, y: height / 2, active: false, vis: false };
    let lastDragX = 0;
    let lastDragY = 0;
    let lastDragSound = 0;

    const pointerBurst = (x: number, y: number, big: boolean) => {
      pointerHue = (pointerHue + rand(18, 40)) % 360;
      burst(x, y, big ? 34 : 12, pointerHue, {
        speed: big ? 7 : 4.5,
        shapes: ["circle", "star", "ring", "triangle"],
        gravity: false,
      });
      if (big) {
        rings.push({
          x,
          y,
          life: 0.7,
          max: 0.7,
          hue: pointerHue,
          rainbow: false,
          maxR: Math.min(width, height) * 0.28,
        });
      }
    };

    const onPointerDown = (e: PointerEvent) => {
      if (modeRef.current !== "playing") return; // menu clicks belong to the buttons
      pointer.x = e.clientX;
      pointer.y = e.clientY;
      pointer.active = true;
      pointer.vis = true;
      lastDragX = e.clientX;
      lastDragY = e.clientY;
      pointerBurst(e.clientX, e.clientY, true);
      tone(noteFreq(Math.floor(rand(0, 10)), 1), { type: "triangle", dur: 0.26, gain: 0.14 });
      tally();
    };

    const onPointerMove = (e: PointerEvent) => {
      if (modeRef.current !== "playing") return; // no canvas cursor/paint over the menu
      pointer.x = e.clientX;
      pointer.y = e.clientY;
      pointer.vis = true;
      if (!pointer.active) return; // only drags paint, not idle hover
      const dx = e.clientX - lastDragX;
      const dy = e.clientY - lastDragY;
      const dist = Math.hypot(dx, dy);
      if (dist < 16) return;
      lastDragX = e.clientX;
      lastDragY = e.clientY;
      pointerBurst(e.clientX, e.clientY, false);
      const now = performance.now();
      if (now - lastDragSound > 55) {
        tone(220 + (e.clientX / width) * 660, { type: "sine", dur: 0.14, gain: 0.07 });
        lastDragSound = now;
      }
    };

    const onPointerUp = () => {
      pointer.active = false;
    };
    const onPointerLeave = () => {
      pointer.active = false;
      pointer.vis = false;
    };

    window.addEventListener("pointerdown", onPointerDown);
    window.addEventListener("pointermove", onPointerMove);
    window.addEventListener("pointerup", onPointerUp);
    window.addEventListener("pointercancel", onPointerUp);
    window.addEventListener("pointerleave", onPointerLeave);

    /* ----------------------- menu / play transitions ---------------- */
    // Shared by the menu buttons and the native shell (Start → playing;
    // quit chord → menu). Each transition wipes the field so the screen is
    // clean on the way in and behind the menu on the way out.
    const clearField = () => {
      particles.length = 0;
      rings.length = 0;
      glyphs.length = 0;
      shake = 0;
    };
    const enterPlaying = () => {
      // Guard so one Start counts once, even if the native shell echoes a
      // "keyparty:playing" after handleStart already entered (a non-issue in
      // the web build, where GA actually runs and there is no native shell).
      const wasPlaying = modeRef.current === "playing";
      clearField();
      releaseAllKeys();
      count = 0;
      if (counterRef.current) {
        counterRef.current.textContent = "0";
        counterRef.current.classList.remove("show");
      }
      if (startRef.current) startRef.current.classList.remove("hidden");
      modeRef.current = "playing";
      setMode("playing");
      if (!wasPlaying) trackEvent("start_game");
    };
    const returnToMenu = () => {
      const wasPlaying = modeRef.current === "playing";
      clearField();
      releaseAllKeys();
      count = 0;
      pointer.active = false;
      pointer.vis = false;
      if (counterRef.current) {
        counterRef.current.textContent = "0";
        counterRef.current.classList.remove("show");
      }
      modeRef.current = "menu";
      setMode("menu");
      if (wasPlaying) trackEvent("return_to_menu");
    };
    engineRef.current = { enterPlaying, returnToMenu };

    // The native shell drives these in kiosk mode: "keyparty:menu" when the
    // quit chord is hit, "keyparty:playing" once the kiosk window is up.
    const onNativeMenu = () => returnToMenu();
    const onNativePlaying = () => enterPlaying();
    const onNativeAccessibility = (detail: unknown) => {
      const d = (detail ?? {}) as Partial<AccessibilityStatus>;
      setAccessibility({ trusted: !!d.trusted, kioskEnabled: !!d.kioskEnabled });
    };
    zero?.on?.("keyparty:menu", onNativeMenu);
    zero?.on?.("keyparty:playing", onNativePlaying);
    zero?.on?.("keyparty:accessibility", onNativeAccessibility);
    const onNativeMenuEvent = () => returnToMenu();
    const onNativePlayingEvent = () => enterPlaying();
    const onNativeAccessibilityEvent = (e: Event) =>
      onNativeAccessibility((e as CustomEvent).detail);
    window.addEventListener("zero-native:keyparty:menu", onNativeMenuEvent);
    window.addEventListener("zero-native:keyparty:playing", onNativePlayingEvent);
    window.addEventListener("zero-native:keyparty:accessibility", onNativeAccessibilityEvent);

    /* ------------------------- draw helpers ------------------------ */
    // Pre-rendered glow sprites: the soft radial halo each particle used to get
    // from a live `shadowBlur` (the single most expensive 2D-canvas op). We bake
    // it once per hue bucket into a small offscreen canvas; at draw time each
    // particle is a cheap `drawImage` of its cached glow instead of a per-draw
    // Gaussian blur — the key to staying at 120fps with thousands of particles.
    const GLOW_BUCKETS = 24;
    const GLOW_SIZE = 64;
    const glowSprites: HTMLCanvasElement[] = [];
    for (let h = 0; h < GLOW_BUCKETS; h++) {
      const sprite = document.createElement("canvas");
      sprite.width = sprite.height = GLOW_SIZE;
      const g = sprite.getContext("2d");
      if (!g) continue;
      const hue = (h / GLOW_BUCKETS) * 360;
      const r = GLOW_SIZE / 2;
      const grad = g.createRadialGradient(r, r, 0, r, r, r);
      grad.addColorStop(0, `hsla(${hue}, 95%, 72%, 0.95)`);
      grad.addColorStop(0.35, `hsla(${hue}, 95%, 60%, 0.45)`);
      grad.addColorStop(1, `hsla(${hue}, 95%, 50%, 0)`);
      g.fillStyle = grad;
      g.fillRect(0, 0, GLOW_SIZE, GLOW_SIZE);
      glowSprites.push(sprite);
    }
    const glowFor = (hue: number): HTMLCanvasElement => {
      let bi = Math.round((hue / 360) * GLOW_BUCKETS) % GLOW_BUCKETS;
      if (bi < 0) bi += GLOW_BUCKETS;
      return glowSprites[bi] ?? glowSprites[0];
    };

    const drawShape = (p: Particle, alpha: number) => {
      ctx.save();
      ctx.translate(p.x, p.y);
      ctx.rotate(p.rot);
      ctx.globalAlpha = alpha;
      ctx.fillStyle = `hsl(${p.hue} 95% ${p.light}%)`;
      // No shadowBlur here — the glow comes from the additive glow sprite drawn
      // behind the shape in the particle loop.
      const s = p.size;
      switch (p.shape) {
        case "circle":
          ctx.beginPath();
          ctx.arc(0, 0, s, 0, Math.PI * 2);
          ctx.fill();
          break;
        case "square":
          ctx.fillRect(-s, -s, s * 2, s * 2);
          break;
        case "ring":
          ctx.lineWidth = Math.max(2, s * 0.35);
          ctx.strokeStyle = ctx.fillStyle;
          ctx.beginPath();
          ctx.arc(0, 0, s, 0, Math.PI * 2);
          ctx.stroke();
          break;
        case "triangle":
          ctx.beginPath();
          ctx.moveTo(0, -s);
          ctx.lineTo(s, s);
          ctx.lineTo(-s, s);
          ctx.closePath();
          ctx.fill();
          break;
        case "star":
          ctx.beginPath();
          for (let i = 0; i < 10; i++) {
            const r = i % 2 === 0 ? s : s * 0.45;
            const a = (Math.PI / 5) * i - Math.PI / 2;
            ctx.lineTo(Math.cos(a) * r, Math.sin(a) * r);
          }
          ctx.closePath();
          ctx.fill();
          break;
      }
      ctx.restore();
    };

    // A friendly custom cursor drawn on the canvas: a glowing, gently spinning
    // star-wand that trails the pointer. (CSS also sets a star cursor so the
    // pointer is always visible even between frames.)
    const drawCursor = (t: number) => {
      if (!pointer.vis) return;
      const pulse = 0.85 + 0.15 * Math.sin(t / 220);
      const hue = (t / 16) % 360;
      ctx.save();
      ctx.translate(pointer.x, pointer.y);
      ctx.globalAlpha = pointer.active ? 0.95 : 0.8;
      ctx.shadowColor = `hsl(${hue} 95% 60%)`;
      ctx.shadowBlur = 22;
      ctx.fillStyle = `hsl(${hue} 95% 66%)`;
      ctx.rotate(t / 900);
      ctx.scale(pulse, pulse);
      const s = 14;
      ctx.beginPath();
      for (let i = 0; i < 10; i++) {
        const r = i % 2 === 0 ? s : s * 0.42;
        const a = (Math.PI / 5) * i - Math.PI / 2;
        ctx.lineTo(Math.cos(a) * r, Math.sin(a) * r);
      }
      ctx.closePath();
      ctx.fill();
      ctx.lineWidth = 2;
      ctx.strokeStyle = "rgba(255,255,255,0.92)";
      ctx.stroke();
      ctx.restore();
    };

    /* --------------------------- the loop -------------------------- */
    // Run at the display's native refresh rate (e.g. 120Hz). Physics is driven
    // by real elapsed time, so motion is correct at any rate; the dt clamp only
    // guards against huge jumps after a stall or a backgrounded tab.
    let raf = 0;
    let last = performance.now();

    const frame = (t: number) => {
      raf = requestAnimationFrame(frame);
      const dt = Math.min((t - last) / 1000, 0.05);
      last = t;

      // Drive repeats for every physically-held key (not just the one the OS
      // chooses to auto-repeat), so holding several keys streams effects from
      // all of them at once.
      if (heldKeys.size > 0) {
        heldKeys.forEach((info, code) => {
          if (t - (heldSince.get(code) ?? 0) >= HOLD_INTERVAL) {
            heldSince.set(code, t);
            // Re-pop the letter at a gentler cadence than the particles, so
            // every held key keeps showing its glyph — not just the one the OS
            // happens to auto-repeat.
            const glyph = t - (heldGlyphSince.get(code) ?? 0) >= GLYPH_INTERVAL;
            if (glyph) heldGlyphSince.set(code, t);
            doEffect({ ...info, repeat: true }, { glyph });
          }
        });
      }

      // Full clear each frame — exact, leaves no residue (every pixel returns to
      // the flat page background). Trails come from the per-particle streaks.
      ctx.globalAlpha = 1;
      ctx.shadowBlur = 0;
      ctx.clearRect(0, 0, width, height);

      ctx.save();
      if (shake > 0.2) {
        ctx.translate(rand(-shake, shake), rand(-shake, shake));
        shake *= Math.pow(0.001, dt);
      } else {
        shake = 0;
      }

      // Rings. Bounded so a held key (which spawns one per press) can't pile up
      // an unbounded number of (shadow-blurred) ring strokes.
      if (rings.length > 28) rings.splice(0, rings.length - 28);
      for (let i = rings.length - 1; i >= 0; i--) {
        const r = rings[i];
        r.life -= dt;
        if (r.life <= 0) {
          rings.splice(i, 1);
          continue;
        }
        const p = 1 - r.life / r.max;
        const radius = r.maxR * (1 - Math.pow(1 - p, 3));
        ctx.globalAlpha = (1 - p) * 0.8;
        ctx.lineWidth = Math.max(3, 22 * (1 - p));
        ctx.strokeStyle = r.rainbow
          ? `hsl(${(p * 360 + r.hue) % 360} 95% 62%)`
          : `hsl(${r.hue} 95% 62%)`;
        ctx.shadowColor = ctx.strokeStyle;
        ctx.shadowBlur = 24;
        ctx.beginPath();
        ctx.arc(r.x, r.y, radius, 0, Math.PI * 2);
        ctx.stroke();
      }

      // Particles — drawn additively (overlaps bloom) with a cached glow sprite
      // for the halo, so there is no per-particle shadowBlur. drawImage of a
      // baked sprite is an order of magnitude cheaper than a live blur, which is
      // what lets thousands of particles (e.g. holding Space) stay smooth.
      ctx.shadowBlur = 0;
      ctx.lineCap = "round";
      ctx.globalCompositeOperation = "lighter";
      // When the field gets dense, the sheer particle count already reads as a
      // trail, so we drop the per-particle streak — shedding a draw call each
      // exactly when load is highest. Sparse play keeps the streaks.
      const dense = particles.length > 600;
      for (let i = particles.length - 1; i >= 0; i--) {
        const p = particles[i];
        p.life -= dt;
        if (p.life <= 0) {
          particles.splice(i, 1);
          continue;
        }
        p.vy += p.grav * dt;
        p.vx *= 0.985;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.rot += p.spin * dt;
        const alpha = Math.min(1, p.life / p.max);
        // Motion streak: a glowing tail behind a fast-moving particle, drawn
        // fresh each frame (no residue).
        if (!dense) {
          const speed = Math.hypot(p.vx, p.vy);
          if (speed > 70) {
            const k = 0.024;
            ctx.globalAlpha = alpha * 0.45;
            ctx.strokeStyle = `hsl(${p.hue} 95% ${p.light}%)`;
            ctx.lineWidth = p.size;
            ctx.beginPath();
            ctx.moveTo(p.x - p.vx * k, p.y - p.vy * k);
            ctx.lineTo(p.x, p.y);
            ctx.stroke();
          }
        }
        // Soft glow halo (cheap cached sprite). A circle particle already *is*
        // this glow, so only the non-circle shapes need a crisp shape on top —
        // which halves the draw calls for the most common particle.
        const glow = glowFor(p.hue);
        const d = p.size * 2.8;
        ctx.globalAlpha = alpha;
        ctx.drawImage(glow, p.x - d / 2, p.y - d / 2, d, d);
        if (p.shape !== "circle") drawShape(p, alpha);
      }
      ctx.globalCompositeOperation = "source-over";

      // Glyphs.
      ctx.shadowBlur = 0;
      for (let i = glyphs.length - 1; i >= 0; i--) {
        const g = glyphs[i];
        g.life -= dt;
        if (g.life <= 0) {
          glyphs.splice(i, 1);
          continue;
        }
        g.pop = Math.min(1, g.pop + dt * 7);
        g.rot += g.vrot * dt * (g.life / g.max);
        const fade = g.life / g.max;
        const pop = 0.6 + 0.4 * (1 - Math.pow(1 - g.pop, 3));
        ctx.save();
        ctx.translate(g.x, g.y);
        ctx.rotate(g.rot);
        ctx.scale(pop, pop);
        ctx.globalAlpha = Math.min(1, fade * 1.4);
        ctx.font = `900 ${g.scale}px ui-rounded, "SF Pro Rounded", system-ui, sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.shadowColor = `hsl(${g.hue} 95% 60%)`;
        ctx.shadowBlur = 40;
        ctx.fillStyle = `hsl(${g.hue} 95% 66%)`;
        ctx.fillText(g.text, 0, 0);
        ctx.lineWidth = Math.max(2, g.scale * 0.04);
        ctx.strokeStyle = "rgba(255,255,255,0.9)";
        ctx.strokeText(g.text, 0, 0);
        ctx.restore();
      }

      ctx.restore();

      // The custom cursor sits on top, in screen space (unaffected by shake).
      ctx.globalAlpha = 1;
      drawCursor(t);
    };
    raf = requestAnimationFrame(frame);

    /* --------------------------- cleanup --------------------------- */
    return () => {
      cancelAnimationFrame(raf);
      window.removeEventListener("resize", resize);
      window.removeEventListener("keydown", onKeyDown);
      window.removeEventListener("keyup", onKeyUp);
      window.removeEventListener("blur", onBlur);
      document.removeEventListener("visibilitychange", onVisibility);
      window.removeEventListener("contextmenu", block);
      window.removeEventListener("gesturestart", block);
      window.removeEventListener("zero-native:key", onNativeKeyEvent as EventListener);
      window.removeEventListener("zero-native:keyup", onNativeKeyUpEvent as EventListener);
      window.removeEventListener("zero-native:hint", showHint as EventListener);
      window.removeEventListener("zero-native:keyparty:menu", onNativeMenuEvent);
      window.removeEventListener("zero-native:keyparty:playing", onNativePlayingEvent);
      window.removeEventListener("zero-native:keyparty:accessibility", onNativeAccessibilityEvent);
      engineRef.current = null;
      window.removeEventListener("pointerdown", onPointerDown);
      window.removeEventListener("pointermove", onPointerMove);
      window.removeEventListener("pointerup", onPointerUp);
      window.removeEventListener("pointercancel", onPointerUp);
      window.removeEventListener("pointerleave", onPointerLeave);
      if (hintTimer) window.clearTimeout(hintTimer);
      void audio?.close();
    };
  }, []);

  // "Web" = the Pages build, or (as a fallback) any run with no native shell.
  const isWeb = IS_WEB_BUILD || hasNative === false;
  const showAccess = hasNative === true && accessibility?.kioskEnabled;
  const showWebNote = isWeb;

  return (
    <div className={mode === "menu" ? "stage stage-menu" : "stage"}>
      <div ref={fxRef} className="fx-layer" aria-hidden="true" />
      <canvas ref={canvasRef} />
      <div ref={counterRef} className="counter">0</div>
      <div ref={startRef} className="start">
        Smash any key!
        <small>🎹 🌈 🎉 — or click and drag</small>
      </div>
      <div ref={hintRef} className="hint">
        Grown-ups: hold <kbd>Control</kbd> + <kbd>Option</kbd> + <kbd>Shift</kbd> + <kbd>Q</kbd> to go back to the menu
      </div>

      {mode === "menu" && (
        <div className="menu" role="dialog" aria-modal="true" aria-label="Key Party menu">
          <div className="menu-card">
            <h1 className="menu-title">Key Party</h1>
            <p className="menu-sub">A key-smashing party for little hands 🎹🌈🎉</p>

            <div className="menu-actions">
              <button type="button" className="btn btn-start" onClick={handleStart} autoFocus>
                ▶ Start
              </button>
              {/* Quit closes the native app; in the browser there's nothing to
                  quit (and window.close() is a no-op for normal tabs), so hide it. */}
              {!isWeb && (
                <button type="button" className="btn btn-quit" onClick={handleQuit}>
                  ✕ Quit
                </button>
              )}
            </div>

            {/* macOS only: cycle the window background solid → blurry → transparent. */}
            {canBackdrop && (
              <button
                type="button"
                className="btn btn-glass"
                aria-pressed={backdrop !== "solid"}
                onClick={handleCycleBackdrop}
              >
                🪟 Background: {backdrop}
              </button>
            )}

            {showAccess && (
              <div className={`access ${accessibility?.trusted ? "ok" : "warn"}`}>
                {accessibility?.trusted ? (
                  <span className="access-line">
                    🔒 Keyboard lock ready — every key is safe to smash.
                  </span>
                ) : (
                  <>
                    <span className="access-line">
                      ⚠ Key Party needs <strong>Accessibility</strong> permission so it can lock the
                      keyboard while playing — otherwise a child could press a system shortcut and
                      slip out of the game.
                    </span>
                    <button type="button" className="btn btn-grant" onClick={handleGrantAccess}>
                      Grant Accessibility Access…
                    </button>
                    <span className="access-hint">
                      Turn on “Key Party” in System Settings → Privacy &amp; Security → Accessibility,
                      then come back here. You can still Start without it.
                    </span>
                  </>
                )}
              </div>
            )}

            {showWebNote && (
              <div className="web-note">
                <span className="access-line">
                  🌐 You’re playing in the browser. Key Party works best as the desktop app —
                  the browser can’t lock the keyboard, so a child can still press a shortcut and
                  slip out of the game.
                </span>
                <div className="downloads">
                  {os === "windows" ? (
                    <>
                      <a className="btn btn-download" href={DOWNLOADS.windows} target="_blank" rel="noreferrer" onClick={handleDownload("windows")}>
                        ⬇ Download for Windows
                      </a>
                      <a className="dl-alt" href={DOWNLOADS.mac} target="_blank" rel="noreferrer" onClick={handleDownload("mac")}>
                        or the macOS version
                      </a>
                    </>
                  ) : os === "mac" ? (
                    <>
                      <a className="btn btn-download" href={DOWNLOADS.mac} target="_blank" rel="noreferrer" onClick={handleDownload("mac")}>
                        ⬇ Download for macOS
                      </a>
                      <a className="dl-alt" href={DOWNLOADS.windows} target="_blank" rel="noreferrer" onClick={handleDownload("windows")}>
                        or the Windows version
                      </a>
                    </>
                  ) : (
                    <>
                      <a className="btn btn-download" href={DOWNLOADS.mac} target="_blank" rel="noreferrer" onClick={handleDownload("mac")}>
                        ⬇ macOS
                      </a>
                      <a className="btn btn-download" href={DOWNLOADS.windows} target="_blank" rel="noreferrer" onClick={handleDownload("windows")}>
                        ⬇ Windows
                      </a>
                    </>
                  )}
                </div>
              </div>
            )}

            <p className="menu-foot">
              While playing, press <kbd>Control</kbd>+<kbd>Option</kbd>+<kbd>Shift</kbd>+
              <kbd>Q</kbd> to return here.
            </p>

            {isWeb && (
              <a className="repo-link" href={REPO_URL} target="_blank" rel="noreferrer" onClick={() => trackEvent("view_repo")}>
                View on GitHub ↗
              </a>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
