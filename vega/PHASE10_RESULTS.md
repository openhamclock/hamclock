# Phase 10 Results: curl panes through the app's fetch

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`, Vega OS 2.0. Vega SDK 2.0.13986. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict (in particular PHASE6_RESULTS.md 6.5).

## Problem

Sat_Alerts (hams.at) and Mesh_Mon fetch over HTTPS by running `curl` through `WiFiClient::connectCommand`. Vega apps can't run subprocesses (PHASE6_RESULTS.md 6.5), so on Vega `connectCommand` returned false and both panes failed.

- The TV has `/usr/bin/curl`, `libcurl.so.4.8.0`, OpenSSL 3 (`libssl.so.3`), mbedTLS 2.28 and nghttp2, but the app can't use them. The SDK's armv7 sysroot has only libc++ (no curl or TLS headers or libraries to link), and the sandbox can't run the binaries. The SDK's OpenSSL is an x86_64 build for host tools.
- `dlopen` of the system libcurl was ruled out: an undocumented private dependency that could break with an OS update or fail Appstore review.
- Vega apps reach HTTPS through RN `fetch()`, which uses the OS network stack and Amazon's CA store.

**Decision (option A):** the engine hands the request to the app, the app fetches it, and the body comes back through a pipe that HamClock reads like curl's stdout. This is the same design as Android's JNI fetch pipe (`android_connect_http`). Option B, bundling mbedTLS and a CA bundle in the engine, was not chosen: we'd own TLS updates and the CA bundle.

## Spike (Debug build, RN `fetch` on the TV; code not kept)

| Request | Result |
|:--|:--|
| `https://hams.at/api/alerts/upcoming` | 200, `{"data":[]}`, 388 ms first, ~90 ms after |
| same with `Authorization: Bearer <invalid>` | 200, same body (header accepted) |
| `https://meshtastic.liamcottle.net/api/v1/nodes/1` | 200, 881 B JSON, 548 / 275 ms |
| `…/nodes/1/neighbours` | 500 with a JSON body (server side; the body came through) |
| `https://httpbin.org/headers` | `User-Agent` arrived as set; `Accept-Encoding: deflate, gzip` (fetch decompresses) |
| 3 in parallel | 280 ms |

No manifest change is needed for HTTPS. `[network-traffic-policy.cleartext]` only covers plain HTTP.

## What changed

### `ESPHamClock/ArduinoLib/WiFiClient.cpp` (the only upstream change)
- Under `_IS_VEGA`, `connectCommand` calls `extern "C" bool vega_connect_http(const char *cmd, int *read_fd)` and uses the fd as its "socket" with `m_isPipe`, exactly as the Android branch does.
- The two fd-close blocks (on reconnect and in `stop()`) are now `_IS_ANDROID || _IS_VEGA`.
- Android's code and the callers (`hamsat.cpp`, `meshtastic.cpp`, `wifi.cpp`) are unchanged. Reads already use `::read`, so a pipe works.

### Engine: `vega/hcengine/kepler/engine/HttpFetch.{h,cpp}` (plain C++, host-tested)
- `parseCurlCommand`: the URL (`https://` or `http://`), `-A` (User-Agent) and one `-H` header, each `"quoted"`, `'quoted'` or bare. The flag must be a whole word. Other flags (`--max-time`, `--silent`) are ignored.
- `fetch::open`: makes a pipe (CLOEXEC; non-blocking write end), queues `{id, url, userAgent, header}` with a 20 s deadline, and returns the read end.
- `fetch::take`: new requests as a JSON array, each returned once.
- `fetch::done`:
  - ok: the body (cut at 1 MB) is written on a detached writer thread, so the JS thread never blocks. The writes are non-blocking with bounded `poll` waits, and SIGPIPE is blocked in that thread only, so a reader that went away can't signal the app.
  - !ok, or an empty body: the pipe is closed empty, which HamClock sees as a failed fetch, as when curl fails.
- Expired requests are closed empty by a 1 s reaper thread, and also on every open, take and done.
- `fetch::closeAll`: runs in the supervisor after each run ends (stop or restart), after `retireEngineSockets()`.
- The link-time wraps (`close`, `write`, `pthread_create`) pass pipe fds and these threads straight through.

### TurboModule
- `takeFetches(): string` and `fetchDone(id: number, ok: boolean, body: string)`. These were added to `src/turbo-modules/HcEngine.ts` and to the generated spec files by hand, in the same form codegen produces.
- Typed wrapper (`hcengine/src/index.ts`): `HcEngine.takeFetches(): FetchRequest[]` and `HcEngine.fetchDone(id, body | null)`.

### App: `src/engine/fetchBridge.ts`, started in `index.js`
- A 250 ms loop calls `takeFetches()`. For each request it runs `fetch(url, {headers})` with the User-Agent and the one header, and a 15 s `AbortController` timeout (curl's `--max-time 15`).
- Like `curl --silent` without `-f`, the body is passed back for any HTTP status. A network error or timeout is passed back as a failure.
- Only `http:` and `https:` URLs are fetched; anything else is refused without fetching.
- The bridge is independent of the React tree. Debug builds log `[HCFETCH] <id>: HTTP <status> <bytes> B <ms> ms <url>`; failures always go to `console.warn`.

## Verification

| Check | Result |
|:--|:--|
| tsc, lint | ✅ |
| Jest 30/30 (new `test/fetchBridge.test.ts`: headers, body on 200 and 500, network error, refused scheme, 15 s abort, each request once, failing native call) | ✅ |
| Host C++ 151 checks, 0 failed (was 108): curl parsing; body; 300 KB body through the pipe; failure → empty; unknown id; reader closed before the body (no SIGPIPE); 200 ms test deadline → empty; `closeAll` | ✅ |
| Release build armv7, no compiler warnings; links with `--no-undefined` (so `vega_connect_http` resolves) | ✅ |
| TV: Sat_Alerts pane draws the hams.at list (empty right now, matching `{"data":[]}`), no "https://hams.at download error" | ✅ (REST `get_capture.bmp`) |
| TV: Mesh_Mon with a watch list set in Setup shows the watched node(s) | ✅ (user) |

## Notes

- For the test, Pane2 = Sat_Alerts and Pane3 = Mesh_Mon were set over REST (`set_pane?Pane2=Sat_Alerts`).
- HamClock's version check (`connecthttpsHCGET`, `wifi.cpp`) also goes through this path now. With `NO_UPGRADE` on Vega it can only report a newer version; updates still come from the Appstore.
- In the background, JS timers may be throttled, so fetches can wait or time out. The panes then retry as they do after a failed curl. Not observed as a problem.
- The TV now has the Phase 10 Release build (local build number 1201). The LAT copy still needs reinstalling from the Appstore (PHASE9_RESULTS.md).

