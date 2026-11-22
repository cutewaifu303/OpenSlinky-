#pragma once

// OpenSlinky++ Alt Manager - Microsoft / Minecraft authentication.
//
// Implements the MSA -> Xbox Live -> XSTS -> Minecraft services chain (login_with_xbox + profile) used by
// every modern Minecraft account manager. Four entry points, each runs the whole blocking flow on a worker
// thread and reports progress / the final Account into a shared LoginJob:
//   * Microsoft login  - opens the system browser to login.live.com and captures the OAuth code on a
//                         127.0.0.1 loopback listener (system-browser redirect flow).
//   * Refresh login    - starts from a pasted MSA refresh token (no browser).
//   * Cookie login      - replays pasted login.live.com cookies through the Xbox SISU endpoint
//                         (reference: naczo5/lunar-cookies) to reach an XSTS token without interactive sign-in.
// Session (offline/name:uuid:token) login needs no network and is handled directly in the Alt Manager.
//
// Networking uses slinky::net::https_request. Nothing here touches JNI; the resulting Account is injected
// into the live game separately (session_inject).

#include <mutex>
#include <string>

#include "modules/altmanager/account_store.hpp"

namespace slinky::altmanager {

// Thread-safe hand-off between a login worker thread and the Alt Manager UI poll.
struct LoginJob {
    std::mutex mutex;
    std::string progress;  // latest human-readable status line (guarded)
    std::string error;     // failure reason (guarded, set with done+!ok)
    Account account;       // result (guarded, valid when done && ok)
    bool done = false;     // guarded
    bool ok = false;       // guarded

    void report(const std::string& msg) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        progress = msg;
    }
    void finish_ok(const Account& result) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        account = result;
        ok = true;
        done = true;
    }
    void finish_error(const std::string& reason) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        error = reason;
        ok = false;
        done = true;
    }
};

// Each blocks until the flow completes, writing into `job`. Intended to run on a detached worker thread.
void run_microsoft_login(LoginJob* job) noexcept;
void run_refresh_login(LoginJob* job, const std::string& refresh_token) noexcept;
void run_cookie_login(LoginJob* job, const std::string& cookie_text) noexcept;

} // namespace slinky::altmanager
