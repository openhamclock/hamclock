/**
 * HTTPS for the engine (Phase 10). HamClock fetches Sat_Alerts (hams.at) and Mesh_Mon data with
 * curl, which Vega apps can't run. The engine queues those requests instead
 * (hcengine/kepler/engine/HttpFetch.cpp); this loop takes them, fetches them with RN fetch() (the
 * OS network stack and CA store), and hands the body back. Like `curl --silent` without -f, the
 * body is returned for any HTTP status; a network error or timeout is reported as a failure.
 * Started in index.js next to the engine; independent of the React tree.
 */
import type {FetchRequest} from '@openhamclock/hcengine';

export const FETCH_POLL_MS = 250;
/** Same as HamClock's `curl --max-time 15`. */
export const FETCH_TIMEOUT_MS = 15_000;

export interface FetchEngine {
  takeFetches(): FetchRequest[];
  fetchDone(id: number, body: string | null): void;
}

export interface FetchBridgeDeps {
  engine: FetchEngine;
  fetchFn: typeof fetch;
  /** Debug log ([HCFETCH] lines); failures always go to console.warn. */
  log?: (msg: string) => void;
}

/** fetch() headers for a request: User-Agent (curl -A) and the one curl -H header. */
export function requestHeaders(r: FetchRequest): Record<string, string> {
  const h: Record<string, string> = {};
  if (r.userAgent) h['User-Agent'] = r.userAgent;
  const colon = r.header.indexOf(':');
  if (colon > 0) {
    const name = r.header.slice(0, colon).trim();
    if (name) h[name] = r.header.slice(colon + 1).trim();
  }
  return h;
}

/** Fetch one request and answer the engine. Never throws. */
export async function runFetch(r: FetchRequest, deps: FetchBridgeDeps): Promise<void> {
  const {engine, fetchFn, log} = deps;
  const t0 = Date.now();
  if (!/^https?:\/\//i.test(r.url)) {
    console.warn(`[HCFETCH] ${r.id}: refused (not http/https)`);
    engine.fetchDone(r.id, null);
    return;
  }
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
  let body: string | null = null;
  try {
    const res = await fetchFn(r.url, {headers: requestHeaders(r), signal: ctl.signal});
    body = await res.text();
    log?.(`[HCFETCH] ${r.id}: HTTP ${res.status} ${body.length} B ${Date.now() - t0} ms ${r.url}`);
  } catch (e) {
    console.warn(`[HCFETCH] ${r.id}: failed after ${Date.now() - t0} ms: ${String(e)} ${r.url}`);
  } finally {
    clearTimeout(timer);
  }
  try {
    engine.fetchDone(r.id, body);
  } catch (e) {
    console.warn(`[HCFETCH] ${r.id}: fetchDone failed: ${String(e)}`);
  }
}

/** One poll: start every new request. Returns their promises (for tests). */
export function pollOnce(deps: FetchBridgeDeps): Promise<void>[] {
  let reqs: FetchRequest[];
  try {
    reqs = deps.engine.takeFetches();
  } catch (e) {
    console.warn(`[HCFETCH] takeFetches failed: ${String(e)}`);
    return [];
  }
  return reqs.map(r => runFetch(r, deps));
}

let started = false;

/** Start the poll loop once per JS runtime. */
export function startFetchBridge(deps: FetchBridgeDeps): void {
  if (started) return;
  started = true;
  const tick = () => {
    pollOnce(deps);
    setTimeout(tick, FETCH_POLL_MS);
  };
  tick();
}

