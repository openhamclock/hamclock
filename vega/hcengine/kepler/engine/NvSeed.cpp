/**
 * First-run check (and a test-only seed) through HamClock's own NV API.
 *
 * Built as part of the hamclock_engine object library (engine include paths and defines), because
 * it needs HamClock.h. Called on the engine thread just before hamclock_main(), so a fatalError()
 * in the NV code ends the engine thread like any other engine exit(), and NV is never touched
 * while the engine runs.
 *
 * Why: with no stored callsign, HamClock forces Setup even with -k, and the RESTful server only
 * starts after Setup completes. The app shows its first-run screen in that case. The app never
 * writes the callsign; the user enters it in HamClock's Setup. testSeedCallsign() exists only so
 * the self-test can reach REST on a fresh data dir.
 */
#include <cstring>
#include <string>

#include "HamClock.h"

namespace hcengine {

namespace {

void useDataDir(const std::string &dataDir) {
  // Same value hamclock_main derives from -d; the NV code needs it to find the eeprom file.
  our_dir = dataDir;
  if (our_dir.empty() || our_dir.back() != '/') our_dir += '/';
}

}  // namespace

bool hasStoredCallsign(const std::string &dataDir) {
  useDataDir(dataDir);
  char cur[NV_CALLSIGN_LEN] = {0};
  return NVReadString(NV_CALLSIGN, cur) && cur[0] != '\0';
}

std::string testSeedCallsign(const std::string &dataDir, const std::string &call) {
  if (call.empty() || hasStoredCallsign(dataDir)) return "";
  char buf[NV_CALLSIGN_LEN] = {0};
  strncpy(buf, call.c_str(), NV_CALLSIGN_LEN - 1);
  NVWriteString(NV_CALLSIGN, buf);
  return std::string("test seed: callsign ") + buf;
}

}  // namespace hcengine
