#include "modules/altmanager/account_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <cstdint>

#include "util/filesystem.hpp"

namespace slinky::altmanager {

namespace {

constexpr char kFieldSep = '\x1f';  // between fields of one record
constexpr char kRecordSep = '\x1e'; // between records
constexpr const char* kMagic = "OSLKACC1";

std::vector<Account>& list() noexcept {
    static std::vector<Account> accounts;
    return accounts;
}

// Builds "%USERPROFILE%\.slinky\accounts.dat", creating the .slinky directory. Empty on failure.
bool account_file_path(fs::WidePath* out) noexcept {
    if (fs::init_user_profile_path(out) != 0) {
        return false;
    }
    if (fs::append_path(out, ".slinky") != 0) {
        fs::free_path(out);
        return false;
    }
    fs::create_directory(out); // ignore "already exists"
    if (fs::append_path(out, "accounts.dat") != 0) {
        fs::free_path(out);
        return false;
    }
    return true;
}

bool read_all_bytes(const fs::WidePath& path, std::vector<uint8_t>* out) noexcept {
    HANDLE h = CreateFileW(path.ptr, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    bool ok = true;
    size_t offset = 0;
    while (offset < out->size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(out->size() - offset);
        if (!ReadFile(h, out->data() + offset, want, &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        offset += read;
    }
    CloseHandle(h);
    return ok && offset == out->size();
}

bool write_all_bytes(const fs::WidePath& path, const uint8_t* data, size_t size) noexcept {
    HANDLE h = CreateFileW(path.ptr, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD want = static_cast<DWORD>(size - offset);
        if (!WriteFile(h, data + offset, want, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    CloseHandle(h);
    return ok;
}

bool dpapi_protect(const std::string& plain, std::vector<uint8_t>* out) noexcept {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, L"OpenSlinky++ accounts", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        return false;
    }
    out->assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool dpapi_unprotect(const std::vector<uint8_t>& cipher, std::string* out) noexcept {
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(cipher.data());
    in.cbData = static_cast<DWORD>(cipher.size());
    DATA_BLOB blob{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        return false;
    }
    out->assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

void append_field(std::string* out, const std::string& field) {
    // Fields never legitimately contain the control separators; strip them defensively.
    for (char c : field) {
        if (c != kFieldSep && c != kRecordSep) {
            out->push_back(c);
        }
    }
    out->push_back(kFieldSep);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : s) {
        if (c == sep) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

} // namespace

std::vector<Account>& accounts() noexcept {
    return list();
}

bool load() noexcept {
    list().clear();
    fs::WidePath path{};
    if (!account_file_path(&path)) {
        return false;
    }
    std::vector<uint8_t> cipher;
    const bool read_ok = read_all_bytes(path, &cipher);
    fs::free_path(&path);
    if (!read_ok) {
        return true; // missing file => empty list, not an error
    }
    std::string plain;
    if (!dpapi_unprotect(cipher, &plain)) {
        return false;
    }
    const std::vector<std::string> records = split(plain, kRecordSep);
    if (records.empty() || records[0] != kMagic) {
        return false;
    }
    for (size_t i = 1; i < records.size(); ++i) {
        if (records[i].empty()) {
            continue;
        }
        const std::vector<std::string> f = split(records[i], kFieldSep);
        if (f.size() < 6) {
            continue;
        }
        Account a{};
        a.tag = f[0];
        a.name = f[1];
        a.uuid = f[2];
        a.token = f[3];
        a.refresh_token = f[4];
        int type = 0;
        for (char c : f[5]) {
            if (c >= '0' && c <= '9') {
                type = type * 10 + (c - '0');
            }
        }
        a.type = static_cast<AccountType>(type);
        list().push_back(std::move(a));
    }
    return true;
}

bool save() noexcept {
    std::string plain = kMagic;
    plain.push_back(kRecordSep);
    for (const Account& a : list()) {
        append_field(&plain, a.tag);
        append_field(&plain, a.name);
        append_field(&plain, a.uuid);
        append_field(&plain, a.token);
        append_field(&plain, a.refresh_token);
        append_field(&plain, std::to_string(static_cast<int>(a.type)));
        plain.push_back(kRecordSep);
    }
    std::vector<uint8_t> cipher;
    if (!dpapi_protect(plain, &cipher)) {
        return false;
    }
    fs::WidePath path{};
    if (!account_file_path(&path)) {
        return false;
    }
    const bool ok = write_all_bytes(path, cipher.data(), cipher.size());
    fs::free_path(&path);
    return ok;
}

size_t upsert(const Account& account) noexcept {
    std::vector<Account>& l = list();
    for (size_t i = 0; i < l.size(); ++i) {
        const bool same =
            (!account.uuid.empty() && l[i].uuid == account.uuid) ||
            (account.uuid.empty() && !account.name.empty() && l[i].name == account.name);
        if (!same) {
            continue;
        }
        Account merged = account;
        if (merged.tag.empty()) {
            merged.tag = l[i].tag.empty() ? account.name : l[i].tag;
        }
        // Never lose a refresh token on re-import if the new entry lacks one.
        if (merged.refresh_token.empty() && !l[i].refresh_token.empty()) {
            merged.refresh_token = l[i].refresh_token;
            if (merged.type == AccountType::Offline) {
                merged.type = AccountType::MSA;
            }
        }
        l[i] = std::move(merged);
        return i;
    }
    Account stored = account;
    if (stored.tag.empty()) {
        stored.tag = stored.name;
    }
    l.push_back(std::move(stored));
    return l.size() - 1;
}

void remove(size_t index) noexcept {
    std::vector<Account>& l = list();
    if (index < l.size()) {
        l.erase(l.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

} // namespace slinky::altmanager
