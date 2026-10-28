/**
 * Per-install virtual MAC address, which HamClock turns into its serial number (ESP::getChipId,
 * sent as "id <S/N>" in the User-Agent).
 *
 * A Vega app can't read the TV's MAC, and HamClock's own lookup shells out to /sbin/ip or
 * /sbin/ifconfig, which the TV doesn't have. So, as the Android app does (hamclock_jni.cpp), we
 * wrap WiFi::macAddress() with the linker (--wrap=_ZN4WiFi10macAddressEv, see CMakeLists.txt):
 *   - read <dataDir>/.mac_address (same file name as Android);
 *   - if it's missing or invalid, generate a random locally administered unicast MAC
 *     (02:xx:xx:xx:xx:xx), save it, and use it from then on.
 * /data survives app updates and is wiped on uninstall, so the S/N lasts for the install.
 * ESP.cpp skips the /proc/cpuinfo serial on Vega, so the S/N never switches to a CPU serial.
 *
 * Built in the hamclock_engine object library for the engine include path (our_dir).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "ArduinoLib.h"

namespace {

std::mutex mu;
std::string cached;  // valid MAC once known

bool validMac(const char *s) {
  unsigned m[6];
  char extra;
  return sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &extra) == 6;
}

std::string readMac(const std::string &path) {
  char buf[64] = {0};
  FILE *f = fopen(path.c_str(), "r");
  if (!f) return "";
  bool ok = fgets(buf, sizeof buf, f) != nullptr;
  fclose(f);
  if (!ok) return "";
  buf[strcspn(buf, "\r\n")] = '\0';
  return validMac(buf) ? std::string(buf) : "";
}

std::string newMac() {
  uint8_t b[6] = {0};
  FILE *f = fopen("/dev/urandom", "re");
  if (f) {
    if (fread(b, 1, sizeof b, f) != sizeof b) memset(b, 0, sizeof b);
    fclose(f);
  }
  b[0] = (uint8_t)((b[0] & 0xFE) | 0x02);  // locally administered, unicast
  char s[32];
  snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return s;
}

bool saveMac(const std::string &path, const std::string &mac) {
  std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "w");
  if (!f) return false;
  bool ok = fprintf(f, "%s\n", mac.c_str()) > 0;
  ok = (fclose(f) == 0) && ok;
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
/** Replaces WiFi::macAddress() for every caller in the engine (linker --wrap). */
extern "C" std::string __wrap__ZN4WiFi10macAddressEv() {
#pragma clang diagnostic pop
  std::lock_guard<std::mutex> l(mu);
  if (!cached.empty()) return cached;
  if (our_dir.empty()) return "FF:FF:FF:FF:FF:FF";  // before -d is known; don't cache
  std::string path = our_dir + ".mac_address";      // our_dir ends with '/'
  std::string mac = readMac(path);
  if (mac.empty()) {
    mac = newMac();
    if (!saveMac(path, mac)) {
      fprintf(stderr, "HcEngine: can't save %s; S/N may change next run\n", path.c_str());
    } else {
      printf("Generated virtual MAC %s\n", mac.c_str());
    }
  }
  cached = mac;
  return cached;
}
