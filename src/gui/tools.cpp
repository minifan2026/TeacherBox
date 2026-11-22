#include "tools.h"

#include <cctype>

#include "json.h"

namespace tb {

namespace {

void ReadCommandField(const Json& value, std::string& cmd, int& timeout) {
  if (value.IsString()) {
    cmd = value.str;
    return;
  }
  if (!value.IsObject()) return;
  cmd = value.GetString("command", value.GetString("cmd", ""));
  double t = value.GetNumber("timeout", 0);
  if (t > 0) timeout = (int)t;
}

} // namespace

bool IsValidToolId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  if (id[0] == '.') return false;
  for (char ch : id) {
    unsigned char c = (unsigned char)ch;
    if (std::isalnum(c)) continue;
    if (c == '.' || c == '_' || c == '-' || c == '+') continue;
    return false;
  }
  return true;
}

std::vector<ToolSpec> ScanTools(const std::wstring& toolsDir, std::vector<std::string>& warnings) {
  std::vector<ToolSpec> tools;
  if (!DirExists(toolsDir)) return tools;

  for (const std::wstring& dirName : ListDirs(toolsDir)) {
    std::string id = ToUtf8(dirName);
    if (!IsValidToolId(id)) {
      warnings.push_back("目录「" + id +
                         "」名称不合规：只允许字母、数字和 . _ - +（不能有中文、空格、斜杠），已忽略");
      continue;
    }

    std::wstring toolDir = JoinPath(toolsDir, dirName);
    std::string manifestText = ReadFileUtf8(JoinPath(toolDir, L"tool.json"));
    if (manifestText.empty()) {
      warnings.push_back("工具「" + id + "」缺少 tool.json，已忽略");
      continue;
    }

    std::string err;
    Json m = Json::Parse(manifestText, &err);
    if (!err.empty() || !m.IsObject()) {
      warnings.push_back("工具「" + id + "」的 tool.json 解析失败：" + (err.empty() ? "不是对象" : err));
      continue;
    }

    ToolSpec spec;
    spec.id = id;
    spec.dir = ToUtf8(toolDir);
    spec.name = m.GetString("name", id);
    spec.version = m.GetString("version", "");
    spec.description = m.GetString("description", m.GetString("desc", ""));

    if (const Json* check = m.Find("check")) ReadCommandField(*check, spec.checkCmd, spec.checkTimeout);
    if (const Json* install = m.Find("install"))
      ReadCommandField(*install, spec.installCmd, spec.installTimeout);

    // 语言别名，方便手写清单
    if (spec.checkCmd.empty()) spec.checkCmd = m.GetString("check_command", "");
    if (spec.installCmd.empty()) spec.installCmd = m.GetString("install_command", "");

    std::string entry = m.GetString("entry", "");
    if (!entry.empty()) spec.actions.push_back({"打开程序", entry});

    if (const Json* acts = m.Find("actions"); acts && acts->IsArray()) {
      for (const Json& a : acts->items) {
        ToolAction action;
        if (a.IsString()) {
          action.label = a.str;
          action.command = a.str;
        } else if (a.IsObject()) {
          action.label = a.GetString("label", a.GetString("name", "运行"));
          action.command = a.GetString("command", a.GetString("cmd", ""));
        }
        if (!action.command.empty()) spec.actions.push_back(action);
      }
    }

    std::string helpName = m.GetString("help", "help.md");
    std::wstring helpPath = JoinPath(toolDir, ToWide(helpName));
    spec.helpPath = ToUtf8(helpPath);
    spec.hasHelp = FileExists(helpPath);

    if (spec.checkCmd.empty() && spec.installCmd.empty() && spec.actions.empty() && !spec.hasHelp) {
      warnings.push_back("工具「" + id + "」没有定义任何指令，已忽略");
      continue;
    }

    tools.push_back(std::move(spec));
  }
  return tools;
}

RunResult RunToolCommand(const std::string& command, int timeoutSec, const std::wstring& workDir) {
  RunResult r;
  if (Trim(command).empty()) return r;
  std::wstring line = L"cmd.exe /d /s /c \"" + ToWide(command) + L"\"";
  DWORD timeout = timeoutSec > 0 ? (DWORD)timeoutSec * 1000u : 0u;
  return RunCommand(line, timeout, workDir);
}

} // namespace tb
