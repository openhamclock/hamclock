import NativeHcEngine from './turbo-modules/HcEngine';

/** Options for starting the embedded HamClock engine. All fields optional. */
export interface EngineOptions {
  /** Engine working directory (-d). Default /data/hamclock. */
  dataDir?: string;
  /** Backend host:port (-b). Omit to use the saved setting (default ohb.hamclock.app:80); given, it is saved. */
  backend?: string;
  /** mDNS host name base (e.g. 'hamclock' for hamclock.local). Omit to use the saved setting; given, it is saved. */
  mdnsName?: string;
  /** RESTful server port (-e); -1 disables. Default 8080. */
  restPort?: number;
  /** Read-write live web port (-w), serves live.html. Default 8081. */
  liveRwPort?: number;
  /** Read-only live web port (-r); 0 = engine default, -1 disables. */
  liveRoPort?: number;
  /**
   * Listen on all interfaces instead of loopback only; only loopback and private LAN addresses
   * may connect. Saved in dataDir; omit it to use the saved choice (false if none).
   */
  lanAccess?: boolean;
  /** Extra engine command-line arguments, appended as-is. */
  extraArgs?: string[];
  /** Diagnostics only: abort the app (to get a crash report) if getStatus() isn't called for this many seconds. */
  watchdogSec?: number;
  /**
   * If no callsign is stored (Setup needed), turn LAN access on and save it before the engine
   * binds, so the first-run QR code works at once. Applied at most once per app process.
   */
  firstRunLanAccess?: boolean;
  /**
   * Tests only (selfTest.ts): store this callsign if none is stored, so REST comes up without
   * Setup. The app never seeds; the user enters their own callsign in HamClock's Setup.
   */
  testSeedCallsign?: string;
  /**
   * Open HamClock's Setup page at once on the next run only (engine arg -u). Every run gets -k,
   * so other starts and restarts go straight to the clock.
   */
  openSetup?: boolean;
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
  /**
   * false: no callsign was stored when this run started, so HamClock shows Setup. Turns true
   * once the REST port listens (HamClock starts REST only after Setup). null until checked.
   */
  setupComplete: boolean | null;
  restPort: number;
  liveRwPort: number;
  liveRoPort: number;
  /** Ports the engine is listening on right now. */
  listening: number[];
  /** The TV's private IPv4 addresses (for showing the LAN URL). */
  lanAddresses: string[];
  /** Connections refused by the address filter this app session. */
  rejectedConnections: number;
  /** mDNS responder: 'off' | 'probing' | 'announced' | 'error: …'. */
  mdnsState: string;
  /** Host name claimed on the LAN, e.g. 'hamclock.local' or 'hamclock-2.local' ('' if none). */
  mdnsHost: string;
  /** Configured mDNS name base (saved setting); mdnsHost may add a -N suffix on a conflict. */
  mdnsName: string;
  dataDir: string;
  backend: string;
  version: string;
  /** Recent host events, oldest first. */
  events: string[];
}

/** Settings saved in <dataDir>/vega-settings.json (one store, read and written natively). */
export interface VegaSettings {
  lanAccess: boolean;
  /** DNS label for <name>.local, 1-40 letters/digits/hyphens. Default 'hamclock'. */
  mdnsName: string;
  /** host:port. Default 'ohb.hamclock.app:80'. */
  backend: string;
  /** Screen safe margin in percent: 0, 3 or 5. Applied by the app around the WebView. */
  overscan: 0 | 3 | 5;
}

export const DEFAULT_MDNS_NAME = 'hamclock';
export const DEFAULT_BACKEND = 'ohb.hamclock.app:80';

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
  /** QR code modules for text: rows of booleans (true = dark), no quiet zone; [] if too long. */
  qrMatrix(text: string): boolean[][] {
    const m = NativeHcEngine.qrMatrix(text);
    return m ? m.split('\n').map(row => Array.from(row, c => c === '1')) : [];
  },
  getSettings(): VegaSettings {
    return JSON.parse(NativeHcEngine.getSettings()) as VegaSettings;
  },
  /**
   * Change and save some settings. LAN or backend changes restart a running engine; a name
   * change re-probes mDNS. Returns '' or a message for the user (then nothing changed).
   */
  updateSettings(changes: Partial<VegaSettings>): string {
    return NativeHcEngine.updateSettings(JSON.stringify(changes));
  },
};

export default HcEngine;
