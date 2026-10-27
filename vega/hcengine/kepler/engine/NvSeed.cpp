/**
 * First-run seeding through HamClock's own NV API.
 *
 * Built as part of the hamclock_engine object library (engine include paths and defines), because
 * it needs HamClock.h. Called on the engine thread just before hamclock_main(), so a fatalError()
 * in the NV code ends the engine thread like any other engine exit().
 *
 * Why: with no stored callsign, HamClock forces Setup even with -k, and the RESTful server only
 * starts after Setup completes. Seeding lets the app's own first-run screen (or a test) provide
 * the callsign and location instead.
 */
#include <cmath>
#include <cstring>
#include <string>

#include "HamClock.h"

namespace hcengine {

/** Store call (and lat/lng if both are finite) only when no callsign is stored yet.
 *  Returns a description of what was written, or '' if nothing was. */
std::string seedFirstRun(const std::string &dataDir, const std::string &call, double lat, double lng) {
  if (call.empty()) return "";

  // Same value hamclock_main derives from -d; the NV code needs it to find the eeprom file.
  our_dir = dataDir;
  if (our_dir.empty() || our_dir.back() != '/') our_dir += '/';

  char cur[NV_CALLSIGN_LEN] = {0};
  if (NVReadString(NV_CALLSIGN, cur) && cur[0] != '\0') return "";

  char buf[NV_CALLSIGN_LEN] = {0};
  strncpy(buf, call.c_str(), NV_CALLSIGN_LEN - 1);
  NVWriteString(NV_CALLSIGN, buf);
  std::string what = std::string("seeded callsign ") + buf;

  if (std::isfinite(lat) && std::isfinite(lng)) {
    LatLong ll;
    ll.lat_d = (float)lat;
    ll.lng_d = (float)lng;
    ll.normalize();
    NVWriteFloat(NV_DE_LAT, ll.lat_d);
    NVWriteFloat(NV_DE_LNG, ll.lng_d);
    setNVMaidenhead(NV_DE_GRID, ll);
    char grid[MAID_CHARLEN] = {0};
    getNVMaidenhead(NV_DE_GRID, grid);
    what += std::string(" at ") + grid;
  }
  return what;
}

}  // namespace hcengine
