# Changesets

This folder is managed by [Changesets](https://github.com/changesets/changesets).
It drives KeyParty's versioning, changelog, and releases.

## Adding a changeset

When you make a change worth releasing, run:

```sh
npx changeset
```

Pick the bump (patch / minor / major) and write a one-line summary. This creates
a markdown file in this folder — commit it with your change.

## What happens next

On every push to `main`, the **Release** workflow
([`.github/workflows/release.yml`](../.github/workflows/release.yml)) runs
Changesets. When unreleased changesets exist, it opens (or updates) a
**"Version Packages"** PR that bumps the version, updates `CHANGELOG.md`, and —
via [`scripts/sync-version.mjs`](../scripts/sync-version.mjs) — syncs that
version into `app.zon`, `build.zig.zon`, and `frontend/package.json`.

Merging that PR tags the release (`keyparty@x.y.z`) and creates a GitHub
Release. The macOS app is then built and attached to it automatically.

See [the Changesets docs](https://github.com/changesets/changesets/blob/main/docs/intro-to-using-changesets.md)
for more.
