#include "subprocess.hpp"

#include <sys/socket.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

extern char** environ;

namespace shigoku::subprocess {

namespace {

using Clock = std::chrono::steady_clock;

// The child's stdin is our end of a socket pair rather than a pipe: a write
// after the child has gone must come back as EPIPE, not as SIGPIPE killing
// the whole app, and sockets have a per-call (MSG_NOSIGNAL) or per-socket
// (SO_NOSIGPIPE) way to ask for that; pipes have none.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::string errno_text(int e) { return std::strerror(e); }

Failure fail(Failure::Kind k, std::string d) { return Failure{k, std::move(d)}; }

void close_all(std::initializer_list<int> fds) {
  for (const int fd : fds) {
    if (fd >= 0) ::close(fd);
  }
}

// Reap `child`, after SIGKILL when asked. SIGKILL cannot be ignored, so the
// wait is short.
void reap(pid_t child, bool kill_first) {
  if (kill_first) ::kill(child, SIGKILL);
  int status = 0;
  for (;;) {
    const pid_t w = ::waitpid(child, &status, 0);
    if (w == child || (w < 0 && errno != EINTR)) return;
  }
}

}  // namespace

Result<Output, Failure> run(const std::vector<std::string>& argv, std::string_view input,
                            int timeout_ms, std::size_t max_out) {
  if (argv.empty() || argv.front().empty()) {
    return err(fail(Failure::Kind::Spawn, "empty command"));
  }
  int in_sock[2] = {-1, -1};   // the child reads [0]; we write [1].
  int out_pipe[2] = {-1, -1};  // the child writes [1]; we read [0].
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, in_sock) != 0 || ::pipe(out_pipe) != 0) {
    const int e = errno;
    close_all({in_sock[0], in_sock[1], out_pipe[0], out_pipe[1]});
    return err(fail(Failure::Kind::Spawn, "pipe: " + errno_text(e)));
  }
  for (const int fd : {in_sock[0], in_sock[1], out_pipe[0], out_pipe[1]}) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
#if defined(SO_NOSIGPIPE)
  {
    int one = 1;
    ::setsockopt(in_sock[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in_sock[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, argv.front().c_str(), &fa, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close_all({in_sock[0], out_pipe[1]});  // the child's ends are its own now.
  if (rc != 0) {
    close_all({in_sock[1], out_pipe[0]});
    // rc is the errno value itself; ENOENT is the missing-dependency case.
    const auto kind = rc == ENOENT ? Failure::Kind::NotFound : Failure::Kind::Spawn;
    return err(fail(kind, argv.front() + ": " + errno_text(rc)));
  }

  // One select loop drives both ends, so a child that answers (or dies)
  // before it has read all of its input cannot wedge us, and the bound
  // covers the whole exchange.
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  int wfd = in_sock[1];
  int rfd = out_pipe[0];
  std::size_t written = 0;
  Output out;
  auto give_up = [&](Failure f) {
    close_all({wfd, rfd});
    reap(child, /*kill_first=*/true);
    return err(std::move(f));
  };
  if (input.empty()) {
    ::close(wfd);
    wfd = -1;
  }
  char buf[16384];
  while (rfd >= 0) {
    const auto now = Clock::now();
    if (now >= deadline) return give_up(fail(Failure::Kind::Timeout, argv.front() + ": timed out"));
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(left.count() / 1000000);
    tv.tv_usec = static_cast<suseconds_t>(left.count() % 1000000);
    fd_set rset;
    fd_set wset;
    FD_ZERO(&rset);
    FD_ZERO(&wset);
    FD_SET(rfd, &rset);
    int maxfd = rfd;
    if (wfd >= 0) {
      FD_SET(wfd, &wset);
      maxfd = std::max(maxfd, wfd);
    }
    const int n = ::select(maxfd + 1, &rset, wfd >= 0 ? &wset : nullptr, nullptr, &tv);
    if (n < 0) {
      if (errno == EINTR) continue;
      return give_up(fail(Failure::Kind::Io, "select: " + errno_text(errno)));
    }
    if (n == 0) continue;  // the deadline check above decides.
    if (wfd >= 0 && FD_ISSET(wfd, &wset)) {
      const ssize_t w = ::send(wfd, input.data() + written, input.size() - written, kSendFlags);
      if (w < 0) {
        if (errno != EINTR && errno != EAGAIN) {
          // EPIPE and friends: the child stopped reading; what it has is
          // what it gets.
          ::close(wfd);
          wfd = -1;
        }
      } else {
        written += static_cast<std::size_t>(w);
        if (written == input.size()) {
          ::close(wfd);
          wfd = -1;
        }
      }
    }
    if (FD_ISSET(rfd, &rset)) {
      const ssize_t r = ::read(rfd, buf, sizeof(buf));
      if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return give_up(fail(Failure::Kind::Io, "read: " + errno_text(errno)));
      }
      if (r == 0) {
        ::close(rfd);
        rfd = -1;
        break;
      }
      if (out.out.size() + static_cast<std::size_t>(r) > max_out) {
        return give_up(fail(Failure::Kind::Io, argv.front() + ": more than " +
                                                   std::to_string(max_out) + " bytes of output"));
      }
      out.out.append(buf, static_cast<std::size_t>(r));
    }
  }
  if (wfd >= 0) {
    ::close(wfd);
    wfd = -1;
  }
  // stdout is closed: the child is on its way out. Wait for it under the
  // same bound, then it is reaped either way.
  for (;;) {
    int status = 0;
    const pid_t w = ::waitpid(child, &status, WNOHANG);
    if (w == child) {
      out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      return out;
    }
    if (w < 0 && errno != EINTR) {
      return err(fail(Failure::Kind::Io, "waitpid: " + errno_text(errno)));
    }
    if (Clock::now() >= deadline) {
      reap(child, /*kill_first=*/true);
      return err(fail(Failure::Kind::Timeout, argv.front() + ": timed out"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace shigoku::subprocess
