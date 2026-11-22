#include "ipc.h"

#include <chrono>
#include <cstdio>

#include "util.h"

namespace tb {

IpcChannel::~IpcChannel() { Close(); }

bool IpcChannel::Listen(const std::wstring& fullPipeName) {
  name_ = fullPipeName;
  // 关键：FILE_FLAG_OVERLAPPED。同步句柄上挂起的 ReadFile 会把同一句柄的
  // WriteFile 一起串行化，导致子进程发 READY 时直接卡死。
  handle_ = CreateNamedPipeW(fullPipeName.c_str(),
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1, 8192, 8192, 0, nullptr);
  return handle_ != INVALID_HANDLE_VALUE;
}

void IpcChannel::StartReader() {
  if (handle_ == INVALID_HANDLE_VALUE || running_) return;
  running_ = true;
  reader_ = std::thread(&IpcChannel::ReaderLoop, this, true);
}

bool IpcChannel::Connect(const std::wstring& fullPipeName, int timeoutMs) {
  name_ = fullPipeName;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    handle_ = CreateFileW(fullPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) break;
    if (std::chrono::steady_clock::now() > deadline) return false;
    Sleep(30);
  }
  running_ = true;
  reader_ = std::thread(&IpcChannel::ReaderLoop, this, false);
  return true;
}

void IpcChannel::ReaderLoop(bool server) {
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) return;
  OVERLAPPED ov{};
  ov.hEvent = ev;

  if (server) {
    while (running_) {
      ResetEvent(ev);
      if (ConnectNamedPipe(handle_, &ov)) break;
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) break;
      if (err == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(ev, 100);
        if (w == WAIT_OBJECT_0) {
          DWORD dummy = 0;
          if (GetOverlappedResult(handle_, &ov, &dummy, FALSE)) break;
        }
      } else {
        Sleep(20);
      }
    }
  }

  char buf[4096];
  while (running_) {
    ResetEvent(ev);
    DWORD got = 0;
    BOOL ok = ReadFile(handle_, buf, sizeof(buf) - 1, &got, &ov);
    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        // 等这次读完成（可被 CancelIoEx / Close 中断）
        for (;;) {
          DWORD w = WaitForSingleObject(ev, 100);
          if (w == WAIT_OBJECT_0) break;
          if (!running_) { CloseHandle(ev); return; }
        }
        if (!GetOverlappedResult(handle_, &ov, &got, FALSE)) {
          DWORD rerr = GetLastError();
          if (rerr == ERROR_BROKEN_PIPE || rerr == ERROR_OPERATION_ABORTED ||
              rerr == ERROR_INVALID_HANDLE)
            break;
          Sleep(10);
          continue;
        }
      } else {
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE)
          break;
        Sleep(10);
        continue;
      }
    }
    if (got == 0) continue;

    buf[got] = '\0';
    std::lock_guard<std::mutex> lk(mu_);
    buffer_.append(buf, got);
    size_t pos;
    while ((pos = buffer_.find('\n')) != std::string::npos) {
      std::string line = buffer_.substr(0, pos);
      buffer_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (line == "READY") ready_ = true;
      lines_.push_back(line);
    }
  }
  CloseHandle(ev);
}

bool IpcChannel::SendLine(const std::string& line) {
  if (handle_ == INVALID_HANDLE_VALUE) return false;
  std::string data = line + "\n";

  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) return false;
  OVERLAPPED ov{};
  ov.hEvent = ev;

  DWORD written = 0;
  BOOL ok = false;

  {
    std::lock_guard<std::mutex> writeLock(writeMu_);
    ResetEvent(ev);
    ok = WriteFile(handle_, data.data(), (DWORD)data.size(), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      if (WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0)
        ok = GetOverlappedResult(handle_, &ov, &written, FALSE);
      else
        CancelIoEx(handle_, &ov);
    }
  }

  if (!ok) {
    char buf[64];
    snprintf(buf, sizeof(buf), "write failed err=%lu", GetLastError());
    Log(L"ipc", buf);
  }
  CloseHandle(ev);
  return ok != FALSE;
}

std::vector<std::string> IpcChannel::TakeLines() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> out = std::move(lines_);
  lines_.clear();
  return out;
}

void IpcChannel::Close() {
  running_ = false;
  HANDLE h = handle_;
  handle_ = INVALID_HANDLE_VALUE;
  if (h != INVALID_HANDLE_VALUE) {
    CancelIoEx(h, nullptr);
    CloseHandle(h);
  }
  if (reader_.joinable()) {
    if (reader_.get_id() == std::this_thread::get_id())
      reader_.detach();
    else
      reader_.join();
  }
}

} // namespace tb
