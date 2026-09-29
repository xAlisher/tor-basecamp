#pragma once

#include <memory>
#include <string>

#include <logos_module_context.h>
#include <logos_result.h>

class TorClient;

/**
 * @brief tor — headless Tor capability module (universal, Qt-free).
 *
 * Owns one embedded tor for the whole Basecamp: a client SocksPort now, onion
 * hosting later. Phase 1 #4: launch tor + report readiness. All methods return
 * StdLogosResult; the codegen exposes them over the module IPC.
 */
class TorImpl : public LogosModuleContext {
public:
    TorImpl();
    ~TorImpl();

    /// {ok, bootstrapped, progress, socks_host, socks_port, tor_version, error}
    StdLogosResult status();

    /// {ok, host, port} — the shared SOCKS5 proxy, for callers that stream.
    StdLogosResult get_socks_endpoint();

    std::string name() const { return "tor"; }
    std::string version() const { return "0.1.0"; }

private:
    // Launch tor if needed. Returns "" on success or an error code.
    std::string ensureStarted();
    std::string resolveTorBin() const;
    std::string dataDir() const;

    std::unique_ptr<TorClient> m_tor;
};
