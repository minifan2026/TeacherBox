#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tb {

// ---------- 路径 / 编码 ----------
std::wstring ExeDir();
std::wstring ExePath();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring AppDataDir();               // %LOCALAPPDATA%\TeacherBox
std::wstring LogDir();                   // AppDataDir()\logs
std::wstring FeedbackDir();              // AppDataDir()\feedback
std::wstring DesktopDir();               // 当前用户的桌面目录（跟随系统重定向）
std::wstring TempDir();                  // %TEMP%

std::string  ToUtf8(const std::wstring& s);
std::wstring ToWide(const std::string& s);

std::string  ReadFileUtf8(const std::wstring& path, bool* ok = nullptr);
bool         WriteFileUtf8(const std::wstring& path, const std::string& data);
bool         EnsureDir(const std::wstring& path);
bool         FileExists(const std::wstring& path);
bool         DirExists(const std::wstring& path);
std::vector<std::wstring> ListDirs(const std::wstring& path);
bool         CopyTree(const std::wstring& src, const std::wstring& dst);
bool         DeleteTree(const std::wstring& path);

// ---------- 网络 ----------
// GitHub 直连时常抽风，raw.githubusercontent.com 与 cdn.jsdelivr.net 互相兜底
bool HttpGetText(const std::wstring& url, std::string& out, bool* networkError = nullptr);
bool HttpGetFile(const std::wstring& url, const std::wstring& dstPath);   // 二进制按字节落盘

// ---------- 时间 ----------
std::string  NowDateTime();        // 2026-11-22 13:04:05
std::string  NowStampForFile();    // 20261122-130405

// ---------- 日志 ----------
// 写入 AppDataDir()\logs\<tag>-YYYY-MM-DD.log（UTF-8，追加）
void Log(const std::wstring& tag, const std::string& message);
std::string TailLog(const std::wstring& tag, size_t maxBytes = 4096);

// ---------- 进程 ----------
struct RunResult {
  int  exitCode = -1;
  bool timedOut = false;
  bool started  = false;
  std::string output;   // stdout + stderr 合并
};

// 隐藏窗口执行命令行（通过管道捕获输出）。timeoutMs = 0 表示不限时。
RunResult RunCommand(const std::wstring& commandLine, DWORD timeoutMs,
                     const std::wstring& workDir = L"");

// 启动后不管（不等待），用于“工具唤起某个 exe”
bool LaunchDetached(const std::wstring& commandLine, const std::wstring& workDir = L"");

// ---------- 杂项 ----------
std::string Trim(const std::string& s);
std::string GetSystemSummary();
HICON       MakeAppIcon(int size);
void        OpenFolder(const std::wstring& path);

} // namespace tb
