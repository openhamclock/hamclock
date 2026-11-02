#pragma once
/**
 * HTTPS for the engine through the app (Phase 10). Vega apps can't run curl, so HamClock's
 * WiFiClient::connectCommand calls vega_connect_http(cmd, &fd) instead (ESPHamClock/ArduinoLib):
 * the curl command is parsed here, the request is queued, and HamClock gets the read end of a
 * pipe. The app takes queued requests (HcEngine.takeFetches), fetches them with RN fetch(), and
 * hands the body back (HcEngine.fetchDone), which is written to the pipe; HamClock reads it like
 * curl's stdout. Same design as Android's android_connect_http (JNI). Plain C++ (host-tested).
 */
#include <string>

namespace hcengine {

/** What HamClock's curl commands carry: URL, -A (User-Agent) and one -H header. */
struct CurlRequest {
  std::string url;
  std::string userAgent;
  std::string header;  // "Name: value", or ""
};

/** Parse a curl command line; false if there is no http(s) URL. Other flags are ignored. */
bool parseCurlCommand(const std::string &cmd, CurlRequest &out);

namespace fetch {

/** No answer from the app after this long: the pipe is closed empty (curl's 15 s + slack). */
constexpr int kTimeoutMs = 20000;
/** Bodies are cut at this size. */
constexpr size_t kMaxBody = 1 << 20;

/** Queue a request; on success *readFd is the pipe's read end (the caller closes it). */
bool open(const CurlRequest &req, int *readFd);
/** New requests as a JSON array ([{"id","url","userAgent","header"}]), or "" if none. */
std::string take();
/**
 * The app's answer. ok: the body is written to the pipe (on a writer thread), then it's
 * closed; !ok: closed empty (a failed fetch, as when curl fails). Unknown ids are ignored.
 */
void done(int id, bool ok, const std::string &body);
/** Close every pending request (engine stop/restart). */
void closeAll();
/** Pending requests (queued or in flight); for tests and status. */
int pending();
/** Close requests past their deadline. Also runs from open/take/done and a reaper thread. */
void sweep();
/** Test hook: timeout in ms for requests opened from now on (0 = kTimeoutMs). */
void setTimeoutForTest(int ms);

}  // namespace fetch
}  // namespace hcengine

/** HamClock's hook (WiFiClient::connectCommand under _IS_VEGA): parse cmd and open(). */
extern "C" bool vega_connect_http(const char *cmd, int *read_fd);
