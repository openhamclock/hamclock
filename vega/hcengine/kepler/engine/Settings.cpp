/**
 * Saved app settings: <dataDir>/vega-settings.json, one flat JSON object.
 *
 *   {"lanAccess":true,"mdnsName":"hamclock","backend":"ohb.hamclock.app:80","overscan":0}
 *
 * Written atomically (tmp + rename). Bad or missing fields fall back to their defaults, so a
 * damaged file never stops the engine. The Phase 3 file <dataDir>/vega-lan-access ("1"/"0") is
 * migrated into it once and then removed.
 */
#include "EngineHost.h"
#include "FlatJson.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace hcengine {

namespace {

std::string settingsPath(const std::string &dataDir) { return dataDir + "/vega-settings.json"; }
std::string oldLanPath(const std::string &dataDir) { return dataDir + "/vega-lan-access"; }

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) ++a;
  while (b > a && isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

bool readFile(const std::string &path, std::string &out) {
  FILE *f = fopen(path.c_str(), "r");
  if (!f) return false;
  char buf[1024];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 16384) out.append(buf, n);
  fclose(f);
  return true;
}

}  // namespace

std::string normalizeMdnsName(std::string &name) {
  std::string n = lower(trim(name));
  if (n.size() > 6 && n.compare(n.size() - 6, 6, ".local") == 0) n.resize(n.size() - 6);
  if (n.empty()) { name = kDefaultMdnsName; return ""; }
  if (n.size() > kMaxMdnsName) return "Name is too long (at most " + std::to_string(kMaxMdnsName) + " characters).";
  for (char c : n)
    if (!(isalnum((unsigned char)c) || c == '-')) return "Use only letters, digits and hyphens.";
  if (n.front() == '-' || n.back() == '-') return "The name can't start or end with a hyphen.";
  name = n;
  return "";
}

std::string normalizeBackend(std::string &backend) {
  std::string b = trim(backend);
  if (b.empty()) { backend = kDefaultBackend; return ""; }
  size_t colon = b.rfind(':');
  std::string host = lower(colon == std::string::npos ? b : b.substr(0, colon));
  std::string port = colon == std::string::npos ? "80" : b.substr(colon + 1);
  if (host.empty() || host.size() > 253) return "Enter a host name, for example ohb.hamclock.app:80.";
  for (char c : host)
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return "The host may use only letters, digits, dots and hyphens.";
  if (host.front() == '.' || host.front() == '-' || host.back() == '.' || host.back() == '-')
    return "That host name isn't valid.";
  if (port.empty() || port.size() > 5) return "The port must be a number from 1 to 65535.";
  for (char c : port)
    if (!isdigit((unsigned char)c)) return "The port must be a number from 1 to 65535.";
  int p = atoi(port.c_str());
  if (p < 1 || p > 65535) return "The port must be a number from 1 to 65535.";
  backend = host + ":" + std::to_string(p);
  return "";
}

bool validOverscan(int pct) { return pct == 0 || pct == 3 || pct == 5; }

std::string settingsJson(const VegaSettings &s) {
  return std::string("{\"lanAccess\":") + (s.lanAccess ? "true" : "false") +
         ",\"mdnsName\":" + jsonQuote(s.mdnsName) + ",\"backend\":" + jsonQuote(s.backend) +
         ",\"overscan\":" + std::to_string(s.overscan) + "}";
}

std::string mergeSettingsJson(const std::string &json, VegaSettings &s, bool strict) {
  VegaSettings n = s;
  std::string fieldErr;
  std::string err = parseFlatJson(json, [&](const std::string &key, const JsonValue &v) {
    std::string e;
    if (key == "lanAccess") {
      if (v.type == JsonValue::Bool) n.lanAccess = v.b; else e = "lanAccess must be true or false";
    } else if (key == "mdnsName") {
      std::string m = v.str;
      if (v.type != JsonValue::String) e = "mdnsName must be a string";
      else if ((e = normalizeMdnsName(m)).empty()) n.mdnsName = m;
    } else if (key == "backend") {
      std::string b = v.str;
      if (v.type != JsonValue::String) e = "backend must be a string";
      else if ((e = normalizeBackend(b)).empty()) n.backend = b;
    } else if (key == "overscan") {
      if (v.type == JsonValue::Number && validOverscan((int)v.num)) n.overscan = (int)v.num;
      else e = "overscan must be 0, 3 or 5";
    }
    if (!e.empty() && fieldErr.empty()) fieldErr = e;
  });
  if (!err.empty()) return "settings: " + err;
  if (strict && !fieldErr.empty()) return fieldErr;
  s = n;  // not strict: good fields are kept, bad ones stay as they were
  return "";
}

VegaSettings loadSettings(const std::string &dataDir, std::string &note) {
  VegaSettings s;
  note.clear();
  std::string text;
  if (readFile(settingsPath(dataDir), text)) {
    std::string err = mergeSettingsJson(text, s, false);
    if (!err.empty()) note = "ignored damaged vega-settings.json (" + err + ")";
    return s;
  }
  // Migrate the Phase 3 LAN file.
  if (readFile(oldLanPath(dataDir), text)) {
    s.lanAccess = !text.empty() && text[0] == '1';
    if (saveSettings(dataDir, s).empty()) {
      unlink(oldLanPath(dataDir).c_str());
      note = std::string("migrated vega-lan-access (") + (s.lanAccess ? "on" : "off") + ") to vega-settings.json";
    }
  }
  return s;
}

std::string saveSettings(const std::string &dataDir, const VegaSettings &s) {
  if (dataDir.empty()) return "no data dir";
  std::string path = settingsPath(dataDir), tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "w");
  if (!f) return std::string("can't save settings: ") + strerror(errno);
  std::string j = settingsJson(s) + "\n";
  bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
  ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    std::string e = std::string("can't save settings: ") + strerror(errno);
    unlink(tmp.c_str());
    return e;
  }
  return "";
}

}  // namespace hcengine
