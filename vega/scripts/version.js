#!/usr/bin/env node
/**
 * Version and build number for the Vega package, the same way the Android app gets them
 * (android/app/build.gradle.kts):
 *   - version: the release tag without its leading "v" (e.g. v4.34b00.0 -> 4.34b00.0), from the
 *     TAG or APP_VERSION env var (release.yml sets TAG), else `git describe --exact-match --tags`;
 *     "edge" when not building a tag.
 *   - build number: the git commit count (Android's versionCode), so it always increases.
 *
 * They are passed to the build as --build-version / --build-number; the device and the Appstore
 * read them from the package's meta-info (manifest.toml's own version is a placeholder).
 *
 *   node scripts/version.js          prints "--build-version <v> --build-number <n>"
 *   node scripts/version.js --json   prints {"version": "...", "buildNumber": n}
 *
 * HamClock's own version (ESPHamClock/version.cpp) is set from the tag by release.yml.
 */
const { execSync } = require('child_process');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..');

function git(args) {
  try {
    return execSync(`git ${args}`, { cwd: rootDir, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch (_) {
    return '';
  }
}

function resolveVersion() {
  const tag = (process.env.TAG || process.env.APP_VERSION || '').trim() || git('describe --exact-match --tags');
  return tag ? tag.replace(/^[vV]/, '') : 'edge';
}

function resolveBuildNumber() {
  const n = parseInt(git('rev-list --count HEAD'), 10);
  return Number.isFinite(n) && n > 0 ? n : 1;
}

const version = resolveVersion();
const buildNumber = resolveBuildNumber();
if (process.argv.includes('--json')) {
  console.log(JSON.stringify({ version, buildNumber }));
} else {
  console.log(`--build-version ${version} --build-number ${buildNumber}`);
}
