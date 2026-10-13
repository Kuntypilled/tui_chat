// gmhelper.cpp
//
// Distributed under the MIT license, see LICENSE for details.

#include "gmhelper.h"

#include <cstdlib>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "timeutil.h"

static std::string GetEnv(const char* p_Name)
{
  const char* value = getenv(p_Name);
  return (value != nullptr) ? std::string(value) : std::string();
}

static std::string HomeDir()
{
  return GetEnv("HOME");
}

static std::string XdgDir(const char* p_Env, const char* p_HomeRelative)
{
  const std::string value = GetEnv(p_Env);
  if (!value.empty()) return value;

  return HomeDir() + "/" + p_HomeRelative;
}

static bool IsExecutable(const std::string& p_Path)
{
  return !p_Path.empty() && (access(p_Path.c_str(), X_OK) == 0) && !FileUtil::IsDir(p_Path);
}

std::string GmHelper::DefaultSocketPath()
{
  // same rule the helper itself uses: runtime dir when set, else its cache dir
  const std::string runtimeDir = GetEnv("XDG_RUNTIME_DIR");
  if (!runtimeDir.empty())
  {
    return runtimeDir + "/omachat/daemon.sock";
  }

  return CacheDir() + "/daemon.sock";
}

std::string GmHelper::DataDir()
{
  return XdgDir("XDG_DATA_HOME", ".local/share") + "/omachat";
}

std::string GmHelper::CacheDir()
{
  return XdgDir("XDG_CACHE_HOME", ".cache") + "/omachat";
}

std::string GmHelper::FindBinary(const std::string& p_Configured)
{
  if (!p_Configured.empty())
  {
    const std::string path = FileUtil::ExpandPath(p_Configured);
    return IsExecutable(path) ? path : std::string();
  }

  const std::string fromEnv = GetEnv("TUI_CHAT_HELPER");
  if (IsExecutable(fromEnv)) return fromEnv;

  const std::string inDataDir = XdgDir("XDG_DATA_HOME", ".local/share") + "/tui_chat/omachatd";
  if (IsExecutable(inDataDir)) return inDataDir;

  const std::string pathVar = GetEnv("PATH");
  size_t start = 0;
  while (start <= pathVar.size())
  {
    size_t end = pathVar.find(':', start);
    if (end == std::string::npos) end = pathVar.size();

    const std::string dir = pathVar.substr(start, end - start);
    if (!dir.empty())
    {
      const std::string candidate = dir + "/omachatd";
      if (IsExecutable(candidate)) return candidate;
    }

    start = end + 1;
  }

  return std::string();
}

bool GmHelper::Start(const std::string& p_Binary, const std::string& p_SocketPath, const std::string& p_LogPath)
{
  std::vector<std::string> args = { p_Binary, "-log-level", "warn" };
  if (!p_SocketPath.empty())
  {
    args.push_back("-socket");
    args.push_back(p_SocketPath);
  }

  std::vector<char*> argv;
  for (auto& arg : args)
  {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }

  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0)
  {
    // first child: detach from the terminal, then fork again so the helper is
    // not our child and cannot become a zombie or be signalled with us
    if (setsid() < 0) _exit(1);

    pid_t pid2 = fork();
    if (pid2 < 0) _exit(1);
    if (pid2 > 0) _exit(0);

    if (chdir("/") != 0) _exit(1);

    int devNull = open("/dev/null", O_RDWR);
    int logFd = p_LogPath.empty() ? -1 : open(p_LogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (devNull >= 0) dup2(devNull, STDIN_FILENO);
    dup2((logFd >= 0) ? logFd : devNull, STDOUT_FILENO);
    dup2((logFd >= 0) ? logFd : devNull, STDERR_FILENO);
    for (int fd = 3; fd < 256; ++fd)
    {
      close(fd);
    }

    execv(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

bool GmHelper::WaitForSocket(const std::string& p_SocketPath, int p_TimeoutMs)
{
  const int64_t deadline = TimeUtil::GetCurrentTimeMSec() + p_TimeoutMs;
  while (true)
  {
    struct stat st;
    if ((stat(p_SocketPath.c_str(), &st) == 0) && S_ISSOCK(st.st_mode))
    {
      return true;
    }

    if (TimeUtil::GetCurrentTimeMSec() >= deadline) return false;

    TimeUtil::Sleep(0.1);
  }
}
