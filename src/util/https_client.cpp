#include "util/https_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace slinky::net {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) noexcept : h(handle) {}
    ~Handle() {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

} // namespace

HttpResponse https_get(const std::string& host, const std::string& path,
                       const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms) noexcept {
    HttpResponse out{};
    try {
        Handle session(WinHttpOpen(L"OpenSlinkyPlusPlus/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.h == nullptr) {
            // Older Windows without automatic proxy support.
            session.h = WinHttpOpen(L"OpenSlinkyPlusPlus/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session.h == nullptr) {
            return out;
        }
        WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

        Handle connect(WinHttpConnect(session.h, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connect.h == nullptr) {
            return out;
        }
        Handle request(WinHttpOpenRequest(connect.h, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request.h == nullptr) {
            return out;
        }

        std::wstring header_block;
        for (const auto& [name, value] : headers) {
            header_block += widen(name) + L": " + widen(value) + L"\r\n";
        }
        const BOOL sent = WinHttpSendRequest(
            request.h,
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
            return out;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(status);

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) {
                break;
            }
            const size_t old_size = out.body.size();
            out.body.resize(old_size + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, out.body.data() + old_size, available, &read)) {
                out.body.resize(old_size);
                break;
            }
            out.body.resize(old_size + read);
            if (out.body.size() > 4u * 1024u * 1024u) {
                break; // sanity cap
            }
        }
        out.transport_ok = true;
    } catch (...) {
        out.transport_ok = false;
    }
    return out;
}

} // namespace slinky::net
