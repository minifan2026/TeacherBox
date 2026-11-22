// TeacherBox 升级器（GUI 子系统，无控制台）
//
// 行为：
//   1. 读取 exe 同目录 update.json（本地版本 / 更新源）
//   2. 有更新源时拉取远端 manifest，比较版本，下载增量包并解压覆盖
//   3. 无更新源（默认）时，显示“离线模式，已是最新版本”，随后自动关闭
//   4. 全程用一个进程内的小窗口显示进度，不产生控制台
//
// 被 main.exe 以命名管道拉起；通过管道回报 READY / STATUS 行。

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include "ipc.h"
#include "json.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

const wchar_t* kTitle = L"TeacherBox 更新";
const char*    kLocalVersionFallback = "1.0.0";

struct Updater {
  HWND         hwnd = nullptr;
  HBRUSH       bg = nullptr;
  HFONT        fontTitle = nullptr;
  HFONT        fontBody = nullptr;
  int          percent = 0;
  std::wstring status = L"正在初始化…";
  tb::IpcChannel* ipc = nullptr;
};

Updater g_up;

void PostStatus(const std::wstring& text) {
  if (g_up.ipc) g_up.ipc->SendLine("STATUS " + tb::ToUtf8(text));
  if (g_up.hwnd) PostMessageW(g_up.hwnd, WM_APP + 1, 0, (LPARAM)new std::wstring(text));
}

void PostProgress(int percent) {
  if (g_up.hwnd) PostMessageW(g_up.hwnd, WM_APP + 2, (WPARAM)percent, 0);
}

// ------------------------------------------------------------ HTTP

bool HttpGet(const std::wstring& url, std::string& out, bool* networkError = nullptr) {
  out.clear();
  if (networkError) *networkError = false;

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

  HINTERNET session = WinHttpOpen(L"TeacherBoxUpdater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) { if (networkError) *networkError = true; return false; }

  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  if (conn) {
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (req) {
      if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
          WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &len, nullptr);
        if (status >= 200 && status < 300) {
          DWORD avail = 0;
          do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            out += chunk;
            if (out.size() > 64u * 1024u * 1024u) break;
          } while (avail > 0);
          ok = true;
        }
      }
      WinHttpCloseHandle(req);
    } else {
      if (networkError) *networkError = true;
    }
    WinHttpCloseHandle(conn);
  } else {
    if (networkError) *networkError = true;
  }
  WinHttpCloseHandle(session);
  return ok;
}

// 国内访问 raw.githubusercontent.com 经常抽风，这里给它和 jsDelivr 之间互相兜底，
// 两个地址指向的都是仓库里同一份文件。
std::wstring AltUrl(const std::wstring& url) {
  const std::wstring rawPrefix = L"https://raw.githubusercontent.com/";
  const std::wstring cdnPrefix = L"https://cdn.jsdelivr.net/gh/";

  if (url.compare(0, rawPrefix.size(), rawPrefix) == 0) {
    // owner/repo/branch/rest -> gh/owner/repo@branch/rest
    std::wstring rest = url.substr(rawPrefix.size());
    size_t p1 = rest.find(L'/');
    if (p1 == std::wstring::npos) return L"";
    size_t p2 = rest.find(L'/', p1 + 1);
    if (p2 == std::wstring::npos) return L"";
    size_t p3 = rest.find(L'/', p2 + 1);
    if (p3 == std::wstring::npos) return L"";
    return cdnPrefix + rest.substr(0, p2) + L"@" + rest.substr(p2 + 1, p3 - p2 - 1) +
           rest.substr(p3);
  }

  if (url.compare(0, cdnPrefix.size(), cdnPrefix) == 0) {
    // gh/owner/repo@branch/rest -> owner/repo/branch/rest
    std::wstring rest = url.substr(cdnPrefix.size());
    size_t at = rest.find(L'@');
    if (at == std::wstring::npos) return L"";
    size_t p = rest.find(L'/', at);
    if (p == std::wstring::npos) return L"";
    return rawPrefix + rest.substr(0, at) + L"/" + rest.substr(at + 1, p - at - 1) +
           rest.substr(p);
  }

  return L"";
}

bool FetchText(const std::wstring& url, std::string& out, bool* networkError = nullptr) {
  if (HttpGet(url, out, networkError)) return true;
  std::wstring alt = AltUrl(url);
  if (!alt.empty()) {
    out.clear();
    if (HttpGet(alt, out, nullptr)) return true;
  }
  return false;
}

bool HttpDownload(const std::wstring& url, const std::wstring& dstPath) {
  std::string data;
  if (!FetchText(url, data)) return false;
  PostProgress(70);
  return tb::WriteFileUtf8(dstPath, data);
}

// ------------------------------------------------------------ 版本 / 文件

int CompareVersion(const std::string& a, const std::string& b) {
  size_t ia = 0, ib = 0;
  while (ia < a.size() || ib < b.size()) {
    int va = 0, vb = 0;
    while (ia < a.size() && (a[ia] < '0' || a[ia] > '9')) ++ia;
    while (ia < a.size() && a[ia] >= '0' && a[ia] <= '9') va = va * 10 + (a[ia++] - '0');
    while (ib < b.size() && (b[ib] < '0' || b[ib] > '9')) ++ib;
    while (ib < b.size() && b[ib] >= '0' && b[ib] <= '9') vb = vb * 10 + (b[ib++] - '0');
    if (va != vb) return va < vb ? -1 : 1;
  }
  return 0;
}

bool ExtractZip(const std::wstring& zip, const std::wstring& dstDir) {
  wchar_t sysRoot[MAX_PATH] = {0};
  GetEnvironmentVariableW(L"SystemRoot", sysRoot, MAX_PATH);
  std::wstring tar = std::wstring(sysRoot) + L"\\System32\\tar.exe";
  tb::EnsureDir(dstDir);
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dstDir + L"\"";
  tb::RunResult r = tb::RunCommand(cmd, 180000);
  return r.started && r.exitCode == 0;
}

bool CopyFileReplacing(const std::wstring& src, const std::wstring& dst) {
  if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
  // 目标被占用（例如正在运行的 main.exe）：先把旧文件改名，再复制
  std::wstring backup = dst + L".old";
  DeleteFileW(backup.c_str());
  if (MoveFileW(dst.c_str(), backup.c_str())) {
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    MoveFileW(backup.c_str(), dst.c_str());   // 回滚
  }
  return false;
}

int CopyTree(const std::wstring& srcDir, const std::wstring& dstDir) {
  int copied = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(tb::JoinPath(srcDir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    std::wstring s = tb::JoinPath(srcDir, fd.cFileName);
    std::wstring d = tb::JoinPath(dstDir, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      tb::EnsureDir(d);
      copied += CopyTree(s, d);
    } else {
      if (CopyFileReplacing(s, d)) ++copied;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return copied;
}

void CleanupOldFiles(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(tb::JoinPath(dir, L"*.old").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do { DeleteFileW(tb::JoinPath(dir, fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// ------------------------------------------------------------ 升级流程

void DoUpdate() {
  const std::wstring dir = tb::ExeDir();
  CleanupOldFiles(dir);

  bool ok = false;
  std::string localCfg = tb::ReadFileUtf8(tb::JoinPath(dir, L"update.json"), &ok);

  std::string localVersion = kLocalVersionFallback;
  std::string remote = "";
  std::string localPackage = "";
  if (ok) {
    std::string err;
    tb::Json cfg = tb::Json::Parse(localCfg, &err);
    if (err.empty()) {
      localVersion = cfg.GetString("version", localVersion);
      remote       = cfg.GetString("remote", "");
      localPackage = cfg.GetString("package", "");
    } else {
      tb::Log(L"update", "[config] update.json 解析失败: " + err);
    }
  }

  PostStatus(L"已启动，正在检查更新…");
  PostProgress(5);
  Sleep(400);

  if (remote.empty()) {
    if (!localPackage.empty()) {
      // 本地包更新（测试/离线分发用）
      std::wstring pkg = tb::JoinPath(dir, tb::ToWide(localPackage));
      if (tb::FileExists(pkg)) {
        PostStatus(L"发现本地更新包，正在应用…");
        PostProgress(40);
        std::wstring stage = tb::JoinPath(dir, L".stage");
        if (ExtractZip(pkg, stage)) {
          int n = CopyTree(stage, dir);
          PostStatus(L"已应用 " + std::to_wstring(n) + L" 个文件");
          PostProgress(100);
          Sleep(600);
        } else {
          PostStatus(L"更新包解压失败，跳过升级");
        }
      } else {
        PostStatus(L"本地更新包不存在，已是最新版本");
        PostProgress(100);
      }
    } else {
      PostStatus(L"离线模式：未配置更新源，当前已是最新版本 v" + tb::ToWide(localVersion));
      PostProgress(100);
      Sleep(900);
    }
    if (g_up.ipc) g_up.ipc->SendLine("LOG update.done local=" + localVersion);
    return;
  }

  PostStatus(L"正在获取远端版本信息…");
  PostProgress(20);

  std::string manifestText;
  bool netErr = false;
  if (!FetchText(tb::ToWide(remote), manifestText, &netErr)) {
    PostStatus(netErr ? L"网络不可用，跳过本次更新，继续启动" : L"获取更新信息失败，跳过本次更新");
    PostProgress(100);
    Sleep(900);
    return;
  }

  std::string err;
  tb::Json manifest = tb::Json::Parse(manifestText, &err);
  if (!err.empty()) {
    PostStatus(L"更新清单格式错误，跳过本次更新");
    PostProgress(100);
    Sleep(900);
    return;
  }

  std::string remoteVersion = manifest.GetString("version", localVersion);
  if (CompareVersion(remoteVersion, localVersion) <= 0) {
    PostStatus(L"当前已是最新版本 v" + tb::ToWide(localVersion));
    PostProgress(100);
    Sleep(900);
    return;
  }

  std::string packageUrl = manifest.GetString("package", "");
  if (packageUrl.empty()) {
    PostStatus(L"远端清单缺少 package 字段，跳过更新");
    PostProgress(100);
    Sleep(900);
    return;
  }

  PostStatus(L"发现新版本 v" + tb::ToWide(remoteVersion) + L"，正在下载…");
  PostProgress(35);

  wchar_t tempPath[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tempPath);
  std::wstring zipPath = tb::JoinPath(tempPath, L"teacherbox-update.zip");
  if (!HttpDownload(tb::ToWide(packageUrl), zipPath)) {
    PostStatus(L"下载失败，跳过本次更新");
    PostProgress(100);
    Sleep(900);
    return;
  }

  PostStatus(L"下载完成，正在解压…");
  PostProgress(80);

  std::wstring stage = tb::JoinPath(tempPath, L"teacherbox-stage");
  if (tb::DirExists(stage)) {
    RemoveDirectoryW(stage.c_str());   // 尽力清理，失败也没关系
  }
  if (!ExtractZip(zipPath, stage)) {
    PostStatus(L"解压失败，跳过本次更新");
    PostProgress(100);
    Sleep(900);
    return;
  }

  int copied = CopyTree(stage, dir);
  PostStatus(L"更新完成，共替换 " + std::to_wstring(copied) + L" 个文件");
  PostProgress(100);
  if (g_up.ipc) g_up.ipc->SendLine("LOG update.applied " + remoteVersion);
  Sleep(800);
}

// ------------------------------------------------------------ 窗口

void Paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);

  FillRect(dc, &rc, g_up.bg);

  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_up.fontTitle);
  SetTextColor(dc, RGB(241, 245, 249));
  RECT t{28, 24, rc.right - 28, 70};
  DrawTextW(dc, L"TeacherBox 更新", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SelectObject(dc, g_up.fontBody);
  SetTextColor(dc, RGB(148, 163, 184));
  RECT s{28, 66, rc.right - 28, 116};
  DrawTextW(dc, g_up.status.c_str(), -1, &s, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);

  // 进度条
  RECT track{28, 132, rc.right - 28, 144};
  HBRUSH trackBrush = CreateSolidBrush(RGB(30, 41, 59));
  FillRect(dc, &track, trackBrush);
  DeleteObject(trackBrush);

  int w = track.right - track.left;
  int fill = w * g_up.percent / 100;
  if (fill > 0) {
    RECT bar{track.left, track.top, track.left + fill, track.bottom};
    HBRUSH barBrush = CreateSolidBrush(RGB(37, 99, 235));
    FillRect(dc, &bar, barBrush);
    DeleteObject(barBrush);
  }

  char pct[32];
  snprintf(pct, sizeof(pct), "%d%%", g_up.percent);
  SetTextColor(dc, RGB(100, 116, 139));
  RECT pr{28, 152, rc.right - 28, 176};
  DrawTextA(dc, pct, -1, &pr, DT_RIGHT | DT_SINGLELINE);

  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_up.hwnd = hwnd;
      g_up.bg = CreateSolidBrush(RGB(15, 23, 42));
      {
        LOGFONTW lf{};
        lf.lfHeight = -24;
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        g_up.fontTitle = CreateFontIndirectW(&lf);
        lf.lfHeight = -15;
        lf.lfWeight = FW_NORMAL;
        g_up.fontBody = CreateFontIndirectW(&lf);
      }
      return 0;

    case WM_APP + 1: {
      std::wstring* text = reinterpret_cast<std::wstring*>(lp);
      if (text) {
        g_up.status = *text;
        delete text;
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_APP + 2:
      g_up.percent = (int)wp;
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_APP + 3:
      DestroyWindow(hwnd);
      return 0;

    case WM_PAINT:
      Paint(hwnd);
      return 0;

    case WM_CLOSE:
      // 升级过程中不允许用户关掉
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void CenterWindow(HWND hwnd, int w, int h) {
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  SetWindowPos(hwnd, HWND_TOPMOST, (sw - w) / 2, (sh - h) / 2, w, h, SWP_SHOWWINDOW);
}

std::wstring GetCmdArg(const std::wstring& cmdLine, const std::wstring& key) {
  size_t pos = cmdLine.find(key + L"=\"");
  if (pos == std::wstring::npos) {
    pos = cmdLine.find(key + L"=");
    if (pos == std::wstring::npos) return std::wstring();
    size_t start = pos + key.size() + 1;
    size_t end = cmdLine.find(L' ', start);
    return cmdLine.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
  }
  size_t start = pos + key.size() + 2;
  size_t end = cmdLine.find(L'"', start);
  return cmdLine.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  tb::IpcChannel ipc;
  std::wstring cmdLine;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) { if (i > 1) cmdLine += L" "; cmdLine += argv[i]; }
    LocalFree(argv);
  }
  std::wstring pipeName = GetCmdArg(cmdLine, L"--ipc");
  if (!pipeName.empty()) {
    if (ipc.Connect(pipeName, 5000)) {
      tb::Log(L"update", "[ipc] connected");
      g_up.ipc = &ipc;
      ipc.SendLine("READY");
      tb::Log(L"update", "[ipc] READY sent");
    } else {
      tb::Log(L"update", "[ipc] connect failed");
    }
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"TeacherBoxUpdaterWindow";
  wc.hIcon = tb::MakeAppIcon(32);
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, wc.lpszClassName, kTitle,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, 520, 210, nullptr, nullptr, hinst,
                              nullptr);
  if (!hwnd) return 1;
  CenterWindow(hwnd, 520, 210);

  std::thread worker([] {
    try {
      DoUpdate();
    } catch (...) {
      PostStatus(L"升级过程出现异常，跳过本次更新");
    }
    if (g_up.hwnd) PostMessageW(g_up.hwnd, WM_APP + 3, 0, 0);
  });

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (worker.joinable()) worker.join();

  if (g_up.fontTitle) DeleteObject(g_up.fontTitle);
  if (g_up.fontBody) DeleteObject(g_up.fontBody);
  if (g_up.bg) DeleteObject(g_up.bg);
  ipc.Close();
  return 0;
}





