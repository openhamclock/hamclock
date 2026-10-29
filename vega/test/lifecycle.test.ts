import {
  firstRunVisible,
  initialState,
  lanUrls,
  lifecycleFromStatus,
  lifecycleReducer,
  qrUrlFor,
} from '../src/lifecycle';
import type {AppState} from '../src/lifecycle';
import type {EngineStatus} from '../src/engine';

function status(over: Partial<EngineStatus> = {}): EngineStatus {
  return {
    state: 'running',
    runCount: 1,
    exitCode: null,
    lastError: '',
    lanAccess: true,
    setupComplete: true,
    restPort: 8080,
    liveRwPort: 8081,
    liveRoPort: 8082,
    listening: [8081, 8082, 8080],
    lanAddresses: ['192.168.1.237'],
    rejectedConnections: 0,
    mdnsState: 'announced',
    mdnsHost: 'hamclock.local',
    mdnsName: 'hamclock',
    dataDir: '/data/hamclock',
    backend: 'ohb.hamclock.app:80',
    restartNeeded: false,
    version: '4.34',
    events: [],
    ...over,
  };
}

const reduce = (s: AppState, st: EngineStatus) => lifecycleReducer(s, {type: 'ENGINE_STATUS', status: st});

describe('lifecycleFromStatus', () => {
  it('is READY only once the live port listens', () => {
    expect(lifecycleFromStatus('STARTING', status({listening: []}))).toBe('STARTING');
    expect(lifecycleFromStatus('STARTING', status())).toBe('READY');
  });

  it('stays RESTARTING until the new run listens', () => {
    expect(lifecycleFromStatus('RESTARTING', status({state: 'stopping'}))).toBe('RESTARTING');
    expect(lifecycleFromStatus('RESTARTING', status({state: 'starting'}))).toBe('RESTARTING');
    expect(lifecycleFromStatus('RESTARTING', status({listening: []}))).toBe('RESTARTING');
    expect(lifecycleFromStatus('RESTARTING', status())).toBe('READY');
  });

  it('treats stopping while READY as a restart', () => {
    expect(lifecycleFromStatus('READY', status({state: 'stopping'}))).toBe('RESTARTING');
    expect(lifecycleFromStatus('STARTING', status({state: 'stopping'}))).toBe('STARTING');
  });

  it('maps stopped/exited/error', () => {
    expect(lifecycleFromStatus('READY', status({state: 'stopped'}))).toBe('STOPPED');
    expect(lifecycleFromStatus('READY', status({state: 'exited'}))).toBe('STOPPED');
    expect(lifecycleFromStatus('READY', status({state: 'error'}))).toBe('ERROR');
  });
});

describe('lifecycleReducer', () => {
  it('reloads the page when it first becomes READY, not on every poll', () => {
    const s1 = reduce(initialState, status({listening: []}));
    expect(s1.lifecycle).toBe('STARTING');
    expect(s1.webGeneration).toBe(0);
    const s2 = reduce(s1, status());
    expect(s2.lifecycle).toBe('READY');
    expect(s2.webGeneration).toBe(1);
    const s3 = reduce({...s2, webLoaded: true}, status());
    expect(s3.webGeneration).toBe(1);
    expect(s3.webLoaded).toBe(true);
  });

  it('reloads the page on a new engine run', () => {
    const ready = {...reduce(reduce(initialState, status({listening: []})), status()), webLoaded: true};
    const next = reduce(ready, status({runCount: 2}));
    expect(next.webGeneration).toBe(ready.webGeneration + 1);
    expect(next.webLoaded).toBe(false);
  });

  it('RESTART_REQUESTED closes the panel unless asked to keep it (LAN change)', () => {
    const open = {...initialState, settingsOpen: true};
    expect(lifecycleReducer(open, {type: 'RESTART_REQUESTED'}).settingsOpen).toBe(false);
    const kept = lifecycleReducer(open, {type: 'RESTART_REQUESTED', keepPanel: true});
    expect(kept.settingsOpen).toBe(true);
    expect(kept.lifecycle).toBe('RESTARTING');
  });

  it('opening the panel starts on the main page', () => {
    const s = lifecycleReducer({...initialState, panelPage: 'settings'}, {type: 'OPEN_SETTINGS'});
    expect(s).toMatchObject({settingsOpen: true, panelPage: 'main'});
  });

  it('web errors and reloads', () => {
    const err = lifecycleReducer(initialState, {type: 'WEB_ERROR', message: 'x'});
    expect(err).toMatchObject({webError: 'x', webLoaded: false});
    const reload = lifecycleReducer(err, {type: 'WEB_RELOAD'});
    expect(reload).toMatchObject({webError: null, webGeneration: 1});
    expect(lifecycleReducer(reload, {type: 'WEB_LOADED'}).webLoaded).toBe(true);
  });

  it('ENGINE_ERROR goes to ERROR', () => {
    expect(lifecycleReducer(initialState, {type: 'ENGINE_ERROR', message: 'boom'})).toMatchObject({
      lifecycle: 'ERROR',
      webError: 'boom',
    });
  });
});

describe('firstRunVisible', () => {
  const setupNeeded = reduce(initialState, status({setupComplete: false, listening: [8081, 8082]}));

  it('shows while Setup is needed', () => {
    expect(firstRunVisible(setupNeeded)).toBe(true);
  });

  it('hides before the first status (setupComplete unknown)', () => {
    expect(firstRunVisible(initialState)).toBe(false);
    expect(firstRunVisible(reduce(initialState, status({setupComplete: null})))).toBe(false);
  });

  it('hides after "Set up on this TV instead"', () => {
    expect(firstRunVisible(lifecycleReducer(setupNeeded, {type: 'SETUP_ON_TV'}))).toBe(false);
  });

  it('closes by itself once Setup is done', () => {
    expect(firstRunVisible(reduce(setupNeeded, status({setupComplete: true})))).toBe(false);
  });

  it('hides when the engine stopped or failed', () => {
    expect(firstRunVisible(reduce(setupNeeded, status({setupComplete: false, state: 'exited'})))).toBe(false);
    expect(firstRunVisible(lifecycleReducer(setupNeeded, {type: 'ENGINE_ERROR', message: 'x'}))).toBe(false);
  });

  it('stays up across an engine restart while Setup is still needed', () => {
    const restarting = lifecycleReducer(setupNeeded, {type: 'RESTART_REQUESTED', keepPanel: true});
    expect(firstRunVisible(restarting)).toBe(true);
  });
});

describe('LAN URLs and QR', () => {
  it('lists the .local URL only once announced', () => {
    expect(lanUrls(status()).mdnsUrl).toBe('http://hamclock.local:8081/live.html');
    expect(lanUrls(status({mdnsState: 'probing'})).mdnsUrl).toBeNull();
    expect(lanUrls(status({mdnsHost: ''})).mdnsUrl).toBeNull();
  });

  it('QR always encodes the first IP URL, never .local (decision 4)', () => {
    const s = status({lanAddresses: ['192.168.1.237', '10.0.0.5']});
    expect(qrUrlFor(s)).toBe('http://192.168.1.237:8081/live.html');
  });

  it('no QR without LAN access or an address', () => {
    expect(qrUrlFor(null)).toBeNull();
    expect(qrUrlFor(status({lanAccess: false}))).toBeNull();
    expect(qrUrlFor(status({lanAddresses: []}))).toBeNull();
  });
});
