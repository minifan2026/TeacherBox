#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace tb {

const Json* Json::Find(const std::string& key) const {
  if (type != Object) return nullptr;
  for (const auto& kv : members)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string Json::GetString(const std::string& key, const std::string& def) const {
  const Json* v = Find(key);
  if (!v) return def;
  if (v->type == String) return v->str;
  if (v->type == Number) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v->number);
    return buf;
  }
  return def;
}

bool Json::GetBool(const std::string& key, bool def) const {
  const Json* v = Find(key);
  if (!v) return def;
  if (v->type == Bool) return v->boolean;
  if (v->type == Number) return v->number != 0;
  return def;
}

double Json::GetNumber(const std::string& key, double def) const {
  const Json* v = Find(key);
  return (v && v->type == Number) ? v->number : def;
}

std::string Json::Quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out.push_back('"');
  return out;
}

namespace {

struct Parser {
  const std::string& s;
  size_t i = 0;
  std::string err;

  explicit Parser(const std::string& text) : s(text) {}

  void SkipWs() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
      else break;
    }
  }

  bool Fail(const char* msg) {
    if (err.empty()) err = msg;
    return false;
  }

  bool ParseValue(Json& out) {
    SkipWs();
    if (i >= s.size()) return Fail("unexpected end");
    char c = s[i];
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == '"') {
      out.type = Json::String;
      return ParseString(out.str);
    }
    if (c == 't') {
      if (s.compare(i, 4, "true") == 0) { i += 4; out.type = Json::Bool; out.boolean = true; return true; }
      return Fail("bad literal");
    }
    if (c == 'f') {
      if (s.compare(i, 5, "false") == 0) { i += 5; out.type = Json::Bool; out.boolean = false; return true; }
      return Fail("bad literal");
    }
    if (c == 'n') {
      if (s.compare(i, 4, "null") == 0) { i += 4; out.type = Json::Null; return true; }
      return Fail("bad literal");
    }
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
    return Fail("unexpected character");
  }

  bool ParseObject(Json& out) {
    out.type = Json::Object;
    ++i; // {
    SkipWs();
    if (i < s.size() && s[i] == '}') { ++i; return true; }
    for (;;) {
      SkipWs();
      std::string key;
      if (i >= s.size() || s[i] != '"') return Fail("expected key");
      if (!ParseString(key)) return false;
      SkipWs();
      if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
      ++i;
      Json value;
      if (!ParseValue(value)) return false;
      out.members.emplace_back(key, std::move(value));
      SkipWs();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Json& out) {
    out.type = Json::Array;
    ++i; // [
    SkipWs();
    if (i < s.size() && s[i] == ']') { ++i; return true; }
    for (;;) {
      Json value;
      if (!ParseValue(value)) return false;
      out.items.emplace_back(std::move(value));
      SkipWs();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseString(std::string& out) {
    ++i; // "
    out.clear();
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i >= s.size()) return Fail("bad escape");
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (i + 4 > s.size()) return Fail("bad \\u escape");
            unsigned code = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (code >= 0xD800 && code <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' &&
                s[i + 1] == 'u') {
              unsigned low = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
              i += 6;
              unsigned cp = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
              out.push_back((char)(0xF0 | (cp >> 18)));
              out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
              out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back((char)(0x80 | (cp & 0x3F)));
            } else if (code < 0x80) {
              out.push_back((char)code);
            } else if (code < 0x800) {
              out.push_back((char)(0xC0 | (code >> 6)));
              out.push_back((char)(0x80 | (code & 0x3F)));
            } else {
              out.push_back((char)(0xE0 | (code >> 12)));
              out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
              out.push_back((char)(0x80 | (code & 0x3F)));
            }
            break;
          }
          default: return Fail("bad escape");
        }
      } else {
        out.push_back(c);
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(Json& out) {
    size_t start = i;
    if (s[i] == '-') ++i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-'))
      ++i;
    out.type = Json::Number;
    out.number = atof(s.substr(start, i - start).c_str());
    return true;
  }
};

} // namespace

Json Json::Parse(const std::string& text, std::string* error) {
  Parser p(text);
  Json root;
  if (!p.ParseValue(root)) {
    if (error) *error = p.err.empty() ? "parse error" : p.err;
    return Json();
  }
  if (error) error->clear();
  return root;
}

} // namespace tb
