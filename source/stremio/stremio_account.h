// stremio_account.h - Stremio account login + library / watch-history sync.
//
// Talks to https://api.strem.io/api/<method> (the same JSON API the official
// Stremio clients use). Self-contained: only needs libcurl (see stremio_curl.cpp)
// and the bundled mini_json.h.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mini_json.h"

namespace stremio {

// POST `jsonBody` to `url`, fill `response`. Return false + `err` on transport failure.
using HttpPostFn = std::function<bool(const std::string& url, const std::string& jsonBody,
                                      std::string& response, std::string& err)>;

// libcurl-backed implementation (stremio_curl.cpp). `caBundlePath` e.g. "romfs:/cacert.pem".
HttpPostFn makeCurlPost(const std::string& caBundlePath);

// Abort in-flight and future requests quickly (call before shutdown so exit never waits on the network).
void cancelPendingRequests();
bool cancelRequested();

// ISO-8601 UTC timestamp with milliseconds, e.g. 2026-10-29T18:30:00.000Z
std::string nowIso();

// ---------------------------------------------------------------------------
// LibraryItem: thin wrapper over the raw JSON object Stremio stores.
// The raw object is kept as-is so fields we don't understand (behaviorHints,
// the `watched` episode bitfield, ...) survive a pull -> edit -> push round trip.
// ---------------------------------------------------------------------------
struct LibraryItem {
    mj::Value raw = mj::Value::object();

    std::string id() const { return raw.getStr("_id"); }
    std::string name() const { return raw.getStr("name"); }
    std::string type() const { return raw.getStr("type"); }  // "movie" | "series" | ...
    std::string poster() const { return raw.getStr("poster"); }
    std::string mtime() const { return raw.getStr("_mtime"); }
    bool removed() const { return raw.getBool("removed"); }
    bool temp() const { return raw.getBool("temp"); }

    // state.* accessors (all times in milliseconds)
    int64_t timeOffset() const { return stateInt("timeOffset"); }
    int64_t duration() const { return stateInt("duration"); }
    int64_t timesWatched() const { return stateInt("timesWatched"); }
    std::string videoId() const;      // "tt0903747:1:2" for episodes, == id for movies
    std::string lastWatched() const;

    // Shown in Stremio's "Continue Watching" row.
    bool inContinueWatching() const;
    // Explicitly in the user's library (bookmarked), not just history.
    bool inLibrary() const { return !removed() && !temp(); }

    // New item for something the user watched without bookmarking it.
    static LibraryItem create(const std::string& id, const std::string& name,
                              const std::string& type, const std::string& poster);

    // Update playback state. `watchedDeltaMs` = real time watched since last report.
    void setProgress(const std::string& videoId, int64_t offsetMs, int64_t durationMs,
                     int64_t watchedDeltaMs = 0);
    // Finished: bump times-watched, clear resume point.
    void markWatched();
    // Bookmark / un-bookmark.
    void setInLibrary(bool inLib);
    // Bump _mtime (called by all mutators).
    void touch();

private:
    int64_t stateInt(const char* k) const;
    mj::Value& stateObj();
};

// true if `remote` is strictly newer than `local` (nullptr local => true).
bool isNewer(const LibraryItem& remote, const LibraryItem* local);

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------
class Account {
public:
    // configDir e.g. "sdmc:/switch/switchstream". `post` defaults to makeCurlPost("romfs:/cacert.pem").
    explicit Account(std::string configDir, HttpPostFn post = nullptr);

    bool load();                    // read saved session + pending queue (call at startup)
    bool isLoggedIn() const;
    std::string email() const;

    bool login(const std::string& email, const std::string& password, std::string& err);
    void logout();                  // best-effort server logout, always clears local session

    // Full library + history pull (every libraryItem on the account).
    bool pullLibrary(std::vector<LibraryItem>& out, std::string& err);
    // Immediate push (no queue).
    bool pushItems(const std::vector<LibraryItem>& items, std::string& err);

    // Offline-safe push: coalesces by id, persisted to disk, flushed by flushPending().
    void queuePush(const LibraryItem& item);
    bool flushPending(std::string& err);
    size_t pendingCount() const;

    // True if the last API error looked like an expired/invalid session.
    bool sessionExpired() const;

private:
    bool apiCall(const char* method, mj::Value body, mj::Value& result, std::string& err);
    std::string authKey() const;
    void saveSession();
    void savePending();                 // caller must hold mu_
    static bool writeFileAtomic(const std::string& path, const std::string& data);
    static bool readFile(const std::string& path, std::string& out);

    std::string dir_;
    HttpPostFn post_;
    mutable std::mutex mu_;
    std::string authKey_;
    std::string email_;
    std::map<std::string, LibraryItem> pending_;
    bool sessionExpired_ = false;
};

// ---------------------------------------------------------------------------
// SyncWorker: background thread so the UI / player never blocks on the network.
// ---------------------------------------------------------------------------
class SyncWorker {
public:
    using PullCb = std::function<void(std::vector<LibraryItem>&& items, const std::string& err)>;

    explicit SyncWorker(Account& acct) : acct_(acct) {}
    ~SyncWorker() { stop(); }

    void start();
    void stop();

    // Flush pending pushes, then pull the library. `cb` runs on the worker thread.
    void requestPull(PullCb cb);
    // Queue a changed item (e.g. every ~30s during playback and on stop) and wake the worker.
    void reportProgress(const LibraryItem& item);

private:
    void run();

    Account& acct_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool running_ = false;
    bool stop_ = false;
    bool pullReq_ = false;
    bool dirty_ = false;
    PullCb cb_;
};

}  // namespace stremio
