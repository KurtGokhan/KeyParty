# Contributing

Thanks for your interest in KeyParty! This page covers the contribution and
release workflow. For building and running the app locally — including the
Windows toolchain and how the keyboard lockdown works — see
[DEVELOPMENT.md](DEVELOPMENT.md).

## Workflow

1. Build and run the app locally (see [DEVELOPMENT.md](DEVELOPMENT.md)).
2. Make your change.
3. With each change worth shipping, add a changeset and commit it (see below).
4. Open a pull request.

## Changesets

Versioning and releases run on [Changesets](https://github.com/changesets/changesets).

For each change worth shipping, add a changeset and commit it alongside your
code:

```sh
npx changeset
```

## Releasing

1. On push to `main`, the [Release workflow](.github/workflows/release.yml) opens
   (or updates) a **"Version Packages"** PR. Merging it bumps the version,
   updates `CHANGELOG.md`, syncs that version into `app.zon`, `build.zig.zon`,
   and `frontend/package.json` (via [`scripts/sync-version.mjs`](scripts/sync-version.mjs)),
   tags the release (`keyparty@x.y.z`), and creates a GitHub Release.
2. The same workflow then builds the apps on two runners and uploads both to the
   release:
   - **macOS** — `KeyParty.dmg` (a drag-to-Applications disk image holding
     `Key Party.app`, built from `zig build package`). **Signed (Developer ID) and
     notarized** when the signing secrets are configured (see [Code signing](#code-signing)
     below) — it then launches with no Gatekeeper prompt. Without those secrets it
     ships **unsigned**, so first launch needs right-click → Open (or clearing the
     quarantine flag).
   - **Windows** — `KeyParty.exe`, a single self-contained file
     (frontend embedded, WebView2 loader static-linked), via `zig build` against
     the WebView2 SDK headers + static loader lib (restored from NuGet). Currently
     **unsigned** (signing is scaffolded but inert — see [Code signing](#code-signing)
     below), so SmartScreen shows a "More info → Run anyway" prompt on first launch;
     the WebView2 Evergreen runtime must be present (it is on Windows 11 and current
     Windows 10).

### One-time repo setup

- **Settings → Pages → Source: GitHub Actions** (for the web build).
- **Settings → Actions → General → Workflow permissions:** allow GitHub Actions
  to **create and approve pull requests** (so the version PR can be opened).

## Code signing

Signing is wired into the [Release workflow](.github/workflows/release.yml) as
**post-build steps gated on secrets** — local `zig build` always produces
unsigned binaries, and CI signs only when the relevant secrets exist, so the
pipeline keeps working before any certificate is in place.

**macOS — Developer ID + notarization.** Once the Apple Developer Program
membership is active:

1. In **Keychain Access → Certificate Assistant → Request a Certificate from a
   Certificate Authority…**, generate a CSR. In the
   [Apple Developer portal](https://developer.apple.com/account/resources/certificates)
   create a **Developer ID Application** certificate from that CSR, download it,
   double-click to add it to your keychain, then export it *with its private key*
   as a `.p12`.
2. Create an **App Store Connect API key** (Users and Access → Integrations →
   Keys, role *Developer*) for notarization. Record the **Key ID** and **Issuer
   ID**, and download the `.p8` (one chance only).
3. Add these repository secrets (Settings → Secrets and variables → Actions):

   | Secret | Value |
   | --- | --- |
   | `MACOS_CERTIFICATE` | the `.p12`, base64-encoded (`base64 -i cert.p12 \| pbcopy`) |
   | `MACOS_CERTIFICATE_PWD` | the password set when exporting the `.p12` |
   | `MACOS_SIGN_IDENTITY` | e.g. `Developer ID Application: Your Name (TEAMID)` |
   | `APPLE_NOTARY_KEY` | the `.p8`, base64-encoded |
   | `APPLE_NOTARY_KEY_ID` | the API Key ID |
   | `APPLE_NOTARY_ISSUER` | the Issuer ID (a UUID) |

   With all six set, the next release signs `Key Party.app` with the Hardened
   Runtime ([`assets/keyparty.entitlements`](assets/keyparty.entitlements)), signs
   and notarizes the `.dmg`, and staples the ticket so it opens with no Gatekeeper
   prompt. Set just the first three to sign without notarizing (Gatekeeper still
   warns). WKWebView runs JavaScript in the Apple-signed system WebKit process, so
   the host needs no JIT/unsigned-memory entitlements.

**Windows — unsigned for now (scaffolded).** The release workflow has an inert
`Sign the Windows exe` step that activates once a `WINDOWS_SIGN_CERT` secret is
set and a real signing command is dropped into it. Since June 2023 every
publicly-trusted Windows code-signing certificate must keep its key on hardware
or a cloud service, so the practical CI-friendly routes are **Microsoft Trusted
Signing** (~$10/mo, cheapest), **DigiCert KeyLocker**, or **SSL.com eSigner** —
not a plain `.pfx`. Until then the exe ships unsigned and SmartScreen shows a
"More info → Run anyway" prompt on first launch.
