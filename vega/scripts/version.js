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
 *   node scripts/version.js --restore restores ESPHamClock/version.cpp to git state
 *
 * HamClock's own version (ESPHamClock/version.cpp) is updated from the tag at build time,
 * matching Android's build.gradle.kts behavior.
 */
const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..');
const versionCppPath = path.resolve(rootDir, 'ESPHamClock', 'version.cpp');

function git(args) {
  try {
    return execSync(`git ${args}`, { cwd: rootDir, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch (_) {
    return '';
  }
}

function resolveRawTag() {
  return (process.env.TAG || process.env.APP_VERSION || '').trim() || git('describe --exact-match --tags');
}

function resolveVersion() {
  const tag = resolveRawTag();
  return tag ? tag.replace(/^[vV]/, '') : 'edge';
}

function resolveBuildNumber() {
  const n = parseInt(git('rev-list --count HEAD'), 10);
  return Number.isFinite(n) && n > 0 ? n : 1;
}

function deriveHcTag(rawTag) {
  let tag = rawTag.trim().replace(/^[vV]/, '');
  const parts = tag.split('.');
  if (parts.length > 2) {
    tag = parts.slice(0, -1).join('.');
  }
  return tag;
}

function updateVersionCpp(rawTag) {
  if (!rawTag || rawTag === 'edge' || !fs.existsSync(versionCppPath)) return;
  const hcTag = deriveHcTag(rawTag);
  if (!hcTag) return;
  const content = fs.readFileSync(versionCppPath, 'utf8');
  const updated = content.replace(/(hc_version\s*=\s*")[^"]+(")/, `$1${hcTag}$2`);
  if (content !== updated) {
    const backupPath = `${versionCppPath}.orig`;
    if (!fs.existsSync(backupPath)) {
      fs.writeFileSync(backupPath, content, 'utf8');
    }
    fs.writeFileSync(versionCppPath, updated, 'utf8');
    console.error(`[version.js] Updated ESPHamClock/version.cpp hc_version to "${hcTag}"`);
  }
}

function restoreVersionCpp() {
  const backupPath = `${versionCppPath}.orig`;
  if (fs.existsSync(backupPath)) {
    try {
      const orig = fs.readFileSync(backupPath, 'utf8');
      fs.writeFileSync(versionCppPath, orig, 'utf8');
      fs.unlinkSync(backupPath);
      return;
    } catch (_) {}
  }
  if (!fs.existsSync(versionCppPath)) return;
  git(`checkout -- "${versionCppPath}"`) || git(`restore "${versionCppPath}"`);
}

if (process.argv.includes('--restore')) {
  restoreVersionCpp();
  process.exit(0);
}

const rawTag = resolveRawTag();
const version = rawTag ? rawTag.replace(/^[vV]/, '') : 'edge';
const buildNumber = resolveBuildNumber();

if (version !== 'edge') {
  updateVersionCpp(rawTag);
}

if (process.argv.includes('--json')) {
  console.log(JSON.stringify({ version, buildNumber }));
} else {
  console.log(`--build-version ${version} --build-number ${buildNumber}`);
}
