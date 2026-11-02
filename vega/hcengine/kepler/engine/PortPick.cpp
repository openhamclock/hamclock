/**
 * Port choice before each engine run. See PortPick.h.
 */
#include "PortPick.h"

#include <set>

namespace hcengine {

namespace {
const std::vector<int> kRwFallbacks = {8083, 8084, 8086, 8091};
const std::vector<int> kRoFallbacks = {8085, 8087, 8092, 8093};
const std::vector<int> kRestFallbacks = {8088, 8085, 8089, 8090};
}  // namespace

PortPlan pickPorts(int rw, int ro, int rest, const std::function<bool(int)> &isFree,
                   const std::function<void()> &waitStep, int graceSteps) {
  auto busy = [&](int p) { return p > 0 && !isFree(p); };
  for (int i = 0; i < graceSteps && (busy(rw) || busy(ro) || busy(rest)); ++i) waitStep();

  std::set<int> taken;
  for (int p : {rw, ro, rest})
    if (p > 0) taken.insert(p);
  // Returns the port to use; `none` when neither the preferred port nor a fallback is free.
  auto pick = [&](int pref, const std::vector<int> &fallbacks, int none) {
    if (pref <= 0 || isFree(pref)) return pref;
    for (int p : fallbacks) {
      if (taken.count(p) || !isFree(p)) continue;
      taken.insert(p);
      return p;
    }
    return none;
  };

  PortPlan plan;
  plan.rw = pick(rw, kRwFallbacks, rw);
  plan.ro = pick(ro, kRoFallbacks, ro);
  plan.rest = pick(rest, kRestFallbacks, -1);

  auto note = [&](const std::string &s) { plan.notice += (plan.notice.empty() ? "" : " ") + s; };
  auto busyMsg = [](int p) { return "Port " + std::to_string(p) + " is in use by another app"; };
  if (rw > 0 && plan.rw != rw) {
    plan.events.push_back("live R/W port " + std::to_string(rw) + " in use; using " + std::to_string(plan.rw));
    note(busyMsg(rw) + ", so the live clock uses port " + std::to_string(plan.rw) + ".");
  } else if (rw > 0 && busy(rw)) {
    plan.events.push_back("live R/W port " + std::to_string(rw) + " and its fallbacks in use");
    note(busyMsg(rw) + " and no other port is free, so HamClock can't start.");
  }
  if (ro > 0 && plan.ro != ro)
    plan.events.push_back("live R/O port " + std::to_string(ro) + " in use; using " + std::to_string(plan.ro));
  if (rest > 0 && plan.rest != rest) {
    if (plan.rest > 0) {
      plan.events.push_back("REST port " + std::to_string(rest) + " in use; using " + std::to_string(plan.rest));
      note(busyMsg(rest) + ", so the REST server (antenna page) uses port " + std::to_string(plan.rest) + ".");
    } else {
      plan.events.push_back("REST port " + std::to_string(rest) + " and its fallbacks in use; REST off");
      note(busyMsg(rest) + " and no other port is free, so the REST server (antenna page) is off.");
    }
  }
  return plan;
}

}  // namespace hcengine

