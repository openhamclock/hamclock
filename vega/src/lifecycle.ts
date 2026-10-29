/**
 * App lifecycle state machine and first-run logic (pure, no React or native calls), fed by
 * HcEngine.getStatus(). Kept apart from App.tsx so it can be unit-tested (test/lifecycle.test.ts).
 */
import type {EngineStatus} from './engine';

export type Lifecycle =
  | 'STARTING' // engine starting, or running but the live port isn't listening yet
  | 'READY' // live port listening: show the WebView
  | 'RESTARTING'
  | 'STOPPED' // engine stopped or exited on its own
  | 'ERROR';

export type PanelPage = 'main' | 'settings';

export interface AppState {
  lifecycle: Lifecycle;
  status: EngineStatus | null;
  /** Bumped to force the WebView to reload (new key). */
  webGeneration: number;
  webError: string | null;
  webLoaded: boolean;
  settingsOpen: boolean;
  panelPage: PanelPage;
  /** The user chose "Set up on this TV instead" on the first-run screen (this launch only). */
  setupOnTv: boolean;
}

export type AppEvent =
  | {type: 'ENGINE_STATUS'; status: EngineStatus}
  | {type: 'ENGINE_ERROR'; message: string}
  | {type: 'RESTART_REQUESTED'; keepPanel?: boolean}
  | {type: 'WEB_LOADED'}
  | {type: 'WEB_ERROR'; message: string}
  | {type: 'WEB_RELOAD'}
  | {type: 'OPEN_SETTINGS'}
  | {type: 'CLOSE_SETTINGS'}
  | {type: 'PANEL_PAGE'; page: PanelPage}
  | {type: 'SETUP_ON_TV'};

export const initialState: AppState = {
  lifecycle: 'STARTING',
  status: null,
  webGeneration: 0,
  webError: null,
  webLoaded: false,
  settingsOpen: false,
  panelPage: 'main',
  setupOnTv: false,
};

/** First-run screen: Setup needed, the user hasn't chosen the TV, and the engine is up or coming up. */
export function firstRunVisible(s: AppState): boolean {
  return (
    s.status?.setupComplete === false && !s.setupOnTv && s.lifecycle !== 'STOPPED' && s.lifecycle !== 'ERROR'
  );
}

export function lifecycleFromStatus(prev: Lifecycle, s: EngineStatus): Lifecycle {
  const liveUp = s.listening.includes(s.liveRwPort);
  switch (s.state) {
    case 'running':
      return liveUp ? 'READY' : prev === 'RESTARTING' ? 'RESTARTING' : 'STARTING';
    case 'starting':
    case 'idle':
      return prev === 'RESTARTING' ? 'RESTARTING' : 'STARTING';
    case 'stopping':
      return prev === 'READY' || prev === 'RESTARTING' ? 'RESTARTING' : prev;
    case 'stopped':
    case 'exited':
      return 'STOPPED';
    case 'error':
      return 'ERROR';
  }
}

export function lifecycleReducer(s: AppState, e: AppEvent): AppState {
  switch (e.type) {
    case 'ENGINE_STATUS': {
      const lifecycle = lifecycleFromStatus(s.lifecycle, e.status);
      // A new engine run means a fresh live server: reload the page.
      const newRun = s.status !== null && e.status.runCount !== s.status.runCount;
      const reload = newRun || (lifecycle === 'READY' && s.lifecycle !== 'READY');
      return {
        ...s,
        lifecycle,
        status: e.status,
        ...(reload ? {webGeneration: s.webGeneration + 1, webError: null, webLoaded: false} : {}),
      };
    }
    case 'ENGINE_ERROR':
      return {...s, lifecycle: 'ERROR', webError: e.message};
    case 'RESTART_REQUESTED':
      return {
        ...s,
        lifecycle: 'RESTARTING',
        settingsOpen: e.keepPanel ? s.settingsOpen : false,
        webLoaded: false,
      };
    case 'WEB_LOADED':
      return {...s, webLoaded: true, webError: null};
    case 'WEB_ERROR':
      return {...s, webLoaded: false, webError: e.message};
    case 'WEB_RELOAD':
      return {...s, webGeneration: s.webGeneration + 1, webError: null, webLoaded: false};
    case 'OPEN_SETTINGS':
      return {...s, settingsOpen: true, panelPage: 'main'};
    case 'CLOSE_SETTINGS':
      return {...s, settingsOpen: false};
    case 'PANEL_PAGE':
      return {...s, panelPage: e.page};
    case 'SETUP_ON_TV':
      return {...s, setupOnTv: true};
  }
}

/** URLs another device can open: mDNS name first once announced, then the IP addresses. */
export function lanUrls(s: EngineStatus): {mdnsUrl: string | null; ipUrls: string[]} {
  const mdnsUrl = s.mdnsState === 'announced' && s.mdnsHost ? `http://${s.mdnsHost}:${s.liveRwPort}/live.html` : null;
  const ipUrls = (s.lanAddresses ?? []).map(a => `http://${a}:${s.liveRwPort}/live.html`);
  return {mdnsUrl, ipUrls};
}

/** The URL QR codes encode: always the first IP URL (decision 4), never .local. */
export function qrUrlFor(s: EngineStatus | null): string | null {
  return s?.lanAccess ? lanUrls(s).ipUrls[0] ?? null : null;
}
