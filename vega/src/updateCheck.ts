/**
 * Update check, as the Android app does on Amazon devices (AppUpdateHelper.kt): ask the backend
 * for the latest HamClock release (/ham/HamClock/version.pl, first line) and, if it's newer than
 * the running HamClock, suggest updating from the Amazon Appstore. Vega apps can't query the
 * Appstore themselves, so this is only a notice (with a link to the app's Appstore page).
 */

export interface Version {
  major: number;
  minor: number;
  patch: number;
  /** null: a stable release. */
  beta: number | null;
}

/** First check after launch, then every CHECK_INTERVAL_MS (as Android). */
export const FIRST_CHECK_DELAY_MS = 15_000;
export const CHECK_INTERVAL_MS = 6 * 3600_000;
/** "Later" hides the notice for that version this long. */
export const SNOOZE_MS = 24 * 3600_000;
const FETCH_TIMEOUT_MS = 8_000;

/**
 * Amazon Appstore ASIN of this app (the public listing). The notice offers
 * "Open Appstore" (amzns://apps?asin=…, the app's Appstore page). The package form
 * (amzns://apps?p=<package>) doesn't help: it launches the app if installed, i.e. ourselves.
 */
export const APPSTORE_ASIN = 'B0HHCXSB4Y'; // the public listing (shared with the Android app)
export const APPSTORE_URL = APPSTORE_ASIN ? `amzns://apps?asin=${APPSTORE_ASIN}` : '';

/**
 * "4.32" / "v4.32.0" / "4.32b00" / "v4.32b01.0" → version; "edge" or junk → null.
 * Same rules as Android's VersionComparator.parse.
 */
export function parseVersion(s: string | null | undefined): Version | null {
  if (!s) return null;
  const clean = s.trim().replace(/^[vV]/, '');
  if (clean.toLowerCase() === 'edge') return null;
  const m = /^(\d+)\.(\d+)(?:b(\d+))?(?:\.(\d+))?/.exec(clean);
  if (!m) return null;
  return {
    major: Number(m[1]),
    minor: Number(m[2]),
    patch: m[4] ? Number(m[4]) : 0,
    beta: m[3] !== undefined ? Number(m[3]) : null,
  };
}

/**
 * Is `remote` newer than `current`? As Android's VersionComparator.isNewer: false if either
 * can't be parsed; a stable install is never offered a beta; a beta install is offered the final
 * release of the same version or a later beta.
 */
export function isNewer(current: string | null | undefined, remote: string | null | undefined): boolean {
  const c = parseVersion(current);
  const r = parseVersion(remote);
  if (!c || !r) return false;
  if (c.beta === null && r.beta !== null) return false;
  if (r.major !== c.major) return r.major > c.major;
  if (r.minor !== c.minor) return r.minor > c.minor;
  if (r.patch !== c.patch) return r.patch > c.patch;
  if (c.beta !== null && r.beta === null) return true;
  if (c.beta !== null && r.beta !== null) return r.beta > c.beta;
  return false;
}

/** URLs to try, in order (as Android): HTTPS on the backend host, the backend as set, the default. */
export function versionUrls(backend: string): string[] {
  const b = backend.trim() || 'ohb.hamclock.app:80';
  const host = b.split(':')[0];
  return Array.from(
    new Set([
      `https://${host}/ham/HamClock/version.pl`,
      `http://${b}/ham/HamClock/version.pl`,
      'https://ohb.hamclock.app/ham/HamClock/version.pl',
    ]),
  );
}

/** The latest release from the backend (first line of version.pl), or null if none answered. */
export async function fetchLatestVersion(
  backend: string,
  userAgent: string,
  fetchFn: typeof fetch = fetch,
  log?: (msg: string) => void,
): Promise<string | null> {
  for (const url of versionUrls(backend)) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
    try {
      const res = await fetchFn(url, {headers: {'User-Agent': userAgent}, signal: ctl.signal});
      if (res.ok) {
        const first = (await res.text()).split(/\r?\n/)[0].trim();
        if (first) {
          log?.(`[HCUPD] ${url}: ${first}`);
          return first;
        }
      }
      log?.(`[HCUPD] ${url}: HTTP ${res.status}`);
    } catch (e) {
      log?.(`[HCUPD] ${url}: ${String(e)}`);
    } finally {
      clearTimeout(timer);
    }
  }
  return null;
}

/** The notice to show, if any: the remote version is newer and not snoozed. */
export function noticeFor(
  current: string | null | undefined,
  remote: string | null | undefined,
  snooze: {version: string; until: number} | null,
  now: number,
): string | null {
  if (!remote || !isNewer(current, remote)) return null;
  if (snooze && snooze.version === remote && now < snooze.until) return null;
  return remote;
}

