import NativeHcEngine from './turbo-modules/HcEngine';

/** Options for starting the embedded HamClock engine. All fields optional. */
export interface EngineOptions {
  /** Engine working directory (-d). Default /data/hamclock. */
  dataDir?: string;
  /** Backend host:port (-b). Default ohb.hamclock.app:80. */
  backend?: string;
  /** RESTful server port (-e); -1 disables. Default 8080. */
  restPort?: number;
  /** Read-write live web port (-w), serves live.html. Default 8081. */
  liveRwPort?: number;
  /** Read-only live web port (-r); 0 = engine default, -1 disables. */
  liveRoPort?: number;
  /** Listen on all interfaces instead of loopback only. Default false. */
  lanAccess?: boolean;
  /** Extra engine command-line arguments, appended as-is. */
  extraArgs?: string[];
  /** Diagnostics only: abort the app (to get a crash report) if getStatus() isn't called for this many seconds. */
  watchdogSec?: number;
  /**
   * First-run seed, written only if no callsign is stored yet. Without a callsign the engine
   * forces Setup (even with -k) and the RESTful server doesn't start until Setup is done.
   */
  seedCallsign?: string;
  /** DE latitude/longitude to seed with seedCallsign (degrees, +N/+E). Both or neither. */
  seedLat?: number;
  seedLng?: number;
}

export type EngineState =
  | 'idle'
  | 'starting'
  | 'running'
  | 'stopping'
  | 'stopped'
  | 'exited'
  | 'error';

export interface EngineStatus {
  state: EngineState;
  /** Number of engine runs this app session (restarts included). */
  runCount: number;
  /** Code passed to exit() by the engine, or null. */
  exitCode: number | null;
  lastError: string;
  lanAccess: boolean;
  restPort: number;
  liveRwPort: number;
  liveRoPort: number;
  /** Ports the engine is listening on right now. */
  listening: number[];
  dataDir: string;
  backend: string;
  version: string;
  /** Recent host events, oldest first. */
  events: string[];
}

/** Typed facade over the HcEngine Turbo Module. Throws on start/restart errors. */
export const HcEngine = {
  start(options: EngineOptions = {}): void {
    const err = NativeHcEngine.start(JSON.stringify(options));
    if (err) {
      throw new Error(err);
    }
  },
  stop(): void {
    NativeHcEngine.stop();
  },
  /** Restart; pass options to change them, or nothing to keep the current ones. */
  restart(options?: EngineOptions): void {
    const err = NativeHcEngine.restart(options ? JSON.stringify(options) : '');
    if (err) {
      throw new Error(err);
    }
  },
  setLanAccess(enabled: boolean): void {
    NativeHcEngine.setLanAccess(enabled);
  },
  getStatus(): EngineStatus {
    return JSON.parse(NativeHcEngine.getStatus()) as EngineStatus;
  },
  getLogTail(maxBytes = 4096): string {
    return NativeHcEngine.getLogTail(maxBytes);
  },
};

export default HcEngine;
