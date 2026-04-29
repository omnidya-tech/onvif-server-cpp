// =============================================================================
// onvif-rtsp-watchdog.cpp -- C++17 port of onvif-rtsp-watchdog.py
//
// One-shot health probe (intended to run from a systemd timer). Sends an
// RTSP OPTIONS request to the local server. OPTIONS is intentional (NOT
// DESCRIBE) so the probe does NOT preroll the media factory and does NOT
// open the dashcam-service streaming valve — preserving the lazy-flag
// encoder-power optimisation.
//
// On failure: increments persistent counter at /run/onvif-rtsp-watchdog.fail.
// On FAIL_THRESHOLD consecutive failures: clears the counter and runs
// `systemctl restart onvif-rtsp.service`.
// On success: clears the counter.
//
// Always exits 0 (the systemd-timer doesn't care about probe-individual
// failures; aggregation is what matters, hence the counter).
// =============================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr const char* HOST           = "127.0.0.1";
constexpr int         TIMEOUT_SEC    = 5;
constexpr const char* STATE_FILE     = "/run/onvif-rtsp-watchdog.fail";
constexpr int         FAIL_THRESHOLD = 2;
constexpr const char* UNIT           = "onvif-rtsp.service";

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_WARNING, fmt, ap);
    va_end(ap);
}

void log_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_ERR, fmt, ap);
    va_end(ap);
}

bool set_socket_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return false;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return false;
    return true;
}

// Returns empty string on success, error message on failure.
std::string probe(int port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::string("socket: ") + std::strerror(errno);

    if (!set_socket_timeout(fd, TIMEOUT_SEC)) {
        ::close(fd);
        return "setsockopt failed";
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, HOST, &addr.sin_addr) != 1) {
        ::close(fd);
        return "inet_pton failed";
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return err;
    }

    char request[512];
    int  reqlen = std::snprintf(request, sizeof(request),
        "OPTIONS rtsp://%s:%d%s RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "User-Agent: onvif-rtsp-watchdog/2\r\n"
        "\r\n",
        HOST, port, path.c_str());
    if (reqlen <= 0 || reqlen >= static_cast<int>(sizeof(request))) {
        ::close(fd);
        return "request snprintf overflow";
    }

    ssize_t off = 0;
    while (off < reqlen) {
        ssize_t n = ::send(fd, request + off, reqlen - off, MSG_NOSIGNAL);
        if (n < 0) {
            std::string err = std::string("send: ") + std::strerror(errno);
            ::close(fd);
            return err;
        }
        off += n;
    }

    // Read until we see the end-of-headers marker or hit a sane cap.
    std::string response;
    response.reserve(1024);
    char buf[4096];
    while (response.size() < 16384) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            std::string err = std::string("recv: ") + std::strerror(errno);
            ::close(fd);
            return err;
        }
        if (n == 0) break; // peer closed
        response.append(buf, static_cast<size_t>(n));
        if (response.find("\r\n\r\n") != std::string::npos) break;
    }
    ::close(fd);

    if (response.compare(0, 12, "RTSP/1.0 200") != 0) {
        std::string snippet = response.substr(0, std::min<size_t>(80, response.size()));
        return "unexpected status: " + snippet;
    }
    return "";
}

int read_count() {
    std::ifstream f(STATE_FILE);
    if (!f) return 0;
    int n = 0;
    f >> n;
    if (!f && !f.eof()) return 0;
    return n;
}

void write_count(int n) {
    std::ofstream f(STATE_FILE);
    if (!f) return;
    f << n;
}

void clear_count() {
    if (::unlink(STATE_FILE) != 0 && errno != ENOENT) {
        // best-effort, no log
    }
}

int run_systemctl_restart() {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // child: replace with systemctl
        execl("/bin/systemctl", "systemctl", "restart", UNIT, nullptr);
        // execl only returns on failure
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

} // namespace

int main() {
    ::openlog("onvif-rtsp-watchdog", LOG_PID, LOG_DAEMON);

    int port = std::atoi(env_or("RTSP_PORT", "554"));
    if (port <= 0 || port > 65535) port = 554;
    std::string path = env_or("RTSP_PATH", "/stream");

    std::string err = probe(port, path);
    if (err.empty()) {
        clear_count();
        ::closelog();
        return 0;
    }

    int count = read_count() + 1;
    write_count(count);
    log_warn("OPTIONS probe failed (%d/%d): %s", count, FAIL_THRESHOLD, err.c_str());

    if (count >= FAIL_THRESHOLD) {
        log_err("restarting %s after %d consecutive failures", UNIT, count);
        clear_count();
        run_systemctl_restart();
    }

    ::closelog();
    return 0;
}
