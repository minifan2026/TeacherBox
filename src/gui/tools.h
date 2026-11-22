#pragma once

#include <string>
#include <vector>

#include "util.h"

namespace tb {

struct ToolAction {
  std::string label;
  std::string command;
};

struct ToolSpec {
  std::string id;           // 目录名 = 工具编号
  std::string dir;          // 工具目录的 UTF-8 路径
  std::string name;         // 显示名，可以用中文
  std::string version;
  std::string description;

  std::string checkCmd;     // 环境检测指令
  int         checkTimeout = 20;     // 秒
  std::string installCmd;   // 检测失败后执行的安装指令
  int         installTimeout = 0;    // 秒，0 表示使用内部上限

  std::vector<ToolAction> actions;   // 额外动作（例如打开某个 exe）

  std::string helpPath;     // help.md 的绝对路径（UTF-8）
  bool        hasHelp = false;
};

// 目录名只允许 A-Z a-z 0-9 . _ - +；不允许中文、空格、斜杠
bool IsValidToolId(const std::string& id);

// 扫描 tools 目录；不合规或损坏的工具记录到 warnings
std::vector<ToolSpec> ScanTools(const std::wstring& toolsDir, std::vector<std::string>& warnings);

// 用 cmd.exe 执行命令（where / && / 重定向 都能用）
RunResult RunToolCommand(const std::string& command, int timeoutSec, const std::wstring& workDir);

} // namespace tb
