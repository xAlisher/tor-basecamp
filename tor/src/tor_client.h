#pragma once

#include <string>
#include <sys/types.h>

// TorClient — a single embedded tor process (Qt-free, POSIX).
//
// Phase 1 scope: launch tor with a client SocksPort + a cookie-authenticated
// ControlPort, and report bootstrap progress via the control protocol (a stable
// API, not log-scraping). One instance owns one tor; the impl keeps it shared.
class TorClient {
public:
    TorClient() = default;
    ~TorClient();

    // Launch tor if not already running. dataDir is tor's DataDirectory
    // (persisted per module instance). torBin is the resolved tor binary.
    // Returns true if the process was spawned (bootstrap is asynchronous —
    // poll bootstrapPercent()). errorOut carries a code on failure.
    bool ensureStarted(const std::string& dataDir,
                       const std::string& torBin,
                       std::string& errorOut);

    bool running() const;

    // 0..100 via ControlPort `GETINFO status/bootstrap-phase`; -1 if the
    // control port is not answerable yet.
    int bootstrapPercent();

    int socksPort()   const { return m_socksPort; }
    int controlPort() const { return m_controlPort; }
    std::string torVersion();   // via ControlPort GETINFO version; "" if unknown

    // Request a fresh circuit (ControlPort SIGNAL NEWNYM). Returns true on 250 OK.
    bool newCircuit();

private:
    // A control-protocol request/response round trip. Returns the raw reply,
    // empty on connection/auth failure.
    std::string controlQuery(const std::string& command);
    bool authenticate(int fd);              // cookie auth on an open control socket
    static int pickFreePort();              // bind :0, read the port, close

    pid_t m_pid = -1;
    int   m_socksPort = 0;
    int   m_controlPort = 0;
    std::string m_dataDir;
};
