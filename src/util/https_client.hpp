#pragma once

// Minimal blocking HTTPS GET on top of WinHTTP (part of Windows, no extra libraries).
// Call it from a worker thread only - it blocks for up to the given timeout.

#include <string>
#include <utility>
#include <vector>

namespace slinky::net {

struct HttpResponse {
    bool transport_ok = false; // false = DNS/TLS/connection/timeout failure
    int status = 0;            // HTTP status code
    std::string body;
};

// `host` e.g. "api.hypixel.net", `path` e.g. "/v2/player?uuid=...". Headers are "Name", "Value" pairs.
// The User-Agent is a generic client name.
HttpResponse https_get(const std::string& host, const std::string& path,
                       const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms) noexcept;

} // namespace slinky::net
