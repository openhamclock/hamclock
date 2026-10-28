/**
 * App-side access to the embedded HamClock engine (@openhamclock/hcengine).
 */
import {HcEngine} from '@openhamclock/hcengine';
import type {EngineOptions, EngineState, EngineStatus} from '@openhamclock/hcengine';

export {HcEngine};
export type {EngineOptions, EngineState, EngineStatus};

/**
 * Production defaults. The backend is user-configurable later (settings panel).
 * lanAccess is left out on purpose: the engine then uses the user's saved choice.
 */
export const DEFAULT_ENGINE_OPTIONS: EngineOptions = {
  dataDir: '/data/hamclock',
  backend: 'ohb.hamclock.app:80',
  restPort: 8080,
  liveRwPort: 8081,
};

/** Start the engine unless it is already starting or running. */
export function ensureEngineStarted(options: EngineOptions = DEFAULT_ENGINE_OPTIONS): void {
  const {state} = HcEngine.getStatus();
  if (state === 'starting' || state === 'running' || state === 'stopping') {
    return;
  }
  HcEngine.start(options);
}

/** Resolve with the status once `want(status)` holds; reject after timeoutMs. */
export function waitForEngine(
  want: (s: EngineStatus) => boolean,
  timeoutMs: number,
  pollMs = 250,
): Promise<EngineStatus> {
  const t0 = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      const s = HcEngine.getStatus();
      if (want(s)) {
        resolve(s);
      } else if (Date.now() - t0 > timeoutMs) {
        reject(new Error(`engine timeout; state=${s.state}`));
      } else {
        setTimeout(tick, pollMs);
      }
    };
    tick();
  });
}
