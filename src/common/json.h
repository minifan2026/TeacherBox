#pragma once

#include <string>
#include <vector>

namespace tb {

// 极简 JSON（不依赖第三方库）。只做本工程需要的部分：
// 解析对象/数组/字符串/数字/布尔/null，键名取字符串。
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object };

  Type        type = Null;
  bool        boolean = false;
  double      number = 0.0;
  std::string str;
  std::vector<Json> items;                              // Array
  std::vector<std::pair<std::string, Json>> members;    // Object

  bool        IsNull() const { return type == Null; }
  bool        IsObject() const { return type == Object; }
  bool        IsArray() const { return type == Array; }
  bool        IsString() const { return type == String; }

  const Json* Find(const std::string& key) const;
  std::string GetString(const std::string& key, const std::string& def = "") const;
  bool        GetBool(const std::string& key, bool def = false) const;
  double      GetNumber(const std::string& key, double def = 0.0) const;

  static Json Parse(const std::string& text, std::string* error = nullptr);
  static std::string Quote(const std::string& s);
};

} // namespace tb
