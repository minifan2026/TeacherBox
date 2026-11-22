#include "util.h"

#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <vector>
#include <cstdint>
#include <ctime>

namespace tb {

// ---------------------------------------------------------------- 路径

std::wstring ExePath() {
  wchar_t buf[4096] = {0};
  GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)));
  return std::wstring(buf);
}

std::wstring ExeDir() {
  std::wstring p = ExePath();
  size_t pos = p.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? std::wstring(L".") : p.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (b[0] == L'\\' || b[0] == L'/') return b;
  std::wstring r = a;
  if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
  r += b;
  return r;
}

std::wstring AppDataDir() {
  static std::wstring cached;
  if (!cached.empty()) return cached;
  wchar_t buf[MAX_PATH * 2] = {0};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
    cached = JoinPath(ExeDir(), L"data");
  } else {
    cached = JoinPath(std::wstring(buf), L"TeacherBox");
  }
  EnsureDir(cached);
  return cached;
}

std::wstring LogDir() {
  static std::wstring d;
  if (d.empty()) { d = JoinPath(AppDataDir(), L"logs"); EnsureDir(d); }
  return d;
}

std::wstring FeedbackDir() {
  static std::wstring d;
  if (d.empty()) { d = JoinPath(AppDataDir(), L"feedback"); EnsureDir(d); }
  return d;
}

std::wstring DesktopDir() {
  wchar_t buf[MAX_PATH * 2] = {0};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) return buf;
  wchar_t up[MAX_PATH * 2] = {0};
  if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2)) return JoinPath(up, L"Desktop");
  return std::wstring();
}

std::wstring TempDir() {
  wchar_t buf[MAX_PATH * 2] = {0};
  if (GetEnvironmentVariableW(L"TEMP", buf, MAX_PATH * 2) && buf[0]) return buf;
  return JoinPath(AppDataDir(), L"temp");
}

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string r((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n, nullptr, nullptr);
  return r;
}

std::wstring ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring r((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
  return r;
}

std::string ReadFileUtf8(const std::wstring& path, bool* ok) {
  if (ok) *ok = false;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return std::string(); }
  if (size.QuadPart > 64LL * 1024 * 1024) { CloseHandle(h); return std::string(); }

  std::string data((size_t)size.QuadPart, '\0');
  DWORD read = 0;
  if (size.QuadPart > 0) {
    if (!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr)) {
      CloseHandle(h);
      return std::string();
    }
    data.resize(read);
  }
  CloseHandle(h);

  // 去掉 UTF-8 BOM
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
      (unsigned char)data[2] == 0xBF) {
    data.erase(0, 3);
  }
  if (ok) *ok = true;
  return data;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    // 目录可能还不存在，尝试创建父目录后重试一次
    std::wstring p = path;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
      EnsureDir(p.substr(0, pos));
      h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  if (h == INVALID_HANDLE_VALUE) return false;

  bool ok = true;
  if (!data.empty()) {
    DWORD written = 0;
    if (!WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) || written != data.size())
      ok = false;
  }
  CloseHandle(h);
  return ok;
}

bool EnsureDir(const std::wstring& path) {
  if (path.empty()) return true;
  if (DirExists(path)) return true;
  int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
  return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS ||
         r == ERROR_FILE_EXISTS || DirExists(path);
}

bool FileExists(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> ListDirs(const std::wstring& path) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd{};
  std::wstring pattern = JoinPath(path, L"*");
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.emplace_back(fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

// ---------------------------------------------------------------- 目录复制

bool CopyTree(const std::wstring& src, const std::wstring& dst) {
  if (!DirExists(src)) return false;
  if (!EnsureDir(dst)) return false;

  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    std::wstring from = JoinPath(src, fd.cFileName);
    std::wstring to = JoinPath(dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ok = CopyTree(from, to) && ok;
    } else {
      ok = CopyFileW(from.c_str(), to.c_str(), FALSE) && ok;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return ok;
}

bool DeleteTree(const std::wstring& path) {
  if (!DirExists(path)) return true;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
      std::wstring p = JoinPath(path, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteTree(p);
      } else {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  return RemoveDirectoryW(path.c_str()) != FALSE;
}

// ---------------------------------------------------------------- 网络

static bool HttpGetRaw(const std::wstring& url, std::vector<char>& out, bool* networkError) {
  out.clear();
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[4096] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4095;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    if (networkError) *networkError = true;
    return false;
  }

  HINTERNET session = WinHttpOpen(L"TeacherBox/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    if (networkError) *networkError = true;
    return false;
  }
  WinHttpSetTimeouts(session, 15000, 15000, 20000, 30000);

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
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                            &status, &len, nullptr);
        if (status >= 200 && status < 300) {
          DWORD avail = 0;
          do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t old = out.size();
            out.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, out.data() + old, avail, &read)) { out.resize(old); break; }
            out.resize(old + read);
            if (out.size() > 128u * 1024u * 1024u) break;
          } while (avail > 0);
          ok = true;
        }
      }
      WinHttpCloseHandle(req);
    } else if (networkError) {
      *networkError = true;
    }
    WinHttpCloseHandle(conn);
  } else if (networkError) {
    *networkError = true;
  }
  WinHttpCloseHandle(session);
  return ok;
}

static std::wstring AltUrl(const std::wstring& url) {
  const std::wstring raw = L"https://raw.githubusercontent.com/";
  const std::wstring cdn = L"https://cdn.jsdelivr.net/gh/";
  if (url.compare(0, raw.size(), raw) == 0) {
    std::wstring rest = url.substr(raw.size());
    size_t p1 = rest.find(L'/');
    if (p1 == std::wstring::npos) return std::wstring();
    size_t p2 = rest.find(L'/', p1 + 1);
    if (p2 == std::wstring::npos) return std::wstring();
    size_t p3 = rest.find(L'/', p2 + 1);
    if (p3 == std::wstring::npos) return std::wstring();
    return cdn + rest.substr(0, p2) + L"@" + rest.substr(p2 + 1, p3 - p2 - 1) + rest.substr(p3);
  }
  if (url.compare(0, cdn.size(), cdn) == 0) {
    std::wstring rest = url.substr(cdn.size());
    size_t at = rest.find(L'@');
    if (at == std::wstring::npos) return std::wstring();
    size_t p = rest.find(L'/', at);
    if (p == std::wstring::npos) return std::wstring();
    return raw + rest.substr(0, at) + L"/" + rest.substr(at + 1, p - at - 1) + rest.substr(p);
  }
  return std::wstring();
}

bool HttpGetText(const std::wstring& url, std::string& out, bool* networkError) {
  if (networkError) *networkError = false;
  std::vector<char> buf;
  if (HttpGetRaw(url, buf, networkError)) {
    out.assign(buf.begin(), buf.end());
    return true;
  }
  std::wstring alt = AltUrl(url);
  if (!alt.empty() && HttpGetRaw(alt, buf, nullptr)) {
    out.assign(buf.begin(), buf.end());
    return true;
  }
  return false;
}

bool HttpGetFile(const std::wstring& url, const std::wstring& dstPath) {
  std::vector<char> buf;
  if (!HttpGetRaw(url, buf, nullptr)) {
    std::wstring alt = AltUrl(url);
    if (alt.empty() || !HttpGetRaw(alt, buf, nullptr)) return false;
  }
  if (buf.empty()) return false;

  std::wstring parent = dstPath;
  size_t pos = parent.find_last_of(L"\\/");
  if (pos != std::wstring::npos) EnsureDir(parent.substr(0, pos));

  HANDLE h = CreateFileW(dstPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) && written == buf.size();
  CloseHandle(h);
  return ok;
}

// ---------------------------------------------------------------- 时间

std::string NowDateTime() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);
  return buf;
}

std::string NowStampForFile() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);
  return buf;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------- 日志

static std::string LogFilePath(const std::wstring& tag) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char name[128];
  snprintf(name, sizeof(name), "%ls-%04d-%02d-%02d.log", tag.c_str(), st.wYear, st.wMonth, st.wDay);
  return ToUtf8(JoinPath(LogDir(), ToWide(name)));
}

void Log(const std::wstring& tag, const std::string& message) {
  std::string path = LogFilePath(tag);
  HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string line = "[" + NowDateTime() + "] " + message + "\r\n";
  DWORD written = 0;
  WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
  CloseHandle(h);
}

std::string TailLog(const std::wstring& tag, size_t maxBytes) {
  std::string path = LogFilePath(tag);
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  LARGE_INTEGER pos{};
  pos.QuadPart = size.QuadPart > (LONGLONG)maxBytes ? size.QuadPart - (LONGLONG)maxBytes : 0;
  SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
  std::string data;
  char buf[4096];
  DWORD read = 0;
  while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
    data.append(buf, read);
    if (data.size() > maxBytes * 2) break;
  }
  CloseHandle(h);
  return data;
}

// ---------------------------------------------------------------- 进程

RunResult RunCommand(const std::wstring& commandLine, DWORD timeoutMs, const std::wstring& workDir) {
  RunResult result;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return result;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::wstring cmd = commandLine;
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    return result;
  }
  result.started = true;

  std::string out;
  char buf[8192];
  const ULONGLONG start = GetTickCount64();

  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
      DWORD got = 0;
      if (ReadFile(rd, buf, want, &got, nullptr) && got > 0) {
        out.append(buf, got);
        if (out.size() > 4u * 1024u * 1024u) out.resize(4u * 1024u * 1024u);
      }
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) break;
    if (timeoutMs != 0 && GetTickCount64() - start > timeoutMs) {
      result.timedOut = true;
      TerminateProcess(pi.hProcess, 124);
      WaitForSingleObject(pi.hProcess, 3000);
      break;
    }
  }

  // 进程结束后把管道里剩余数据读干净
  for (int i = 0; i < 64; ++i) {
    DWORD avail = 0;
    if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
    DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
    DWORD got = 0;
    if (!ReadFile(rd, buf, want, &got, nullptr) || got == 0) break;
    out.append(buf, got);
  }

  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  result.exitCode = (int)code;

  CloseHandle(rd);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  result.output = out;
  return result;
}

bool LaunchDetached(const std::wstring& commandLine, const std::wstring& workDir) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = commandLine;
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
  }

  // CreateProcess 只认可执行文件，ms-settings: 这类 shell 协议、开始菜单里的快捷方式对它都是
  // "找不到文件"。这类走 ShellExecute，顺带把 UAC 提升也交给 shell 处理。
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  sei.lpVerb = L"open";
  sei.lpFile = commandLine.c_str();
  sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) return false;
  if (sei.hProcess) CloseHandle(sei.hProcess);
  return true;
}

void OpenFolder(const std::wstring& path) {
  EnsureDir(path);
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- 系统信息

static std::string RegStr(HKEY root, const wchar_t* sub, const wchar_t* name) {
  wchar_t buf[512] = {0};
  DWORD size = sizeof(buf) - 1;
  DWORD type = 0;
  if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
    return std::string();
  return ToUtf8(buf);
}

std::string GetSystemSummary() {
  std::string os = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          L"ProductName");
  std::string build = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             L"CurrentBuild");
  std::string disp = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                            L"DisplayVersion");
  wchar_t name[256] = {0};
  DWORD nameSize = sizeof(name) / sizeof(name[0]);
  GetComputerNameW(name, &nameSize);

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  const char* arch = "unknown";
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
  }

  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {0};
  GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

  char buf[1024];
  snprintf(buf, sizeof(buf),
           "OS: %s (%s build %s)\nArch: %s\nCPU cores: %lu\nComputer: %ls\nLocale: %s\nSnapshot: %s",
           os.empty() ? "Windows" : os.c_str(), disp.empty() ? "-" : disp.c_str(),
           build.empty() ? "-" : build.c_str(), arch, (unsigned long)si.dwNumberOfProcessors, name,
           ToUtf8(locale).c_str(), NowDateTime().c_str());
  return buf;
}

// ---------------------------------------------------------------- 托盘/窗口图标

HICON MakeAppIcon(int size) {
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = size;   // 自底向上
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color) { DeleteDC(dc); return LoadIconW(nullptr, IDI_APPLICATION); }

  HGDIOBJ oldBmp = SelectObject(dc, color);

  // 朱红底 + 米白衬线 T：和界面上那枚“印章”logo 一致
  HBRUSH bg = CreateSolidBrush(RGB(171, 58, 44));
  HGDIOBJ oldBrush = SelectObject(dc, bg);
  HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
  RoundRect(dc, 0, 0, size, size, (int)(size * 0.16f), (int)(size * 0.16f));

  // 内圈细边，像印章的边框（小图标不画，避免糊成一团）
  if (size >= 24) {
    HPEN innerPen = CreatePen(PS_SOLID, 1, RGB(253, 246, 232));
    HGDIOBJ prevPen = SelectObject(dc, innerPen);
    HGDIOBJ prevBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int inset = size / 14;
    RoundRect(dc, inset, inset, size - inset, size - inset,
              (int)(size * 0.08f), (int)(size * 0.08f));
    SelectObject(dc, prevBrush);
    SelectObject(dc, prevPen);
    DeleteObject(innerPen);
  }

  // 中间的 “T”
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(253, 246, 232));
  LOGFONTW lf{};
  lf.lfHeight = -(int)(size * 0.64f);
  lf.lfWeight = FW_NORMAL;
  lf.lfQuality = ANTIALIASED_QUALITY;
  wcscpy_s(lf.lfFaceName, L"Georgia");
  HFONT font = CreateFontIndirectW(&lf);
  HGDIOBJ oldFont = SelectObject(dc, font);
  RECT r{0, 0, size, size};
  DrawTextW(dc, L"T", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  SelectObject(dc, oldFont);
  DeleteObject(font);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(bg);
  SelectObject(dc, oldBmp);

  // 掩码位图：全不透明
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HICON icon = CreateIconIndirect(&ii);

  DeleteObject(mask);
  DeleteObject(color);
  DeleteDC(dc);
  if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
  return icon;
}

} // namespace tb
