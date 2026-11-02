/**
 * HamClock for Amazon Vega OS — app shell.
 *
 * The HamClock engine runs inside this app (see ../PHASE1_RESULTS.md) and serves its own
 * live.html on 127.0.0.1. This shell:
 *   - waits until the engine is listening on its live port, then shows live.html in a WebView;
 *   - shows a status card while starting, restarting or after an error;
 *   - gives a native settings panel (Back or Menu) in two pages:
 *       main: resume, LAN access, Setup HamClock, exit, plus the address and a QR code to open HamClock
 *             (live clock, or the antenna configuration page on the REST port, as in the Android app)
 *             from another device while LAN access is on;
 *       settings: mDNS name, backend host, screen safe margin (overscan), update check.
 *     Settings are saved natively in <dataDir>/vega-settings.json (HcEngine.updateSettings). A
 *     backend change is applied with one engine restart when leaving the Settings page.
 *   - applies the screen safe margin inside the page (the clock canvas is fitted, see pageScript);
 *   - shows a first-run screen (QR code + address) while HamClock needs Setup, so the user can
 *     enter the callsign from a phone. It closes by itself once the engine reports Setup done.
 *   QR codes always encode the IP URL (phones may not resolve .local); the .local URL is text.
 *   - keeps the screensaver away while HamClock is on screen: an inaudible WebAudio tone in the
 *     page (Vega holds the screensaver off only during playback), paused in the background.
 *
 * D-Pad: remote keys go to the page as DOM key events (allowSystemKeyEvents). pageScript
 * (injected) sends arrows/OK to live.html's virtual cursor and posts Back to this shell, because
 * with allowSystemKeyEvents Back reaches only the page. A held arrow repeats every 50 ms (Vega's
 * own repeat, ~200 ms, is too slow for live.html's speed-up). While a native overlay (first-run
 * screen or settings panel) is up, keys go to RN instead. The engine is started in index.js.
 */
import React, {useCallback, useContext, useEffect, useMemo, useReducer, useRef, useState} from 'react';
import {
  Animated,
  AppState as RNAppState,
  BackHandler,
  Keyboard,
  Linking,
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
  WebViewNavigationEvent,
} from '@amazon-devices/webview/dist/types/WebViewTypes';
import {DEFAULT_BACKEND, DEFAULT_ENGINE_OPTIONS, DEFAULT_MDNS_NAME, HcEngine, ensureEngineStarted} from './engine';
import type {EngineStatus, VegaSettings} from './engine';
import {QrCode} from './QrCode';
import {antennasReady, engineBusy, firstRunVisible, initialState, lanUrls, lifecycleReducer, qrUrlFor} from './lifecycle';
import type {LanTarget} from './lifecycle';
import type {AppEvent, AppState, Lifecycle} from './lifecycle';
import {APPSTORE_URL, CHECK_INTERVAL_MS, FIRST_CHECK_DELAY_MS, SNOOZE_MS, fetchLatestVersion, noticeFor} from './updateCheck';

/* ────────────────────────────────────────────────────────────────────────── */
/* Constants                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

const ENGINE_HOST = '127.0.0.1';
/**
 * Debug builds only (__DEV__ is false in Release): diagnostic lines in the device log, i.e.
 * [HCWEB] keys/keyups/visibility/audio, [HCAPP], [HCSET], [HCLAN], [HCSETUP], [HCFIELD], [HCEV].
 * Failures (console.warn/error) are always logged.
 */
const DEBUG = __DEV__;
function debugLog(msg: string): void {
  if (DEBUG) console.info(msg);
}
const POLL_STARTING_MS = 500;
const POLL_READY_MS = 3_000;
/** Retry delay after the WebView fails to load the page. */
const WEB_RETRY_MS = 2_000;
/**
 * After a LAN access change, the panel shows "Restarting HamClock…" at least this long. The
 * restart takes about a second (one or two polls), so the notice would otherwise only flicker.
 */
const LAN_RESTART_HOLD_MS = 2_000;

/** The clock page (live.html from the engine), as opposed to a page HamClock opened. */
function isLivePage(url: string): boolean {
  return url === '' || (url.startsWith(`http://${ENGINE_HOST}:`) && url.includes('/live.html'));
}

/**
 * Injected into live.html before its own scripts, so this capture-phase listener runs before the
 * page's keydown handler. live.html defines handleVirtualCursorMove/Click but leaves calling them
 * to the host app (the Android app does it from native key handling). While HamClock shows a
 * dialog with keyboard navigation (QR code: Open/Close), live.html sets window.hc_kbnav and those
 * functions send arrows and OK to it as keys; Back then sends Escape (closes the dialog).
 * While live.html's embed overlay (Wind, ADS-B) is up, or on a page HamClock opened, the D-pad
 * moves a pointer drawn over the page and OK taps there (a real touch from RN, see tapAt), so it
 * works inside the framed site too; Back closes the overlay.
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
  // Diagnostics for the device log ([HCWEB]); Debug builds only.
  var DEBUG = ${DEBUG ? 'true' : 'false'};
  function dbg(m) { if (DEBUG) post(m); }

  // Hold to speed up. Vega repeats a held key after ~520 ms and then only every ~200 ms;
  // live.html speeds the cursor up on presses < 200 ms apart (tuned for Android's ~50 ms repeat).
  // So once a held arrow repeats, move every HOLD_MS ourselves until the key is released: keyup,
  // or no repeat from Vega for HOLD_END_MS (in case keyup isn't delivered).
  var HOLD_MS = 50, HOLD_END_MS = 350;
  var holdKey = null, holdTimer = null, lastDown = {key: null, t: 0, up: true};
  function stopHold() {
    if (holdTimer) clearInterval(holdTimer);
    holdTimer = null;
    holdKey = null;
  }
  function startHold(k) {
    stopHold();
    holdKey = k;
    holdTimer = setInterval(function () {
      if (Date.now() - lastDown.t > HOLD_END_MS) { stopHold(); return; }
      if (typeof handleVirtualCursorMove === 'function') handleVirtualCursorMove(holdKey);
    }, HOLD_MS);
  }
  function arrowDown(e) {
    var k = e.key, now = Date.now();
    var repeat = e.repeat || (lastDown.key === k && !lastDown.up && now - lastDown.t < 700);
    lastDown = {key: k, t: now, up: false};
    if (holdKey === k) return;                   // already repeating; Vega's repeat just keeps it alive
    // While a HamClock dialog takes the arrows as keys (window.hc_kbnav, see below), no speed-up.
    if (repeat && !window.hc_kbnav) { startHold(k); return; }
    stopHold();
    if (typeof handleVirtualCursorMove === 'function') handleVirtualCursorMove(k);
  }
  window.addEventListener('keyup', function (e) {
    if (ARROWS[e.key]) {
      if (lastDown.key === e.key) lastDown.up = true;
      if (holdKey === e.key) stopHold();
      dbg('keyup:' + e.key);
    }
  }, true);

  // A pointer for pages that aren't the clock: live.html's embed overlay (Wind, ADS-B: a site in
  // an iframe with "Open in new tab" and "Close" buttons) and pages HamClock opened in the WebView
  // ("Open"). The D-pad moves a cursor drawn over the page (hold to speed up), OK taps where it
  // points. The page can't click into the framed site (another origin), so the tap goes to RN,
  // which sends a real touch with the WebView's dispatchTouchEvent; that reaches the iframe and the
  // overlay's buttons alike. Back closes the overlay (on an opened page, RN goes back). The framed
  // site must not keep the focus after a tap: its key events would go to the iframe and never
  // reach this page, so the page takes the focus back.
  var ptr = {el: null, x: 0, y: 0, key: null, timer: null, v: 0, last: 0, up: true, placed: false};
  function embedUp() {
    var ov = document.getElementById('embed-overlay');
    return !!(ov && getComputedStyle(ov).display !== 'none');
  }
  function onClockPage() { return !!document.getElementById('hamclock-cvs'); }
  function pointerMode() { return embedUp() || !onClockPage(); }
  function ptrShow() {
    if (!document.body) return;
    if (!ptr.el) {
      // Same arrow as live.html's clock cursor (#virtual-cursor), tip at the pointer position.
      ptr.el = document.createElement('div');
      ptr.el.style.cssText = 'position:fixed;left:0;top:0;width:28px;height:28px;pointer-events:none;' +
        'z-index:2147483647;transition:transform 50ms ease-out;filter:drop-shadow(0 2px 5px rgba(0,0,0,0.85));';
      ptr.el.innerHTML = '<svg width="28" height="28" viewBox="0 0 28 28" fill="none" xmlns="http://www.w3.org/2000/svg">' +
        '<polygon points="1,1 1,23 6.8,17.2 12,27 15.5,25 10.3,15.5 18,15.5" fill="#000000"/>' +
        '<polygon points="2,3 2,20.5 6.5,16 11.2,24.8 13.5,23.5 8.8,14.8 15.5,14.8" fill="#FFFFFF"/></svg>';
    }
    if (!ptr.el.parentNode) document.body.appendChild(ptr.el);
    if (!ptr.placed) {
      // Take over from the clock's cursor: start where it is (if shown) and hide it, so there's
      // one cursor. Else start in the middle of the overlay (or the page).
      var ov = document.getElementById('embed-overlay'), r = embedUp() ? ov.getBoundingClientRect() : null;
      ptr.x = r ? r.left + r.width / 2 : window.innerWidth / 2;
      ptr.y = r ? r.top + r.height / 2 : window.innerHeight / 2;
      var vc = document.getElementById('virtual-cursor');
      if (vc && getComputedStyle(vc).display !== 'none') {
        var vr = vc.getBoundingClientRect();
        if (vr.width) { ptr.x = vr.left + 1; ptr.y = vr.top + 1; }
      }
      if (typeof hideVirtualCursor === 'function') hideVirtualCursor();
      ptr.placed = true;
    }
    ptr.el.style.display = 'block';
    ptr.el.style.transform = 'translate(' + Math.round(ptr.x - 1) + 'px,' + Math.round(ptr.y - 1) + 'px)';
  }
  function ptrHide() {
    ptrStop();
    if (ptr.el) ptr.el.style.display = 'none';
    ptr.placed = false;
  }
  function ptrMove(k, step) {
    var w = window.innerWidth, h = window.innerHeight;
    var dx = k === 'ArrowLeft' ? -step : k === 'ArrowRight' ? step : 0;
    var dy = k === 'ArrowUp' ? -step : k === 'ArrowDown' ? step : 0;
    var nx = ptr.x + dx, ny = ptr.y + dy;
    // On an opened page, pushing past the top/bottom edge scrolls it.
    if (!embedUp() && (ny < 0 || ny > h - 1)) window.scrollBy(0, dy);
    ptr.x = Math.max(0, Math.min(w - 1, nx));
    ptr.y = Math.max(0, Math.min(h - 1, ny));
    ptrShow();
  }
  function ptrStop() {
    if (ptr.timer) clearInterval(ptr.timer);
    ptr.timer = null;
    ptr.key = null;
  }
  function ptrArrow(e) {
    var k = e.key, now = Date.now();
    var repeat = e.repeat || (ptr.lastKey === k && !ptr.up && now - ptr.last < 700);
    ptr.lastKey = k; ptr.last = now; ptr.up = false;
    if (ptr.key === k) return;                     // already moving; Vega's repeat keeps it alive
    if (!repeat) { ptrStop(); ptrMove(k, 12); return; }
    // Held: move every HOLD_MS, faster the longer it's held, until keyup or no repeat for HOLD_END_MS.
    ptrStop();
    ptr.key = k; ptr.v = 6;
    ptr.timer = setInterval(function () {
      if (Date.now() - ptr.last > HOLD_END_MS) { ptrStop(); return; }
      ptr.v = Math.min(30, ptr.v + 1.5);
      ptrMove(ptr.key, ptr.v);
    }, HOLD_MS);
  }
  window.addEventListener('keyup', function (e) {
    if (ARROWS[e.key]) {
      if (ptr.lastKey === e.key) ptr.up = true;
      if (ptr.key === e.key) ptrStop();
    }
  }, true);
  function ptrTap() {
    ptrShow();
    if (ptr.el) {
      ptr.el.style.opacity = '0.5';
      setTimeout(function () { if (ptr.el) ptr.el.style.opacity = ''; }, 150);
    }
    var t = document.elementFromPoint(ptr.x, ptr.y);
    dbg('tap:' + Math.round(ptr.x) + ',' + Math.round(ptr.y) + ' on ' + (t ? t.tagName + (t.id ? '#' + t.id : '') : '-'));
    post('tap:' + Math.round(ptr.x) + ',' + Math.round(ptr.y));
  }
  function refocus() {
    // Focus moved into the iframe (e.g. after a tap on the framed site): take it back.
    var ov = document.getElementById('embed-overlay');
    if (!ov) return;
    if (!ov.hasAttribute('tabindex')) ov.setAttribute('tabindex', '-1');
    ov.style.outline = 'none';
    window.focus();
    ov.focus({preventScroll: true});
  }
  window.addEventListener('blur', function () {
    if (!embedUp()) return;
    dbg('embed:blur');
    setTimeout(function () { if (embedUp()) refocus(); }, 0);
  });
  // The overlay closes by its own buttons too (a tap on Close): hide the pointer then.
  setInterval(function () { if (ptr.el && ptr.el.style.display !== 'none' && !pointerMode()) ptrHide(); }, 500);
  function pointerKey(e) {
    var k = e.key, c = e.keyCode;
    if (ARROWS[k]) ptrArrow(e);
    else if (k === 'Enter' || k === 'Select' || c === 13 || c === 23) { ptrStop(); ptrTap(); }
    else if (k === 'Escape' || k === 'GoBack' || k === 'BrowserBack' || c === 27) {
      ptrHide();
      if (embedUp()) { if (typeof hideEmbed === 'function') hideEmbed(); }
      else post('back');
    } else return false;
    return true;
  }

  window.addEventListener('keydown', function (e) {
    var k = e.key, c = e.keyCode, handled = false;
    if (pointerMode()) {
      stopHold();
      handled = pointerKey(e);
      if (!ARROWS[k] || !e.repeat) dbg('ptr-key:' + k + ':' + c + (handled ? ':h' : '') + ' active=' + (document.activeElement && document.activeElement.tagName));
      if (handled) { e.preventDefault(); e.stopImmediatePropagation(); }
      return;
    }
    if (ARROWS[k]) {
      if (typeof handleVirtualCursorMove === 'function') { arrowDown(e); handled = true; }
    } else if (k === 'Enter' || k === 'Select' || c === 13 || c === 23) {
      stopHold();
      if (typeof handleVirtualCursorClick === 'function') { handleVirtualCursorClick(); handled = true; }
    } else if (k === 'Escape' || k === 'GoBack' || k === 'BrowserBack' || c === 27) {
      stopHold();
      if (window.hc_kbnav && typeof sendKey === 'function') {
        // A HamClock dialog with keyboard navigation (e.g. the QR code) is up: Back closes it.
        sendKey('Escape');
        handled = true;
      } else { post('back'); handled = true; }
    }
    dbg('key:' + k + ':' + c + (handled ? ':h' : '') + (e.repeat ? ':r' : ''));
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

  // Keep the screensaver away: Vega starts it after the idle timeout unless media is playing
  // (TimeoutManager doesn't help). A 30 Hz tone at gain 0.0001 is inaudible but counts as
  // playback. Restarted if the context gets suspended (e.g. after the app was in the background).
  var lastAudio = '';
  function keepAwake() {
    try {
      var AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) { if (lastAudio !== 'none') dbg('audio:none'); lastAudio = 'none'; return; }
      if (!window.__hcAudioCtx) {
        var ctx = new AC(), osc = ctx.createOscillator(), gain = ctx.createGain();
        osc.type = 'sine';
        osc.frequency.setValueAtTime(30, ctx.currentTime);
        gain.gain.setValueAtTime(0.0001, ctx.currentTime);
        osc.connect(gain);
        gain.connect(ctx.destination);
        osc.start();
        window.__hcAudioCtx = ctx;
      }
      var c = window.__hcAudioCtx;
      if (c.state === 'suspended') c.resume().catch(function (e) { dbg('audio:resume-failed ' + e); });
      if (c.state !== lastAudio) dbg('audio:' + c.state);
      lastAudio = c.state;
    } catch (e) { dbg('audio:error ' + e); }
  }
  setTimeout(keepAwake, 1500);
  setInterval(function () { if (!window.__hcAudioPaused) keepAwake(); }, 30000);
  // Only while HamClock is on screen: in the background the tone would keep the screensaver away
  // in other apps too. RN also calls __hcAudio(false/true) on AppState changes.
  window.__hcAudio = function (on) {
    window.__hcAudioPaused = !on;
    if (on) { setTimeout(keepAwake, 500); return; }
    var c = window.__hcAudioCtx;
    if (c && c.state === 'running') c.suspend().then(function () { dbg('audio:' + c.state); lastAudio = c.state; });
  };
  document.addEventListener('visibilitychange', function () {
    dbg('visibility:' + document.visibilityState);
    window.__hcAudio(document.visibilityState === 'visible');
  });
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
  /** Called when the button gets focus (e.g. a choice that applies on highlight). */
  onFocus?: () => void;
}) {
  const {label, onPress, metrics: m, preferred, tone = 'default', selected, onFocus} = props;
  const [focused, setFocused] = useState(false);
  return (
    <TouchableOpacity
      hasTVPreferredFocus={preferred}
      onFocus={() => {
        setFocused(true);
        onFocus?.();
      }}
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
          <FocusButton metrics={m} label="Exit HamClock" onPress={() => BackHandler.exitApp()} tone="danger" />
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

/**
 * In the settings panel instead of the address and QR code while the engine isn't serving: the
 * status card is hidden behind the panel, and the address wouldn't work yet (e.g. right after a
 * LAN access change, which restarts the engine with the panel kept open).
 */
function EngineNotice({state, metrics: m}: {state: AppState; metrics: TvMetrics}) {
  const s = state.status;
  const busy = engineBusy(state);
  let detail: string;
  if (busy) {
    detail = s?.lanAccess
      ? 'The address and QR code appear here when HamClock is ready.'
      : 'LAN access is off: only this TV can open HamClock.';
  } else if (state.lifecycle === 'ERROR') {
    detail = s?.lastError || state.webError || 'Try Setup HamClock, or exit and open the app again.';
  } else {
    detail = 'Try Setup HamClock, or exit and open the app again.';
  }
  return (
    <View style={[styles.lanBoxOff, {padding: 28 * m.dp, borderRadius: 18 * m.dp}]}>
      <View style={styles.stepRow}>
        {busy && <PulseDot size={18 * m.dp} color={C.accent} />}
        <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}, busy && {marginLeft: 16 * m.dp}]}>
          {LIFECYCLE_COPY[state.lifecycle]}
        </Text>
      </View>
      <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 12 * m.dp}]}>{detail}</Text>
    </View>
  );
}

/**
 * Where to browse from another device (LAN access on), or what LAN access does (off). With LAN on,
 * two choices switch the address and QR code between the live clock and the antenna configuration
 * page, as the Android app's settings tabs do; moving the highlight onto a choice switches. The choice resets to the clock when the panel opens.
 */
function LanInfo({state, metrics: m}: {state: AppState; metrics: TvMetrics}) {
  const [target, setTarget] = useState<LanTarget>('live');
  const s = state.status;
  if (!s) return null;
  if (engineBusy(state) || state.lifecycle === 'ERROR' || state.lifecycle === 'STOPPED') {
    return <EngineNotice state={state} metrics={m} />;
  }
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
  const antennas = target === 'antennas';
  const waiting = antennas && !antennasReady(s);
  const {mdnsUrl, ipUrls} = lanUrls(s, target);
  return (
    <View style={[styles.lanBox, styles.lanBoxCol, {padding: 24 * m.dp, borderRadius: 18 * m.dp}]}>
      <View style={[styles.stepRow, {columnGap: 16 * m.dp, marginTop: -16 * m.dp}]}>
        {/* The choice applies on highlight; OK isn't needed. */}
        <FocusButton
          metrics={m}
          label="Live clock"
          selected={!antennas}
          onFocus={() => setTarget('live')}
          onPress={() => setTarget('live')}
        />
        <FocusButton
          metrics={m}
          label="Antennas"
          selected={antennas}
          onFocus={() => setTarget('antennas')}
          onPress={() => setTarget('antennas')}
        />
      </View>
      <Text style={[styles.panelMeta, {fontSize: 24 * m.dp, marginTop: 16 * m.dp}]}>
        {antennas ? 'Antenna configuration page:' : 'Scan, or open on a phone or computer:'}
      </Text>
      {waiting ? (
        <Text style={[styles.lanNoAddr, styles.textCenter, {fontSize: 24 * m.dp, marginTop: 18 * m.dp}]}>
          {s.restPort > 0
            ? 'The antenna page opens once HamClock’s Setup is done.'
            : 'The antenna page is off: its ports are in use by other apps.'}
        </Text>
      ) : (
        <View style={{marginTop: 16 * m.dp}}>
          <LanQr url={qrUrlFor(s, target)} size={300 * m.dp} metrics={m} />
        </View>
      )}
      {!waiting && mdnsUrl && (
        <Text style={[styles.lanUrl, styles.textCenter, {fontSize: 28 * m.dp, marginTop: 18 * m.dp}]}>{mdnsUrl}</Text>
      )}
      {waiting ? null : ipUrls.length > 0 ? (
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
          {s ? `Live :${s.liveRwPort}  ·  REST ${s.restPort > 0 ? `:${s.restPort}` : 'off'}` : ''}
          {lan ? '  ·  LAN on' : '  ·  loopback only'}
        </Text>
        {!!s?.portNotice && (
          <Text style={[styles.lanNoAddr, {fontSize: 22 * m.dp, marginTop: 8 * m.dp}]}>{s.portNotice}</Text>
        )}
        <View style={{height: 20 * m.dp}} />
        <FocusButton metrics={m} label="Resume HamClock" onPress={() => dispatch({type: 'CLOSE_SETTINGS'})} preferred />
        <FocusButton
          metrics={m}
          label={lan ? 'LAN access: On' : 'LAN access: Off'}
          selected={lan}
          // Ignored while the engine restarts (e.g. from the last press); the side box says so.
          onPress={() => {
            if (!engineBusy(state)) setError(update({lanAccess: !lan}));
          }}
        />
        <FocusButton metrics={m} label="Settings…" onPress={() => dispatch({type: 'PANEL_PAGE', page: 'settings'})} />
        <FocusButton metrics={m} label="Setup HamClock" onPress={openSetup} />
        <FocusButton metrics={m} label="Exit HamClock" onPress={() => BackHandler.exitApp()} tone="danger" />
        {!!error && <Text style={[styles.fieldError, {fontSize: 22 * m.dp, marginTop: 12 * m.dp}]}>{error}</Text>}
      </View>
      <View style={[styles.panelSide, {marginLeft: 40 * m.dp}]}>
        <LanInfo state={state} metrics={m} />
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
    debugLog(`[HCFIELD] ${label}: edit ${how}${next?.error ? ` (${next.error})` : ''}`);
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
  const updateCheck = settings?.updateCheck ?? true;
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
        help={`Where HamClock gets its data. Leave empty for ${DEFAULT_BACKEND}. HamClock restarts when you leave this page.`}
        metrics={m}
        onSave={t => update({backend: t})}
      />

      <View style={{marginTop: 26 * m.dp}}>
        <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}]}>Screen safe margin (Overscan)</Text>
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

      <View style={{marginTop: 26 * m.dp}}>
        <Text style={[styles.sectionTitle, {fontSize: 28 * m.dp}]}>Check for updates</Text>
        <View style={[styles.stepRow, {columnGap: 16 * m.dp}]}>
          <FocusButton metrics={m} label="On" selected={updateCheck} onPress={() => update({updateCheck: true})} />
          <FocusButton metrics={m} label="Off" selected={!updateCheck} onPress={() => update({updateCheck: false})} />
        </View>
        <Text style={[styles.panelMeta, {fontSize: 22 * m.dp, marginTop: 8 * m.dp}]}>
          Tells you when a new HamClock is out, so you can update it in the Amazon Appstore.
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

/**
 * "A new HamClock is out" (update check, see updateCheck.ts). A centered card over the clock,
 * focus trapped; Back = Later. "Open Appstore" only with an ASIN (APPSTORE_URL) the TV accepts.
 */
function UpdateNotice(props: {
  version: string;
  canOpenStore: boolean;
  metrics: TvMetrics;
  onOpenStore: () => void;
  onLater: () => void;
  onNever: () => void;
}) {
  const {version, canOpenStore, metrics: m, onOpenStore, onLater, onNever} = props;
  return (
    <View style={[styles.backdrop, styles.center]}>
      <TVFocusGuideView
        autoFocus
        trapFocusUp
        trapFocusDown
        trapFocusLeft
        trapFocusRight
        style={[styles.statusCard, {width: 1100 * m.dp, padding: 48 * m.dp, borderRadius: 24 * m.dp}]}>
        <Text style={[styles.panelTitle, {fontSize: 44 * m.dp}]}>Update available</Text>
        <Text style={[styles.updateText, {fontSize: 30 * m.dp, marginTop: 20 * m.dp}]}>
          A new version of HamClock ({version}) has been released. Check the Amazon Appstore to update.
        </Text>
        <View style={[styles.updateButtons, {columnGap: 20 * m.dp, marginTop: 20 * m.dp}]}>
          {canOpenStore && <FocusButton metrics={m} label="Open Appstore" onPress={onOpenStore} preferred />}
          <FocusButton metrics={m} label="Later" onPress={onLater} preferred={!canOpenStore} />
          <FocusButton metrics={m} label="Don't check again" onPress={onNever} />
        </View>
        <Text style={[styles.panelMeta, styles.textCenter, {fontSize: 22 * m.dp, marginTop: 16 * m.dp}]}>
          You can turn update checks back on in Settings.
        </Text>
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
  const webRef = useRef<{
    injectJavaScript: (js: string) => void;
    goBack: () => void;
    dispatchTouchEvent: (ev: {type: 'touchstart' | 'touchend'; touches: {clientX: number; clientY: number}[]}) => void;
  } | null>(null);
  // OK on the page's pointer (embed overlay, opened pages): a real touch at "x,y" (CSS px in the
  // viewport), which reaches cross-origin iframes, unlike a click the page script could make.
  const tapAt = useCallback((xy: string) => {
    const [x, y] = xy.split(',').map(Number);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    const touches = [{clientX: x, clientY: y}];
    debugLog(`[HCWEB] touch ${x},${y}`);
    webRef.current?.dispatchTouchEvent({type: 'touchstart', touches});
    setTimeout(() => webRef.current?.dispatchTouchEvent({type: 'touchend', touches}), 60);
  }, []);
  // Where the WebView is. HamClock's "open" links (e.g. Show movies -> Open Movie) navigate this
  // WebView away from live.html; Back then returns to the clock (see onBack).
  const webNavRef = useRef<{url: string; canGoBack: boolean}>({url: '', canGoBack: false});
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

  // App lifecycle (Home, screensaver) to the device log. The screensaver is held off by the
  // page's keep-awake tone (pageScript); TimeoutManager lifespan/timeout didn't stop it.
  useEffect(() => {
    debugLog(`[HCAPP] state ${RNAppState.currentState}`);
    const sub = RNAppState.addEventListener('change', s => {
      debugLog(`[HCAPP] state ${s}`);
      webRef.current?.injectJavaScript(
        `if (typeof window.__hcAudio === 'function') window.__hcAudio(${s === 'active'}); true;`,
      );
    });
    return () => sub.remove();
  }, []);

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
    if (settingsSummary) debugLog(`[HCSET] ${settingsSummary}`);
  }, [settingsSummary]);

  const updateSettings = useCallback(
    (changes: Partial<VegaSettings>): string => {
      const st = stateRef.current.status;
      const lanBefore = st?.lanAccess;
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
      if (running && next && next.lanAccess !== lanBefore) {
        // The engine restarts at once for LAN; keep the panel open to show the QR. Until the new
        // run listens, the panel shows "Restarting HamClock…" in its place (EngineNotice). A
        // backend change is only saved; it's applied when leaving the Settings page (below).
        dispatch({type: 'RESTART_REQUESTED', keepPanel: true, hold: true});
      }
      return '';
    },
    [],
  );

  // End the minimum display time of a LAN restart (see LAN_RESTART_HOLD_MS).
  useEffect(() => {
    if (!state.restartHold) return;
    const t = setTimeout(() => dispatch({type: 'RESTART_HOLD_DONE'}), LAN_RESTART_HOLD_MS);
    return () => clearTimeout(t);
  }, [state.restartHold]);

  // Leaving the Settings page (Back button or remote Back) applies a saved backend change with
  // one restart, so several edits on the page cost one restart.
  const onSettingsPage = state.settingsOpen && state.panelPage === 'settings';
  const wasOnSettingsPage = useRef(false);
  useEffect(() => {
    const was = wasOnSettingsPage.current;
    wasOnSettingsPage.current = onSettingsPage;
    if (!was || onSettingsPage) return;
    let st: EngineStatus;
    try {
      st = HcEngine.getStatus();
    } catch {
      return;
    }
    if (!st.restartNeeded) return;
    debugLog('[HCSET] leaving Settings: restart to apply the backend');
    dispatch({type: 'RESTART_REQUESTED'});
    try {
      HcEngine.restart(DEFAULT_ENGINE_OPTIONS);
    } catch (err) {
      dispatch({type: 'ENGINE_ERROR', message: String(err)});
    }
  }, [onSettingsPage]);

  // Diagnostics: log LAN state changes to the device log (native stderr doesn't reach it).
  const lanSummary = DEBUG && state.status
    ? `lan=${state.status.lanAccess} addrs=${(state.status.lanAddresses ?? []).join(',') || '-'} ` +
      `refused=${state.status.rejectedConnections ?? 0} mdns=${state.status.mdnsState ?? '?'}:${state.status.mdnsHost || '-'}`
    : '';
  useEffect(() => {
    if (lanSummary) debugLog(`[HCLAN] ${lanSummary}`);
  }, [lanSummary]);

  // Diagnostics: app process threads/memory/fds once a minute (Debug), to catch a slow leak.
  const procRef = useRef<string | undefined>(undefined);
  procRef.current = state.status?.proc;
  useEffect(() => {
    if (!DEBUG) return;
    const log = () => { if (procRef.current) debugLog(`[HCPROC] ${procRef.current}`); };
    const first = setTimeout(log, 5000);
    const t = setInterval(log, 60000);
    return () => { clearTimeout(first); clearInterval(t); };
  }, []);

  const firstRun = firstRunVisible(state);
  const setupSummary = `setupComplete=${String(state.status?.setupComplete ?? 'unknown')} firstRun=${firstRun} onTv=${state.setupOnTv}`;
  useEffect(() => {
    debugLog(`[HCSETUP] ${setupSummary}`);
  }, [setupSummary]);

  // Diagnostics: engine events (getStatus().events, a ring of the last 30) to the device log,
  // each once. The new ones are what follows the longest seen tail that starts the new list.
  const seenEvents = useRef<string[]>([]);
  const events = DEBUG ? state.status?.events : undefined;
  useEffect(() => {
    if (!events) return;
    const prev = seenEvents.current;
    let k = Math.min(prev.length, events.length);
    for (; k > 0; k--) {
      const tail = prev.slice(prev.length - k);
      if (tail.every((x, i) => x === events[i])) break;
    }
    events.slice(k).forEach(e => debugLog(`[HCEV] ${e}`));
    seenEvents.current = events;
  }, [events]);

  // Retry loading the page after a WebView error while the engine is up.
  useEffect(() => {
    if (!state.webError || state.lifecycle !== 'READY') return;
    const t = setTimeout(() => dispatch({type: 'WEB_RELOAD'}), WEB_RETRY_MS);
    return () => clearTimeout(t);
  }, [state.webError, state.lifecycle]);

  // Update check (as the Android app on Amazon): FIRST_CHECK_DELAY_MS after the clock is up, then
  // every CHECK_INTERVAL_MS, ask the backend for the latest release; if it's newer than the
  // running HamClock, show UpdateNotice. "Later" snoozes that version for SNOOZE_MS (this run).
  const [updateNotice, setUpdateNotice] = useState<string | null>(null);
  const [canOpenStore, setCanOpenStore] = useState(false);
  const snoozeRef = useRef<{version: string; until: number} | null>(null);
  const checkOn = state.lifecycle === 'READY' && settings !== null && settings.updateCheck !== false;
  useEffect(() => {
    if (!checkOn) return;
    let cancelled = false;
    const check = async () => {
      const st = stateRef.current.status;
      if (!st?.version) return;
      const remote = await fetchLatestVersion(st.backend, `HamClock-vega/${st.version}`, fetch, debugLog);
      if (cancelled) return;
      const v = noticeFor(st.version, remote, snoozeRef.current, Date.now());
      debugLog(`[HCUPD] running ${st.version}, latest ${remote ?? '?'}${v ? ': notice' : ''}`);
      if (!v) return;
      let can = false;
      try {
        can = APPSTORE_URL !== '' && (await Linking.canOpenURL(APPSTORE_URL));
      } catch (e) {
        debugLog(`[HCUPD] canOpenURL: ${String(e)}`);
      }
      if (cancelled) return;
      debugLog(`[HCUPD] canOpenURL ${APPSTORE_URL}: ${can}`);
      setCanOpenStore(can);
      setUpdateNotice(v);
    };
    const first = setTimeout(check, FIRST_CHECK_DELAY_MS);
    const every = setInterval(check, CHECK_INTERVAL_MS);
    return () => {
      cancelled = true;
      clearTimeout(first);
      clearInterval(every);
    };
  }, [checkOn]);
  // Shown only over the clock: not over the first-run screen or the settings panel (it waits).
  const showUpdate = updateNotice !== null && !state.settingsOpen && !firstRun && checkOn;
  const showUpdateRef = useRef(showUpdate);
  showUpdateRef.current = showUpdate;
  const updateLater = useCallback(() => {
    setUpdateNotice(v => {
      if (v) snoozeRef.current = {version: v, until: Date.now() + SNOOZE_MS};
      return null;
    });
  }, []);
  const openStore = useCallback(() => {
    updateLater();
    debugLog(`[HCUPD] openURL ${APPSTORE_URL}`);
    Linking.openURL(APPSTORE_URL)
      .then(() => debugLog('[HCUPD] openURL ok'))
      .catch(e => console.warn(`[HCUPD] openURL failed: ${String(e)}`));
  }, [updateLater]);
  const updateNever = useCallback(() => {
    setUpdateNotice(null);
    const err = updateSettings({updateCheck: false});
    if (err) console.warn(`[HCUPD] turning update checks off failed: ${err}`);
  }, [updateSettings]);

  const toggleSettings = useCallback(() => {
    dispatch({type: stateRef.current.settingsOpen ? 'CLOSE_SETTINGS' : 'OPEN_SETTINGS'});
  }, []);

  // Back: settings page -> main page; otherwise close the panel if open, or open it (it has "Exit HamClock").
  // On a page HamClock opened (not live.html), Back first returns to the clock.
  const onBack = useCallback(() => {
    const st = stateRef.current;
    const nav = webNavRef.current;
    if (editCancelRef.current) editCancelRef.current(); // Back while typing: cancel the edit only
    else if (showUpdateRef.current) updateLater(); // Back on the update notice: Later
    else if (st.settingsOpen && st.panelPage === 'settings') dispatch({type: 'PANEL_PAGE', page: 'main'});
    else if (!st.settingsOpen && nav.url && !isLivePage(nav.url)) {
      debugLog(`[HCWEB] back from ${nav.url}`);
      if (nav.canGoBack) webRef.current?.goBack();
      else dispatch({type: 'WEB_RELOAD'});
    } else toggleSettings();
  }, [toggleSettings, updateLater]);
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
  const webHasKeys = !state.settingsOpen && !firstRun && !showUpdate;

  // Memoized so the status polling (a re-render every few seconds) doesn't re-render the WebView;
  // each WebView render logs Amazon's "useComponentInstance is deprecated" warning. Handlers only
  // use dispatch, refs and onBack, so they stay valid between the listed deps.
  const webView = useMemo(
    () =>
      showWeb ? (
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
            else if (data.startsWith('tap:')) tapAt(data.slice(4));
            else debugLog(`[HCWEB] ${data}`);
          }}
          hasTVPreferredFocus={webHasKeys}
          javaScriptEnabled
          domStorageEnabled
          mediaPlaybackRequiresUserAction={false}
          onLoadStart={({nativeEvent: {url, canGoBack}}: WebViewNavigationEvent) => {
            webNavRef.current = {url, canGoBack};
          }}
          onLoad={({nativeEvent: {url, canGoBack}}: WebViewNavigationEvent) => {
            webNavRef.current = {url, canGoBack};
            dispatch({type: 'WEB_LOADED'});
            webRef.current?.injectJavaScript(overscanJs(overscanRef.current));
          }}
          // Errors on a page HamClock opened are that site's; Back returns to the clock.
          onError={({nativeEvent: {code, description, url}}: WebViewErrorEvent) => {
            if (!url || isLivePage(url)) dispatch({type: 'WEB_ERROR', message: `${code}: ${description}`});
          }}
          onHttpError={({nativeEvent: {statusCode, isMainFrame}}: WebViewHttpErrorEvent) => {
            if (isMainFrame && isLivePage(webNavRef.current.url)) dispatch({type: 'WEB_ERROR', message: `HTTP ${statusCode}`});
          }}
        />
      ) : null,
    [showWeb, state.webGeneration, liveUri, webHasKeys, injectedJs, onBack, tapAt],
  );

  return (
    <FieldContext.Provider value={fieldCtx}>
    <View style={styles.root}>
      {webView}
      {showStatus && (
        <View style={styles.center} pointerEvents="none">
          <StatusCard state={state} metrics={metrics} />
        </View>
      )}
      {firstRun && <FirstRunScreen state={state} metrics={metrics} dispatch={dispatch} />}
      {state.settingsOpen && (
        <SettingsPanel state={state} settings={settings} metrics={metrics} dispatch={dispatch} update={updateSettings} />
      )}
      {showUpdate && updateNotice && (
        <UpdateNotice
          version={updateNotice}
          canOpenStore={canOpenStore}
          metrics={metrics}
          onOpenStore={openStore}
          onLater={updateLater}
          onNever={updateNever}
        />
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

  updateText: {color: C.text, textAlign: 'center'},
  updateButtons: {flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center'},

  button: {backgroundColor: 'rgba(255,255,255,0.06)', borderWidth: 2, borderColor: 'transparent'},
  buttonSelected: {borderColor: C.accent},
  buttonFocused: {backgroundColor: C.accent, borderColor: '#fff'},
  buttonFocusedDanger: {backgroundColor: C.danger, borderColor: '#fff'},
  buttonText: {color: C.text, fontWeight: '600'},
  buttonTextFocused: {color: '#10131a'},
});
