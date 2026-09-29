#include "tor_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string toHex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) { out.push_back(d[c >> 4]); out.push_back(d[c & 0xf]); }
    return out;
}

// Connect a TCP socket to 127.0.0.1:port. -1 on failure.
int connectLoopback(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return -1; }
    return fd;
}

std::string recvAll(int fd, int maxMs = 4000) {
    std::string out;
    char buf[4096];
    timeval tv{}; tv.tv_sec = maxMs / 1000; tv.tv_usec = (maxMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        // Control replies terminate with a "250 OK"/"5xx" final line; stop once we have one.
        if (out.find("\r\n250 OK\r\n") != std::string::npos ||
            out.rfind("250 OK\r\n") != std::string::npos ||
            (out.size() >= 5 && (out.compare(0, 3, "515") == 0 || out.compare(0, 3, "551") == 0)))
            break;
        if (n < static_cast<ssize_t>(sizeof(buf))) break;
    }
    return out;
}

bool sendLine(int fd, const std::string& line) {
    std::string l = line + "\r\n";
    return ::send(fd, l.data(), l.size(), 0) == static_cast<ssize_t>(l.size());
}

} // namespace

TorClient::~TorClient() {
    if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 20; ++i) { if (::waitpid(m_pid, &st, WNOHANG) == m_pid) break; usleep(100000); }
        ::kill(m_pid, SIGKILL);
        ::waitpid(m_pid, &st, WNOHANG);
    }
}

int TorClient::pickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return 0; }
    socklen_t len = sizeof(a);
    int port = 0;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

bool TorClient::running() const {
    if (m_pid <= 0) return false;
    return ::kill(m_pid, 0) == 0;
}

bool TorClient::ensureStarted(const std::string& dataDir,
                              const std::string& torBin,
                              std::string& errorOut) {
    if (running()) return true;

    if (torBin.empty()) { errorOut = "tor_not_found"; return false; }
    ::mkdir(dataDir.c_str(), 0700);

    m_dataDir = dataDir;
    m_socksPort = pickFreePort();
    m_controlPort = pickFreePort();
    if (m_socksPort == 0 || m_controlPort == 0) { errorOut = "no_free_port"; return false; }

    const std::string torrc = dataDir + "/torrc";
    {
        std::ofstream f(torrc);
        if (!f) { errorOut = "torrc_write_failed"; return false; }
        f << "SocksPort 127.0.0.1:" << m_socksPort << "\n"
          << "ControlPort 127.0.0.1:" << m_controlPort << "\n"
          << "CookieAuthentication 1\n"
          << "DataDirectory " << dataDir << "\n"
          << "Log notice file " << dataDir << "/notice.log\n";
    }

    pid_t pid = ::fork();
    if (pid < 0) { errorOut = "fork_failed"; return false; }
    if (pid == 0) {
        // child: silence stdio, exec tor
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, 1); ::dup2(devnull, 2); }
        execl(torBin.c_str(), torBin.c_str(), "-f", torrc.c_str(), (char*)nullptr);
        _exit(127);
    }
    m_pid = pid;
    // Give tor a moment to create the control socket + cookie; bootstrap polls after.
    usleep(300000);
    return true;
}

bool TorClient::authenticate(int fd) {
    // Cookie auth: read control_auth_cookie (32 raw bytes), send hex.
    std::ifstream ck(m_dataDir + "/control_auth_cookie", std::ios::binary);
    if (!ck) return false;
    std::stringstream ss; ss << ck.rdbuf();
    std::string cookie = ss.str();
    if (cookie.empty()) return false;
    if (!sendLine(fd, "AUTHENTICATE " + toHex(cookie))) return false;
    std::string reply = recvAll(fd, 2000);
    return reply.find("250 OK") != std::string::npos;
}

std::string TorClient::controlQuery(const std::string& command) {
    if (m_controlPort == 0) return {};
    int fd = connectLoopback(m_controlPort);
    if (fd < 0) return {};
    std::string out;
    if (authenticate(fd)) {
        if (sendLine(fd, command)) out = recvAll(fd, 4000);
    }
    ::close(fd);
    return out;
}

int TorClient::bootstrapPercent() {
    std::string r = controlQuery("GETINFO status/bootstrap-phase");
    if (r.empty()) return -1;
    auto p = r.find("PROGRESS=");
    if (p == std::string::npos) return -1;
    return std::atoi(r.c_str() + p + 9);
}

std::string TorClient::torVersion() {
    std::string r = controlQuery("GETINFO version");
    auto p = r.find("version=");
    if (p == std::string::npos) return {};
    p += 8;
    auto e = r.find_first_of("\r\n", p);
    return r.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
