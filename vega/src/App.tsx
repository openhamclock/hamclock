/**
 * HamClock for Amazon Vega OS — app shell.
 *
 * The HamClock engine runs inside this app (see ../PHASE1_RESULTS.md) and serves its own
 * live.html on 127.0.0.1. This shell:
 *   - waits until the engine is listening on its live port, then shows live.html in a WebView;
 *   - shows a status card while starting, restarting or after an error;
 *   - gives a small native settings panel (Back or Menu) with LAN access, restart, exit.
 *     With LAN access on it shows the address to browse to from another device; the choice is
 *     saved by the engine and reused at the next launch.
 *   - shows a first-run screen (QR code + address) while HamClock needs Setup, so the user can
 *     enter the callsign from a phone. It closes by itself once the engine reports Setup done.
 *
 * D-Pad: remote keys go to the page as DOM key events (allowSystemKeyEvents). KEY_ROUTER_JS
 * (injected) sends arrows/OK to live.html's virtual cursor and posts Back to this shell, because
 * with allowSystemKeyEvents Back reaches only the page. While a native overlay (first-run screen
 * or settings panel) is up, keys go to RN instead. The engine is started in index.js.
 */
import React, {useCallback, useEffect, useMemo, useReducer, useRef, useState} from 'react';
import {Animated, BackHandler, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions} from 'react-native';
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
import {QrCode} from './QrCode';

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
  /** The user chose "Set up on this TV instead" on the first-run screen (this launch only). */
  setupOnTv: boolean;
}

type AppEvent =
  | {type: 'ENGINE_STATUS'; status: EngineStatus}
  | {type: 'ENGINE_ERROR'; message: string}
  | {type: 'RESTART_REQUESTED'}
  | {type: 'WEB_LOADED'}
  | {type: 'WEB_ERROR'; message: string}
  | {type: 'WEB_RELOAD'}
  | {type: 'OPEN_SETTINGS'}
  | {type: 'CLOSE_SETTINGS'}
  | {type: 'SETUP_ON_TV'};

const initialState: AppState = {
  lifecycle: 'STARTING',
  status: null,
  webGeneration: 0,
  webError: null,
  webLoaded: false,
  settingsOpen: false,
  setupOnTv: false,
};

/** First-run screen: Setup needed, the user hasn't chosen the TV, and the engine is up or coming up. */
export function firstRunVisible(s: AppState): boolean {
  return (
    s.status?.setupComplete === false && !s.setupOnTv && s.lifecycle !== 'STOPPED' && s.lifecycle !== 'ERROR'
  );
}

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
    case 'SETUP_ON_TV':
      return {...s, setupOnTv: true};
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

/** URLs another device can open: mDNS name first once announced, then the IP addresses. */
function lanUrls(s: EngineStatus): {mdnsUrl: string | null; ipUrls: string[]} {
  const mdnsUrl = s.mdnsState === 'announced' && s.mdnsHost ? `http://${s.mdnsHost}:${s.liveRwPort}/live.html` : null;
  const ipUrls = (s.lanAddresses ?? []).map(a => `http://${a}:${s.liveRwPort}/live.html`);
  return {mdnsUrl, ipUrls};
}

/** A slowly pulsing dot for "waiting" status lines. */
function PulseDot({size, color}: {size: number; color: string}) {
  const v = useRef(new Animated.Value(0.25)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(v, {toValue: 1, duration: 800, useNativeDriver: true}),
        Animated.timing(v, {toValue: 0.25, duration: 800, useNativeDriver: true}),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [v]);
  return <Animated.View style={{width: size, height: size, borderRadius: size / 2, backgroundColor: color, opacity: v}} />;
}

const FIRST_RUN_STEPS = [
  'Scan the QR code with your phone, or open the address below in a browser.',
  'Enter your callsign in HamClock’s Setup page.',
  'Select Done. This TV switches to the clock by itself.',
];

/**
 * Shown over the WebView while HamClock needs Setup (no callsign stored). The engine turned LAN
 * access on before binding (firstRunLanAccess), so the QR code works at once.
 */
function FirstRunScreen(props: {state: AppState; metrics: TvMetrics; dispatch: React.Dispatch<AppEvent>}) {
  const {state, metrics: m, dispatch} = props;
  const s = state.status;
  const {mdnsUrl, ipUrls} = s ? lanUrls(s) : {mdnsUrl: null, ipUrls: []};
  const qrUrl = s?.lanAccess ? mdnsUrl ?? ipUrls[0] ?? null : null;
  const matrix = useMemo(() => {
    if (!qrUrl) return [];
    try {
      return HcEngine.qrMatrix(qrUrl);
    } catch {
      return [];
    }
  }, [qrUrl]);
  const ready = state.lifecycle === 'READY';
  const qrSize = 560 * m.dp;

  const turnLanOn = () => {
    try {
      HcEngine.setLanAccess(true);
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  };

  let qrNote: string | null = null;
  if (!s?.lanAccess) qrNote = 'LAN access is off, so other devices can’t reach HamClock.';
  else if (!qrUrl) qrNote = 'No network address found. Check the TV’s network connection.';

  return (
    <View style={[styles.firstRunRoot, {paddingHorizontal: m.safeX + 24 * m.dp, paddingVertical: m.safeY + 24 * m.dp}]}>
      <View style={styles.firstRunText}>
        <Text style={[styles.firstRunTitle, {fontSize: 60 * m.dp}]}>Welcome to HamClock on TV</Text>
        <Text style={[styles.firstRunLead, {fontSize: 30 * m.dp, marginTop: 20 * m.dp}]}>
          HamClock needs a one-time setup with your callsign. The easiest way is from a phone, tablet or
          computer on the same network:
        </Text>
        {FIRST_RUN_STEPS.map((t, i) => (
          <View key={i} style={[styles.stepRow, {marginTop: 18 * m.dp}]}>
            <View style={[styles.stepNum, {width: 48 * m.dp, height: 48 * m.dp, borderRadius: 24 * m.dp}]}>
              <Text style={[styles.stepNumText, {fontSize: 26 * m.dp}]}>{i + 1}</Text>
            </View>
            <Text style={[styles.stepText, {fontSize: 30 * m.dp, marginLeft: 20 * m.dp}]}>{t}</Text>
          </View>
        ))}

        <View style={[styles.lanBox, {marginTop: 32 * m.dp, padding: 24 * m.dp, borderRadius: 16 * m.dp}]}>
          {mdnsUrl && s?.lanAccess && (
            <Text style={[styles.lanUrl, {fontSize: 34 * m.dp}]}>{mdnsUrl}</Text>
          )}
          {s?.lanAccess &&
            ipUrls.map((u, i) => (
              <Text
                key={u}
                style={[
                  mdnsUrl ? styles.lanUrlAlt : styles.lanUrl,
                  {fontSize: (mdnsUrl ? 28 : 34) * m.dp},
                  (mdnsUrl || i > 0) && {marginTop: 8 * m.dp},
                ]}>
                {`${mdnsUrl ? 'or ' : ''}${u}`}
              </Text>
            ))}
          {qrNote && <Text style={[styles.lanNoAddr, {fontSize: 28 * m.dp}]}>{qrNote}</Text>}
        </View>

        <View style={[styles.stepRow, {marginTop: 28 * m.dp}]}>
          <PulseDot size={18 * m.dp} color={ready ? C.accent : C.muted} />
          <Text style={[styles.firstRunStatus, {fontSize: 28 * m.dp, marginLeft: 16 * m.dp}]}>
            {ready ? 'Waiting for setup…' : LIFECYCLE_COPY[state.lifecycle]}
          </Text>
        </View>

        <TVFocusGuideView
          autoFocus
          trapFocusUp
          trapFocusDown
          trapFocusLeft
          trapFocusRight
          style={[styles.firstRunButtons, {marginTop: 20 * m.dp, columnGap: 24 * m.dp}]}>
          {!s?.lanAccess && (
            <FocusButton metrics={m} label="Turn on LAN access" onPress={turnLanOn} preferred={!state.settingsOpen} />
          )}
          <FocusButton
            metrics={m}
            label="Set up on this TV instead"
            onPress={() => dispatch({type: 'SETUP_ON_TV'})}
            preferred={!!s?.lanAccess && !state.settingsOpen}
          />
          <FocusButton metrics={m} label="Exit app" onPress={() => BackHandler.exitApp()} tone="danger" />
        </TVFocusGuideView>
      </View>

      <View style={[styles.firstRunQrCol, {marginLeft: 64 * m.dp}]}>
        <View style={[styles.qrFrame, {width: qrSize + 48 * m.dp, height: qrSize + 48 * m.dp, borderRadius: 28 * m.dp}]}>
          {matrix.length > 0 ? (
            <QrCode matrix={matrix} size={qrSize} />
          ) : (
            <Text style={[styles.qrPlaceholder, {fontSize: 26 * m.dp}]}>QR code appears when HamClock is reachable</Text>
          )}
        </View>
        <Text style={[styles.panelMeta, styles.qrCaption, {fontSize: 24 * m.dp, marginTop: 18 * m.dp}]}>
          Scan with a phone or tablet
        </Text>
      </View>
    </View>
  );
}

/** Where to browse from another device (LAN access on), or what LAN access does (off). */
function LanInfo({status: s, metrics: m}: {status: EngineStatus | null; metrics: TvMetrics}) {
  if (!s) return null;
  if (!s.lanAccess) {
    return (
      <Text style={[styles.panelMeta, {fontSize: 22 * m.dp, marginTop: 16 * m.dp}]}>
        Turn on LAN access to open HamClock from a computer or phone on your home network.
      </Text>
    );
  }
  const addrs = s.lanAddresses ?? [];
  const {mdnsUrl} = lanUrls(s);
  return (
    <View style={[styles.lanBox, {marginTop: 20 * m.dp, padding: 20 * m.dp, borderRadius: 14 * m.dp}]}>
      <Text style={[styles.panelMeta, {fontSize: 22 * m.dp}]}>On a computer or phone on your network, open:</Text>
      {mdnsUrl && (
        <Text style={[styles.lanUrl, {fontSize: 28 * m.dp, marginTop: 8 * m.dp}]}>{mdnsUrl}</Text>
      )}
      {addrs.length > 0 ? (
        addrs.map(a => (
          <Text
            key={a}
            style={[mdnsUrl ? styles.lanUrlAlt : styles.lanUrl, {fontSize: (mdnsUrl ? 24 : 28) * m.dp, marginTop: 8 * m.dp}]}>
            {`${mdnsUrl ? 'or ' : ''}http://${a}:${s.liveRwPort}/live.html`}
          </Text>
        ))
      ) : (
        <Text style={[styles.lanNoAddr, {fontSize: 24 * m.dp, marginTop: 8 * m.dp}]}>
          No network address found. Check the TV's network connection.
        </Text>
      )}
      {s.rejectedConnections > 0 && (
        <Text style={[styles.panelMeta, {fontSize: 20 * m.dp, marginTop: 8 * m.dp}]}>
          {`Refused ${s.rejectedConnections} connection${s.rejectedConnections === 1 ? '' : 's'} from outside your network.`}
        </Text>
      )}
    </View>
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
          {width: 680 * m.dp, right: m.safeX, top: m.safeY, bottom: m.safeY, padding: 40 * m.dp, borderRadius: 24 * m.dp},
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
        <LanInfo status={s} metrics={m} />
        <View style={{height: 16 * m.dp}} />
        <FocusButton metrics={m} label="Resume clock" onPress={close} preferred />
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

  // Diagnostics: log LAN state changes to the device log (native stderr doesn't reach it).
  const lanSummary = state.status
    ? `lan=${state.status.lanAccess} addrs=${(state.status.lanAddresses ?? []).join(',') || '-'} ` +
      `refused=${state.status.rejectedConnections ?? 0} mdns=${state.status.mdnsState ?? '?'}:${state.status.mdnsHost || '-'}`
    : '';
  useEffect(() => {
    if (lanSummary) console.info(`[HCLAN] ${lanSummary}`);
  }, [lanSummary]);

  const firstRun = firstRunVisible(state);
  const setupSummary = `setupComplete=${String(state.status?.setupComplete ?? 'unknown')} firstRun=${firstRun} onTv=${state.setupOnTv}`;
  useEffect(() => {
    console.info(`[HCSETUP] ${setupSummary}`);
  }, [setupSummary]);

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
  const showStatus = (!showWeb || !state.webLoaded) && !firstRun;
  // Keys (incl. Back) go to RN while a native overlay is up, else to the page.
  const webHasKeys = !state.settingsOpen && !firstRun;

  return (
    <View style={styles.root}>
      {showWeb && (
        <WebView
          key={state.webGeneration}
          style={styles.webview}
          source={{uri: liveUri}}
          allowSystemKeyEvents={webHasKeys}
          injectedJavaScriptBeforeContentLoaded={KEY_ROUTER_JS}
          onMessage={(e: WebViewMessageEvent) => {
            const data = e.nativeEvent.data;
            if (data === 'back') toggleSettings();
            else console.info(`[HCWEB] ${data}`);
          }}
          hasTVPreferredFocus={webHasKeys}
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
      {firstRun && <FirstRunScreen state={state} metrics={metrics} dispatch={dispatch} />}
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
  lanBox: {backgroundColor: 'rgba(255, 181, 71, 0.08)', borderWidth: 1, borderColor: 'rgba(255, 181, 71, 0.35)'},
  lanUrl: {color: C.accent, fontWeight: '700'},
  lanUrlAlt: {color: C.text, fontWeight: '600'},
  lanNoAddr: {color: C.text},

  firstRunRoot: {...StyleSheet.absoluteFillObject, backgroundColor: C.bg, flexDirection: 'row', alignItems: 'center'},
  firstRunText: {flex: 1},
  firstRunTitle: {color: C.text, fontWeight: '700'},
  firstRunLead: {color: C.muted},
  stepRow: {flexDirection: 'row', alignItems: 'center'},
  stepNum: {backgroundColor: 'rgba(255, 181, 71, 0.16)', alignItems: 'center', justifyContent: 'center'},
  stepNumText: {color: C.accent, fontWeight: '700'},
  stepText: {color: C.text, flex: 1},
  firstRunStatus: {color: C.text},
  firstRunButtons: {flexDirection: 'row', flexWrap: 'wrap'},
  firstRunQrCol: {alignItems: 'center'},
  qrFrame: {backgroundColor: '#ffffff', alignItems: 'center', justifyContent: 'center'},
  qrPlaceholder: {color: '#5a6478', textAlign: 'center', paddingHorizontal: 40},
  qrCaption: {textAlign: 'center'},

  button: {backgroundColor: 'rgba(255,255,255,0.06)', borderWidth: 2, borderColor: 'transparent'},
  buttonFocused: {backgroundColor: C.accent, borderColor: '#fff'},
  buttonFocusedDanger: {backgroundColor: C.danger, borderColor: '#fff'},
  buttonText: {color: C.text, fontWeight: '600'},
  buttonTextFocused: {color: '#10131a'},
});
