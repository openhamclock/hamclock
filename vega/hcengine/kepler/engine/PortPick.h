/**
 * Port choice before each engine run, as in the Android app (MainActivity.kt
 * findAvailableRwPort/findAvailableRoPort/findAvailableRestPort): use the preferred port if it's
 * free, else the first free fallback from the same lists.
 *   - live R/W (live.html): 8083, 8084, 8086, 8091; none free -> keep the preferred port
 *   - live R/O:             8085, 8087, 8092, 8093; none free -> keep the preferred port
 *   - REST (antenna page):  8088, 8085, 8089, 8090; none free -> -1 (REST off)
 * Unlike Android, a port chosen for one server is never offered to another (Android's R/O and
 * REST lists share 8085).
 *
 * Android first kills leftover HamClock processes and rechecks after 250 ms. A Vega app can't, so
 * a busy preferred port is rechecked for graceSteps wait steps: during a relaunch the previous app
 * process may still hold the ports for a moment.
 *
 * Pure logic (isFree/waitStep are passed in) so the host tests can run it.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hcengine {

struct PortPlan {
  int rw = 0;    // live R/W port to use
  int ro = 0;    // live R/O port to use (-1 = off)
  int rest = 0;  // REST port to use (-1 = off)
  std::vector<std::string> events;  // one line per fallback, for the event log
  std::string notice;               // for the user ('' = all preferred ports were free)
};

/** Ports <= 0 are left as they are (disabled). */
PortPlan pickPorts(int rw, int ro, int rest, const std::function<bool(int)> &isFree,
                   const std::function<void()> &waitStep, int graceSteps);

}  // namespace hcengine

