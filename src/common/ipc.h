#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tb {

// 简单的命名管道通道：按行传输纯文本协议。
//   main.exe 侧 Listen()，update.exe / gui.exe 侧 Connect()。
// 协议（单行）：
//   READY            子进程已就绪
//   STATUS <任意文本> 进度文本
//   EXIT_USER        用户主动关闭窗口
//   LOG <任意文本>    日志
//   SHUTDOWN         父进程要求退出
class IpcChannel {
public:
  IpcChannel() = default;
  ~IpcChannel();

  IpcChannel(const IpcChannel&)            = delete;
  IpcChannel& operator=(const IpcChannel&) = delete;

  bool Listen(const std::wstring& fullPipeName);
  bool Connect(const std::wstring& fullPipeName, int timeoutMs);

  bool SendLine(const std::string& line);

  void StartReader();
  std::vector<std::string> TakeLines();
  bool SawReady() const { return ready_; }
  bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

  void Close();

private:
  void ReaderLoop(bool server);

  HANDLE                   handle_ = INVALID_HANDLE_VALUE;
  std::wstring             name_;
  std::thread              reader_;
  std::atomic<bool>        running_{false};
  std::atomic<bool>        ready_{false};
  std::mutex               mu_;
  std::mutex               writeMu_;
  std::vector<std::string> lines_;
  std::string              buffer_;
};

} // namespace tb
