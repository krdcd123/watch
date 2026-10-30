#pragma once

// ─────────────────────────────────────────────
// StremioSync — glue between ss::Library and a Stremio account.
//
//  * mirrors local watch progress / bookmarks to the account (background thread)
//  * pulls the account's library + history and merges it into ss::Library
//  * all network work happens off the UI thread; call pump() once per frame
// ─────────────────────────────────────────────

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/library.h"
#include "stremio_account.h"

namespace ss {

class StremioSync {
public:
    StremioSync() = default;
    ~StremioSync() { shutdown(); }

    // Call once after the library is loaded. `dir` is the app's data folder.
    // `post` can be injected for tests; default uses libcurl.
    void init(Library& lib, const std::string& dir, stremio::HttpPostFn post = nullptr);
    void shutdown();

    // Call once per frame on the main thread. Returns true when the library changed
    // (remote data merged) so the caller can refresh the UI and save library.json.
    bool pump();

    bool loggedIn() const;
    bool busy() const { return m_loginState.load() != 0; }
    std::string email() const;

    // Non-blocking login: runs on a thread, then pulls the library.
    void beginLogin(const std::string& email, const std::string& password);
    void logout();
    void syncNow();                    // request a fresh pull

    // Short text for the Settings row ("Signing in...", "Signed in as ...", error, ...).
    std::string statusText() const;

private:
    void onLibraryEvent(LibraryItem& item, const LibraryEvent& ev);
    void setStatus(const std::string& s);
    void requestPull();
    size_t mergeRemote(std::vector<stremio::LibraryItem>& items);
    void flushTrailing(bool persistOnly);

    Library* m_lib = nullptr;
    std::string m_dir;
    stremio::HttpPostFn m_post;
    stremio::Account* m_acct = nullptr;
    stremio::SyncWorker* m_worker = nullptr;

    // remote -> main thread hand-off
    std::mutex m_inboxMutex;
    std::vector<stremio::LibraryItem> m_inbox;
    bool m_inboxReady = false;
    std::string m_pullError;

    // throttled progress pushes (latest state per id)
    std::unordered_map<std::string, stremio::LibraryItem> m_trailing;
    int64_t m_lastPushMs = -1000000;

    // login thread
    std::thread m_loginThread;
    std::atomic<int> m_loginState{0};     // 0 idle, 1 busy, 2 done
    bool m_loginOk = false;
    bool m_loginIsLogout = false;
    std::string m_loginErr;
    std::string m_loginEmail;
    std::atomic<bool> m_pulling{false};

    mutable std::mutex m_statusMutex;
    std::string m_status;
    bool m_initialized = false;
};

} // namespace ss
