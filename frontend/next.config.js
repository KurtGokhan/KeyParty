/** @type {import('next').NextConfig} */
// On GitHub Pages the site is served from a project subpath (e.g. /KeyParty),
// passed in as BASE_PATH by the deploy workflow so assets resolve correctly.
// The native app build leaves BASE_PATH unset and serves from the root of the
// zero:// asset origin.
const basePath = process.env.BASE_PATH || "";

const nextConfig = {
  output: "export",
  basePath,
  assetPrefix: basePath || undefined,
};

module.exports = nextConfig;
