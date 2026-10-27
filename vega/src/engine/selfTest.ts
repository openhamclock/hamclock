/**
 * Phase 1 on-device self-test for the embedded engine. Off by default; enable with
 * ENGINE_SELFTEST in index.js for a test build. Results are logged with the prefix [HCTEST]
 * (read them with `vega device start-log-stream`).
 *
 * Uses its own data directory so it never touches the user's settings.
 */
import {HcEngine, waitForEngine} from './index';
import type {EngineOptions, EngineStatus} from './index';

const TAG = '[HCTEST]';
const REST = 8080;
const opts = (extra: string[] = []): EngineOptions => ({
  dataDir: '/data/hamclock-selftest',
  backend: 'ohb.hamclock.app:80',
  restPort: REST,
  liveRwPort: 8081,
  lanAccess: false,
  extraArgs: extra,
  watchdogSec: 20,
  // A fresh data dir has no callsign, which forces Setup and keeps REST 8080 down.
  seedCallsign: 'N0CALL',
  seedLat: 40.0,
  seedLng: -105.3,
});

const log = (msg: string) => console.log(`${TAG} ${msg}`);
const running = (s: EngineStatus) => s.state === 'running';
const stopped = (s: EngineStatus) => s.state === 'stopped';

async function httpGet(port: number, path: string, timeoutMs = 4000): Promise<string> {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(`http://127.0.0.1:${port}${path}`, {signal: ctl.signal});
    return `${r.status} ${(await r.text()).split('\n')[0]}`;
  } finally {
    clearTimeout(t);
  }
}

/** Poll until REST answers (it starts after live web, once setup() finishes). */
async function restUp(timeoutMs: number): Promise<string> {
  const t0 = Date.now();
  for (;;) {
    try {
      return await httpGet(REST, '/get_sys.txt');
    } catch (e) {
      if (Date.now() - t0 > timeoutMs) {
        throw new Error(`REST not up: ${String(e)}`);
      }
      await new Promise<void>(r => setTimeout(r, 1000));
    }
  }
}

export async function runEngineSelfTest(): Promise<void> {
  const results: string[] = [];
  const step = async (name: string, fn: () => Promise<string | void>) => {
    const t0 = Date.now();
    log(`BEGIN ${name}`);
    try {
      const info = await fn();
      results.push(`PASS ${name}`);
      log(`PASS ${name} (${Date.now() - t0} ms)${info ? ': ' + info : ''}`);
    } catch (e) {
      results.push(`FAIL ${name}`);
      log(`FAIL ${name}: ${String(e)}`);
      log(`status: ${JSON.stringify(HcEngine.getStatus())}`);
      HcEngine.getLogTail(2500)
        .split('\n')
        .forEach(l => log(`diag ${l}`));
    }
  };

  log('begin');

  // Heartbeat: shows whether JS is alive and what the native host is doing.
  let seen = 0;
  const beat = setInterval(() => {
    const s = HcEngine.getStatus();
    log(`beat state=${s.state} run=${s.runCount} listening=${s.listening.join(',')}`);
    const fresh = s.events.slice(Math.min(seen, s.events.length));
    fresh.forEach(e => log(`event ${e}`));
    seen = s.events.length;
  }, 5000);

  // 1. Stop while the engine sits in its first-run Setup screen (nested wait loop).
  await step('start into Setup', async () => {
    HcEngine.start(opts());
    const s = await waitForEngine(running, 60_000);
    return `listening ${s.listening.join(',')}`;
  });
  await step('stop from Setup', async () => {
    HcEngine.stop();
    await waitForEngine(stopped, 30_000);
  });

  // 2. Repeated start / REST / stop cycles in one app session, skipping Setup (-k).
  for (let i = 1; i <= 3; i++) {
    await step(`cycle ${i} start + REST`, async () => {
      HcEngine.start(opts(['-k']));
      await waitForEngine(running, 60_000);
      return restUp(90_000);
    });
    await step(`cycle ${i} stop`, async () => {
      HcEngine.stop();
      await waitForEngine(stopped, 60_000);
      try {
        await httpGet(REST, '/get_sys.txt', 1500);
      } catch {
        return 'REST closed';
      }
      throw new Error('REST still answers after stop');
    });
  }

  // 3. Engine-initiated restart (ESP.restart -> host restarts the thread).
  await step('engine restart via REST', async () => {
    HcEngine.start(opts(['-k']));
    await waitForEngine(running, 60_000);
    await restUp(90_000);
    const before = HcEngine.getStatus().runCount;
    await httpGet(REST, '/restart').catch(() => undefined);
    await waitForEngine(s => s.runCount > before && running(s), 60_000);
    return restUp(90_000);
  });

  // 4. JS restart, and LAN toggle (restarts with a different bind address).
  await step('JS restart', async () => {
    const before = HcEngine.getStatus().runCount;
    HcEngine.restart();
    await waitForEngine(s => s.runCount > before && running(s), 60_000);
    return restUp(90_000);
  });
  await step('LAN on', async () => {
    HcEngine.setLanAccess(true);
    await waitForEngine(s => s.lanAccess && running(s), 60_000);
    return restUp(90_000);
  });
  await step('LAN off', async () => {
    HcEngine.setLanAccess(false);
    await waitForEngine(s => !s.lanAccess && running(s), 60_000);
    return restUp(90_000);
  });
  await step('final stop', async () => {
    HcEngine.stop();
    await waitForEngine(stopped, 60_000);
  });

  clearInterval(beat);
  const failed = results.filter(r => r.startsWith('FAIL')).length;
  log(`events: ${JSON.stringify(HcEngine.getStatus().events)}`);
  log(`done: ${results.length - failed}/${results.length} passed${failed ? ' -- FAILED' : ''}`);
}
