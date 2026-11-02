/**
 * Tiny reader for flat JSON objects (EngineOptions, vega-settings.json): string, number, bool,
 * null and arrays of strings as values; no nested objects. Enough for the host's own formats.
 */
#pragma once

#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace hcengine {

struct JsonValue {
  enum Type { Null, Bool, Number, String, StringArray } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<std::string> arr;
};

namespace flatjson_detail {

struct Reader {
  const std::string &s;
  size_t i = 0;
  std::string err;
  explicit Reader(const std::string &str) : s(str) {}
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
  bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
  bool fail(const char *m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
  bool str(std::string &out) {
    ws();
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'u': i += 4; out += '?'; break;  // not needed for our formats
          default: out += e;
        }
      } else out += c;
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
};

}  // namespace flatjson_detail

/** s as a quoted JSON string. */
inline std::string jsonQuote(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { static const char *hx = "0123456789abcdef"; o += "\\u00"; o += hx[c >> 4]; o += hx[c & 15]; }
        else o += (char)c;
    }
  }
  return o + "\"";
}

/** Calls fn(key, value) for each member of a flat object. Returns '' or an error. '' input is {}. */
inline std::string parseFlatJson(const std::string &json,
                                 const std::function<void(const std::string &, const JsonValue &)> &fn) {
  if (json.empty()) return "";
  flatjson_detail::Reader j(json);
  if (!j.eat('{')) return "expected object";
  if (j.eat('}')) return "";
  do {
    std::string key;
    if (!j.str(key) || !j.eat(':')) return j.err.empty() ? "expected ':'" : j.err;
    j.ws();
    JsonValue v;
    char c = j.i < json.size() ? json[j.i] : 0;
    if (c == '"') {
      if (!j.str(v.str)) return j.err;
      v.type = JsonValue::String;
    } else if (c == '[') {
      ++j.i;
      if (!j.eat(']')) {
        do {
          std::string e;
          if (!j.str(e)) return j.err;
          v.arr.push_back(e);
        } while (j.eat(','));
        if (!j.eat(']')) return "expected ']'";
      }
      v.type = JsonValue::StringArray;
    } else if (json.compare(j.i, 4, "true") == 0 || json.compare(j.i, 5, "false") == 0) {
      v.b = json[j.i] == 't';
      j.i += v.b ? 4 : 5;
      v.type = JsonValue::Bool;
    } else if (json.compare(j.i, 4, "null") == 0) {
      j.i += 4;
    } else {
      char *end = nullptr;
      v.num = strtod(json.c_str() + j.i, &end);
      if (end == json.c_str() + j.i) return "bad value for " + key;
      j.i = end - json.c_str();
      v.type = JsonValue::Number;
    }
    fn(key, v);
  } while (j.eat(','));
  if (!j.eat('}')) return "expected '}'";
  return "";
}

}  // namespace hcengine
