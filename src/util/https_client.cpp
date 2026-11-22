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

std::string header_value(const std::string& raw_headers, const std::string& name) noexcept {
    // raw_headers is a CRLF-separated block; the first line is the status line.
    const auto ieq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    };
    size_t pos = 0;
    while (pos < raw_headers.size()) {
        size_t eol = raw_headers.find("\r\n", pos);
        if (eol == std::string::npos) {
            eol = raw_headers.size();
        }
        const std::string line = raw_headers.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && colon == name.size()) {
            bool match = true;
            for (size_t i = 0; i < name.size(); ++i) {
                if (!ieq(line[i], name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t v = colon + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) {
                    ++v;
                }
                return line.substr(v);
            }
        }
        if (eol >= raw_headers.size()) {
            break;
        }
        pos = eol + 2;
    }
    return {};
}

HttpResult https_request(const std::string& method, const std::string& host, const std::string& path,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, const std::string& content_type, bool follow_redirects,
                         int timeout_ms) noexcept {
    HttpResult out{};
    try {
        Handle session(WinHttpOpen(L"OpenSlinkyPlusPlus/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.h == nullptr) {
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
        Handle request(WinHttpOpenRequest(connect.h, widen(method).c_str(), widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request.h == nullptr) {
            return out;
        }

        if (!follow_redirects) {
            DWORD option = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option));
        }
        // Disable WinHTTP's own cookie store so an explicit Cookie header is used verbatim.
        {
            DWORD option = WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option));
        }

        std::wstring header_block;
        if (!content_type.empty()) {
            header_block += L"Content-Type: " + widen(content_type) + L"\r\n";
        }
        for (const auto& [name, value] : headers) {
            header_block += widen(name) + L": " + widen(value) + L"\r\n";
        }

        const BOOL sent = WinHttpSendRequest(
            request.h, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(-1L),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
            return out;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(status);

        // Full raw response header block (for Location / Set-Cookie).
        DWORD header_bytes = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                            &header_bytes, WINHTTP_NO_HEADER_INDEX);
        if (header_bytes > 0) {
            std::wstring wbuf(header_bytes / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    wbuf.data(), &header_bytes, WINHTTP_NO_HEADER_INDEX)) {
                const int len = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    std::string utf8(static_cast<size_t>(len), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, utf8.data(), len, nullptr, nullptr);
                    if (!utf8.empty() && utf8.back() == '\0') {
                        utf8.pop_back();
                    }
                    out.raw_headers = utf8;
                }
            }
        }

        std::string result_body;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) {
                break;
            }
            const size_t old_size = result_body.size();
            result_body.resize(old_size + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, result_body.data() + old_size, available, &read)) {
                result_body.resize(old_size);
                break;
            }
            result_body.resize(old_size + read);
            if (result_body.size() > 8u * 1024u * 1024u) {
                break; // sanity cap
            }
        }
        out.body = std::move(result_body);
        out.transport_ok = true;
    } catch (...) {
        out.transport_ok = false;
    }
    return out;
}

} // namespace slinky::net
