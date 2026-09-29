#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

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

    /// Buffered HTTP over Tor (clearnet + .onion). request JSON:
    /// {method,url,headers,body_b64,timeout_ms,connect_timeout_ms,isolation_tag}
    /// -> {status,headers,body_b64,final_url,ok,error,error_kind}
    StdLogosResult http_request(const std::string& requestJson);

    /// Force a fresh circuit. request JSON (optional): {isolation_tag}. -> {ok}
    StdLogosResult new_circuit(const std::string& requestJson);

    // ── Phase 2: onion-service hosting (server) ──────────────────────────────
    /// {local_port, virtual_port?=80, persist_id?, require_auth?} -> {ok,id,onion,error}
    StdLogosResult create_onion_service(const std::string& requestJson);
    /// {id} -> {ok,published,onion}
    StdLogosResult onion_service_status(const std::string& requestJson);
    /// {id} -> {ok}
    StdLogosResult remove_onion_service(const std::string& requestJson);

    // ── Phase 2: pairing (v3 client auth) ────────────────────────────────────
    /// -> {ok, public, private}  (x25519, base32; server keeps public, peer gets private)
    StdLogosResult generate_client_auth_keypair();
    /// {id, client_public} -> {ok}   (authorize a client on a hosted service; reissue)
    StdLogosResult authorize_client(const std::string& requestJson);
    /// {id, client_public} -> {ok}
    StdLogosResult deauthorize_client(const std::string& requestJson);
    /// {id} -> {ok, clients:[...]}
    StdLogosResult list_authorized_clients(const std::string& requestJson);
    /// {onion_host, private_key} -> {ok}   (client side: reach an auth-gated .onion)
    StdLogosResult register_client_auth(const std::string& requestJson);
    /// {onion_host} -> {ok}
    StdLogosResult remove_client_auth(const std::string& requestJson);
    /// -> {ok, onions:[...]}
    StdLogosResult list_client_auth();

    std::string name() const { return "tor"; }
    std::string version() const { return "0.1.0"; }

private:
    // Launch tor if needed. Returns "" on success or an error code.
    std::string ensureStarted();
    std::string resolveTorBin() const;
    std::string resolveCurlBin() const;
    std::string caBundle() const;
    std::string dataDir() const;

    struct HostedService {
        std::string key;            // ED25519-V3:<b64> for re-issue across restarts
        int localPort = 0;
        int virtualPort = 80;
        std::string persistId;
        bool requireAuth = false;
        std::vector<std::string> authClients;   // v3 client-auth public keys (b32)
    };
    // Re-issue ADD_ONION for an existing service with its current client set (same
    // key -> same .onion). Used by create + authorize/deauthorize.
    bool reissueService(HostedService& s, std::string& onionOut, std::string& err);
    std::string onionKeyPath(const std::string& persistId) const;

    std::unique_ptr<TorClient> m_tor;
    std::map<std::string, HostedService> m_services;   // serviceId -> info
    std::vector<std::string> m_clientAuthOnions;       // serviceIds we hold a client key for
};
