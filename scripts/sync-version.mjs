// Propagate the version Changesets just wrote into the root package.json out to
// the other places the build reads it:
//   - app.zon          -> the packaged .app's version (read by `zero-native package`)
//   - build.zig.zon    -> read by build.zig for the package artifact filename
//   - frontend/package.json (kept in lockstep; private, but tidy)
//
// Run automatically by `npm run version` (i.e. `changeset version`).
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const version = JSON.parse(readFileSync(join(root, "package.json"), "utf8")).version;

/** Replace the first match of `pattern` in a file, erroring if the field is absent. */
function syncFile(relPath, pattern, replacement, label) {
  const path = join(root, relPath);
  const before = readFileSync(path, "utf8");
  if (!pattern.test(before)) {
    throw new Error(`sync-version: could not find ${label} in ${relPath}`);
  }
  const after = before.replace(pattern, replacement);
  if (after !== before) writeFileSync(path, after);
  console.log(`sync-version: ${relPath} -> ${version}`);
}

// `.version = "x.y.z"` — anchored to line start so it never matches
// `.minimum_zig_version` in build.zig.zon.
const zonVersion = /^(\s*\.version\s*=\s*")[^"]*(")/m;
syncFile("app.zon", zonVersion, `$1${version}$2`, ".version");
syncFile("build.zig.zon", zonVersion, `$1${version}$2`, ".version");

// `"version": "x.y.z"`
syncFile(
  "frontend/package.json",
  /("version"\s*:\s*")[^"]*(")/,
  `$1${version}$2`,
  '"version" field'
);
