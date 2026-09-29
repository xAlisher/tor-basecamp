#include "tor_impl.h"
#include "tor_client.h"

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool isExecutable(const std::string& p) {
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}
} // namespace

TorImpl::TorImpl() : m_tor(std::make_unique<TorClient>()) {}
TorImpl::~TorImpl() = default;

// The .lgx bundles tor at <moduleDir>/lib/bin/tor (see flake postInstall). Fall
// back to $TOR_BIN (survives AppImage PATH resets) then a PATH search.
std::string TorImpl::resolveTorBin() const {
    const std::string mp = modulePath();
    if (!mp.empty()) {
        for (const std::string cand : { mp + "/lib/bin/tor", mp + "/bin/tor" })
            if (isExecutable(cand)) return cand;
    }
    if (const char* env = std::getenv("TOR_BIN"))
        if (isExecutable(env)) return env;
    if (const char* path = std::getenv("PATH")) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty() && isExecutable(dir + "/tor")) return dir + "/tor";
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return {};
}

// tor's DataDirectory, persisted per module instance so the SOCKS/consensus cache
// survives restarts (a fresh dir forces a cold consensus fetch each launch).
std::string TorImpl::dataDir() const {
    std::string base = instancePersistencePath();
    if (base.empty()) {
        const char* xdg = std::getenv("XDG_DATA_HOME");
        base = xdg ? std::string(xdg) : (std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.local/share");
        base += "/tor_module";
    }
    ::mkdir(base.c_str(), 0700);
    return base + "/tor-data";
}

std::string TorImpl::ensureStarted() {
    if (m_tor->running()) return {};
    const std::string bin = resolveTorBin();
    std::string err;
    if (!m_tor->ensureStarted(dataDir(), bin, err)) return err.empty() ? "tor_start_failed" : err;
    return {};
}

StdLogosResult TorImpl::status() {
    const std::string err = ensureStarted();
    nlohmann::json v;
    if (!err.empty()) {
        v = { {"bootstrapped", false}, {"progress", 0}, {"error", err} };
        return { true, v };   // report the state; not a call failure
    }
    const int pct = m_tor->bootstrapPercent();
    v = {
        {"bootstrapped", pct >= 100},
        {"progress", pct < 0 ? 0 : pct},
        {"socks_host", "127.0.0.1"},
        {"socks_port", m_tor->socksPort()},
        {"tor_version", m_tor->torVersion()},
        {"error", ""}
    };
    return { true, v };
}

StdLogosResult TorImpl::get_socks_endpoint() {
    const std::string err = ensureStarted();
    if (!err.empty()) return { false, {}, err };
    return { true, nlohmann::json{ {"host", "127.0.0.1"}, {"port", m_tor->socksPort()} } };
}
