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

// Full response of a generic request, including the raw response headers (needed for Set-Cookie / Location).
struct HttpResult {
    bool transport_ok = false;                 // false = DNS/TLS/connection/timeout failure
    int status = 0;                            // HTTP status code
    std::string body;                          // response body
    std::string raw_headers;                   // full CRLF-separated response header block
};

// Generic blocking HTTPS request on WinHTTP. `method` e.g. "POST"/"GET". `content_type` may be empty.
// When `follow_redirects` is false a 3xx returns with its Location in `raw_headers` (use header_value()).
// Call from a worker thread only - it blocks for up to the given timeout.
HttpResult https_request(const std::string& method, const std::string& host, const std::string& path,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, const std::string& content_type, bool follow_redirects,
                         int timeout_ms) noexcept;

// Case-insensitive lookup of the first value of `name` in a raw CRLF header block ("" if absent).
std::string header_value(const std::string& raw_headers, const std::string& name) noexcept;

} // namespace slinky::net
