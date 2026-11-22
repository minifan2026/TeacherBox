// TeacherBox 主调度器（GUI 子系统，无控制台）
//
// 职责：
//   1. 启动 update.exe 完成升级（等待其退出）
//   2. 启动 gui.exe 并常驻后台守护
//   3. gui.exe 异常退出时询问是否生成反馈文件，然后自动重启
//   4. 用户正常关闭 gui.exe 时，主程序一并退出，不重启
//   5. 连续 10 次异常退出 -> 判定应用致命失败，提示后退出
//
// 与子进程之间使用命名管道通信（见 ipc.h 的文本协议）。

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "ipc.h"
#include "json.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* kAppTitle   = L"TeacherBox 工具箱";
const char*    kVersion    = "1.0.0";
const int      kMaxFailures = 10;
const DWORD    kStableRunMs = 30000;   // 稳定运行超过 30s 后崩溃，视为新的一轮失败
const DWORD    kReadyTimeoutMs = 25000;
const DWORD    kUpdateMaxMs = 180000;

std::wstring MakePipeName(const wchar_t* role) {
  wchar_t buf[256];
  swprintf(buf, 256, L"\\\\.\\pipe\\teacherbox-%ls-%lu", role, (unsigned long)GetCurrentProcessId());
  return buf;
}

bool SpawnChild(const std::wstring& exePath, const std::wstring& args, PROCESS_INFORMATION& pi) {
  std::wstring cmd = L"\"" + exePath + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  // 注意：不要设置 STARTF_USESHOWWINDOW / SW_HIDE，
  // 否则子进程第一次 ShowWindow 会忽略自己的参数，界面永远藏在后台。
  si.dwFlags = 0;
  si.wShowWindow = SW_SHOWNORMAL;

  ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, tb::ExeDir().c_str(), &si, &pi);
  return ok != FALSE;
}

struct ChildOutcome {
  int         exitCode = -1;
  bool        started  = false;
  bool        ready    = false;
  bool        graceful = false;
  bool        timedOut = false;
  ULONGLONG   uptimeMs = 0;
  std::string tail;                  // 管道里收到的最后若干行
};

ChildOutcome RunChild(const std::wstring& role, const std::wstring& exePath,
                      const std::wstring& extraArgs, DWORD readyTimeoutMs, DWORD maxRunMs) {
  ChildOutcome outcome;
  tb::IpcChannel ipc;
  std::wstring pipeName = MakePipeName(role.c_str());

  if (!ipc.Listen(pipeName)) {
    tb::Log(L"main", "[ipc] CreateNamedPipe 失败: " + tb::ToUtf8(pipeName));
  }
  ipc.StartReader();

  std::wstring args = L"--ipc=\"" + pipeName + L"\" --parent=" +
                      std::to_wstring(GetCurrentProcessId());
  if (!extraArgs.empty()) args += L" " + extraArgs;

  PROCESS_INFORMATION pi{};
  if (!SpawnChild(exePath, args, pi)) {
    tb::Log(L"main", "[spawn] 无法启动 " + tb::ToUtf8(exePath) + "，错误码 " +
                         std::to_string(GetLastError()));
    ipc.Close();
    return outcome;
  }
  outcome.started = true;
  tb::Log(L"main", "[spawn] 已启动 " + tb::ToUtf8(exePath) + " pid=" + std::to_string(pi.dwProcessId));

  const ULONGLONG start = GetTickCount64();
  std::string tail;
  bool exited = false;
  DWORD exitCode = 0;

  for (;;) {
    for (const std::string& line : ipc.TakeLines()) {
      if (line == "READY") outcome.ready = true;
      if (line == "EXIT_USER") outcome.graceful = true;
      if (line.rfind("STATUS ", 0) == 0 || line.rfind("LOG ", 0) == 0) {
        tb::Log(L"main", "[" + tb::ToUtf8(role) + "] " + line);
      }
      tail += line;
      tail += "\n";
      if (tail.size() > 20000) tail.erase(0, tail.size() - 20000);
    }

    if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) {
      exited = true;
      GetExitCodeProcess(pi.hProcess, &exitCode);
      break;
    }

    const ULONGLONG elapsed = GetTickCount64() - start;
    if (!outcome.ready && readyTimeoutMs && elapsed > readyTimeoutMs) {
      outcome.timedOut = true;
      tb::Log(L"main", "[" + tb::ToUtf8(role) + "] 启动超时，强制结束进程");
      TerminateProcess(pi.hProcess, 125);
      WaitForSingleObject(pi.hProcess, 3000);
      GetExitCodeProcess(pi.hProcess, &exitCode);
      break;
    }
    if (maxRunMs && elapsed > maxRunMs) {
      outcome.timedOut = true;
      tb::Log(L"main", "[" + tb::ToUtf8(role) + "] 运行超时，强制结束进程");
      TerminateProcess(pi.hProcess, 126);
      WaitForSingleObject(pi.hProcess, 3000);
      GetExitCodeProcess(pi.hProcess, &exitCode);
      break;
    }
  }

  for (const std::string& line : ipc.TakeLines()) {
    if (line == "READY") outcome.ready = true;
    if (line == "EXIT_USER") outcome.graceful = true;
    tail += line + "\n";
  }

  outcome.uptimeMs = GetTickCount64() - start;
  outcome.exitCode = exited ? (int)exitCode : -1;
  outcome.tail = tail;

  ipc.Close();
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  tb::Log(L"main", "[" + tb::ToUtf8(role) + "] 退出，code=" + std::to_string(outcome.exitCode) +
                       " 运行 " + std::to_string(outcome.uptimeMs) + "ms" +
                       (outcome.graceful ? "（用户主动关闭）" : ""));
  return outcome;
}

// ------------------------------------------------------------------ 反馈

int AskFeedback(int exitCode) {
  wchar_t text[1024];
  swprintf(text, 1024,
           L"TeacherBox 界面进程异常退出（退出码 %d）。\n\n"
           L"是否生成错误反馈并提交给开发者？\n"
           L"选择“是”将生成一份错误报告文件，并打开所在文件夹，由你手动发送给开发者。\n"
           L"选择“否”将直接重启界面进程。",
           exitCode);
  return MessageBoxW(nullptr, text, L"TeacherBox 运行异常",
                     MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON1);
}

std::wstring BuildFeedbackReport(const ChildOutcome& outcome, int failures) {
  std::string r;
  r += "TeacherBox 错误反馈\n";
  r += "====================\n";
  r += "生成时间: " + tb::NowDateTime() + "\n";
  r += std::string("主程序版本: ") + kVersion + "\n";
  r += "进程架构: " + std::string(sizeof(void*) == 8 ? "x64" : "x86") + "\n";
  r += "退出码: " + std::to_string(outcome.exitCode) + "\n";
  r += "运行时长(ms): " + std::to_string(outcome.uptimeMs) + "\n";
  r += "连续失败次数: " + std::to_string(failures) + "\n";
  r += "是否启动超时: " + std::string(outcome.timedOut ? "是" : "否") + "\n";
  r += "用户主动关闭: " + std::string(outcome.graceful ? "是" : "否") + "\n\n";

  r += "---------------- 系统信息 ----------------\n";
  r += tb::GetSystemSummary() + "\n\n";

  r += "---------------- gui.exe 管道输出 ----------------\n";
  std::string tail = outcome.tail;
  if (tail.empty()) tail = "(无输出)\n";
  r += tail.substr(tail.size() > 8000 ? tail.size() - 8000 : 0) + "\n";

  r += "---------------- gui 日志尾部 ----------------\n";
  r += tb::TailLog(L"gui", 8000) + "\n\n";

  r += "---------------- main 日志尾部 ----------------\n";
  r += tb::TailLog(L"main", 8000) + "\n\n";

  std::wstring file = tb::JoinPath(tb::FeedbackDir(),
                                   L"feedback-" + tb::ToWide(tb::NowStampForFile()) + L".txt");
  if (!tb::WriteFileUtf8(file, r)) {
    tb::Log(L"main", "[feedback] 写入反馈文件失败");
  }
  return file;
}

void ShowFatal() {
  std::wstring text =
      L"TeacherBox 出现致命性问题：界面进程连续 " + std::to_wstring(kMaxFailures) +
      L" 次异常退出，应用无法正常运行。\n\n主程序将退出，请携带日志文件联系开发者。\n\n日志目录：\n" +
      tb::LogDir();
  MessageBoxW(nullptr, text.c_str(), L"TeacherBox 致命错误",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

// ------------------------------------------------------------------ 桌面快捷方式

// CLSID_ShellLink / IID_IShellLinkW / IID_IPersistFile，硬编码省掉 uuid.lib
const GUID kCLSID_ShellLink  = {0x00021401, 0x0000, 0x0000,
                                {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIID_IShellLinkW  = {0x000214F9, 0x0000, 0x0000,
                                {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIID_IPersistFile = {0x0000010B, 0x0000, 0x0000,
                                {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// 桌面没有「老师工具箱」就建一个；早期版本的 TeacherBox.lnk 顺手清掉，免得桌面上两个图标
void EnsureDesktopShortcut() {
  std::wstring desk = tb::DesktopDir();
  if (desk.empty()) return;

  std::wstring link = tb::JoinPath(desk, L"老师工具箱.lnk");
  std::wstring oldLink = tb::JoinPath(desk, L"TeacherBox.lnk");
  if (tb::FileExists(oldLink)) DeleteFileW(oldLink.c_str());
  if (tb::FileExists(link)) return;

  std::wstring exe = tb::JoinPath(tb::ExeDir(), L"main.exe");

  IShellLinkW* link0 = nullptr;
  if (FAILED(CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, kIID_IShellLinkW,
                              reinterpret_cast<void**>(&link0))) ||
      !link0) {
    tb::Log(L"main", "[shortcut] CoCreateInstance(ShellLink) 失败");
    return;
  }
  link0->SetPath(exe.c_str());
  link0->SetWorkingDirectory(tb::ExeDir().c_str());
  link0->SetDescription(L"老师工具箱");
  link0->SetIconLocation(exe.c_str(), 0);

  IPersistFile* pf = nullptr;
  if (SUCCEEDED(link0->QueryInterface(kIID_IPersistFile, reinterpret_cast<void**>(&pf))) && pf) {
    if (SUCCEEDED(pf->Save(link.c_str(), TRUE)))
      tb::Log(L"main", "[shortcut] 已创建桌面快捷方式：老师工具箱");
    pf->Release();
  }
  link0->Release();
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  SetProcessShutdownParameters(0x300, 0);

  bool skipUpdate = false;
  if (lpCmdLine) {
    std::wstring cmd(lpCmdLine);
    if (cmd.find(L"--skip-update") != std::wstring::npos) skipUpdate = true;
  }

  // 单实例
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TeacherBox.Main.SingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"TeacherBox 已经在运行中。", kAppTitle,
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    return 0;
  }

  tb::EnsureDir(tb::LogDir());
  tb::Log(L"main", std::string("=== TeacherBox 主程序启动 v") + kVersion + " ===");

  // 快捷方式用 COM 建，先初始化套间（后面不影响子进程）
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  EnsureDesktopShortcut();

  const std::wstring dir = tb::ExeDir();
  const std::wstring updateExe = tb::JoinPath(dir, L"update.exe");
  const std::wstring guiExe = tb::JoinPath(dir, L"gui.exe");

  if (!tb::FileExists(guiExe)) {
    MessageBoxW(nullptr, (L"找不到主界面程序：\n" + guiExe).c_str(), kAppTitle,
                MB_OK | MB_ICONERROR | MB_TOPMOST);
    return 2;
  }

  // 1) 升级
  if (skipUpdate) {
    tb::Log(L"main", "[update] 已按参数跳过升级");
  } else if (tb::FileExists(updateExe)) {
    ChildOutcome up = RunChild(L"update", updateExe, L"", 0, kUpdateMaxMs);
    tb::Log(L"main", "[update] 升级进程结束 code=" + std::to_string(up.exitCode));
  } else {
    tb::Log(L"main", "[update] 未找到 update.exe，跳过升级");
  }

  // 2) 守护 GUI
  int failures = 0;
  for (;;) {
    ChildOutcome gui = RunChild(L"gui", guiExe, L"", kReadyTimeoutMs, 0);

    if (gui.graceful) {
      tb::Log(L"main", "[main] 用户正常关闭界面，主程序退出");
      break;
    }

    if (gui.uptimeMs >= kStableRunMs) {
      failures = 1;                       // 之前稳定运行过，重新计数
    } else {
      ++failures;
    }
    tb::Log(L"main", "[main] 界面进程异常退出，连续失败 " + std::to_string(failures) + " 次");

    if (failures >= kMaxFailures) {
      ShowFatal();
      break;
    }

    // 托盘挂在 gui 上，gui 一没托盘就跟着没。所以能静默拉回来就先静默拉回来，
    // 只有连着崩才打扰用户、收反馈。
    if (failures < 2) {
      tb::Log(L"main", "[main] 静默重启界面进程");
      Sleep(600);
      continue;
    }

    if (AskFeedback(gui.exitCode) == IDYES) {
      std::wstring file = BuildFeedbackReport(gui, failures);
      MessageBoxW(nullptr,
                  (L"错误报告已生成：\n" + file +
                   L"\n\n请把这个文件发送给开发者（微信/邮件/Git 仓库 Issue 均可）。")
                      .c_str(),
                  kAppTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
      tb::OpenFolder(tb::FeedbackDir());
    }

    tb::Log(L"main", "[main] 准备重启界面进程");
    Sleep(800);   // 给崩溃现场一点缓冲时间
  }

  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  if (co == S_OK) CoUninitialize();
  return 0;
}
