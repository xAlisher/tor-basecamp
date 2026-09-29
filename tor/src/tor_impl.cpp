#include "tor_impl.h"
#include "tor_client.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace {
bool isExecutable(const std::string& p) {
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}
// Single-quote for the shell (wrap in '', escape embedded ').
std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}
std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64encode(const std::string& in) {
    std::string out; int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out.push_back(B64[(val >> bits) & 0x3f]); bits -= 6; }
    }
    if (bits > -6) out.push_back(B64[((val << 8) >> (bits + 8)) & 0x3f]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
std::string b64decode(const std::string& in) {
    std::array<int, 256> T; T.fill(-1);
    for (int i = 0; i < 64; ++i) T[(unsigned char)B64[i]] = i;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c]; bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}
// RFC4648 base32, uppercase, no padding (tor's v3 client-auth key encoding).
std::string base32Encode(const std::string& in) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out; int bits = 0; unsigned long val = 0;
    for (unsigned char c : in) {
        val = (val << 8) | c; bits += 8;
        while (bits >= 5) { out.push_back(A[(val >> (bits - 5)) & 0x1f]); bits -= 5; }
    }
    if (bits > 0) out.push_back(A[(val << (5 - bits)) & 0x1f]);
    return out;
}
// Generate an x25519 keypair; return raw 32-byte pub + priv. false on failure.
bool genX25519(std::string& pub, std::string& priv) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return false;
    bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) { if (pkey) EVP_PKEY_free(pkey); return false; }
    size_t n = 32; std::string pb(32, '\0'), sb(32, '\0');
    ok = EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char*>(&pb[0]), &n) > 0 && n == 32;
    n = 32;
    ok = ok && EVP_PKEY_get_raw_private_key(pkey, reinterpret_cast<unsigned char*>(&sb[0]), &n) > 0 && n == 32;
    EVP_PKEY_free(pkey);
    if (ok) { pub = pb; priv = sb; }
    return ok;
}
// curl exit code -> our error_kind
std::string curlErrorKind(int rc) {
    switch (rc) {
        case 0:  return "";
        case 6:  return "dns_failed";
        case 7:  return "connect_failed";
        case 28: return "timeout";
        case 35: case 51: case 58: case 60: case 77: case 91: return "connect_failed"; // TLS/cert
        case 97: return "connect_failed"; // CURLE_PROXY: SOCKS couldn't resolve/reach (e.g. bad host)
        default: return "internal";
    }
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

std::string TorImpl::resolveCurlBin() const {
    const std::string mp = modulePath();
    if (!mp.empty()) {
        for (const std::string cand : { mp + "/lib/bin/curl", mp + "/bin/curl" })
            if (isExecutable(cand)) return cand;
    }
    if (const char* env = std::getenv("CURL_BIN"))
        if (isExecutable(env)) return env;
    if (const char* path = std::getenv("PATH")) {
        std::string p(path); size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty() && isExecutable(dir + "/curl")) return dir + "/curl";
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return {};
}

std::string TorImpl::caBundle() const {
    const std::string mp = modulePath();
    for (const std::string cand : { mp + "/lib/bin/ca-bundle.crt", mp + "/bin/ca-bundle.crt",
                                    std::string("/etc/ssl/certs/ca-certificates.crt") })
        if (!cand.empty() && ::access(cand.c_str(), R_OK) == 0) return cand;
    return {};
}

StdLogosResult TorImpl::http_request(const std::string& requestJson) {
    const std::string sErr = ensureStarted();
    if (!sErr.empty())
        return { true, nlohmann::json{ {"ok", false}, {"status", 0}, {"error", sErr}, {"error_kind", "not_bootstrapped"} } };

    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    if (req.is_discarded())
        return { true, nlohmann::json{ {"ok", false}, {"status", 0}, {"error", "invalid request JSON"}, {"error_kind", "bad_request"} } };

    const std::string url = req.value("url", std::string());
    if (url.empty())
        return { true, nlohmann::json{ {"ok", false}, {"status", 0}, {"error", "url required"}, {"error_kind", "bad_request"} } };

    const std::string curl = resolveCurlBin();
    if (curl.empty())
        return { true, nlohmann::json{ {"ok", false}, {"status", 0}, {"error", "curl_not_found"}, {"error_kind", "internal"} } };

    const std::string method = req.value("method", std::string("GET"));
    const int timeoutMs  = req.value("timeout_ms", 30000);
    const int connectMs  = req.value("connect_timeout_ms", 15000);
    const std::string iso = req.value("isolation_tag", std::string());

    const std::string tmp = dataDir();
    const std::string bodyOut = tmp + "/.req_body";
    const std::string hdrOut  = tmp + "/.req_hdr";
    std::string bodyIn;
    if (req.contains("body_b64") && req["body_b64"].is_string() && !req["body_b64"].get<std::string>().empty()) {
        bodyIn = tmp + "/.req_bodyin";
        std::ofstream bf(bodyIn, std::ios::binary);
        bf << b64decode(req["body_b64"].get<std::string>());
    }

    std::string cmd = shq(curl) + " -sS --socks5-hostname 127.0.0.1:" + std::to_string(m_tor->socksPort())
        + " -X " + shq(method)
        + " --max-time " + std::to_string((timeoutMs + 999) / 1000)
        + " --connect-timeout " + std::to_string((connectMs + 999) / 1000)
        + " -o " + shq(bodyOut) + " -D " + shq(hdrOut)
        + " -w '%{http_code}\\n%{url_effective}'";
    const std::string ca = caBundle();
    if (!ca.empty()) cmd += " --cacert " + shq(ca);
    if (!iso.empty()) cmd += " --proxy-user " + shq(iso + ":" + iso);
    if (req.contains("headers") && req["headers"].is_object())
        for (auto& [k, v] : req["headers"].items())
            if (v.is_string()) cmd += " -H " + shq(k + ": " + v.get<std::string>());
    if (!bodyIn.empty()) cmd += " --data-binary @" + shq(bodyIn);
    cmd += " " + shq(url);

    std::array<char, 1024> buf{};
    std::string wout;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return { true, nlohmann::json{ {"ok", false}, {"status", 0}, {"error", "popen failed"}, {"error_kind", "internal"} } };
    while (fgets(buf.data(), buf.size(), pipe)) wout += buf.data();
    int rc = WEXITSTATUS(::pclose(pipe));

    int status = 0; std::string finalUrl;
    { std::istringstream ss(wout); std::string line; if (std::getline(ss, line)) status = std::atoi(line.c_str()); std::getline(ss, finalUrl); }

    // Parse the LAST response header block into an object.
    nlohmann::json headers = nlohmann::json::object();
    { std::istringstream hs(readFile(hdrOut)); std::string line;
      while (std::getline(hs, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.rfind("HTTP/", 0) == 0) { headers = nlohmann::json::object(); continue; } // new block
          auto c = line.find(':'); if (c == std::string::npos) continue;
          std::string k = line.substr(0, c); std::string val = line.substr(c + 1);
          size_t s = val.find_first_not_of(" \t"); if (s != std::string::npos) val = val.substr(s);
          for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
          headers[k] = val;
      } }

    const std::string kind = curlErrorKind(rc);
    const std::string bodyBytes = readFile(bodyOut);
    ::unlink(bodyOut.c_str()); ::unlink(hdrOut.c_str()); if (!bodyIn.empty()) ::unlink(bodyIn.c_str());
    nlohmann::json out = {
        {"ok", rc == 0},
        {"status", status},
        {"headers", headers},
        {"body_b64", b64encode(bodyBytes)},
        {"final_url", finalUrl.empty() ? url : finalUrl},
        {"error", rc == 0 ? "" : ("curl exit " + std::to_string(rc))},
        {"error_kind", kind}
    };
    return { true, out };
}

StdLogosResult TorImpl::new_circuit(const std::string& requestJson) {
    const std::string sErr = ensureStarted();
    if (!sErr.empty()) return { false, {}, sErr };
    (void)requestJson; // isolation_tag reserved; NEWNYM is global for now
    bool ok = m_tor->newCircuit();
    return { true, nlohmann::json{ {"ok", ok} } };
}

std::string TorImpl::onionKeyPath(const std::string& persistId) const {
    std::string d = dataDir() + "/onions";
    ::mkdir(d.c_str(), 0700);
    return d + "/" + persistId + ".key";
}

bool TorImpl::reissueService(HostedService& s, std::string& onionOut, std::string& err) {
    std::string keyOut;
    if (!m_tor->addOnion(s.virtualPort, s.localPort, s.key, s.authClients, onionOut, keyOut, err))
        return false;
    if (!keyOut.empty()) s.key = keyOut;   // capture the key on first (NEW) creation
    if (!s.persistId.empty() && !s.key.empty()) {
        std::ofstream kf(onionKeyPath(s.persistId));
        kf << s.key;
    }
    return true;
}

StdLogosResult TorImpl::create_onion_service(const std::string& requestJson) {
    const std::string sErr = ensureStarted();
    if (!sErr.empty()) return { false, {}, sErr };
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    if (req.is_discarded() || !req.contains("local_port"))
        return { false, {}, "local_port required" };

    HostedService s;
    s.localPort   = req.value("local_port", 0);
    s.virtualPort = req.value("virtual_port", 80);
    s.persistId   = req.value("persist_id", std::string());
    s.requireAuth = req.value("require_auth", false);
    if (!s.persistId.empty()) {
        std::string saved = readFile(onionKeyPath(s.persistId));
        if (!saved.empty()) s.key = saved;   // reuse persisted key -> stable .onion
    }

    std::string onion, err;
    if (!reissueService(s, onion, err))
        return { true, nlohmann::json{ {"ok", false}, {"error", err} } };
    const std::string id = onion.substr(0, onion.find(".onion"));
    m_services[id] = s;
    return { true, nlohmann::json{ {"ok", true}, {"id", id}, {"onion", onion}, {"error", ""} } };
}

StdLogosResult TorImpl::onion_service_status(const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    const std::string id = req.is_object() ? req.value("id", std::string()) : std::string();
    if (id.empty()) return { false, {}, "id required" };
    bool pub = m_tor->onionPublished(id);
    return { true, nlohmann::json{ {"ok", true}, {"published", pub}, {"onion", id + ".onion"} } };
}

StdLogosResult TorImpl::remove_onion_service(const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    const std::string id = req.is_object() ? req.value("id", std::string()) : std::string();
    if (id.empty()) return { false, {}, "id required" };
    bool ok = m_tor->delOnion(id);
    m_services.erase(id);
    return { true, nlohmann::json{ {"ok", ok} } };
}

StdLogosResult TorImpl::generate_client_auth_keypair() {
    std::string pub, priv;
    if (!genX25519(pub, priv)) return { false, {}, "keygen_failed" };
    // tor's formats are asymmetric: ClientAuthV3 (server) wants the PUBLIC key in
    // base32; ONION_CLIENT_AUTH_ADD (client) wants the PRIVATE key in base64.
    return { true, nlohmann::json{ {"ok", true}, {"public", base32Encode(pub)}, {"private", b64encode(priv)} } };
}

StdLogosResult TorImpl::authorize_client(const std::string& requestJson) {
    const std::string sErr = ensureStarted(); if (!sErr.empty()) return { false, {}, sErr };
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    const std::string id = req.is_object() ? req.value("id", std::string()) : std::string();
    const std::string pub = req.is_object() ? req.value("client_public", std::string()) : std::string();
    if (id.empty() || pub.empty()) return { false, {}, "id and client_public required" };
    auto it = m_services.find(id);
    if (it == m_services.end()) return { false, {}, "unknown service (create it first)" };
    auto& cs = it->second.authClients;
    if (std::find(cs.begin(), cs.end(), pub) == cs.end()) cs.push_back(pub);
    m_tor->delOnion(id);                       // reissue with the updated client set (same key -> same .onion)
    std::string onion, err;
    if (!reissueService(it->second, onion, err)) return { true, nlohmann::json{ {"ok", false}, {"error", err} } };
    return { true, nlohmann::json{ {"ok", true} } };
}

StdLogosResult TorImpl::deauthorize_client(const std::string& requestJson) {
    const std::string sErr = ensureStarted(); if (!sErr.empty()) return { false, {}, sErr };
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    const std::string id = req.is_object() ? req.value("id", std::string()) : std::string();
    const std::string pub = req.is_object() ? req.value("client_public", std::string()) : std::string();
    if (id.empty() || pub.empty()) return { false, {}, "id and client_public required" };
    auto it = m_services.find(id);
    if (it == m_services.end()) return { false, {}, "unknown service" };
    auto& cs = it->second.authClients;
    cs.erase(std::remove(cs.begin(), cs.end(), pub), cs.end());
    m_tor->delOnion(id);
    std::string onion, err;
    if (!reissueService(it->second, onion, err)) return { true, nlohmann::json{ {"ok", false}, {"error", err} } };
    return { true, nlohmann::json{ {"ok", true} } };
}

StdLogosResult TorImpl::list_authorized_clients(const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    const std::string id = req.is_object() ? req.value("id", std::string()) : std::string();
    if (id.empty()) return { false, {}, "id required" };
    auto it = m_services.find(id);
    if (it == m_services.end()) return { false, {}, "unknown service" };
    return { true, nlohmann::json{ {"ok", true}, {"clients", it->second.authClients} } };
}

StdLogosResult TorImpl::register_client_auth(const std::string& requestJson) {
    const std::string sErr = ensureStarted(); if (!sErr.empty()) return { false, {}, sErr };
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    std::string host = req.is_object() ? req.value("onion_host", std::string()) : std::string();
    const std::string priv = req.is_object() ? req.value("private_key", std::string()) : std::string();
    if (host.empty() || priv.empty()) return { false, {}, "onion_host and private_key required" };
    const std::string id = host.substr(0, host.find(".onion"));
    if (!m_tor->clientAuthAdd(id, priv)) return { true, nlohmann::json{ {"ok", false}, {"error", "client_auth_add_failed"} } };
    if (std::find(m_clientAuthOnions.begin(), m_clientAuthOnions.end(), id) == m_clientAuthOnions.end())
        m_clientAuthOnions.push_back(id);
    return { true, nlohmann::json{ {"ok", true} } };
}

StdLogosResult TorImpl::remove_client_auth(const std::string& requestJson) {
    const std::string sErr = ensureStarted(); if (!sErr.empty()) return { false, {}, sErr };
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    std::string host = req.is_object() ? req.value("onion_host", std::string()) : std::string();
    if (host.empty()) return { false, {}, "onion_host required" };
    const std::string id = host.substr(0, host.find(".onion"));
    m_tor->clientAuthRemove(id);
    m_clientAuthOnions.erase(std::remove(m_clientAuthOnions.begin(), m_clientAuthOnions.end(), id), m_clientAuthOnions.end());
    return { true, nlohmann::json{ {"ok", true} } };
}

StdLogosResult TorImpl::list_client_auth() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& id : m_clientAuthOnions) arr.push_back(id + ".onion");
    return { true, nlohmann::json{ {"ok", true}, {"onions", arr} } };
}
