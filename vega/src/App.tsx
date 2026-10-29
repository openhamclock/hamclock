/**
 * HamClock for Amazon Vega OS — app shell.
 *
 * The HamClock engine runs inside this app (see ../PHASE1_RESULTS.md) and serves its own
 * live.html on 127.0.0.1. This shell:
 *   - waits until the engine is listening on its live port, then shows live.html in a WebView;
 *   - shows a status card while starting, restarting or after an error;
 *   - gives a native settings panel (Back or Menu) in two pages:
 *       main: resume, LAN access, Setup HamClock, exit, plus the address and a QR code to open HamClock
 *             from another device while LAN access is on;
 *       settings: mDNS name, backend host, screen safe margin (overscan).
 *     Settings are saved natively in <dataDir>/vega-settings.json (HcEngine.updateSettings).
 *   - applies the screen safe margin inside the page (the clock canvas is fitted, see pageScript);
 *   - shows a first-run screen (QR code + address) while HamClock needs Setup, so the user can
 *     enter the callsign from a phone. It closes by itself once the engine reports Setup done.
 *   QR codes always encode the IP URL (phones may not resolve .local); the .local URL is text.
 *
 * D-Pad: remote keys go to the page as DOM key events (allowSystemKeyEvents). KEY_ROUTER_JS
 * (injected) sends arrows/OK to live.html's virtual cursor and posts Back to this shell, because
 * with allowSystemKeyEvents Back reaches only the page. While a native overlay (first-run screen
 * or settings panel) is up, keys go to RN instead. The engine is started in index.js.
 */
import React, {useCallback, useContext, useEffect, useMemo, useReducer, useRef, useState} from 'react';
import {
  Animated,
  BackHandler,
  Keyboard,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
  useWindowDimensions,
} from 'react-native';
import {TVFocusGuideView, useTVEventHandler} from '@amazon-devices/react-native-kepler';
import type {HWEvent} from '@amazon-devices/react-native-kepler';
import {WebView} from '@amazon-devices/webview';
import type {
  WebViewErrorEvent,
  WebViewHttpErrorEvent,
  WebViewMessageEvent,
} from '@amazon-devices/webview/dist/types/WebViewTypes';
import {DEFAULT_BACKEND, DEFAULT_ENGINE_OPTIONS, DEFAULT_MDNS_NAME, HcEngine, ensureEngineStarted} from './engine';
import type {EngineStatus, VegaSettings} from './engine';
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
 *
 * It also fits the clock to the screen with the safe margin (overscan). live.html's initCanvas()
 * only shrinks a clock larger than the window and centers a smaller one at its own size, and it
 * runs again on every full frame, so the fit has to happen inside the page: initCanvas is wrapped
 * (after the page's scripts ran) to scale the canvas to fit the window minus the margin, and to
 * keep app_scale in step so the virtual cursor and touches still line up. Padding around the
 * WebView in RN doesn't work: the Vega WebView surface doesn't follow the RN layout change.
 * window.__hcSetOverscan(pct) changes the margin at run time.
 */
function pageScript(overscanPct: number): string {
  return `(function () {
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

  window.__hcOverscan = ${Number(overscanPct) || 0};
  function fit() {
    var c = document.getElementById('hamclock-cvs');
    if (!c || !c.width || !c.height) return;
    var de = document.documentElement;
    var p = Number(window.__hcOverscan) || 0;
    var aw = (de.clientWidth || window.innerWidth) * (1 - p / 50);
    var ah = (de.clientHeight || window.innerHeight) * (1 - p / 50);
    var w, h;
    if (aw * c.height > ah * c.width) { h = ah; w = ah * c.width / c.height; }
    else { w = aw; h = aw * c.height / c.width; }
    w = Math.round(w); h = Math.round(h);
    c.style.position = 'absolute';
    c.style.top = '50%';
    c.style.left = '50%';
    c.style.width = w + 'px';
    c.style.height = h + 'px';
    c.style.margin = (-Math.round(h / 2)) + 'px 0 0 ' + (-Math.round(w / 2)) + 'px';
    app_scale = w / (typeof APP_W === 'number' ? APP_W : 800);
    if (typeof updateVirtualCursorDom === 'function') updateVirtualCursorDom();
  }
  window.__hcSetOverscan = function (pct) { window.__hcOverscan = Number(pct) || 0; fit(); };
  function hook() {
    if (typeof window.initCanvas !== 'function' || window.initCanvas.__hcFit) return;
    var orig = window.initCanvas;
    var wrapped = function (w, h) { orig(w, h); fit(); };
    wrapped.__hcFit = true;
    window.initCanvas = wrapped;
    fit();
  }
  document.addEventListener('DOMContentLoaded', hook);
  window.addEventListener('load', hook);
})(); true;`;
}

/** Change the safe margin in a page that is already loaded. */
function overscanJs(pct: number): string {
  return `if (typeof window.__hcSetOverscan === 'function') window.__hcSetOverscan(${Number(pct) || 0}); true;`;
}

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

export type PanelPage = 'main' | 'settings';

interface AppState {
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

type AppEvent =
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

const initialState: AppState = {
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
  /** Toggle/choice state: amber outline while not focused. */
  selected?: boolean;
}) {
  const {label, onPress, metrics: m, preferred, tone = 'default', selected} = props;
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
        selected && styles.buttonSelected,
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

/** The URL QR codes encode: always the first IP URL (decision 4), never .local. */
function qrUrlFor(s: EngineStatus | null): string | null {
  return s?.lanAccess ? lanUrls(s).ipUrls[0] ?? null : null;
}

function useQrMatrix(url: string | null): boolean[][] {
  return useMemo(() => {
    if (!url) return [];
    try {
      return HcEngine.qrMatrix(url);
    } catch {
      return [];
    }
  }, [url]);
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
  const qrUrl = qrUrlFor(s);
  const matrix = useQrMatrix(qrUrl);
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

/** QR code of the IP URL on a white card, or a short note when there's nothing to encode. */
function LanQr({url, size, metrics: m}: {url: string | null; size: number; metrics: TvMetrics}) {
  const matrix = useQrMatrix(url);
  const pad = Math.round(size * 0.06);
  return (
    <View style={[styles.qrFrame, {width: size + 2 * pad, height: size + 2 * pad, borderRadius: 20 * m.dp}]}>
      {matrix.length > 0 ? (
        <QrCode matrix={matrix} size={size} />
      ) : (
        <Text style={[styles.qrPlaceholder, {fontSize: 22 * m.dp}]}>No network address yet</Text>
      )}
    </View>
  );
}

/** Where to browse from another device (LAN access on), or what LAN access does (off). */
function LanInfo({status: s, metrics: m}: {status: EngineStatus | null; metrics: TvMetrics}) {
  if (!s) return null;
  if (!s.lanAccess) {
    return (
      <View style={[styles.lanBoxOff, {padding: 28 * m.dp, borderRadius: 18 * m.dp}]}>
        <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}]}>LAN access is off</Text>
        <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 12 * m.dp}]}>
          Turn it on to open HamClock from a phone, tablet or computer on your home network, for
          example to change HamClock’s settings with a keyboard.
        </Text>
      </View>
    );
  }
  const {mdnsUrl, ipUrls} = lanUrls(s);
  return (
    <View style={[styles.lanBox, styles.lanBoxCol, {padding: 24 * m.dp, borderRadius: 18 * m.dp}]}>
      <Text style={[styles.panelMeta, {fontSize: 24 * m.dp}]}>Scan, or open on a phone or computer:</Text>
      <View style={{marginTop: 16 * m.dp}}>
        <LanQr url={qrUrlFor(s)} size={300 * m.dp} metrics={m} />
      </View>
      {mdnsUrl && <Text style={[styles.lanUrl, styles.textCenter, {fontSize: 28 * m.dp, marginTop: 18 * m.dp}]}>{mdnsUrl}</Text>}
      {ipUrls.length > 0 ? (
        ipUrls.map((u, i) => (
          <Text
            key={u}
            style={[
              mdnsUrl ? styles.lanUrlAlt : styles.lanUrl,
              styles.textCenter,
              {fontSize: (mdnsUrl ? 24 : 28) * m.dp, marginTop: (mdnsUrl || i > 0 ? 6 : 18) * m.dp},
            ]}>
            {`${mdnsUrl ? 'or ' : ''}${u}`}
          </Text>
        ))
      ) : (
        <Text style={[styles.lanNoAddr, styles.textCenter, {fontSize: 24 * m.dp, marginTop: 18 * m.dp}]}>
          No network address found. Check the TV’s network connection.
        </Text>
      )}
      {s.rejectedConnections > 0 && (
        <Text style={[styles.panelMeta, styles.textCenter, {fontSize: 20 * m.dp, marginTop: 10 * m.dp}]}>
          {`Refused ${s.rejectedConnections} connection${s.rejectedConnections === 1 ? '' : 's'} from outside your network.`}
        </Text>
      )}
    </View>
  );
}

/** Applies a settings change; returns '' or a message for the user. */
type UpdateSettings = (changes: Partial<VegaSettings>) => string;

function PanelMain(props: {
  state: AppState;
  metrics: TvMetrics;
  dispatch: React.Dispatch<AppEvent>;
  update: UpdateSettings;
}) {
  const {state, metrics: m, dispatch, update} = props;
  const s = state.status;
  const lan = s?.lanAccess ?? false;
  const [error, setError] = useState('');

  // Restart straight into HamClock's Setup page (engine arg -u, this run only).
  const openSetup = () => {
    dispatch({type: 'RESTART_REQUESTED'});
    const opts = {...DEFAULT_ENGINE_OPTIONS, openSetup: true};
    try {
      if (s && (s.state === 'running' || s.state === 'starting')) HcEngine.restart(opts);
      else HcEngine.start(opts);
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  };

  return (
    <View style={styles.panelRow}>
      <View style={{width: 440 * m.dp}}>
        <Text style={[styles.panelTitle, {fontSize: 44 * m.dp}]}>HamClock</Text>
        <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 8 * m.dp}]}>
          {s ? `Engine ${s.state}` : 'Engine unknown'}
          {s?.version ? `  ·  v${s.version}` : ''}
        </Text>
        <Text style={[styles.panelMeta, {fontSize: 22 * m.dp, marginTop: 4 * m.dp}]}>
          {s ? `Live :${s.liveRwPort}  ·  REST :${s.restPort}` : ''}
          {lan ? '  ·  LAN on' : '  ·  loopback only'}
        </Text>
        <View style={{height: 20 * m.dp}} />
        <FocusButton metrics={m} label="Resume clock" onPress={() => dispatch({type: 'CLOSE_SETTINGS'})} preferred />
        <FocusButton
          metrics={m}
          label={lan ? 'LAN access: On' : 'LAN access: Off'}
          selected={lan}
          onPress={() => setError(update({lanAccess: !lan}))}
        />
        <FocusButton metrics={m} label="Settings…" onPress={() => dispatch({type: 'PANEL_PAGE', page: 'settings'})} />
        <FocusButton metrics={m} label="Setup HamClock" onPress={openSetup} />
        <FocusButton metrics={m} label="Exit app" onPress={() => BackHandler.exitApp()} tone="danger" />
        {!!error && <Text style={[styles.fieldError, {fontSize: 22 * m.dp, marginTop: 12 * m.dp}]}>{error}</Text>}
      </View>
      <View style={[styles.panelSide, {marginLeft: 40 * m.dp}]}>
        <LanInfo status={s} metrics={m} />
      </View>
    </View>
  );
}

/** Result of the last edit of a field, kept in App so it survives the panel remount. */
interface FieldNote {
  error: string;
  saved: boolean;
  /** What the user typed, shown again when it was rejected. */
  text: string;
}

/**
 * Shared by the settings fields and App:
 *   - cancelRef is set while a field is being edited; Back calls it to cancel the edit instead of
 *     leaving the settings page;
 *   - afterEdit() records the result and remounts the panel's focus guide (panelGen) with focus on
 *     that field (lastEdited). Removing the focused TextInput leaves Vega's focus engine stuck on
 *     the field (keys arrive, focus never moves); a fresh guide, as on a page switch, works.
 */
interface FieldCtx {
  cancelRef: React.MutableRefObject<(() => void) | null>;
  notes: Record<string, FieldNote>;
  lastEdited: string | null;
  panelGen: number;
  afterEdit: (label: string, note: FieldNote | null) => void;
}
const FieldContext = React.createContext<FieldCtx | null>(null);

/**
 * A labelled text setting. On a TV a focused TextInput opens the on-screen keyboard at once, which
 * traps D-pad navigation; so the field is a plain focusable button showing the value, and the
 * TextInput is mounted (autoFocus, keyboard up) only after OK. Done saves. Back cancels: the TV
 * keyboard takes Back itself and just blurs the input, so a blur without Done means cancel. Focus
 * then returns to the button (see FieldCtx).
 */
function SettingField(props: {
  label: string;
  value: string;
  placeholder: string;
  help: string;
  suffix?: string;
  metrics: TvMetrics;
  onSave: (text: string) => string;
}) {
  const {label, value, placeholder, help, suffix, metrics: m, onSave} = props;
  const ctx = useContext(FieldContext);
  const note = ctx?.notes[label];
  const shown = note?.error ? note.text : value;
  const [text, setText] = useState(shown);
  const [editing, setEditing] = useState(false);
  const [focused, setFocused] = useState(false);
  const endedRef = useRef(true);
  // Never leave a stale cancel hook behind.
  useEffect(
    () => () => {
      if (ctx) ctx.cancelRef.current = null;
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  const finish = (how: 'save' | 'cancel', t: string) => {
    if (endedRef.current) return; // Done fires submit and then blur: act once
    endedRef.current = true;
    if (ctx) ctx.cancelRef.current = null;
    Keyboard.dismiss();
    let next: FieldNote | null = note ?? null;
    if (how === 'save') {
      if (t.trim() === value) next = null;
      else {
        const err = onSave(t);
        next = {error: err, saved: !err, text: t};
      }
    }
    console.info(`[HCFIELD] ${label}: edit ${how}${next?.error ? ` (${next.error})` : ''}`);
    setEditing(false);
    ctx?.afterEdit(label, next);
  };

  const start = () => {
    endedRef.current = false;
    setText(shown);
    setEditing(true);
  };
  // Back while editing (see App onBack). Reassigned each render so it sees the current text.
  if (editing && ctx) ctx.cancelRef.current = () => finish('cancel', text);
  // The first Back on the TV keyboard only hides it; the input keeps focus (so the D-pad can't
  // leave it). Treat the keyboard closing as cancel; a Done arriving right after still wins.
  const finishRef = useRef(finish);
  finishRef.current = finish;
  useEffect(() => {
    if (!editing) return;
    const sub = Keyboard.addListener('keyboardDidHide', () => {
      setTimeout(() => finishRef.current('cancel', ''), 200);
    });
    return () => sub.remove();
  }, [editing]);

  const boxStyle = [
    styles.textInput,
    {paddingVertical: 14 * m.dp, paddingHorizontal: 22 * m.dp, borderRadius: 12 * m.dp},
    {width: (suffix ? 560 : 700) * m.dp},
    (focused || editing) && styles.textInputFocused,
  ];

  return (
    <View style={{marginTop: 26 * m.dp}}>
      <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}]}>{label}</Text>
      <View style={[styles.stepRow, {marginTop: 10 * m.dp}]}>
        {editing ? (
          <TextInput
            autoFocus
            value={text}
            placeholder={placeholder}
            placeholderTextColor={C.muted}
            onChangeText={t => setText(t)}
            onBlur={() => setTimeout(() => finish('cancel', text), 200)}
            onSubmitEditing={e => finish('save', e.nativeEvent.text)}
            autoCapitalize="none"
            autoCorrect={false}
            returnKeyType="done"
            blurOnSubmit
            style={[boxStyle, styles.textInputText, {fontSize: 30 * m.dp}]}
          />
        ) : (
          <TouchableOpacity
            hasTVPreferredFocus={ctx?.lastEdited === label}
            onFocus={() => setFocused(true)}
            onBlur={() => setFocused(false)}
            onPress={start}
            activeOpacity={0.85}
            style={boxStyle}>
            <Text style={[shown ? styles.textInputText : styles.panelMeta, {fontSize: 30 * m.dp}]} numberOfLines={1}>
              {shown || placeholder}
            </Text>
          </TouchableOpacity>
        )}
        {suffix && <Text style={[styles.lanUrlAlt, {fontSize: 30 * m.dp, marginLeft: 12 * m.dp}]}>{suffix}</Text>}
      </View>
      {note?.error ? (
        <Text style={[styles.fieldError, {fontSize: 22 * m.dp, marginTop: 8 * m.dp}]}>{note.error}</Text>
      ) : (
        <Text style={[styles.panelMeta, {fontSize: 22 * m.dp, marginTop: 8 * m.dp}]}>
          {note?.saved ? 'Saved. ' : ''}
          {focused && !editing ? 'Press OK to edit. ' : ''}
          {help}
        </Text>
      )}
    </View>
  );
}

const OVERSCAN_CHOICES: {pct: VegaSettings['overscan']; label: string}[] = [
  {pct: 0, label: 'None'},
  {pct: 3, label: 'Normal (3%)'},
  {pct: 5, label: 'Large (5%)'},
];

function PanelSettings(props: {
  state: AppState;
  settings: VegaSettings | null;
  metrics: TvMetrics;
  dispatch: React.Dispatch<AppEvent>;
  update: UpdateSettings;
}) {
  const {state, settings, metrics: m, dispatch, update} = props;
  const fieldCtx = useContext(FieldContext);
  const s = state.status;
  const name = settings?.mdnsName ?? DEFAULT_MDNS_NAME;
  const backend = settings?.backend ?? DEFAULT_BACKEND;
  const overscan = settings?.overscan ?? 0;
  const announced = s?.mdnsState === 'announced' ? s.mdnsHost : '';
  let nameHelp = `Other devices can open http://${name}.local:${s?.liveRwPort ?? 8081}/live.html.`;
  if (!s?.lanAccess) nameHelp = 'Used while LAN access is on.';
  else if (announced && announced !== `${name}.local`) nameHelp = `That name is taken on your network, so this TV uses ${announced}.`;
  else if (s?.mdnsState === 'probing') nameHelp = `Checking that ${name}.local is free…`;

  return (
    <View>
      <Text style={[styles.panelTitle, {fontSize: 44 * m.dp}]}>Settings</Text>
      <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 8 * m.dp}]}>
        Saved on this TV. Press Back to return.
      </Text>

      <SettingField
        label="Network name"
        value={name}
        placeholder={DEFAULT_MDNS_NAME}
        suffix=".local"
        help={nameHelp}
        metrics={m}
        onSave={t => update({mdnsName: t})}
      />
      <SettingField
        label="Backend server (host:port)"
        value={backend}
        placeholder={DEFAULT_BACKEND}
        help={`Where HamClock gets its data. Leave empty for ${DEFAULT_BACKEND}. Saving restarts HamClock.`}
        metrics={m}
        onSave={t => update({backend: t})}
      />

      <View style={{marginTop: 26 * m.dp}}>
        <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}]}>Screen safe margin</Text>
        <View style={[styles.stepRow, {columnGap: 16 * m.dp}]}>
          {OVERSCAN_CHOICES.map(c => (
            <FocusButton
              key={c.pct}
              metrics={m}
              label={c.label}
              selected={overscan === c.pct}
              onPress={() => update({overscan: c.pct})}
            />
          ))}
        </View>
        <Text style={[styles.panelMeta, {fontSize: 22 * m.dp, marginTop: 8 * m.dp}]}>
          Shrinks the clock if your TV cuts off its edges.
        </Text>
      </View>

      <View style={[styles.stepRow, {marginTop: 20 * m.dp}]}>
        <FocusButton
          metrics={m}
          label="Back"
          onPress={() => dispatch({type: 'PANEL_PAGE', page: 'main'})}
          preferred={!fieldCtx?.lastEdited}
        />
      </View>
    </View>
  );
}

function SettingsPanel(props: {
  state: AppState;
  settings: VegaSettings | null;
  metrics: TvMetrics;
  dispatch: React.Dispatch<AppEvent>;
  update: UpdateSettings;
}) {
  const {state, metrics: m} = props;
  const fieldCtx = useContext(FieldContext);
  return (
    <View style={styles.backdrop}>
      <TVFocusGuideView
        key={`${state.panelPage}:${fieldCtx?.panelGen ?? 0}`}
        autoFocus
        trapFocusUp
        trapFocusDown
        trapFocusLeft
        trapFocusRight
        style={[
          styles.panel,
          {width: 1120 * m.dp, right: m.safeX, top: m.safeY, bottom: m.safeY, padding: 40 * m.dp, borderRadius: 24 * m.dp},
        ]}>
        {state.panelPage === 'settings' ? <PanelSettings {...props} /> : <PanelMain {...props} />}
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
  const stateRef = useRef(state);
  stateRef.current = state;
  const webRef = useRef<{injectJavaScript: (js: string) => void} | null>(null);
  const [settings, setSettings] = useState<VegaSettings | null>(null);
  const editCancelRef = useRef<(() => void) | null>(null);
  const [fieldNotes, setFieldNotes] = useState<Record<string, FieldNote>>({});
  const [lastEdited, setLastEdited] = useState<string | null>(null);
  const [panelGen, setPanelGen] = useState(0);
  const afterEdit = useCallback((label: string, note: FieldNote | null) => {
    setFieldNotes(n => {
      const next = {...n};
      if (note) next[label] = note;
      else delete next[label];
      return next;
    });
    setLastEdited(label);
    setPanelGen(g => g + 1);
  }, []);
  // A new page (or closing the panel) starts with no notes and normal preferred focus.
  useEffect(() => {
    setFieldNotes({});
    setLastEdited(null);
  }, [state.settingsOpen, state.panelPage]);
  const fieldCtx = useMemo<FieldCtx>(
    () => ({cancelRef: editCancelRef, notes: fieldNotes, lastEdited, panelGen, afterEdit}),
    [fieldNotes, lastEdited, panelGen, afterEdit],
  );

  // index.js normally starts the engine; make sure it's running if the app was resumed after a stop.
  useEffect(() => {
    try {
      ensureEngineStarted(DEFAULT_ENGINE_OPTIONS);
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  }, []);

  useEngineStatus(state.lifecycle, dispatch);

  // Saved settings: read at start, and again when the engine reports a change (e.g. first-run
  // LAN on, or a name/backend change saved from here).
  const refreshSettings = useCallback(() => {
    try {
      setSettings(HcEngine.getSettings());
    } catch (err) {
      console.warn(`[HCSET] getSettings failed: ${String(err)}`);
    }
  }, []);
  useEffect(refreshSettings, [refreshSettings, state.status?.lanAccess, state.status?.mdnsName, state.status?.backend]);
  const settingsSummary = settings ? JSON.stringify(settings) : '';
  useEffect(() => {
    if (settingsSummary) console.info(`[HCSET] ${settingsSummary}`);
  }, [settingsSummary]);

  const updateSettings = useCallback(
    (changes: Partial<VegaSettings>): string => {
      const st = stateRef.current.status;
      const before = {lan: st?.lanAccess, backend: st?.backend};
      let err = '';
      try {
        err = HcEngine.updateSettings(changes);
      } catch (x) {
        err = String(x);
      }
      if (err) return err;
      let next: VegaSettings | null = null;
      try {
        next = HcEngine.getSettings();
        setSettings(next);
      } catch {
        /* polled again below */
      }
      const running = st && (st.state === 'running' || st.state === 'starting');
      if (running && next && (next.lanAccess !== before.lan || next.backend !== before.backend)) {
        // The engine restarts; keep the panel open for LAN (to show the QR), close it for backend.
        dispatch({type: 'RESTART_REQUESTED', keepPanel: next.backend === before.backend});
      }
      return '';
    },
    [],
  );

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
    dispatch({type: stateRef.current.settingsOpen ? 'CLOSE_SETTINGS' : 'OPEN_SETTINGS'});
  }, []);

  // Back: settings page -> main page; otherwise close the panel if open, or open it (it has "Exit app").
  const onBack = useCallback(() => {
    const st = stateRef.current;
    if (editCancelRef.current) editCancelRef.current(); // Back while typing: cancel the edit only
    else if (st.settingsOpen && st.panelPage === 'settings') dispatch({type: 'PANEL_PAGE', page: 'main'});
    else toggleSettings();
  }, [toggleSettings]);
  useEffect(() => {
    const sub = BackHandler.addEventListener('hardwareBackPress', () => {
      onBack();
      return true;
    });
    return () => sub.remove();
  }, [onBack]);

  // Menu key also toggles the settings panel.
  useTVEventHandler((evt: HWEvent) => {
    if (evt?.eventType === 'menu' && evt.eventKeyAction === 0) toggleSettings();
  });

  // Screen safe margin: fitted inside the page (see pageScript). The value at mount goes into the
  // script injected before load, so the first frame is already fitted; changes are pushed live.
  const overscan = settings?.overscan ?? 0;
  const overscanRef = useRef(overscan);
  overscanRef.current = overscan;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const injectedJs = useMemo(() => pageScript(overscanRef.current), [state.webGeneration]);
  useEffect(() => {
    webRef.current?.injectJavaScript(overscanJs(overscan));
  }, [overscan]);

  const s = state.status;
  const liveUri = s ? `http://${ENGINE_HOST}:${s.liveRwPort}/live.html` : null;
  const showWeb = state.lifecycle === 'READY' && liveUri !== null;
  const showStatus = (!showWeb || !state.webLoaded) && !firstRun;
  // Keys (incl. Back) go to RN while a native overlay is up, else to the page.
  const webHasKeys = !state.settingsOpen && !firstRun;

  return (
    <FieldContext.Provider value={fieldCtx}>
    <View style={styles.root}>
      {showWeb && (
        <WebView
          key={state.webGeneration}
          ref={webRef as never}
          style={styles.webview}
          source={{uri: liveUri}}
          allowSystemKeyEvents={webHasKeys}
          injectedJavaScriptBeforeContentLoaded={injectedJs}
          onMessage={(e: WebViewMessageEvent) => {
            const data = e.nativeEvent.data;
            if (data === 'back') onBack();
            else console.info(`[HCWEB] ${data}`);
          }}
          hasTVPreferredFocus={webHasKeys}
          javaScriptEnabled
          domStorageEnabled
          onLoad={() => {
            dispatch({type: 'WEB_LOADED'});
            webRef.current?.injectJavaScript(overscanJs(overscanRef.current));
          }}
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
      {state.settingsOpen && (
        <SettingsPanel state={state} settings={settings} metrics={metrics} dispatch={dispatch} update={updateSettings} />
      )}
    </View>
    </FieldContext.Provider>
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
  textCenter: {textAlign: 'center'},

  statusCard: {backgroundColor: C.panel, borderWidth: 1, borderColor: C.border, alignItems: 'center'},
  statusTitle: {color: C.text, fontWeight: '600', textAlign: 'center'},
  statusDetail: {color: C.muted, textAlign: 'center'},

  backdrop: {...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(2, 4, 8, 0.6)'},
  panel: {position: 'absolute', backgroundColor: C.panel, borderWidth: 1, borderColor: C.border},
  panelTitle: {color: C.text, fontWeight: '700'},
  panelMeta: {color: C.muted},
  lanBox: {backgroundColor: 'rgba(255, 181, 71, 0.08)', borderWidth: 1, borderColor: 'rgba(255, 181, 71, 0.35)'},
  lanBoxCol: {alignItems: 'center'},
  lanBoxOff: {backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: C.border},
  lanUrl: {color: C.accent, fontWeight: '700'},
  lanUrlAlt: {color: C.text, fontWeight: '600'},
  lanNoAddr: {color: C.text},
  panelRow: {flex: 1, flexDirection: 'row'},
  panelSide: {flex: 1},
  sectionTitle: {color: C.text, fontWeight: '700'},
  fieldError: {color: C.danger, fontWeight: '600'},
  textInput: {
    color: C.text,
    backgroundColor: 'rgba(255,255,255,0.06)',
    borderWidth: 2,
    borderColor: 'rgba(255,255,255,0.18)',
  },
  textInputFocused: {borderColor: C.accent, backgroundColor: 'rgba(255, 181, 71, 0.10)'},
  textInputText: {color: C.text},

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
  buttonSelected: {borderColor: C.accent},
  buttonFocused: {backgroundColor: C.accent, borderColor: '#fff'},
  buttonFocusedDanger: {backgroundColor: C.danger, borderColor: '#fff'},
  buttonText: {color: C.text, fontWeight: '600'},
  buttonTextFocused: {color: '#10131a'},
});
