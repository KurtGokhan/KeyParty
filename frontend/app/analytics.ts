// Thin wrapper over Google Analytics (gtag.js). The gtag base snippet ships in
// both the web build and the native desktop app (see app/layout.tsx), and every
// hit is tagged there with a `platform` dimension. If GA ever isn't loaded
// (script blocked, offline at startup), window.gtag is undefined and each call
// here is a silent no-op — so trackEvent is safe to call from anywhere, on any
// build, without guards at the call site.

type EventParams = Record<string, string | number | boolean | undefined>;

declare global {
  interface Window {
    gtag?: (command: "event", name: string, params?: EventParams) => void;
  }
}

/** Send a GA4 custom event. No-op unless the GA snippet is loaded (web build). */
export function trackEvent(name: string, params?: EventParams): void {
  if (typeof window === "undefined") return;
  window.gtag?.("event", name, params);
}
