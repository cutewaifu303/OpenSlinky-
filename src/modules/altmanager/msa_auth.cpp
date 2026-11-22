#include "modules/altmanager/msa_auth.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util/https_client.hpp"

namespace slinky::altmanager {

namespace {

// ---- endpoints / constants --------------------------------------------------------------------------------
constexpr const char* kLiveHost = "login.live.com";
constexpr const char* kAuthorizePath = "/oauth20_authorize.srf";
constexpr const char* kTokenPath = "/oauth20_token.srf";
constexpr const char* kXblHost = "user.auth.xboxlive.com";
constexpr const char* kXstsHost = "xsts.auth.xboxlive.com";
constexpr const char* kMcHost = "api.minecraftservices.com";
constexpr const char* kSisuHost = "sisu.xboxlive.com";

constexpr const char* kClientId = "00000000402b5328";           // Minecraft legacy public client id
constexpr const char* kScope = "service::user.auth.xboxlive.com::MBI_SSL";
constexpr const char* kDesktopRedirect = "https://login.live.com/oauth20_desktop.srf";
constexpr int kTimeoutMs = 20000;

// ---- small helpers ----------------------------------------------------------------------------------------
std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Extracts the string value of "key" from a flat JSON body. Returns "" if absent.
std::string json_string(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return {};
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\n' ||
                                 body[pos] == '\r')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return {};
    }
    ++pos;
    std::string out;
    while (pos < body.size() && body[pos] != '"') {
        if (body[pos] == '\\' && pos + 1 < body.size()) {
            const char esc = body[pos + 1];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '/': out.push_back('/'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(esc); break;
            }
            pos += 2;
        } else {
            out.push_back(body[pos]);
            ++pos;
        }
    }
    return out;
}

// Extracts the first "uhs" from a DisplayClaims structure.
std::string extract_uhs(const std::string& body) {
    return json_string(body, "uhs");
}

std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62; // url-safe
        if (c == '/' || c == '_') return 63; // url-safe
        return -1;
    };
    std::string out;
    int buffer = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\0') {
            break;
        }
        const int v = val(c);
        if (v < 0) {
            continue;
        }
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// Splits "https://host/path?query" into host + "/path?query". Returns false if not https.
bool split_url(const std::string& url, std::string* host, std::string* path) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        return false;
    }
    const size_t start = scheme.size();
    size_t slash = url.find('/', start);
    if (slash == std::string::npos) {
        *host = url.substr(start);
        *path = "/";
    } else {
        *host = url.substr(start, slash - start);
        *path = url.substr(slash);
    }
    return true;
}

// Reads the value of query parameter `name` from a URL / request target. Returns "" if absent.
std::string query_param(const std::string& url, const std::string& name) {
    const std::string key = name + "=";
    size_t q = url.find('?');
    size_t search = (q == std::string::npos) ? 0 : q;
    size_t pos = url.find(key, search);
    while (pos != std::string::npos) {
        const bool boundary = pos == 0 || url[pos - 1] == '?' || url[pos - 1] == '&';
        if (boundary) {
            size_t end = url.find('&', pos + key.size());
            if (end == std::string::npos) {
                end = url.find(' ', pos + key.size()); // request line "GET <target> HTTP/1.1"
            }
            if (end == std::string::npos) {
                end = url.size();
            }
            return url.substr(pos + key.size(), end - pos - key.size());
        }
        pos = url.find(key, pos + key.size());
    }
    return {};
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// ---- Xbox / Minecraft tail (shared by every login kind) ---------------------------------------------------
// Given an MSA access token, runs XBL -> XSTS -> login_with_xbox -> profile and fills `out`.
bool msa_token_to_account(LoginJob* job, const std::string& msa_access, const std::string& refresh_token,
                          Account* out, std::string* error) {
    job->report("Authenticating with Xbox Live...");
    {
        std::string b = "{\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\","
                        "\"RpsTicket\":\"t=" + msa_access +
                        "\"},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}";
        const net::HttpResult r = net::https_request("POST", kXblHost, "/user/authenticate",
                                                     {{"Accept", "application/json"}}, b, "application/json",
                                                     true, kTimeoutMs);
        if (!r.transport_ok || r.status / 100 != 2) {
            *error = "Xbox Live auth failed (HTTP " + std::to_string(r.status) + ")";
            return false;
        }
        const std::string xbl = json_string(r.body, "Token");
        const std::string uhs = extract_uhs(r.body);
        if (xbl.empty() || uhs.empty()) {
            *error = "Xbox Live auth returned no token";
            return false;
        }

        job->report("Requesting XSTS token...");
        std::string xb = "{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"" + xbl +
                         "\"]},\"RelyingParty\":\"rp://api.minecraftservices.com/\",\"TokenType\":\"JWT\"}";
        const net::HttpResult xr = net::https_request("POST", kXstsHost, "/xsts/authorize",
                                                      {{"Accept", "application/json"}}, xb, "application/json",
                                                      true, kTimeoutMs);
        if (!xr.transport_ok || xr.status / 100 != 2) {
            if (xr.status == 401) {
                *error = "This Microsoft account has no Xbox / Minecraft profile";
            } else {
                *error = "XSTS auth failed (HTTP " + std::to_string(xr.status) + ")";
            }
            return false;
        }
        const std::string xsts = json_string(xr.body, "Token");
        const std::string xsts_uhs = extract_uhs(xr.body);
        if (xsts.empty() || xsts_uhs.empty()) {
            *error = "XSTS auth returned no token";
            return false;
        }
        return [&]() {
            job->report("Logging in to Minecraft services...");
            std::string mb = "{\"identityToken\":\"XBL3.0 x=" + xsts_uhs + ";" + xsts + "\"}";
            const net::HttpResult mr = net::https_request("POST", kMcHost, "/authentication/login_with_xbox",
                                                          {{"Accept", "application/json"}}, mb,
                                                          "application/json", true, kTimeoutMs);
            if (!mr.transport_ok || mr.status / 100 != 2) {
                *error = "Minecraft login failed (HTTP " + std::to_string(mr.status) + ")";
                return false;
            }
            const std::string mc_token = json_string(mr.body, "access_token");
            if (mc_token.empty()) {
                *error = "Minecraft login returned no access token";
                return false;
            }

            job->report("Fetching profile...");
            const net::HttpResult pr = net::https_request("GET", kMcHost, "/minecraft/profile",
                                                          {{"Authorization", "Bearer " + mc_token},
                                                           {"Accept", "application/json"}},
                                                          "", "", true, kTimeoutMs);
            if (!pr.transport_ok || pr.status / 100 != 2) {
                if (pr.status == 404) {
                    *error = "This account does not own Minecraft";
                } else {
                    *error = "Could not fetch Minecraft profile (HTTP " + std::to_string(pr.status) + ")";
                }
                return false;
            }
            const std::string name = json_string(pr.body, "name");
            const std::string id = json_string(pr.body, "id");
            if (name.empty() || id.empty()) {
                *error = "Profile response was incomplete";
                return false;
            }
            out->name = name;
            out->uuid = id;
            out->token = mc_token;
            out->refresh_token = refresh_token;
            out->type = AccountType::MSA;
            if (out->tag.empty()) {
                out->tag = name;
            }
            return true;
        }();
    }
}

// Exchanges an MSA refresh token for a fresh access token (and rotated refresh token).
bool refresh_to_msa_access(const std::string& refresh_token, std::string* msa_access, std::string* new_refresh,
                           std::string* error) {
    std::string body = "client_id=" + url_encode(kClientId) + "&refresh_token=" + url_encode(refresh_token) +
                       "&grant_type=refresh_token&redirect_uri=" + url_encode(kDesktopRedirect) + "&scope=" +
                       url_encode(kScope);
    const net::HttpResult r = net::https_request("POST", kLiveHost, kTokenPath, {}, body,
                                                 "application/x-www-form-urlencoded", true, kTimeoutMs);
    if (!r.transport_ok || r.status / 100 != 2) {
        *error = "Refresh token rejected (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    *msa_access = json_string(r.body, "access_token");
    *new_refresh = json_string(r.body, "refresh_token");
    if (new_refresh->empty()) {
        *new_refresh = refresh_token;
    }
    if (msa_access->empty()) {
        *error = "Token endpoint returned no access token";
        return false;
    }
    return true;
}

// ---- loopback listener for the system-browser redirect ----------------------------------------------------
struct WsaGuard {
    bool ok = false;
    WsaGuard() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaGuard() {
        if (ok) {
            WSACleanup();
        }
    }
};

// Binds 127.0.0.1:0, returns the socket and chosen port. INVALID_SOCKET on failure.
SOCKET open_loopback(int* out_port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    int len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    *out_port = ntohs(addr.sin_port);
    return s;
}

// Accepts one browser request and returns its request target (e.g. "/?code=..."). "" on timeout/failure.
std::string accept_oauth_redirect(SOCKET listener, int timeout_ms) {
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) {
        return {};
    }
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        return {};
    }
    std::string request;
    char buf[2048];
    for (int i = 0; i < 8; ++i) {
        const int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        request.append(buf, static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    const char* resp =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<html><body style='font-family:sans-serif;background:#141414;color:#ddd;text-align:center;"
        "padding-top:80px'><h2>OpenSlinky++</h2><p>Login received. You can close this tab.</p></body></html>";
    send(client, resp, static_cast<int>(std::strlen(resp)), 0);
    closesocket(client);

    // request line: "GET /?code=... HTTP/1.1"
    const size_t sp1 = request.find(' ');
    if (sp1 == std::string::npos) {
        return {};
    }
    const size_t sp2 = request.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        return {};
    }
    return request.substr(sp1 + 1, sp2 - sp1 - 1);
}

} // namespace

void run_refresh_login(LoginJob* job, const std::string& refresh_token) noexcept {
    std::string trimmed = refresh_token;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        job->finish_error("No refresh token entered");
        return;
    }
    job->report("Refreshing Microsoft token...");
    std::string msa_access;
    std::string new_refresh;
    std::string error;
    if (!refresh_to_msa_access(trimmed, &msa_access, &new_refresh, &error)) {
        job->finish_error(error);
        return;
    }
    Account account{};
    if (!msa_token_to_account(job, msa_access, new_refresh, &account, &error)) {
        job->finish_error(error);
        return;
    }
    job->finish_ok(account);
}

void run_microsoft_login(LoginJob* job) noexcept {
    WsaGuard wsa;
    if (!wsa.ok) {
        job->finish_error("Could not initialize networking");
        return;
    }
    int port = 0;
    SOCKET listener = open_loopback(&port);
    if (listener == INVALID_SOCKET) {
        job->finish_error("Could not open loopback listener");
        return;
    }

    char redirect[64];
    std::snprintf(redirect, sizeof(redirect), "http://127.0.0.1:%d/", port);
    const std::string authorize = std::string("https://") + kLiveHost + kAuthorizePath + "?client_id=" +
                                  url_encode(kClientId) + "&response_type=code&redirect_uri=" +
                                  url_encode(redirect) + "&scope=" + url_encode(kScope);

    job->report("Opening browser - sign in, then return here...");
    ShellExecuteA(nullptr, "open", authorize.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    const std::string target = accept_oauth_redirect(listener, 180000); // 3 min for the user to sign in
    closesocket(listener);
    if (target.empty()) {
        job->finish_error("Timed out waiting for the browser login");
        return;
    }
    const std::string code = percent_decode(query_param(target, "code"));
    if (code.empty()) {
        const std::string err = query_param(target, "error");
        job->finish_error(err.empty() ? "No authorization code received" : ("Login error: " + err));
        return;
    }

    job->report("Exchanging authorization code...");
    std::string body = "client_id=" + url_encode(kClientId) + "&code=" + url_encode(code) +
                       "&grant_type=authorization_code&redirect_uri=" + url_encode(redirect) + "&scope=" +
                       url_encode(kScope);
    const net::HttpResult r = net::https_request("POST", kLiveHost, kTokenPath, {}, body,
                                                 "application/x-www-form-urlencoded", true, kTimeoutMs);
    if (!r.transport_ok || r.status / 100 != 2) {
        job->finish_error("Token exchange failed (HTTP " + std::to_string(r.status) + ")");
        return;
    }
    const std::string msa_access = json_string(r.body, "access_token");
    const std::string refresh = json_string(r.body, "refresh_token");
    if (msa_access.empty()) {
        job->finish_error("Token endpoint returned no access token");
        return;
    }

    Account account{};
    std::string error;
    if (!msa_token_to_account(job, msa_access, refresh, &account, &error)) {
        job->finish_error(error);
        return;
    }
    job->finish_ok(account);
}

void run_cookie_login(LoginJob* job, const std::string& cookie_text) noexcept {
    // Normalize the pasted cookies into a single "name=value; ..." Cookie header.
    std::string cookies;
    for (char c : cookie_text) {
        if (c != '\r' && c != '\n') {
            cookies.push_back(c);
        } else if (!cookies.empty() && cookies.back() != ' ') {
            cookies.push_back(' ');
        }
    }
    if (cookies.find("__Host-MSAAUTHP") == std::string::npos &&
        cookies.find("__Host-MSAAUTH") == std::string::npos) {
        job->finish_error("Cookies must include __Host-MSAAUTHP or __Host-MSAAUTH");
        return;
    }

    job->report("Replaying cookies through Xbox SISU...");
    // SISU connect endpoint (reference: naczo5/lunar-cookies). Follow redirects manually, carrying cookies,
    // until a Location carries an accessToken= (a base64 XSTS JSON blob).
    std::string host = kSisuHost;
    std::string path = std::string("/connect/XboxLive/?state=login"
                                   "&cobrandId=8058f65d-ce06-4c30-9559-473c9275a65d"
                                   "&tid=896928775"
                                   "&ru=") + url_encode("https://www.minecraft.net/en-us/login") +
                       "&aid=1142970254";

    std::string encoded_xsts;
    for (int hop = 0; hop < 10; ++hop) {
        const net::HttpResult r = net::https_request(
            "GET", host, path,
            {{"Cookie", cookies},
             {"User-Agent",
              "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/120.0 Safari/537.36"}},
            "", "", false, kTimeoutMs);
        if (!r.transport_ok) {
            job->finish_error("Network error during cookie login");
            return;
        }
        std::string location = net::header_value(r.raw_headers, "Location");
        // The accessToken can appear in this hop's Location (or request target).
        std::string candidate = query_param(location, "accessToken");
        if (!candidate.empty()) {
            encoded_xsts = percent_decode(candidate);
            break;
        }
        if (r.status / 100 != 3 || location.empty()) {
            break; // no more redirects
        }
        // Resolve the next hop (absolute or relative Location).
        if (location.compare(0, 8, "https://") == 0) {
            if (!split_url(location, &host, &path)) {
                break;
            }
        } else {
            path = location; // same host, relative path
        }
    }

    if (encoded_xsts.empty()) {
        job->finish_error("Cookies did not yield a token (expired or invalid)");
        return;
    }
    const std::string xsts_json = base64_decode(encoded_xsts);
    const std::string xsts = json_string(xsts_json, "Token");
    const std::string uhs = extract_uhs(xsts_json);
    if (xsts.empty() || uhs.empty()) {
        job->finish_error("Could not decode XSTS token from cookies");
        return;
    }

    job->report("Logging in to Minecraft services...");
    std::string mb = "{\"identityToken\":\"XBL3.0 x=" + uhs + ";" + xsts + "\"}";
    const net::HttpResult mr = net::https_request("POST", kMcHost, "/authentication/login_with_xbox",
                                                  {{"Accept", "application/json"}}, mb, "application/json",
                                                  true, kTimeoutMs);
    if (!mr.transport_ok || mr.status / 100 != 2) {
        job->finish_error("Minecraft login failed (HTTP " + std::to_string(mr.status) + ")");
        return;
    }
    const std::string mc_token = json_string(mr.body, "access_token");
    if (mc_token.empty()) {
        job->finish_error("Minecraft login returned no access token");
        return;
    }

    job->report("Fetching profile...");
    const net::HttpResult pr = net::https_request(
        "GET", kMcHost, "/minecraft/profile",
        {{"Authorization", "Bearer " + mc_token}, {"Accept", "application/json"}}, "", "", true, kTimeoutMs);
    if (!pr.transport_ok || pr.status / 100 != 2) {
        job->finish_error("Could not fetch Minecraft profile (HTTP " + std::to_string(pr.status) + ")");
        return;
    }
    Account account{};
    account.name = json_string(pr.body, "name");
    account.uuid = json_string(pr.body, "id");
    account.token = mc_token;
    account.type = AccountType::MSA;
    account.tag = account.name;
    if (account.name.empty() || account.uuid.empty()) {
        job->finish_error("Profile response was incomplete");
        return;
    }
    job->finish_ok(account);
}

} // namespace slinky::altmanager
