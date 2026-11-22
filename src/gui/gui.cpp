// TeacherBox 主界面（GUI 子系统，无控制台）
//
// - 用 WebView2 显示 web/ 目录里的工具箱界面
// - 系统托盘：左键唤出窗口，右键菜单（显示 / 隐藏 / 工具目录 / 退出）
// - 与 JavaScript 之间用 WebMessage(JSON) 双向通信
// - 所有工具指令、help.md 的读取都通过 WebMessage 请求触发
//
// 若用户点窗口关闭按钮 -> 向 main.exe 发送 EXIT_USER 并正常退出（主程序不会重启）

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

#include "webview2_compat.h"

#include <string>
#include <thread>
#include <vector>

#include "ipc.h"
#include "json.h"
#include "tools.h"
#include "util.h"
#include "webview2_iids.h"

namespace {

const wchar_t* kWindowClass = L"TeacherBoxGuiWindow";
const wchar_t* kWindowTitle = L"TeacherBox";
const wchar_t* kVhost       = L"teacherbox.local";
const UINT     kTrayMessage = WM_APP + 200;
const UINT     kPostJson    = WM_APP + 201;
const UINT     kTrayId      = 1;

// IID_IUnknown（避免依赖 uuid.lib）
const GUID kIID_Unknown = {0x00000000, 0x0000, 0x0000,
                           {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct AppState {
  HWND   hwnd = nullptr;
  HICON  icon = nullptr;
  bool   trayAdded = false;
  bool   exiting = false;
  NOTIFYICONDATAW nid{};

  ICoreWebView2Controller* controller = nullptr;
  ICoreWebView2*           webview = nullptr;
  bool                     webReady = false;

  tb::IpcChannel    ipc;
  std::wstring      exeDir;
  std::wstring      webDir;
  std::wstring      toolsDir;
  std::vector<tb::ToolSpec> tools;
  std::vector<std::string>  warnings;
  std::string       version = "1.0.0";
};

AppState g;

// ------------------------------------------------------------------ 工具函数

std::wstring CmdArg(const std::wstring& cmdLine, const std::wstring& key) {
  size_t pos = cmdLine.find(key + L"=\"");
  if (pos != std::wstring::npos) {
    size_t start = pos + key.size() + 2;
    size_t end = cmdLine.find(L'"', start);
    return cmdLine.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
  }
  pos = cmdLine.find(key + L"=");
  if (pos == std::wstring::npos) return std::wstring();
  size_t start = pos + key.size() + 1;
  size_t end = cmdLine.find(L' ', start);
  return cmdLine.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

// 把 JSON 文本送回页面（任何线程都可以调用）
void PostJson(const std::string& json) {
  if (!g.hwnd) return;
  auto* payload = new std::wstring(tb::ToWide(json));
  PostMessageW(g.hwnd, kPostJson, 0, reinterpret_cast<LPARAM>(payload));
}

tb::ToolSpec* FindTool(const std::string& id) {
  for (auto& t : g.tools)
    if (t.id == id) return &t;
  return nullptr;
}

std::string Truncate(const std::string& s, size_t maxLen) {
  if (s.size() <= maxLen) return s;
  return s.substr(0, maxLen) + "\n…（输出已截断）";
}

std::string ToolsJson() {
  std::string s = "{\"type\":\"tools\",\"items\":[";
  for (size_t i = 0; i < g.tools.size(); ++i) {
    const tb::ToolSpec& t = g.tools[i];
    if (i) s += ",";
    s += "{\"id\":" + tb::Json::Quote(t.id);
    s += ",\"name\":" + tb::Json::Quote(t.name);
    s += ",\"version\":" + tb::Json::Quote(t.version);
    s += ",\"description\":" + tb::Json::Quote(t.description);
    s += ",\"hasCheck\":";
    s += t.checkCmd.empty() ? "false" : "true";
    s += ",\"hasInstall\":";
    s += t.installCmd.empty() ? "false" : "true";
    s += ",\"hasHelp\":";
    s += t.hasHelp ? "true" : "false";
    s += ",\"actions\":[";
    for (size_t k = 0; k < t.actions.size(); ++k) {
      if (k) s += ",";
      s += tb::Json::Quote(t.actions[k].label);
    }
    s += "]}";
  }
  s += "],\"warnings\":[";
  for (size_t i = 0; i < g.warnings.size(); ++i) {
    if (i) s += ",";
    s += tb::Json::Quote(g.warnings[i]);
  }
  s += "],\"toolsDir\":" + tb::Json::Quote(tb::ToUtf8(g.toolsDir));
  s += ",\"version\":" + tb::Json::Quote(g.version);
  s += "}";
  return s;
}

void RescanTools() {
  g.warnings.clear();
  g.tools = tb::ScanTools(g.toolsDir, g.warnings);
  tb::Log(L"gui", "[tools] 扫描到 " + std::to_string(g.tools.size()) + " 个工具，问题 " +
                      std::to_string(g.warnings.size()) + " 条");
}

// ------------------------------------------------------------------ 终止

void QuitApp(bool graceful) {
  if (g.exiting) return;
  g.exiting = true;
  if (graceful) g.ipc.SendLine("EXIT_USER");
  if (g.hwnd) DestroyWindow(g.hwnd);
}

void ShowMainWindow() {
  ShowWindow(g.hwnd, SW_SHOW);
  SetForegroundWindow(g.hwnd);
  if (g.controller) g.controller->put_IsVisible(TRUE);
}

// ------------------------------------------------------------------ 命令处理

void RunCheckAsync(std::string id) {
  std::thread([id]() {
    tb::ToolSpec* tool = FindTool(id);
    if (!tool) {
      PostJson("{\"type\":\"check\",\"id\":" + tb::Json::Quote(id) +
               ",\"installed\":false,\"output\":\"工具不存在\"}");
      return;
    }
    PostJson("{\"type\":\"busy\",\"id\":" + tb::Json::Quote(id) + ",\"kind\":\"check\",\"busy\":true}");

    tb::RunResult r = tb::RunToolCommand(tool->checkCmd, tool->checkTimeout, tb::ToWide(tool->dir));
    std::string out = tb::Trim(r.output);
    bool installed = r.started && !r.timedOut && r.exitCode == 0 && !out.empty();

    std::string json = "{\"type\":\"check\",\"id\":" + tb::Json::Quote(id);
    json += ",\"installed\":";
    json += installed ? "true" : "false";
    json += ",\"exitCode\":" + std::to_string(r.exitCode);
    json += ",\"timedOut\":";
    json += r.timedOut ? "true" : "false";
    json += ",\"command\":" + tb::Json::Quote(tool->checkCmd);
    json += ",\"output\":" + tb::Json::Quote(Truncate(out, 2000)) + "}";

    tb::Log(L"gui", "[check] " + id + " -> " + (installed ? "true" : "false"));
    PostJson(json);
    PostJson("{\"type\":\"busy\",\"id\":" + tb::Json::Quote(id) + ",\"kind\":\"check\",\"busy\":false}");
  }).detach();
}

void RunInstallAsync(std::string id) {
  std::thread([id]() {
    tb::ToolSpec* tool = FindTool(id);
    if (!tool) return;
    PostJson("{\"type\":\"busy\",\"id\":" + tb::Json::Quote(id) +
             ",\"kind\":\"install\",\"busy\":true}");
    PostJson("{\"type\":\"install_start\",\"id\":" + tb::Json::Quote(id) +
             ",\"command\":" + tb::Json::Quote(tool->installCmd) + "}");

    int limit = tool->installTimeout > 0 ? tool->installTimeout : 1800;
    tb::RunResult r = tb::RunToolCommand(tool->installCmd, limit, tb::ToWide(tool->dir));

    std::string out = Truncate(tb::Trim(r.output), 6000);
    std::string json = "{\"type\":\"install\",\"id\":" + tb::Json::Quote(id);
    json += ",\"ok\":";
    json += (r.started && !r.timedOut && r.exitCode == 0) ? "true" : "false";
    json += ",\"exitCode\":" + std::to_string(r.exitCode);
    json += ",\"timedOut\":";
    json += r.timedOut ? "true" : "false";
    json += ",\"output\":" + tb::Json::Quote(out) + "}";
    tb::Log(L"gui", "[install] " + id + " exit=" + std::to_string(r.exitCode));
    PostJson(json);
    PostJson("{\"type\":\"busy\",\"id\":" + tb::Json::Quote(id) +
             ",\"kind\":\"install\",\"busy\":false}");

    if (r.started && r.exitCode == 0 && !tool->checkCmd.empty()) {
      tb::RunResult c = tb::RunToolCommand(tool->checkCmd, tool->checkTimeout, tb::ToWide(tool->dir));
      std::string cout = tb::Trim(c.output);
      bool installed = c.started && !c.timedOut && c.exitCode == 0 && !cout.empty();
      PostJson("{\"type\":\"check\",\"id\":" + tb::Json::Quote(id) + ",\"installed\":" +
               (installed ? "true" : "false") + ",\"exitCode\":" + std::to_string(c.exitCode) +
               ",\"command\":" + tb::Json::Quote(tool->checkCmd) +
               ",\"output\":" + tb::Json::Quote(Truncate(cout, 2000)) + "}");
    }
  }).detach();
}

void RunHelp(const std::string& id) {
  tb::ToolSpec* tool = FindTool(id);
  if (!tool) return;
  if (!tool->hasHelp) {
    PostJson("{\"type\":\"help\",\"id\":" + tb::Json::Quote(id) +
             ",\"markdown\":\"该工具没有提供 help.md。\",\"name\":" + tb::Json::Quote(tool->name) + "}");
    return;
  }
  bool ok = false;
  std::string md = tb::ReadFileUtf8(tb::ToWide(tool->helpPath), &ok);
  if (!ok) md = "help.md 读取失败。";
  PostJson("{\"type\":\"help\",\"id\":" + tb::Json::Quote(id) + ",\"name\":" +
           tb::Json::Quote(tool->name) + ",\"markdown\":" + tb::Json::Quote(md) + "}");
}

void RunAction(const std::string& id, int index) {
  tb::ToolSpec* tool = FindTool(id);
  if (!tool || index < 0 || index >= (int)tool->actions.size()) {
    PostJson("{\"type\":\"action\",\"id\":" + tb::Json::Quote(id) + ",\"ok\":false}");
    return;
  }
  const tb::ToolAction& a = tool->actions[index];
  std::wstring cmd = tb::ToWide(a.command);

  // 相对路径可执行：拼成工具目录下的绝对路径
  if (cmd.find(L' ') == std::wstring::npos && cmd.find(L'\\') == std::wstring::npos) {
    std::wstring full = tb::JoinPath(tb::ToWide(tool->dir), cmd);
    if (tb::FileExists(full)) cmd = L"\"" + full + L"\"";
    else if (cmd.size() > 4 && _wcsicmp(cmd.c_str() + cmd.size() - 4, L".exe") == 0)
      cmd = L"\"" + cmd + L"\"";
  }

  bool ok = tb::LaunchDetached(cmd, tb::ToWide(tool->dir));
  tb::Log(L"gui", "[action] " + id + " -> " + tb::ToUtf8(cmd) + (ok ? " ok" : " fail"));
  PostJson("{\"type\":\"action\",\"id\":" + tb::Json::Quote(id) + ",\"ok\":" +
           (ok ? "true" : "false") + ",\"label\":" + tb::Json::Quote(a.label) + "}");
}

void HandleMessage(const std::string& text) {
  std::string err;
  tb::Json msg = tb::Json::Parse(text, &err);
  if (!err.empty() || !msg.IsObject()) return;

  std::string cmd = msg.GetString("cmd");
  const std::string id = msg.GetString("id");

  if (cmd == "ready" || cmd == "refresh") {
    if (cmd == "refresh") RescanTools();
    PostJson(ToolsJson());
  } else if (cmd == "check") {
    if (tb::ToolSpec* t = FindTool(id)) {
      if (t->checkCmd.empty())
        PostJson("{\"type\":\"check\",\"id\":" + tb::Json::Quote(id) +
                 ",\"installed\":false,\"exitCode\":-1,\"output\":\"该工具未配置检测指令\"}");
      else
        RunCheckAsync(id);
    } else {
      PostJson("{\"type\":\"check\",\"id\":" + tb::Json::Quote(id) +
               ",\"installed\":false,\"output\":\"工具不存在\"}");
    }
  } else if (cmd == "install") {
    if (tb::ToolSpec* t = FindTool(id)) {
      if (t->installCmd.empty())
        PostJson("{\"type\":\"install\",\"id\":" + tb::Json::Quote(id) +
                 ",\"ok\":false,\"exitCode\":-1,\"output\":\"该工具未配置安装指令\"}");
      else
        RunInstallAsync(id);
    }
  } else if (cmd == "help") {
    RunHelp(id);
  } else if (cmd == "action") {
    RunAction(id, (int)msg.GetNumber("index", 0));
  } else if (cmd == "openFolder") {
    std::string which = msg.GetString("target", "tools");
    std::wstring dir = (which == "logs") ? tb::LogDir() : g.toolsDir;
    tb::OpenFolder(dir);
    PostJson("{\"type\":\"openFolder\",\"ok\":true,\"path\":" + tb::Json::Quote(tb::ToUtf8(dir)) + "}");
  } else if (cmd == "hide") {
    ShowWindow(g.hwnd, SW_HIDE);
    PostJson("{\"type\":\"hidden\"}");
  } else if (cmd == "quit") {
    QuitApp(true);
  } else if (cmd == "appInfo") {
    PostJson("{\"type\":\"appInfo\",\"version\":" + tb::Json::Quote(g.version) +
             ",\"toolsDir\":" + tb::Json::Quote(tb::ToUtf8(g.toolsDir)) +
             ",\"logDir\":" + tb::Json::Quote(tb::ToUtf8(tb::LogDir())) + "}");
  }
}

// ------------------------------------------------------------------ WebView2

class MessageReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, kIID_ICoreWebView2WebMessageReceivedEventHandler) ||
        IsEqualIID(riid, kIID_Unknown)) {
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                   ICoreWebView2WebMessageReceivedEventArgs* args) override {
    (void)sender;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
      HandleMessage(tb::ToUtf8(raw));
      CoTaskMemFree(raw);
    }
    return S_OK;
  }

private:
  LONG ref_ = 1;
};

class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, kIID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) ||
        IsEqualIID(riid, kIID_Unknown)) {
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }

  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
    if (result != S_OK || !controller) {
      wchar_t msg[256];
      swprintf(msg, 256, L"WebView2 控制器创建失败（0x%08X）。\n请确认已安装 Microsoft Edge WebView2 运行时。",
               (unsigned)result);
      MessageBoxW(g.hwnd, msg, kWindowTitle, MB_OK | MB_ICONERROR);
      QuitApp(true);
      return S_OK;
    }

    g.controller = controller;
    g.controller->AddRef();

    ICoreWebView2* webview = nullptr;
    if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
      MessageBoxW(g.hwnd, L"WebView2 内核获取失败。", kWindowTitle, MB_OK | MB_ICONERROR);
      QuitApp(true);
      return S_OK;
    }
    g.webview = webview;
    g.webview->AddRef();

    RECT rc;
    GetClientRect(g.hwnd, &rc);
    controller->put_Bounds(rc);
    controller->put_IsVisible(TRUE);

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
      settings->put_IsStatusBarEnabled(FALSE);
      settings->put_IsZoomControlEnabled(FALSE);
      settings->put_AreDevToolsEnabled(TRUE);
      settings->Release();
    }

    // 用虚拟域名把 web/ 目录映射成 https 源，避免 file:// 的各种限制
    bool mapped = false;
    ICoreWebView2_3* webview3 = nullptr;
    if (SUCCEEDED(webview->QueryInterface(kIID_ICoreWebView2_3, reinterpret_cast<void**>(&webview3))) &&
        webview3) {
      mapped = SUCCEEDED(webview3->SetVirtualHostNameToFolderMapping(
          kVhost, g.webDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW));
      webview3->Release();
    }

    std::wstring url;
    if (mapped) {
      url = L"https://" + std::wstring(kVhost) + L"/index.html";
    } else {
      std::wstring path = g.webDir;
      for (auto& ch : path)
        if (ch == L'\\') ch = L'/';
      url = L"file:///" + path + L"/index.html";
    }
    EventRegistrationToken token{};
    webview->add_WebMessageReceived(new MessageReceivedHandler(), &token);

    webview->Navigate(url.c_str());

    g.webReady = true;
    tb::Log(L"gui", "[webview2] 已加载 " + tb::ToUtf8(url));
    return S_OK;
  }

private:
  LONG ref_ = 1;
};

class EnvironmentCompletedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, kIID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) ||
        IsEqualIID(riid, kIID_Unknown)) {
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }

  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
    if (result != S_OK || !env) {
      wchar_t msg[256];
      swprintf(msg, 256, L"WebView2 环境初始化失败（0x%08X）。", (unsigned)result);
      MessageBoxW(g.hwnd, msg, kWindowTitle, MB_OK | MB_ICONERROR);
      QuitApp(true);
      return S_OK;
    }
    env->CreateCoreWebView2Controller(g.hwnd, new ControllerCompletedHandler());
    return S_OK;
  }

private:
  LONG ref_ = 1;
};

void StartWebView2() {
  using CreateEnvFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                              ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

  HMODULE loader = LoadLibraryW(tb::JoinPath(g.exeDir, L"WebView2Loader.dll").c_str());
  if (!loader) loader = LoadLibraryW(L"WebView2Loader.dll");
  if (!loader) {
    MessageBoxW(g.hwnd,
                L"缺少 WebView2Loader.dll，无法加载界面。\n请把该文件放在程序目录下重新运行。",
                kWindowTitle, MB_OK | MB_ICONERROR);
    QuitApp(true);
    return;
  }

  CreateEnvFn createEnv = reinterpret_cast<CreateEnvFn>(
      GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
  if (!createEnv) {
    MessageBoxW(g.hwnd, L"WebView2Loader.dll 版本不匹配。", kWindowTitle, MB_OK | MB_ICONERROR);
    QuitApp(true);
    return;
  }

  std::wstring userData = tb::JoinPath(tb::AppDataDir(), L"WebView2");
  tb::EnsureDir(userData);

  HRESULT hr = createEnv(nullptr, userData.c_str(), nullptr, new EnvironmentCompletedHandler());
  if (FAILED(hr)) {
    wchar_t msg[256];
    swprintf(msg, 256, L"WebView2 启动失败（0x%08X）。", (unsigned)hr);
    MessageBoxW(g.hwnd, msg, kWindowTitle, MB_OK | MB_ICONERROR);
    QuitApp(true);
  }
}

// ------------------------------------------------------------------ 托盘

void AddTrayIcon() {
  if (g.trayAdded) return;
  ZeroMemory(&g.nid, sizeof(g.nid));
  g.nid.cbSize = sizeof(g.nid);
  g.nid.hWnd = g.hwnd;
  g.nid.uID = kTrayId;
  g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.nid.uCallbackMessage = kTrayMessage;
  g.nid.hIcon = g.icon;
  wcscpy_s(g.nid.szTip, L"TeacherBox 工具箱");
  Shell_NotifyIconW(NIM_ADD, &g.nid);
  g.trayAdded = true;
}

void RemoveTrayIcon() {
  if (!g.trayAdded) return;
  Shell_NotifyIconW(NIM_DELETE, &g.nid);
  g.trayAdded = false;
}

void ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"显示主界面");
  AppendMenuW(menu, MF_STRING, 2, L"隐藏到托盘");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 3, L"打开工具目录");
  AppendMenuW(menu, MF_STRING, 4, L"打开日志目录");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 5, L"退出 TeacherBox");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(menu);

  switch (cmd) {
    case 1: ShowMainWindow(); break;
    case 2: ShowWindow(g.hwnd, SW_HIDE); break;
    case 3: tb::OpenFolder(g.toolsDir); break;
    case 4: tb::OpenFolder(tb::LogDir()); break;
    case 5: QuitApp(true); break;
    default: break;
  }
}

// ------------------------------------------------------------------ 窗口过程

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g.hwnd = hwnd;
      return 0;

    case WM_SIZE:
      if (g.controller) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g.controller->put_Bounds(rc);
      }
      return 0;

    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_MINIMIZE) {   // 最小化 = 收进托盘
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      break;


    case WM_CLOSE:
      QuitApp(true);
      return 0;

    case WM_DESTROY:
      RemoveTrayIcon();
      if (g.controller) { g.controller->Close(); g.controller->Release(); g.controller = nullptr; }
      if (g.webview) { g.webview->Release(); g.webview = nullptr; }
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }

  if (msg == kTrayMessage) {
    switch (LOWORD(lp)) {
      case WM_LBUTTONUP:
        ShowMainWindow();
        return 0;
      case WM_RBUTTONUP:
        ShowTrayMenu();
        return 0;
      default:
        return 0;
    }
  }

  if (msg == kPostJson) {
    std::wstring* payload = reinterpret_cast<std::wstring*>(lp);
    if (payload) {
      if (g.webview) g.webview->PostWebMessageAsJson(payload->c_str());
      delete payload;
    }
    return 0;
  }

  return DefWindowProcW(hwnd, msg, wp, lp);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  char buf[320];
  snprintf(buf, sizeof(buf), "!!! 未捕获异常 code=0x%08X address=%p",
           info && info->ExceptionRecord ? (unsigned)info->ExceptionRecord->ExceptionCode : 0u,
           info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
  tb::Log(L"gui", buf);
  std::wstring crashFile = tb::JoinPath(tb::LogDir(),
                                        L"crash-" + tb::ToWide(tb::NowStampForFile()) + L".txt");
  tb::WriteFileUtf8(crashFile, std::string("TeacherBox GUI 崩溃\n") + buf + "\n\n" +
                                   tb::GetSystemSummary() + "\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

std::wstring ReadVersion() {
  bool ok = false;
  std::string cfg = tb::ReadFileUtf8(tb::JoinPath(g.exeDir, L"update.json"), &ok);
  if (!ok) return L"1.0.0";
  std::string err;
  tb::Json j = tb::Json::Parse(cfg, &err);
  if (!err.empty()) return L"1.0.0";
  std::string v = j.GetString("version", "1.0.0");
  return tb::ToWide(v);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetUnhandledExceptionFilter(CrashFilter);

  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  g.exeDir = tb::ExeDir();
  g.webDir = tb::JoinPath(g.exeDir, L"web");
  g.toolsDir = tb::JoinPath(g.exeDir, L"tools");
  g.version = tb::ToUtf8(ReadVersion());

  tb::Log(L"gui", std::string("=== GUI 启动 v") + g.version + " ===");

  // 连接主程序的管道
  std::wstring cmdLine;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) { if (i > 1) cmdLine += L" "; cmdLine += argv[i]; }
    LocalFree(argv);
  }
  std::wstring pipeName = CmdArg(cmdLine, L"--ipc");
  if (!pipeName.empty() && g.ipc.Connect(pipeName, 8000)) {
    g.ipc.SendLine("READY");
    tb::Log(L"gui", "[ipc] 已连接主程序");
  }

  if (!tb::DirExists(g.webDir)) {
    MessageBoxW(nullptr, (L"缺少界面资源目录：\n" + g.webDir).c_str(), kWindowTitle,
                MB_OK | MB_ICONERROR);
    g.ipc.SendLine("EXIT_USER");
    return 3;
  }

  RescanTools();

  g.icon = tb::MakeAppIcon(32);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = g.icon;
  wc.hIconSm = g.icon;
  wc.lpszClassName = kWindowClass;
  wc.hbrBackground = CreateSolidBrush(RGB(15, 23, 42));
  RegisterClassExW(&wc);

  RECT frame{0, 0, 920, 620};
  AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
  int w = frame.right - frame.left;
  int h = frame.bottom - frame.top;
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

  HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                              (sw - w) / 2, (sh - h) / 2 > 40 ? (sh - h) / 2 - 20 : 0, w, h,
                              nullptr, nullptr, hinst, nullptr);
  if (!hwnd) {
    MessageBoxW(nullptr, L"主窗口创建失败。", kWindowTitle, MB_OK | MB_ICONERROR);
    return 4;
  }

  if (g.icon) {
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g.icon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g.icon);
  }

  // 标题栏跟随浅色界面（系统默认可能按深色主题画成黑条）
  {
    BOOL dark = FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark)))
     && FAILED(DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark))) == S_OK) {
      // 旧版本属性号，失败时静默忽略
    }
    COLORREF caption = RGB(0xFF, 0xFD, 0xF8);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
  }

  AddTrayIcon();
  ShowWindow(hwnd, SW_SHOW);
  ShowWindow(hwnd, SW_SHOWNORMAL);   // 再调一次：父进程若指定过 STARTUPINFO 显示方式，第一次会被忽略
  SetForegroundWindow(hwnd);
  UpdateWindow(hwnd);

  StartWebView2();

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g.ipc.Close();
  if (g.icon) DestroyIcon(g.icon);
  if (co == S_OK) CoUninitialize();
  tb::Log(L"gui", "=== GUI 退出 ===");
  return 0;
}




