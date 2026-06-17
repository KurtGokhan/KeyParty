import Script from "next/script";
import "./globals.css";

export const metadata = {
  title: "Key Party",
  description: "A key-smashing game for kids.",
};

export const viewport = {
  width: "device-width",
  initialScale: 1,
  maximumScale: 1,
  userScalable: false,
  themeColor: "#0b0420",
};

// Which analytics ship in which build is gated here.
//
// Cloudflare Web Analytics: web build only. The GitHub Pages deploy sets
// NEXT_PUBLIC_WEB_BUILD=1 (see .github/workflows/deploy.yml); the native app
// builds without it. Cloudflare has no custom-dimension support (it can't carry
// a platform tag) and reports unreliably from the desktop app's zero:// origin,
// so the beacon stays on the public site.
//
// Google Analytics: web AND the native desktop app. gtag reaches Google fine
// from the WebView, and — unlike Cloudflare — it lets us tag every hit with the
// platform below, so web / macOS / Windows usage all land in one property.
const IS_WEB_BUILD = process.env.NEXT_PUBLIC_WEB_BUILD === "1";

// Injected at build time from NEXT_PUBLIC_GA_ID. GitHub Actions maps the
// GA_MEASUREMENT_ID repo secret into it for the public site (deploy.yml) and the
// release binaries (release.yml). A GA4 measurement ID isn't truly secret — it
// ships in the client bundle — but keeping it out of the repo means forks and
// local/CI builds don't report into the property, and the ID stays swappable.
// Unset (any build without the secret) → no GA scripts are emitted at all.
const GA_ID = process.env.NEXT_PUBLIC_GA_ID;

// Cloudflare Web Analytics beacon token — injected the same way as GA_ID
// (deploy.yml maps the CLOUDFLARE_ANALYTICS_TOKEN secret into this var). Web
// build only; unset (e.g. a fork that hasn't set its own) → no beacon emitted.
const CF_BEACON_TOKEN = process.env.NEXT_PUBLIC_CF_BEACON_TOKEN;

// `platform` rides on the GA page_view and every custom event (set as a default
// param before config). It's the distribution channel, not the visitor's OS: a
// constant "web" on the site (GA already reports OS there), resolved from the UA
// in the native app, which can be running on either macOS or Windows.
const GA_PLATFORM_EXPR = IS_WEB_BUILD
  ? "'web'"
  : "(/Windows/.test(navigator.userAgent)?'windows':/Macintosh|Mac OS X/.test(navigator.userAgent)?'macos':'desktop')";

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        {children}

        {/* Cloudflare Web Analytics — web build only, and only when a token was
            injected at build time (see note above). */}
        {IS_WEB_BUILD && CF_BEACON_TOKEN && (
          <Script
            id="cloudflare-web-analytics"
            strategy="afterInteractive"
            src="https://static.cloudflareinsights.com/beacon.min.js"
            data-cf-beacon={`{"token": "${CF_BEACON_TOKEN}"}`}
          />
        )}

        {/* Google Analytics (gtag.js) — web + native app, emitted only when an
            ID was injected at build time. The page_view and the custom events
            from app/analytics.ts are all tagged with `platform`. */}
        {GA_ID && (
          <>
            <Script
              id="ga-loader"
              strategy="afterInteractive"
              src={`https://www.googletagmanager.com/gtag/js?id=${GA_ID}`}
            />
            <Script id="ga-init" strategy="afterInteractive">
              {`window.dataLayer = window.dataLayer || [];
function gtag(){dataLayer.push(arguments);}
gtag('js', new Date());
gtag('set', { platform: ${GA_PLATFORM_EXPR} });
gtag('config', '${GA_ID}');`}
            </Script>
          </>
        )}
      </body>
    </html>
  );
}
