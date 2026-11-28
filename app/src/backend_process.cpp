#include "backend_process.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace gsa {

std::string random_token() {
  std::random_device rd;
  std::string t;
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    const unsigned v = rd() & 0xff;
    t += hex[v >> 4];
    t += hex[v & 15];
  }
  return t;
}

namespace {
int parse_ready(const std::string& line) {
  auto j = nlohmann::json::parse(line, nullptr, false);
  if (j.is_discarded() || j.value("event", "") != "ready") return 0;
  return j.value("port", 0);
}
}  // namespace

#ifdef _WIN32

struct BackendProcess::Impl {
  PROCESS_INFORMATION pi{};
  HANDLE stdin_w = nullptr, stdout_r = nullptr;
  HANDLE job = nullptr;
};

BackendProcess::BackendProcess(const std::filesystem::path& exe, const std::filesystem::path& data, double timeout_s)
    : impl_(std::make_unique<Impl>()), token_(random_token()) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE in_r, out_w;
  if (!CreatePipe(&in_r, &impl_->stdin_w, &sa, 0) || !CreatePipe(&impl_->stdout_r, &out_w, &sa, 0))
    throw std::runtime_error("CreatePipe failed");
  SetHandleInformation(impl_->stdin_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(impl_->stdout_r, HANDLE_FLAG_INHERIT, 0);

  // A job object kills the back-end with us, even if we crash.
  impl_->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &li, sizeof(li));

  SetEnvironmentVariableW(L"GSA_TOKEN", std::wstring(token_.begin(), token_.end()).c_str());
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --data \"" + data.wstring() + L"\" --port 0 --exit-with-stdin";
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_r;
  si.hStdOutput = out_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, nullptr, &si, &impl_->pi);
  SetEnvironmentVariableW(L"GSA_TOKEN", nullptr);
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (!ok) throw std::runtime_error("cannot start back-end: " + exe.string());
  AssignProcessToJobObject(impl_->job, impl_->pi.hProcess);
  ResumeThread(impl_->pi.hThread);

  std::string line;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    DWORD avail = 0;
    if (!PeekNamedPipe(impl_->stdout_r, nullptr, 0, nullptr, &avail, nullptr)) break;  // child exited
    if (avail == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    char c;
    DWORD got = 0;
    if (!ReadFile(impl_->stdout_r, &c, 1, &got, nullptr) || got == 0) break;
    if (c == '\n') {
      if ((port_ = parse_ready(line))) return;
      line.clear();
    } else {
      line += c;
    }
  }
  throw std::runtime_error("back-end did not start (see its messages above)");
}

BackendProcess::~BackendProcess() {
  if (impl_->stdin_w) CloseHandle(impl_->stdin_w);  // back-end sees EOF and stops
  if (impl_->pi.hProcess) {
    if (WaitForSingleObject(impl_->pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(impl_->pi.hProcess, 1);
    CloseHandle(impl_->pi.hProcess);
    CloseHandle(impl_->pi.hThread);
  }
  if (impl_->stdout_r) CloseHandle(impl_->stdout_r);
  if (impl_->job) CloseHandle(impl_->job);
}

#else

struct BackendProcess::Impl {
  pid_t pid = -1;
  int stdin_w = -1, stdout_r = -1;
};

BackendProcess::BackendProcess(const std::filesystem::path& exe, const std::filesystem::path& data, double timeout_s)
    : impl_(std::make_unique<Impl>()), token_(random_token()) {
  int in[2], out[2];
  if (pipe(in) || pipe(out)) throw std::runtime_error("pipe failed");
  fcntl(in[1], F_SETFD, FD_CLOEXEC);
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);

  // environment: ours plus GSA_TOKEN (never on the command line)
  std::vector<std::string> env_s;
  for (char** e = environ; *e; ++e)
    if (std::string(*e).rfind("GSA_TOKEN=", 0) != 0) env_s.emplace_back(*e);
  env_s.push_back("GSA_TOKEN=" + token_);
  std::vector<char*> envp;
  for (auto& s : env_s) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::string exe_s = exe.string(), data_s = data.string();
  std::vector<std::string> args = {exe_s, "--data", data_s, "--port", "0", "--exit-with-stdin", "--live", "--no-plane-poll"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  const int rc = posix_spawn(&impl_->pid, exe_s.c_str(), &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  close(in[0]);
  close(out[1]);
  impl_->stdin_w = in[1];
  impl_->stdout_r = out[0];
  if (rc != 0) throw std::runtime_error("cannot start back-end: " + exe_s);

  std::string line;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd p{impl_->stdout_r, POLLIN, 0};
    if (poll(&p, 1, 50) <= 0) continue;
    char c;
    if (read(impl_->stdout_r, &c, 1) != 1) break;  // child exited
    if (c == '\n') {
      if ((port_ = parse_ready(line))) return;
      line.clear();
    } else {
      line += c;
    }
  }
  throw std::runtime_error("back-end did not start (see its messages above)");
}

BackendProcess::~BackendProcess() {
  if (impl_->stdin_w >= 0) close(impl_->stdin_w);  // back-end sees EOF and stops
  if (impl_->pid > 0) {
    for (int i = 0; i < 60; ++i) {
      if (waitpid(impl_->pid, nullptr, WNOHANG) == impl_->pid) {
        impl_->pid = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (impl_->pid > 0) {
      kill(impl_->pid, SIGTERM);
      waitpid(impl_->pid, nullptr, 0);
    }
  }
  if (impl_->stdout_r >= 0) close(impl_->stdout_r);
}

#endif

}  // namespace gsa
