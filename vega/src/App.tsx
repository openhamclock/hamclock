/**
 * HamClock for Amazon Vega OS — app shell.
 *
 * The HamClock engine runs inside this app (see ../PHASE1_RESULTS.md) and serves its own
 * live.html on 127.0.0.1. This shell:
 *   - waits until the engine is listening on its live port, then shows live.html in a WebView;
 *   - shows a status card while starting, restarting or after an error;
 *   - gives a small native settings panel (Back or Menu) with LAN access, reload, restart, exit.
 *
 * D-Pad: remote keys go to the page as DOM key events (allowSystemKeyEvents). KEY_ROUTER_JS
 * (injected) sends arrows/OK to live.html's virtual cursor and posts Back to this shell, because
 * with allowSystemKeyEvents Back reaches only the page. The engine is started in index.js.
 */
import React, {useCallback, useEffect, useMemo, useReducer, useRef, useState} from 'react';
import {BackHandler, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions} from 'react-native';
import {TVFocusGuideView, useTVEventHandler} from '@amazon-devices/react-native-kepler';
import type {HWEvent} from '@amazon-devices/react-native-kepler';
import {WebView} from '@amazon-devices/webview';
import type {
  WebViewErrorEvent,
  WebViewHttpErrorEvent,
  WebViewMessageEvent,
} from '@amazon-devices/webview/dist/types/WebViewTypes';
import {DEFAULT_ENGINE_OPTIONS, HcEngine, ensureEngineStarted} from './engine';
import type {EngineStatus} from './engine';

/* ────────────────────────────────────────────────────────────────────────── */
/* Constants                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

const ENGINE_HOST = '127.0.0.1';
const POLL_STARTING_MS = 500;
const POLL_READY_MS = 3_000;
/** Retry delay after the WebView fails to load the page. */
const WEB_RETRY_MS = 2_000;

/**
 * Injected into live.html before its own scripts, so this capture-phase listener runs before the
 * page's keydown handler. live.html defines handleVirtualCursorMove/Click but leaves calling them
 * to the host app (the Android app does it from native key handling).
 */
const KEY_ROUTER_JS = `(function () {
  var ARROWS = {ArrowUp: 1, ArrowDown: 1, ArrowLeft: 1, ArrowRight: 1};
  function post(m) { if (window.ReactNativeWebView) window.ReactNativeWebView.postMessage(m); }
  window.addEventListener('keydown', function (e) {
    var k = e.key, c = e.keyCode, handled = false;
    if (ARROWS[k]) {
      if (typeof handleVirtualCursorMove === 'function') { handleVirtualCursorMove(k); handled = true; }
    } else if (k === 'Enter' || k === 'Select' || c === 13 || c === 23) {
      if (typeof handleVirtualCursorClick === 'function') { handleVirtualCursorClick(); handled = true; }
    } else if (k === 'Escape' || k === 'GoBack' || k === 'BrowserBack' || c === 27) {
      var ov = document.getElementById('embed-overlay');
      if (!(ov && getComputedStyle(ov).display !== 'none')) { post('back'); handled = true; }
    }
    post('key:' + k + ':' + c + (handled ? ':h' : ''));
    if (handled) { e.preventDefault(); e.stopImmediatePropagation(); }
  }, true);
})(); true;`;

// 1080p TV design grid
const TV_DESIGN_W = 1920;
const TV_DESIGN_H = 1080;
const TV_SAFE_X = 96; // 5 % title-safe
const TV_SAFE_Y = 54;

/* ────────────────────────────────────────────────────────────────────────── */
/* Lifecycle state machine (fed by HcEngine.getStatus())                     */
/* ────────────────────────────────────────────────────────────────────────── */

export type Lifecycle =
  | 'STARTING' // engine starting, or running but the live port isn't listening yet
  | 'READY' // live port listening: show the WebView
  | 'RESTARTING'
  | 'STOPPED' // engine stopped or exited on its own
  | 'ERROR';

interface AppState {
  lifecycle: Lifecycle;
  status: EngineStatus | null;
  /** Bumped to force the WebView to reload (new key). */
  webGeneration: number;
  webError: string | null;
  webLoaded: boolean;
  settingsOpen: boolean;
}

type AppEvent =
  | {type: 'ENGINE_STATUS'; status: EngineStatus}
  | {type: 'ENGINE_ERROR'; message: string}
  | {type: 'RESTART_REQUESTED'}
  | {type: 'WEB_LOADED'}
  | {type: 'WEB_ERROR'; message: string}
  | {type: 'WEB_RELOAD'}
  | {type: 'OPEN_SETTINGS'}
  | {type: 'CLOSE_SETTINGS'};

const initialState: AppState = {
  lifecycle: 'STARTING',
  status: null,
  webGeneration: 0,
  webError: null,
  webLoaded: false,
  settingsOpen: false,
};

function lifecycleFromStatus(prev: Lifecycle, s: EngineStatus): Lifecycle {
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

function lifecycleReducer(s: AppState, e: AppEvent): AppState {
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
      return {...s, lifecycle: 'RESTARTING', settingsOpen: false, webLoaded: false};
    case 'WEB_LOADED':
      return {...s, webLoaded: true, webError: null};
    case 'WEB_ERROR':
      return {...s, webLoaded: false, webError: e.message};
    case 'WEB_RELOAD':
      return {...s, webGeneration: s.webGeneration + 1, webError: null, webLoaded: false};
    case 'OPEN_SETTINGS':
      return {...s, settingsOpen: true};
    case 'CLOSE_SETTINGS':
      return {...s, settingsOpen: false};
  }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Engine status polling                                                     */
/* ────────────────────────────────────────────────────────────────────────── */

function useEngineStatus(lifecycle: Lifecycle, dispatch: React.Dispatch<AppEvent>) {
  const ready = lifecycle === 'READY';
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout>;
    let cancelled = false;
    const tick = () => {
      if (cancelled) return;
      try {
        dispatch({type: 'ENGINE_STATUS', status: HcEngine.getStatus()});
      } catch (err) {
        dispatch({type: 'ENGINE_ERROR', message: String(err)});
      }
      timer = setTimeout(tick, ready ? POLL_READY_MS : POLL_STARTING_MS);
    };
    tick();
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [ready, dispatch]);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Layout                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

function useTvMetrics() {
  const {width, height} = useWindowDimensions();
  return useMemo(() => {
    const dp = Math.min(width / TV_DESIGN_W, height / TV_DESIGN_H);
    return {dp, safeX: TV_SAFE_X * dp, safeY: TV_SAFE_Y * dp, width, height};
  }, [width, height]);
}

type TvMetrics = ReturnType<typeof useTvMetrics>;

/* ────────────────────────────────────────────────────────────────────────── */
/* Components                                                                */
/* ────────────────────────────────────────────────────────────────────────── */

const LIFECYCLE_COPY: Record<Lifecycle, string> = {
  STARTING: 'Starting HamClock…',
  READY: 'Loading HamClock…',
  RESTARTING: 'Restarting HamClock…',
  STOPPED: 'HamClock has stopped.',
  ERROR: 'HamClock could not start.',
};

function StatusCard({state, metrics: m}: {state: AppState; metrics: TvMetrics}) {
  const s = state.status;
  let detail = s ? `Engine ${s.state}${s.version ? `  ·  v${s.version}` : ''}` : 'Waiting for engine…';
  if (state.lifecycle === 'ERROR' && (s?.lastError || state.webError)) {
    detail = s?.lastError || state.webError || detail;
  } else if (state.lifecycle === 'STOPPED') {
    detail = 'Press Back for options.';
  } else if (state.webError) {
    detail = `Retrying: ${state.webError}`;
  }
  return (
    <View style={[styles.statusCard, {width: 900 * m.dp, padding: 40 * m.dp, borderRadius: 24 * m.dp}]}>
      <Text style={[styles.statusTitle, {fontSize: 44 * m.dp}]}>{LIFECYCLE_COPY[state.lifecycle]}</Text>
      <Text style={[styles.statusDetail, {fontSize: 26 * m.dp, marginTop: 16 * m.dp}]}>{detail}</Text>
    </View>
  );
}

function FocusButton(props: {
  label: string;
  onPress: () => void;
  metrics: TvMetrics;
  preferred?: boolean;
  tone?: 'default' | 'danger';
}) {
  const {label, onPress, metrics: m, preferred, tone = 'default'} = props;
  const [focused, setFocused] = useState(false);
  return (
    <TouchableOpacity
      hasTVPreferredFocus={preferred}
      onFocus={() => setFocused(true)}
      onBlur={() => setFocused(false)}
      onPress={onPress}
      activeOpacity={0.85}
      style={[
        styles.button,
        {paddingVertical: 20 * m.dp, paddingHorizontal: 32 * m.dp, borderRadius: 14 * m.dp, marginTop: 16 * m.dp},
        focused && (tone === 'danger' ? styles.buttonFocusedDanger : styles.buttonFocused),
        focused && {transform: [{scale: 1.04}]},
      ]}>
      <Text style={[styles.buttonText, {fontSize: 30 * m.dp}, focused && styles.buttonTextFocused]}>
        {label}
      </Text>
    </TouchableOpacity>
  );
}

function SettingsPanel(props: {
  state: AppState;
  metrics: TvMetrics;
  dispatch: React.Dispatch<AppEvent>;
}) {
  const {state, metrics: m, dispatch} = props;
  const s = state.status;
  const lan = s?.lanAccess ?? false;
  const close = () => dispatch({type: 'CLOSE_SETTINGS'});

  const restartWith = (lanAccess: boolean) => {
    dispatch({type: 'RESTART_REQUESTED'});
    try {
      if (s && (s.state === 'running' || s.state === 'starting')) {
        HcEngine.restart({...DEFAULT_ENGINE_OPTIONS, lanAccess});
      } else {
        HcEngine.start({...DEFAULT_ENGINE_OPTIONS, lanAccess});
      }
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  };

  return (
    <View style={styles.backdrop}>
      <TVFocusGuideView
        autoFocus
        trapFocusUp
        trapFocusDown
        trapFocusLeft
        trapFocusRight
        style={[
          styles.panel,
          {width: 560 * m.dp, right: m.safeX, top: m.safeY, bottom: m.safeY, padding: 40 * m.dp, borderRadius: 24 * m.dp},
        ]}>
        <Text style={[styles.panelTitle, {fontSize: 44 * m.dp}]}>HamClock</Text>
        <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 8 * m.dp}]}>
          {s ? `Engine ${s.state}` : 'Engine unknown'}
          {s?.version ? `  ·  v${s.version}` : ''}
        </Text>
        <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 4 * m.dp}]}>
          {s ? `Live :${s.liveRwPort}  ·  REST :${s.restPort}` : ''}
          {lan ? '  ·  LAN on' : '  ·  loopback only'}
        </Text>
        <View style={{height: 24 * m.dp}} />
        <FocusButton metrics={m} label="Resume clock" onPress={close} preferred />
        <FocusButton
          metrics={m}
          label="Reload display"
          onPress={() => {
            dispatch({type: 'WEB_RELOAD'});
            close();
          }}
        />
        <FocusButton
          metrics={m}
          label={lan ? 'LAN access: On' : 'LAN access: Off'}
          onPress={() => restartWith(!lan)}
        />
        <FocusButton metrics={m} label="Restart HamClock" onPress={() => restartWith(lan)} />
        <FocusButton metrics={m} label="Exit app" onPress={() => BackHandler.exitApp()} tone="danger" />
      </TVFocusGuideView>
    </View>
  );
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Root                                                                      */
/* ────────────────────────────────────────────────────────────────────────── */

export const App = () => {
  const [state, dispatch] = useReducer(lifecycleReducer, initialState);
  const metrics = useTvMetrics();
  const settingsOpenRef = useRef(state.settingsOpen);
  settingsOpenRef.current = state.settingsOpen;

  // index.js normally starts the engine; make sure it's running if the app was resumed after a stop.
  useEffect(() => {
    try {
      ensureEngineStarted(DEFAULT_ENGINE_OPTIONS);
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  }, []);

  useEngineStatus(state.lifecycle, dispatch);

  // Retry loading the page after a WebView error while the engine is up.
  useEffect(() => {
    if (!state.webError || state.lifecycle !== 'READY') return;
    const t = setTimeout(() => dispatch({type: 'WEB_RELOAD'}), WEB_RETRY_MS);
    return () => clearTimeout(t);
  }, [state.webError, state.lifecycle]);

  const toggleSettings = useCallback(() => {
    dispatch({type: settingsOpenRef.current ? 'CLOSE_SETTINGS' : 'OPEN_SETTINGS'});
  }, []);

  // Back: close the settings panel if open, otherwise open it (it has "Exit app").
  useEffect(() => {
    const sub = BackHandler.addEventListener('hardwareBackPress', () => {
      toggleSettings();
      return true;
    });
    return () => sub.remove();
  }, [toggleSettings]);

  // Menu key also toggles the settings panel.
  useTVEventHandler((evt: HWEvent) => {
    if (evt?.eventType === 'menu' && evt.eventKeyAction === 0) toggleSettings();
  });

  const s = state.status;
  const liveUri = s ? `http://${ENGINE_HOST}:${s.liveRwPort}/live.html` : null;
  const showWeb = state.lifecycle === 'READY' && liveUri !== null;
  const showStatus = !showWeb || !state.webLoaded;

  return (
    <View style={styles.root}>
      {showWeb && (
        <WebView
          key={state.webGeneration}
          style={styles.webview}
          source={{uri: liveUri}}
          // While the settings panel is open, keys (incl. Back) must reach RN instead.
          allowSystemKeyEvents={!state.settingsOpen}
          injectedJavaScriptBeforeContentLoaded={KEY_ROUTER_JS}
          onMessage={(e: WebViewMessageEvent) => {
            const data = e.nativeEvent.data;
            if (data === 'back') toggleSettings();
            else console.info(`[HCWEB] ${data}`);
          }}
          hasTVPreferredFocus={!state.settingsOpen}
          javaScriptEnabled
          domStorageEnabled
          onLoad={() => dispatch({type: 'WEB_LOADED'})}
          onError={({nativeEvent: {code, description}}: WebViewErrorEvent) =>
            dispatch({type: 'WEB_ERROR', message: `${code}: ${description}`})
          }
          onHttpError={({nativeEvent: {statusCode, isMainFrame}}: WebViewHttpErrorEvent) => {
            if (isMainFrame) dispatch({type: 'WEB_ERROR', message: `HTTP ${statusCode}`});
          }}
        />
      )}
      {showStatus && (
        <View style={styles.center} pointerEvents="none">
          <StatusCard state={state} metrics={metrics} />
        </View>
      )}
      {state.settingsOpen && <SettingsPanel state={state} metrics={metrics} dispatch={dispatch} />}
    </View>
  );
};

export default App;

/* ────────────────────────────────────────────────────────────────────────── */
/* Styles                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

const C = {
  bg: '#05070c',
  panel: 'rgba(14, 20, 32, 0.94)',
  border: 'rgba(120, 170, 255, 0.18)',
  text: '#e8eefc',
  muted: '#8ea0c2',
  accent: '#ffb547', // amber — readable at 10 ft
  danger: '#ff5d6c',
};

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: C.bg},
  webview: {flex: 1, backgroundColor: '#000000'},
  center: {...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center'},

  statusCard: {backgroundColor: C.panel, borderWidth: 1, borderColor: C.border, alignItems: 'center'},
  statusTitle: {color: C.text, fontWeight: '600', textAlign: 'center'},
  statusDetail: {color: C.muted, textAlign: 'center'},

  backdrop: {...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(2, 4, 8, 0.6)'},
  panel: {position: 'absolute', backgroundColor: C.panel, borderWidth: 1, borderColor: C.border},
  panelTitle: {color: C.text, fontWeight: '700'},
  panelMeta: {color: C.muted},

  button: {backgroundColor: 'rgba(255,255,255,0.06)', borderWidth: 2, borderColor: 'transparent'},
  buttonFocused: {backgroundColor: C.accent, borderColor: '#fff'},
  buttonFocusedDanger: {backgroundColor: C.danger, borderColor: '#fff'},
  buttonText: {color: C.text, fontWeight: '600'},
  buttonTextFocused: {color: '#10131a'},
});
