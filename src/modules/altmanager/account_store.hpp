#pragma once

// OpenSlinky++ Alt Manager - persistent account store.
//
// Accounts are kept in %USERPROFILE%\.slinky\accounts.dat as a single DPAPI-encrypted (user scope) blob so
// the access / refresh tokens are not readable at rest. The in-memory list is the source of truth; call
// save() after any mutation. All functions are main-thread only (no internal locking).

#include <cstdint>
#include <string>
#include <vector>

namespace slinky::altmanager {

enum class AccountType : int32_t {
    Offline = 0, // cracked / offline-mode name (no network validation)
    Mojang  = 1, // legacy name:uuid:token session
    MSA     = 2, // Microsoft account (has a refresh token)
};

struct Account {
    std::string tag;           // user-facing label (defaults to name)
    std::string name;          // Minecraft username
    std::string uuid;          // profile id, no dashes
    std::string token;         // Minecraft access token (Bearer)
    std::string refresh_token; // MSA refresh token (empty for offline/mojang)
    AccountType type = AccountType::Offline;
};

// Loads accounts.dat into the in-memory list (clears it first). Missing file = empty list, returns true.
bool load() noexcept;

// Writes the in-memory list back to accounts.dat (DPAPI-encrypted). Returns false on I/O failure.
bool save() noexcept;

// Access to the in-memory list.
std::vector<Account>& accounts() noexcept;

// Adds or replaces (matched by uuid when present, else by name). Never downgrades an MSA entry that has a
// refresh token to one without. Returns the index of the stored account.
size_t upsert(const Account& account) noexcept;

// Removes the account at `index` (no-op if out of range).
void remove(size_t index) noexcept;

} // namespace slinky::altmanager
