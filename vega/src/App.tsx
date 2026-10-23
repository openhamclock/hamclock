/**
 * HamClock for Amazon Vega OS — primary application entry point.
 *
 * Source of truth: ARCHITECTURE.md (HamClock Technical Blueprint).
 *   §2.1  Live WebSocket protocol (/live-ws)          -> useLiveSocket()
 *   §2.2  REST API (/get_sys.txt, /restart, /exit)     -> useDaemonProbe(), restGet()
 *   §3.1  Daemon lifecycle state machine               -> lifecycleReducer()
 *   §3.2  Port allocation ladder                       -> RW_PORT_LADDER / REST_PORT_LADDER
 *   §4.1  800x480 logical coordinate space             -> TvStage
 *   §4.3  Virtual cursor + D-Pad acceleration          -> useVirtualCursor()
 *   §4.4  Modal overlay (embed <url>)                  -> EmbedOverlay
 *
 * The C++ daemon is supervised by the host (systemd, §5). This UI never
 * spawns it; it observes it over loopback and mirrors its lifecycle.
 */
import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';
import {
  Animated,
  BackHandler,
  Image,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
  useWindowDimensions,
} from 'react-native';
import {
  TVFocusGuideView,
  useTVEventHandler,
} from '@amazon-devices/react-native-kepler';
import type {HWEvent} from '@amazon-devices/react-native-kepler';

/* ────────────────────────────────────────────────────────────────────────── */
/* Blueprint constants                                                       */
/* ────────────────────────────────────────────────────────────────────────── */

const DAEMON_HOST = '127.0.0.1'; // §3.3 default mode: loopback only

// §3.2 — preferred port first, then the fallback ladder.
const RW_PORT_LADDER = [8081, 8083, 8084, 8086, 8091];
const REST_PORT_LADDER = [8080, 8088, 8085, 8089, 8090];

// §5 systemd unit launches the daemon with `-k` (skip 10 s boot countdown).
const DAEMON_LAUNCHED_WITH_MINUS_K = true;
const BOOT_COUNTDOWN_MS = 10_000; // §3.1 (only when launched without -k)

// §4.1 logical canvas
const LOGICAL_W = 800;
const LOGICAL_H = 480;

// §2.1 polling cadence
const DELTA_POLL_MS = 100;
const FULL_FRAME_MIN_INTERVAL_MS = 250;

// §4.3 D-Pad virtual cursor
const REPEAT_WINDOW_MS = 200;
const CURSOR_HIDE_MS = 12_000;
const SELECT_LONGPRESS_MS = 500;
const CURSOR_PRESS_ANIM_MS = 120;
// Bottom-right corner settings hotspot (logical coords) — "Hand focus to Native Settings".
const SETTINGS_HOTSPOT = {x: LOGICAL_W - 40, y: LOGICAL_H - 40};

// 1080p TV design grid
const TV_DESIGN_W = 1920;
const TV_DESIGN_H = 1080;
const TV_SAFE_X = 96; // 5 % title-safe
const TV_SAFE_Y = 54;
const GRID_COLUMNS = 12;
const GRID_GUTTER = 24;

const PNG_MAGIC = [0x89, 0x50, 0x4e, 0x47];

/* ────────────────────────────────────────────────────────────────────────── */
/* §3.1 Global lifecycle state machine                                       */
/* ────────────────────────────────────────────────────────────────────────── */

export type DaemonLifecycle =
  | 'STOPPED'
  | 'INITIALIZING'
  | 'BOOT_COUNTDOWN'
  | 'SETUP_WIZARD'
  | 'CLOCK_RUNNING'
  | 'RESTARTING'
  | 'TERMINATED';

export type LiveConnection =
  | 'idle'
  | 'connecting'
  | 'open'
  | 'closed'
  | 'session_timed_out' // §2.1 "Session timed out" — do NOT auto-reconnect
  | 'too_many_connections';

type KeyValues = Record<string, string>;

interface AppState {
  lifecycle: DaemonLifecycle;
  connection: LiveConnection;
  restPort: number | null;
  rwPort: number | null;
  sys: KeyValues; // GET /get_sys.txt
  config: KeyValues; // GET /get_config.txt
  settingsOpen: boolean;
  embedUrl: string | null;
}

type AppEvent =
  | {type: 'REST_UP'; restPort: number; sys: KeyValues}
  | {type: 'REST_DOWN'}
  | {type: 'CONFIG_VALID'; config: KeyValues}
  | {type: 'CONFIG_CHANGED'; config: KeyValues}
  | {type: 'COUNTDOWN_DONE'}
  | {type: 'USER_TAP'} // virtual tap during countdown -> setup wizard
  | {type: 'SETUP_SAVED'}
  | {type: 'RESTART_REQUESTED'}
  | {type: 'EXIT_REQUESTED'}
  | {type: 'CONNECTION'; connection: LiveConnection; rwPort?: number | null}
  | {type: 'OPEN_SETTINGS'}
  | {type: 'CLOSE_SETTINGS'}
  | {type: 'OPEN_EMBED'; url: string}
  | {type: 'CLOSE_EMBED'};

const initialState: AppState = {
  lifecycle: 'STOPPED',
  connection: 'idle',
  restPort: null,
  rwPort: null,
  sys: {},
  config: {},
  settingsOpen: false,
  embedUrl: null,
};

/** Transition table mirrors the §3.1 diagram exactly; illegal events are no-ops. */
function lifecycleReducer(s: AppState, e: AppEvent): AppState {
  switch (e.type) {
    case 'REST_UP':
      if (s.lifecycle === 'STOPPED' || s.lifecycle === 'RESTARTING') {
        return {...s, lifecycle: 'INITIALIZING', restPort: e.restPort, sys: e.sys};
      }
      return {...s, restPort: e.restPort, sys: e.sys};
    case 'REST_DOWN':
      // While restarting, the daemon is expected to vanish briefly; TERMINATED is final.
      if (s.lifecycle === 'RESTARTING' || s.lifecycle === 'TERMINATED') return s;
      return {...s, lifecycle: 'STOPPED', restPort: null};
    case 'CONFIG_VALID':
      return s.lifecycle === 'INITIALIZING'
        ? {...s, lifecycle: 'BOOT_COUNTDOWN', config: e.config}
        : s;
    case 'CONFIG_CHANGED':
      // CLOCK_RUNNING -> CLOCK_RUNNING self-loop ("Config change / Host update")
      return {...s, config: e.config};
    case 'COUNTDOWN_DONE':
      return s.lifecycle === 'BOOT_COUNTDOWN' ? {...s, lifecycle: 'CLOCK_RUNNING'} : s;
    case 'USER_TAP':
      return s.lifecycle === 'BOOT_COUNTDOWN' ? {...s, lifecycle: 'SETUP_WIZARD'} : s;
    case 'SETUP_SAVED':
      return s.lifecycle === 'SETUP_WIZARD' ? {...s, lifecycle: 'CLOCK_RUNNING'} : s;
    case 'RESTART_REQUESTED':
      return s.lifecycle === 'CLOCK_RUNNING'
        ? {...s, lifecycle: 'RESTARTING', settingsOpen: false}
        : s;
    case 'EXIT_REQUESTED':
      return {...s, lifecycle: 'TERMINATED', settingsOpen: false, embedUrl: null};
    case 'CONNECTION':
      return {
        ...s,
        connection: e.connection,
        rwPort: e.rwPort === undefined ? s.rwPort : e.rwPort,
      };
    case 'OPEN_SETTINGS':
      return {...s, settingsOpen: true};
    case 'CLOSE_SETTINGS':
      return {...s, settingsOpen: false};
    case 'OPEN_EMBED':
      return {...s, embedUrl: e.url};
    case 'CLOSE_EMBED':
      return {...s, embedUrl: null};
  }
}

/** States in which the daemon serves the live socket. */
const isDaemonLive = (l: DaemonLifecycle) =>
  l === 'INITIALIZING' ||
  l === 'BOOT_COUNTDOWN' ||
  l === 'SETUP_WIZARD' ||
  l === 'CLOCK_RUNNING';

/* ────────────────────────────────────────────────────────────────────────── */
/* Global state context                                                      */
/* ────────────────────────────────────────────────────────────────────────── */

interface AppContextValue {
  state: AppState;
  dispatch: React.Dispatch<AppEvent>;
  sendLive: (cmd: string) => void;
  requestFullFrame: () => void;
}

const AppContext = createContext<AppContextValue | null>(null);

export function useHamClock(): AppContextValue {
  const ctx = useContext(AppContext);
  if (!ctx) throw new Error('useHamClock must be used inside <App>');
  return ctx;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* §2.2 REST helpers                                                         */
/* ────────────────────────────────────────────────────────────────────────── */

async function restGet(port: number, path: string, timeoutMs = 1500): Promise<string> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(`http://${DAEMON_HOST}:${port}${path}`, {signal: ctl.signal});
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.text();
  } finally {
    clearTimeout(timer);
  }
}

/** Parses the newline-separated "Key   value" schema used by all *.txt endpoints. */
function parseKeyValues(body: string): KeyValues {
  const out: KeyValues = {};
  for (const raw of body.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const m = /^(\S+)\s+(.*)$/.exec(line);
    if (m) out[m[1]] = m[2];
  }
  return out;
}

/** Walks the §3.2 REST ladder; preferred (last known) port first. */
async function discoverRest(
  lastKnown: number | null,
): Promise<{port: number; sys: KeyValues} | null> {
  const order = lastKnown
    ? [lastKnown, ...REST_PORT_LADDER.filter(p => p !== lastKnown)]
    : REST_PORT_LADDER;
  for (const port of order) {
    try {
      const sys = parseKeyValues(await restGet(port, '/get_sys.txt'));
      if (sys.Version) return {port, sys}; // 8085 may be an RO live port — require schema
    } catch {
      /* try next rung */
    }
  }
  return null;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Lifecycle observer hook                                                   */
/* ────────────────────────────────────────────────────────────────────────── */

function useDaemonProbe(state: AppState, dispatch: React.Dispatch<AppEvent>) {
  const stateRef = useRef(state);
  stateRef.current = state;
  const terminated = state.lifecycle === 'TERMINATED';

  // Poll REST to observe STOPPED / INITIALIZING / RESTARTING transitions.
  useEffect(() => {
    if (terminated) return;
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout>;

    const tick = async () => {
      const s = stateRef.current;
      const found = await discoverRest(s.restPort);
      if (cancelled) return;

      if (!found) {
        dispatch({type: 'REST_DOWN'});
      } else {
        dispatch({type: 'REST_UP', restPort: found.port, sys: found.sys});
        try {
          const config = parseKeyValues(await restGet(found.port, '/get_config.txt'));
          if (!cancelled && Object.keys(config).length > 0) {
            const l = stateRef.current.lifecycle;
            dispatch(
              l === 'INITIALIZING' || l === 'STOPPED' || l === 'RESTARTING'
                ? {type: 'CONFIG_VALID', config}
                : {type: 'CONFIG_CHANGED', config},
            );
          }
        } catch {
          /* config not yet served; stay INITIALIZING */
        }
      }
      const settled = stateRef.current.lifecycle === 'CLOCK_RUNNING';
      timer = setTimeout(tick, settled ? 10_000 : 2_000);
    };

    tick();
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [terminated, dispatch]);

  // BOOT_COUNTDOWN: zero-length with -k, otherwise 10 s.
  useEffect(() => {
    if (state.lifecycle !== 'BOOT_COUNTDOWN') return;
    const t = setTimeout(
      () => dispatch({type: 'COUNTDOWN_DONE'}),
      DAEMON_LAUNCHED_WITH_MINUS_K ? 0 : BOOT_COUNTDOWN_MS,
    );
    return () => clearTimeout(t);
  }, [state.lifecycle, dispatch]);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* §2.1 Live WebSocket client                                                */
/* ────────────────────────────────────────────────────────────────────────── */

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/* eslint-disable no-bitwise -- binary encoding */
function bytesToBase64(b: Uint8Array): string {
  const parts: string[] = [];
  let i = 0;
  for (; i + 2 < b.length; i += 3) {
    const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    parts.push(B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63]);
  }
  const rem = b.length - i;
  if (rem === 1) {
    const n = b[i] << 16;
    parts.push(B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + '==');
  } else if (rem === 2) {
    const n = (b[i] << 16) | (b[i + 1] << 8);
    parts.push(B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + '=');
  }
  return parts.join('');
}
/* eslint-enable no-bitwise */

const isPng = (b: Uint8Array) => b.length >= 4 && PNG_MAGIC.every((v, i) => b[i] === v);

/**
 * React Native has no 2D canvas to composite the §2.1 delta sprite strips onto,
 * so delta packets are used as a dirty signal: when a delta reports n_regn > 0,
 * a full `get_live.png?` is requested (rate-limited) and shown via <Image>.
 */
function useLiveSocket(
  enabled: boolean,
  dispatch: React.Dispatch<AppEvent>,
  onFrame: (dataUri: string) => void,
) {
  const wsRef = useRef<WebSocket | null>(null);
  const needFullRef = useRef(true);
  const lastFullRef = useRef(0);

  const sendLive = useCallback((cmd: string) => {
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(cmd);
  }, []);

  const requestFullFrame = useCallback(() => {
    needFullRef.current = true;
  }, []);

  useEffect(() => {
    if (!enabled) return;
    let disposed = false;
    let ladderIdx = 0;
    let backoffMs = 1000;
    let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
    let pollTimer: ReturnType<typeof setInterval> | undefined;
    let noReconnect = false;

    const connect = () => {
      if (disposed) return;
      const port = RW_PORT_LADDER[ladderIdx % RW_PORT_LADDER.length];
      let opened = false;
      let inFlight = false;
      let inFlightSince = 0;

      dispatch({type: 'CONNECTION', connection: 'connecting'});
      const ws = new WebSocket(`ws://${DAEMON_HOST}:${port}/live-ws`);
      // Supported by the RN/Kepler WebSocket runtime, but absent from its .d.ts.
      (ws as WebSocket & {binaryType: 'blob' | 'arraybuffer'}).binaryType = 'arraybuffer';
      wsRef.current = ws;

      ws.onopen = () => {
        opened = true;
        backoffMs = 1000;
        needFullRef.current = true;
        dispatch({type: 'CONNECTION', connection: 'open', rwPort: port});

        pollTimer = setInterval(() => {
          const now = Date.now();
          if (inFlight && now - inFlightSince < 2000) return; // one request outstanding
          const wantFull =
            needFullRef.current && now - lastFullRef.current >= FULL_FRAME_MIN_INTERVAL_MS;
          ws.send(wantFull ? 'get_live.png?' : 'get_live.bin?');
          if (wantFull) {
            needFullRef.current = false;
            lastFullRef.current = now;
          }
          inFlight = true;
          inFlightSince = now;
        }, DELTA_POLL_MS);
      };

      ws.onmessage = ev => {
        inFlight = false;
        const data = ev.data;

        if (typeof data === 'string') {
          const text = data.trim();
          if (text === 'Session timed out') {
            noReconnect = true;
            dispatch({type: 'CONNECTION', connection: 'session_timed_out'});
          } else if (text === 'Too many connections') {
            dispatch({type: 'CONNECTION', connection: 'too_many_connections'});
          } else if (text.startsWith('embed ') || text.startsWith('open ')) {
            // TV has no external tabs: both directives route to the in-app overlay (§4.4).
            dispatch({type: 'OPEN_EMBED', url: text.slice(text.indexOf(' ') + 1).trim()});
          }
          // 'full-screen': app is always fullscreen on TV.
          // 'paste': no system clipboard on Vega TV remotes — ignored.
          return;
        }

        const bytes = new Uint8Array(data as ArrayBuffer);
        if (isPng(bytes)) {
          onFrame(`data:image/png;base64,${bytesToBase64(bytes)}`);
        } else if (bytes.length >= 4) {
          // §2.1 delta header: bytes 2..3 = n_regn (big-endian)
          const nRegions = bytes[2] * 256 + bytes[3];
          if (nRegions > 0) needFullRef.current = true;
        }
      };

      ws.onclose = () => {
        if (pollTimer) clearInterval(pollTimer);
        if (wsRef.current === ws) wsRef.current = null;
        if (disposed) return;
        if (noReconnect) return; // §2.1: never auto-reconnect after session timeout
        if (!opened) ladderIdx++; // walk §3.2 RW ladder until a port answers
        dispatch({type: 'CONNECTION', connection: 'closed', rwPort: null});
        reconnectTimer = setTimeout(connect, backoffMs);
        backoffMs = Math.min(backoffMs * 2, 10_000);
      };

      ws.onerror = () => {
        /* onclose follows */
      };
    };

    connect();
    return () => {
      disposed = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      if (pollTimer) clearInterval(pollTimer);
      wsRef.current?.close();
      wsRef.current = null;
    };
  }, [enabled, dispatch, onFrame]);

  return {sendLive, requestFullFrame};
}

/* ────────────────────────────────────────────────────────────────────────── */
/* §4.3 D-Pad virtual cursor                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

interface CursorState {
  x: number;
  y: number;
  visible: boolean;
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));

function accelStep(n: number): number {
  if (n > 30) return 24;
  if (n > 18) return 16;
  if (n > 8) return 10;
  return 6;
}

const inSettingsHotspot = (x: number, y: number) =>
  x >= SETTINGS_HOTSPOT.x && y >= SETTINGS_HOTSPOT.y;

function useVirtualCursor(opts: {
  sendLive: (cmd: string) => void;
  dispatch: React.Dispatch<AppEvent>;
  inputBlocked: boolean; // settings / embed overlay owns native focus
}) {
  const {sendLive, dispatch, inputBlocked} = opts;
  const [cursor, setCursor] = useState<CursorState>({
    x: Math.round(LOGICAL_W / 2),
    y: Math.round(LOGICAL_H / 2),
    visible: false,
  });
  const pressScale = useRef(new Animated.Value(1)).current;

  const cursorRef = useRef(cursor);
  cursorRef.current = cursor;
  const blockedRef = useRef(inputBlocked);
  blockedRef.current = inputBlocked;

  const lastRepeatTs = useRef(0);
  const repeatCount = useRef(0);
  const hideTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const selectDown = useRef(false);
  const longPressTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const longPressFired = useRef(false);

  const commit = useCallback((next: CursorState) => {
    cursorRef.current = next;
    setCursor(next);
  }, []);

  const armHideTimer = useCallback(() => {
    if (hideTimer.current) clearTimeout(hideTimer.current);
    hideTimer.current = setTimeout(() => {
      commit({...cursorRef.current, visible: false});
      sendLive('set_mouse?x=-1&y=-1'); // pointer exit
    }, CURSOR_HIDE_MS);
  }, [commit, sendLive]);

  const openSettings = useCallback(() => dispatch({type: 'OPEN_SETTINGS'}), [dispatch]);

  const handleDirection = useCallback(
    (dir: 'up' | 'down' | 'left' | 'right') => {
      const c = cursorRef.current;

      // Hidden cursor: wake in place, no positional shift.
      if (!c.visible) {
        commit({...c, visible: true});
        sendLive(`set_mouse?x=${c.x}&y=${c.y}`);
        armHideTimer();
        return;
      }

      const now = Date.now();
      repeatCount.current = now - lastRepeatTs.current < REPEAT_WINDOW_MS ? repeatCount.current + 1 : 0;
      lastRepeatTs.current = now;
      const step = accelStep(repeatCount.current);

      // Pushing past the bottom-right corner hands focus to native settings.
      if (
        inSettingsHotspot(c.x, c.y) &&
        ((dir === 'right' && c.x >= LOGICAL_W - 1) || (dir === 'down' && c.y >= LOGICAL_H - 1))
      ) {
        openSettings();
        return;
      }

      const dx = dir === 'left' ? -step : dir === 'right' ? step : 0;
      const dy = dir === 'up' ? -step : dir === 'down' ? step : 0;
      const next = {
        x: clamp(c.x + dx, 0, LOGICAL_W - 1),
        y: clamp(c.y + dy, 0, LOGICAL_H - 1),
        visible: true,
      };
      commit(next);
      sendLive(`set_mouse?x=${next.x}&y=${next.y}`);
      armHideTimer();
    },
    [armHideTimer, commit, openSettings, sendLive],
  );

  const handleSelect = useCallback(
    (keyAction: number) => {
      if (keyAction === 0) {
        if (selectDown.current) return; // ignore auto-repeat while held
        selectDown.current = true;
        longPressFired.current = false;
        longPressTimer.current = setTimeout(() => {
          longPressFired.current = true; // > 500 ms: abort touch, open settings
          openSettings();
        }, SELECT_LONGPRESS_MS);
        return;
      }

      // Key up
      selectDown.current = false;
      if (longPressTimer.current) clearTimeout(longPressTimer.current);
      if (longPressFired.current) return;

      const c = cursorRef.current;
      if (!c.visible) {
        // Prevent blind clicks: first press only reveals the cursor.
        commit({...c, visible: true});
        armHideTimer();
        return;
      }
      if (inSettingsHotspot(c.x, c.y)) {
        openSettings();
        return;
      }

      Animated.sequence([
        Animated.timing(pressScale, {toValue: 0.85, duration: CURSOR_PRESS_ANIM_MS / 2, useNativeDriver: true}),
        Animated.timing(pressScale, {toValue: 1, duration: CURSOR_PRESS_ANIM_MS / 2, useNativeDriver: true}),
      ]).start();
      sendLive(`set_touch?x=${c.x}&y=${c.y}&button=0`);
      dispatch({type: 'USER_TAP'});
      armHideTimer();
    },
    [armHideTimer, commit, dispatch, openSettings, pressScale, sendLive],
  );

  const onTvEvent = useCallback(
    (evt: HWEvent) => {
      if (!evt || blockedRef.current) return;
      const action = evt.eventKeyAction; // 0 = down (repeats while held), 1 = up
      switch (evt.eventType) {
        case 'up':
        case 'down':
        case 'left':
        case 'right':
          if (action === 0) handleDirection(evt.eventType);
          break;
        case 'select':
          // Only 0 (down) / 1 (up) are meaningful; SDK types also allow undefined / -1.
          if (action === 0 || action === 1) handleSelect(action);
          break;
      }
    },
    [handleDirection, handleSelect],
  );

  useTVEventHandler(onTvEvent);

  // Cancel any in-progress select gesture when an overlay takes focus.
  useEffect(() => {
    if (!inputBlocked) return;
    selectDown.current = false;
    if (longPressTimer.current) clearTimeout(longPressTimer.current);
  }, [inputBlocked]);

  useEffect(
    () => () => {
      if (hideTimer.current) clearTimeout(hideTimer.current);
      if (longPressTimer.current) clearTimeout(longPressTimer.current);
    },
    [],
  );

  return {cursor, pressScale};
}

/* ────────────────────────────────────────────────────────────────────────── */
/* 1080p TV layout                                                           */
/* ────────────────────────────────────────────────────────────────────────── */

/** Scales the 1920x1080 design grid to the actual window. */
function useTvMetrics() {
  const {width, height} = useWindowDimensions();
  return useMemo(() => {
    const dp = Math.min(width / TV_DESIGN_W, height / TV_DESIGN_H);
    // Clock canvas: largest 5:3 box that fits (1800x1080 on a 1080p panel).
    const canvasScale = Math.min(width / LOGICAL_W, height / LOGICAL_H);
    const canvasW = LOGICAL_W * canvasScale;
    const canvasH = LOGICAL_H * canvasScale;
    const safeW = width - 2 * TV_SAFE_X * dp;
    const colW = (safeW - (GRID_COLUMNS - 1) * GRID_GUTTER * dp) / GRID_COLUMNS;
    return {
      width,
      height,
      dp,
      canvasScale,
      canvasW,
      canvasH,
      canvasLeft: (width - canvasW) / 2,
      canvasTop: (height - canvasH) / 2,
      safeX: TV_SAFE_X * dp,
      safeY: TV_SAFE_Y * dp,
      /** Width of an n-column span on the 12-col title-safe grid. */
      span: (n: number) => n * colW + (n - 1) * GRID_GUTTER * dp,
    };
  }, [width, height]);
}

type TvMetrics = ReturnType<typeof useTvMetrics>;

function TvStage(props: {
  metrics: TvMetrics;
  frameUri: string | null;
  cursor: CursorState;
  pressScale: Animated.Value;
}) {
  const {metrics: m, frameUri, cursor, pressScale} = props;
  const size = 28 * m.dp;
  return (
    <View
      style={[
        styles.canvas,
        {left: m.canvasLeft, top: m.canvasTop, width: m.canvasW, height: m.canvasH},
      ]}>
      {frameUri && (
        <Image
          source={{uri: frameUri}}
          style={StyleSheet.absoluteFill}
          resizeMode="stretch"
          fadeDuration={0}
        />
      )}

      {cursor.visible && (
        <>
          <View
            pointerEvents="none"
            style={[
              styles.settingsHotspot,
              {
                left: SETTINGS_HOTSPOT.x * m.canvasScale,
                top: SETTINGS_HOTSPOT.y * m.canvasScale,
                width: (LOGICAL_W - SETTINGS_HOTSPOT.x) * m.canvasScale,
                height: (LOGICAL_H - SETTINGS_HOTSPOT.y) * m.canvasScale,
              },
              inSettingsHotspot(cursor.x, cursor.y) && styles.settingsHotspotActive,
            ]}>
            <Text style={[styles.settingsGlyph, {fontSize: 30 * m.dp}]}>⚙</Text>
          </View>
          <Animated.View
            pointerEvents="none"
            style={[
              styles.cursor,
              {
                width: size,
                height: size,
                borderRadius: size / 2,
                transform: [
                  {translateX: cursor.x * m.canvasScale - size / 2},
                  {translateY: cursor.y * m.canvasScale - size / 2},
                  {scale: pressScale},
                ],
              },
            ]}>
            <View style={styles.cursorDot} />
          </Animated.View>
        </>
      )}
    </View>
  );
}

const LIFECYCLE_COPY: Record<DaemonLifecycle, string> = {
  STOPPED: 'Waiting for the HamClock daemon…',
  INITIALIZING: 'HamClock is initializing…',
  BOOT_COUNTDOWN: 'HamClock is starting…',
  SETUP_WIZARD: 'Setup',
  CLOCK_RUNNING: 'Connecting to live display…',
  RESTARTING: 'Restarting HamClock…',
  TERMINATED: 'HamClock has exited.',
};

function StatusCard({metrics: m}: {metrics: TvMetrics}) {
  const {state} = useHamClock();
  let detail = `Probing ${DAEMON_HOST} on ports ${REST_PORT_LADDER.join(', ')}`;
  if (state.connection === 'session_timed_out') detail = 'Live session timed out. Press Back and relaunch.';
  else if (state.connection === 'too_many_connections') detail = 'Too many live connections to HamClock.';
  else if (state.restPort) detail = `REST :${state.restPort}${state.sys.Version ? `  ·  v${state.sys.Version}` : ''}`;

  return (
    <View style={[styles.statusCard, {width: m.span(6), padding: 40 * m.dp, borderRadius: 24 * m.dp}]}>
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

function SettingsPanel({metrics: m}: {metrics: TvMetrics}) {
  const {state, dispatch, requestFullFrame} = useHamClock();
  const close = () => dispatch({type: 'CLOSE_SETTINGS'});
  const restart = () => {
    if (state.restPort) restGet(state.restPort, '/restart').catch(() => {});
    dispatch({type: 'RESTART_REQUESTED'});
  };
  const exit = () => {
    if (state.restPort) restGet(state.restPort, '/exit').catch(() => {});
    dispatch({type: 'EXIT_REQUESTED'});
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
          {
            width: m.span(4),
            right: m.safeX,
            top: m.safeY,
            bottom: m.safeY,
            padding: 40 * m.dp,
            borderRadius: 24 * m.dp,
          },
        ]}>
        <Text style={[styles.panelTitle, {fontSize: 44 * m.dp}]}>HamClock</Text>
        <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 8 * m.dp}]}>
          {state.lifecycle}
          {state.sys.Version ? `  ·  v${state.sys.Version}` : ''}
          {state.rwPort ? `  ·  live :${state.rwPort}` : ''}
          {state.restPort ? `  ·  rest :${state.restPort}` : ''}
        </Text>
        <View style={{height: 24 * m.dp}} />
        <FocusButton metrics={m} label="Resume clock" onPress={close} preferred />
        <FocusButton
          metrics={m}
          label="Refresh display"
          onPress={() => {
            requestFullFrame();
            close();
          }}
        />
        <FocusButton metrics={m} label="Restart HamClock" onPress={restart} />
        <FocusButton metrics={m} label="Exit HamClock" onPress={exit} tone="danger" />
      </TVFocusGuideView>
    </View>
  );
}

/** §4.4 modal overlay. Focus is directed to Close; Back also dismisses. */
function EmbedOverlay({metrics: m, url}: {metrics: TvMetrics; url: string}) {
  const {dispatch} = useHamClock();
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
          {
            left: m.safeX,
            right: m.safeX,
            top: m.safeY,
            bottom: m.safeY,
            padding: 40 * m.dp,
            borderRadius: 24 * m.dp,
          },
        ]}>
        <View style={styles.embedHeader}>
          <Text numberOfLines={1} style={[styles.panelMeta, styles.embedUrl, {fontSize: 26 * m.dp}]}>
            {url}
          </Text>
          <View style={{width: m.span(2)}}>
            <FocusButton metrics={m} label="Close" onPress={() => dispatch({type: 'CLOSE_EMBED'})} preferred />
          </View>
        </View>
        {/* Web content host: mount the Vega WebView here with source={{uri: url}}. */}
        <View style={[styles.embedBody, {marginTop: 24 * m.dp, borderRadius: 16 * m.dp}]} />
      </TVFocusGuideView>
    </View>
  );
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Root                                                                      */
/* ────────────────────────────────────────────────────────────────────────── */

export const App = () => {
  const [state, dispatch] = useReducer(lifecycleReducer, initialState);
  const [frameUri, setFrameUri] = useState<string | null>(null);
  const metrics = useTvMetrics();

  useDaemonProbe(state, dispatch);

  const liveEnabled = isDaemonLive(state.lifecycle) && state.connection !== 'session_timed_out';
  const {sendLive, requestFullFrame} = useLiveSocket(liveEnabled, dispatch, setFrameUri);

  // Drop the stale frame when the daemon goes away.
  useEffect(() => {
    if (!isDaemonLive(state.lifecycle)) setFrameUri(null);
  }, [state.lifecycle]);

  const overlayOpen = state.settingsOpen || state.embedUrl !== null;
  const {cursor, pressScale} = useVirtualCursor({sendLive, dispatch, inputBlocked: overlayOpen});

  // Remote Back: dismiss overlays first; otherwise let the system handle it.
  useEffect(() => {
    const sub = BackHandler.addEventListener('hardwareBackPress', () => {
      if (state.embedUrl !== null) {
        dispatch({type: 'CLOSE_EMBED'});
        return true;
      }
      if (state.settingsOpen) {
        dispatch({type: 'CLOSE_SETTINGS'});
        return true;
      }
      return false;
    });
    return () => sub.remove();
  }, [state.embedUrl, state.settingsOpen]);

  const ctx = useMemo<AppContextValue>(
    () => ({state, dispatch, sendLive, requestFullFrame}),
    [state, sendLive, requestFullFrame],
  );

  const showStatus = !frameUri || state.connection !== 'open';

  return (
    <AppContext.Provider value={ctx}>
      <View style={styles.root}>
        <TvStage metrics={metrics} frameUri={frameUri} cursor={cursor} pressScale={pressScale} />
        {showStatus && (
          <View style={styles.center} pointerEvents="none">
            <StatusCard metrics={metrics} />
          </View>
        )}
        {state.settingsOpen && <SettingsPanel metrics={metrics} />}
        {state.embedUrl !== null && <EmbedOverlay metrics={metrics} url={state.embedUrl} />}
      </View>
    </AppContext.Provider>
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
  accentGlow: 'rgba(255, 181, 71, 0.35)',
  danger: '#ff5d6c',
};

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: C.bg},
  canvas: {position: 'absolute', backgroundColor: '#000', overflow: 'hidden'},
  center: {...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center'},

  cursor: {
    position: 'absolute',
    left: 0,
    top: 0,
    borderWidth: 3,
    borderColor: C.accent,
    backgroundColor: C.accentGlow,
    alignItems: 'center',
    justifyContent: 'center',
  },
  cursorDot: {width: 6, height: 6, borderRadius: 3, backgroundColor: '#fff'},
  settingsHotspot: {
    position: 'absolute',
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(0,0,0,0.35)',
    borderTopLeftRadius: 12,
  },
  settingsHotspotActive: {backgroundColor: C.accentGlow},
  settingsGlyph: {color: C.text},

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

  embedHeader: {flexDirection: 'row', alignItems: 'center'},
  embedUrl: {flex: 1},
  embedBody: {flex: 1, backgroundColor: '#000'},
});
