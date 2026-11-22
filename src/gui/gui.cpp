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
#include <commdlg.h>

#include "webview2_compat.h"

#include <mutex>
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
const int      kIconId      = 101;   // resources/app.rc 里的 ICON 资源号
const UINT     kTrayMessage = WM_APP + 200;
const UINT     kPostJson    = WM_APP + 201;
const UINT     kTrayId      = 1;

// 工具市场清单：仓库根目录的 market.json，raw 与 jsDelivr 互相兜底
const wchar_t* kMarketUrl = L"https://raw.githubusercontent.com/minifan2026/TeacherBox/main/market.json";

// IID_IUnknown（避免依赖 uuid.lib）
const GUID kIID_Unknown = {0x00000000, 0x0000, 0x0000,
                           {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct MarketItem {
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  std::string url;
};

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
  std::vector<MarketItem>   market;
  bool                      marketLoaded  = false;
  bool                      marketFetching = false;
  std::string               version = "1.0.0";
};

std::mutex g_marketLock;

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

// ------------------------------------------------------------------ 工具市场

std::string SanitizeId(const std::string& raw) {
  std::string s;
  for (char ch : raw) {
    unsigned char c = (unsigned char)ch;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
        c == '_' || c == '-' || c == '+')
      s += (char)c;
    else if (c == ' ')
      s += '-';
  }
  while (!s.empty() && s[0] == '.') s.erase(0, 1);
  if (s.size() > 48) s.resize(48);
  return s;
}

std::string MarketJson(const std::string& error) {
  std::string s = "{\"type\":\"market\"";
  if (!error.empty()) s += ",\"error\":" + tb::Json::Quote(error);
  s += ",\"items\":[";
  std::lock_guard<std::mutex> lock(g_marketLock);
  for (size_t i = 0; i < g.market.size(); ++i) {
    const MarketItem& m = g.market[i];
    if (i) s += ",";
    s += "{\"id\":" + tb::Json::Quote(m.id);
    s += ",\"name\":" + tb::Json::Quote(m.name);
    s += ",\"version\":" + tb::Json::Quote(m.version);
    s += ",\"description\":" + tb::Json::Quote(m.description);
    s += ",\"url\":" + tb::Json::Quote(m.url);
    s += ",\"installed\":";
    s += tb::DirExists(tb::JoinPath(g.toolsDir, tb::ToWide(m.id))) ? "true" : "false";
    s += "}";
  }
  s += "]}";
  return s;
}

void FetchMarketAsync() {
  if (g.marketFetching) return;
  g.marketFetching = true;
  PostJson("{\"type\":\"marketLoading\",\"busy\":true}");

  std::thread([]() {
    std::string text;
    bool netErr = false;
    std::string error;
    std::vector<MarketItem> items;

    if (!tb::HttpGetText(kMarketUrl, text, &netErr)) {
      error = netErr ? "连不上 GitHub，先打开 Watt Toolkit 里的 GitHub 加速再试"
                     : "清单地址返回异常";
    } else {
      std::string err;
      tb::Json doc = tb::Json::Parse(text, &err);
      if (!err.empty() || !doc.IsObject()) {
        error = "market.json 解析失败";
      } else {
        const tb::Json* arr = doc.Find("items");
        if (arr && arr->IsArray()) {
          for (const tb::Json& one : arr->items) {
            if (!one.IsObject()) continue;
            MarketItem m;
            m.id = one.GetString("id", "");
            m.name = one.GetString("name", m.id);
            m.version = one.GetString("version", "");
            m.description = one.GetString("description", one.GetString("desc", ""));
            m.url = one.GetString("url", one.GetString("package", ""));
            if (!tb::IsValidToolId(m.id) || m.url.empty()) continue;
            items.push_back(m);
          }
        }
        if (items.empty()) error = "清单里没有可用工具";
      }
    }

    {
      std::lock_guard<std::mutex> lock(g_marketLock);
      g.market = items;
    }
    g.marketLoaded = !error.empty() ? false : true;
    g.marketFetching = false;
    tb::Log(L"gui", "[market] 清单 " + std::to_string(items.size()) + " 项" +
                        (error.empty() ? "" : "，错误：" + error));
    PostJson(MarketJson(error));
    PostJson("{\"type\":\"marketLoading\",\"busy\":false}");
  }).detach();
}

// ---- 安装（zip 展开 / 目录复制共用）

bool ExtractZip(const std::wstring& zip, const std::wstring& dst) {
  tb::EnsureDir(dst);
  auto esc = [](const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) { if (c == L'\'') r += L"''"; r += c; }
    return r;
  };
  std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command " +
                    std::wstring(L"\"Expand-Archive -LiteralPath '") + esc(zip) +
                    L"' -DestinationPath '" + esc(dst) + L"' -Force\"";
  tb::RunResult r = tb::RunCommand(ps, 120000, L"");
  if (r.started && r.exitCode == 0) return true;

  // 兜底：Win10 自带的 tar（bsdtar 能直接解 zip）
  std::wstring tar = L"tar.exe -xf \"" + zip + L"\" -C \"" + dst + L"\"";
  tb::RunResult r2 = tb::RunCommand(tar, 120000, L"");
  return r2.started && r2.exitCode == 0;
}

// 工具包可能把 tool.json 放在根，也可能外面还套一层目录
std::wstring FindToolRoot(const std::wstring& dir) {
  if (tb::FileExists(tb::JoinPath(dir, L"tool.json"))) return dir;
  for (const std::wstring& sub : tb::ListDirs(dir)) {
    std::wstring p = tb::JoinPath(dir, sub);
    if (tb::FileExists(tb::JoinPath(p, L"tool.json"))) return p;
  }
  return std::wstring();
}

bool InstallPackage(const std::wstring& srcRoot, const std::string& preferId, std::string& outId,
                    std::string& error) {
  if (srcRoot.empty() || !tb::FileExists(tb::JoinPath(srcRoot, L"tool.json"))) {
    error = "包里没有 tool.json，不是工具包";
    return false;
  }
  std::wstring base = tb::ToWide(preferId);
  if (base.empty()) {
    size_t pos = srcRoot.find_last_of(L"\\/");
    base = (pos == std::wstring::npos) ? srcRoot : srcRoot.substr(pos + 1);
  }
  std::string id = SanitizeId(tb::ToUtf8(base));
  if (!tb::IsValidToolId(id)) {
    error = "工具 id 不合规（只允许字母、数字和 . _ - +）";
    return false;
  }

  std::wstring target = tb::JoinPath(g.toolsDir, tb::ToWide(id));
  if (tb::DirExists(target)) tb::DeleteTree(target);
  if (!tb::CopyTree(srcRoot, target)) {
    error = "写入 tools/" + id + " 失败，可能有文件被占用";
    return false;
  }
  outId = id;
  return true;
}

void PostImportResult(bool ok, const std::string& id, const std::string& message) {
  std::string json = "{\"type\":\"import\",\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"id\":" + tb::Json::Quote(id);
  json += ",\"message\":" + tb::Json::Quote(message) + "}";
  PostJson(json);
  if (ok) {
    RescanTools();
    PostJson(ToolsJson());
    PostJson(MarketJson(""));
  }
}

// 页面里拖进来的 zip 是 base64 文本，这里还原成文件再走安装流程
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void ImportZipAsync(const std::wstring& zipPath, const std::string& preferId);   // 前置声明

std::string Base64Decode(const std::string& in) {
  std::string out;
  int val = 0, bits = 0;
  for (char ch : in) {
    if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
    const char* p = (ch == '+') ? kB64 + 62 : (ch == '/') ? kB64 + 63 : strchr(kB64, ch);
    if (!p || ch == '\0') continue;
    val = (val << 6) | (int)(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((char)((val >> bits) & 0xFF));
    }
  }
  return out;
}

void ImportFromPageData(const std::string& fileName, const std::string& b64) {
  std::string bytes = Base64Decode(b64);
  if (bytes.size() < 4 || bytes[0] != 'P' || bytes[1] != 'K') {
    PostImportResult(false, "", "收到的不是 zip 文件");
    return;
  }
  std::wstring stem = tb::ToWide(fileName);
  size_t dot = stem.find_last_of(L'.');
  if (dot != std::wstring::npos) stem = stem.substr(0, dot);
  std::wstring zip = tb::JoinPath(tb::TempDir(), L"tb-drop-" + tb::ToWide(tb::NowStampForFile()) + L".zip");
  if (!tb::WriteFileUtf8(zip, bytes)) {
    PostImportResult(false, "", "临时文件写入失败");
    return;
  }
  ImportZipAsync(zip, tb::ToUtf8(stem));
}

void ImportZipAsync(const std::wstring& zipPath, const std::string& preferId) {
  tb::Log(L"gui", "[import] zip： " + tb::ToUtf8(zipPath));
  PostJson("{\"type\":\"importStart\",\"source\":" + tb::Json::Quote(tb::ToUtf8(zipPath)) + "}");

  // 没指定 id 时用 zip 自身的文件名（而不是临时目录名）当工具目录名
  std::string prefer = preferId;
  if (prefer.empty()) {
    size_t sep = zipPath.find_last_of(L"\\/");
    std::wstring name = (sep == std::wstring::npos) ? zipPath : zipPath.substr(sep + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    prefer = SanitizeId(tb::ToUtf8(name));
  }

  std::thread([zipPath, prefer]() {
    std::wstring temp = tb::JoinPath(tb::TempDir(),
                                     L"tb-import-" + tb::ToWide(tb::NowStampForFile()));
    std::string error;
    std::string id;

    if (!ExtractZip(zipPath, temp)) {
      error = "解压失败（zip 可能损坏或被占用）";
    } else {
      std::wstring root = FindToolRoot(temp);
      if (!InstallPackage(root, prefer, id, error)) {
        // 错误信息已经在 error 里
      }
    }
    tb::DeleteTree(temp);

    if (error.empty()) {
      tb::Log(L"gui", "[import] 已安装工具 " + id);
      PostImportResult(true, id, "已安装 " + id);
    } else {
      tb::Log(L"gui", "[import] 失败：" + error);
      PostImportResult(false, id, error);
    }
    PostJson("{\"type\":\"importEnd\"}");
  }).detach();
}

void ImportDirAsync(const std::wstring& dirPath) {
  tb::Log(L"gui", "[import] 目录： " + tb::ToUtf8(dirPath));
  PostJson("{\"type\":\"importStart\",\"source\":" + tb::Json::Quote(tb::ToUtf8(dirPath)) + "}");

  std::thread([dirPath]() {
    std::wstring root = FindToolRoot(dirPath);
    std::string error, id;
    if (!InstallPackage(root, "", id, error)) {
      PostImportResult(false, id, error);
    } else {
      PostImportResult(true, id, "已安装 " + id);
    }
    PostJson("{\"type\":\"importEnd\"}");
  }).detach();
}

void ImportMarketItem(const std::string& id) {
  MarketItem item;
  {
    std::lock_guard<std::mutex> lock(g_marketLock);
    bool found = false;
    for (const MarketItem& m : g.market)
      if (m.id == id) { item = m; found = true; break; }
    if (!found) {
      PostImportResult(false, id, "清单里没有这个工具，先刷新一下");
      return;
    }
  }
  std::wstring zip = tb::JoinPath(tb::TempDir(), L"tb-market-" + tb::ToWide(SanitizeId(id)) + L".zip");
  PostJson("{\"type\":\"marketBusy\",\"id\":" + tb::Json::Quote(id) + ",\"busy\":true}");

  std::thread([id, url = item.url, zip]() {
    if (!tb::HttpGetFile(tb::ToWide(url), zip)) {
      PostJson("{\"type\":\"marketBusy\",\"id\":" + tb::Json::Quote(id) + ",\"busy\":false}");
      PostImportResult(false, id, "下载失败，检查网络或 GitHub 加速");
      return;
    }
    PostJson("{\"type\":\"marketBusy\",\"id\":" + tb::Json::Quote(id) + ",\"busy\":false}");
    ImportZipAsync(zip, id);
  }).detach();
}

// 选择本地 zip 安装（“上传”按钮）
void ImportFromDialog() {
  wchar_t path[MAX_PATH * 2] = {0};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.hwnd;
  ofn.lpstrFilter = L"工具包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH * 2;
  ofn.lpstrTitle = L"选择工具包";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  tb::Log(L"gui", "[import] 打开选择框");
  BOOL ok = GetOpenFileNameW(&ofn);
  if (!ok) {
    tb::Log(L"gui", "[import] 选择框取消或失败，CommDlgExtendedError=" +
                        std::to_string((unsigned)CommDlgExtendedError()));
    return;
  }
  tb::Log(L"gui", "[import] 选中 " + tb::ToUtf8(std::wstring(path)));
  ImportZipAsync(path, "");
}

// 拖进窗口：zip 解压安装，目录直接安装
void HandleDroppedFiles(HDROP drop) {  UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    wchar_t path[MAX_PATH * 2] = {0};
    if (DragQueryFileW(drop, i, path, MAX_PATH * 2) == 0) continue;
    std::wstring p = path;
    if (tb::DirExists(p)) {
      ImportDirAsync(p);
    } else {
      size_t dot = p.find_last_of(L'.');
      std::wstring ext = (dot == std::wstring::npos) ? L"" : p.substr(dot);
      if (_wcsicmp(ext.c_str(), L".zip") == 0) {
        ImportZipAsync(p, "");
      } else {
        PostImportResult(false, "", "只认 .zip 工具包或工具目录");
      }
    }
  }
  DragFinish(drop);
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
  } else if (cmd == "market") {
    if (g.marketLoaded) PostJson(MarketJson(""));
    else FetchMarketAsync();
  } else if (cmd == "marketRefresh") {
    g.marketLoaded = false;
    FetchMarketAsync();
  } else if (cmd == "marketInstall") {
    ImportMarketItem(id);
  } else if (cmd == "importFile") {
    ImportFromDialog();
  } else if (cmd == "importData") {
    ImportFromPageData(msg.GetString("name", "tool.zip"), msg.GetString("data", ""));
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
    // 注意：不关 AllowExternalDrop。文件拖放走页面里 HTML5 的 drop 事件
    // （WebView2 的 chrome 子窗口会吃掉 WM_DROPFILES，只在宿主窗口接是收不到的）

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
      DragAcceptFiles(hwnd, TRUE);
      return 0;

    case WM_DROPFILES:
      HandleDroppedFiles(reinterpret_cast<HDROP>(wp));
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

  // 用嵌在 exe 里的图标（resources/app.ico），资源管理器、任务栏、托盘看到的是同一个，
  // 老 exe 没这个资源时才退回运行时画的那个
  HICON icoBig = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CXICON), LR_SHARED);
  HICON icoSmall = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CXSMICON), LR_SHARED);
  g.icon = icoBig ? icoBig : tb::MakeAppIcon(32);

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
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(icoSmall ? icoSmall : g.icon));
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




