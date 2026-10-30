#include "stremio_account.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace stremio {

static const char* kApiBase = "https://api.strem.io/api/";
static const char* kSessionFile = "stremio_account.json";
static const char* kPendingFile = "stremio_pending.json";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::atomic<bool> g_cancel{false};
void cancelPendingRequests() { g_cancel.store(true); }
bool cancelRequested() { return g_cancel.load(); }

std::string nowIso() {
    using namespace std::chrono;
    long long ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    time_t sec = (time_t)(ms / 1000);
    int msr = (int)(ms % 1000);
    struct tm t;
    gmtime_r(&sec, &t);
    char buf[96];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, msr);
    return buf;
}

bool isNewer(const LibraryItem& remote, const LibraryItem* local) {
    if (!local) return true;
    // Same fixed-width ISO format => lexicographic order == chronological order.
    return remote.mtime() > local->mtime();
}

// ---------------------------------------------------------------------------
// LibraryItem
// ---------------------------------------------------------------------------
int64_t LibraryItem::stateInt(const char* k) const {
    const mj::Value* s = raw.find("state");
    return s ? s->getInt(k) : 0;
}

mj::Value& LibraryItem::stateObj() {
    mj::Value& s = raw["state"];
    if (!s.isObject()) s = mj::Value::object();
    return s;
}

std::string LibraryItem::videoId() const {
    const mj::Value* s = raw.find("state");
    return s ? s->getStr("video_id") : "";
}

std::string LibraryItem::lastWatched() const {
    const mj::Value* s = raw.find("state");
    return s ? s->getStr("lastWatched") : "";
}

bool LibraryItem::inContinueWatching() const {
    // Mirrors Stremio's rule: visible item (or watch-only "temp" item) with a resume point.
    return (!removed() || temp()) && timeOffset() > 0 && type() != "other";
}

LibraryItem LibraryItem::create(const std::string& id, const std::string& name,
                                const std::string& type, const std::string& poster) {
    LibraryItem it;
    std::string now = nowIso();
    it.raw.set("_id", mj::Value::string(id));
    it.raw.set("name", mj::Value::string(name));
    it.raw.set("type", mj::Value::string(type));
    it.raw.set("poster", mj::Value::string(poster));
    it.raw.set("posterShape", mj::Value::string("poster"));
    // Watched-but-not-bookmarked: hidden from Library, still shown in Continue Watching.
    it.raw.set("removed", mj::Value::boolean(true));
    it.raw.set("temp", mj::Value::boolean(true));
    it.raw.set("_ctime", mj::Value::string(now));
    it.raw.set("_mtime", mj::Value::string(now));

    mj::Value st = mj::Value::object();
    st.set("lastWatched", mj::Value::string(now));
    st.set("timeWatched", mj::Value::number(0));
    st.set("timeOffset", mj::Value::number(0));
    st.set("overallTimeWatched", mj::Value::number(0));
    st.set("timesWatched", mj::Value::number(0));
    st.set("flaggedWatched", mj::Value::number(0));
    st.set("duration", mj::Value::number(0));
    st.set("noNotif", mj::Value::boolean(false));
    it.raw.set("state", std::move(st));

    mj::Value bh = mj::Value::object();
    bh.set("defaultVideoId", mj::Value());
    bh.set("featuredVideoId", mj::Value());
    bh.set("hasScheduledVideos", mj::Value::boolean(false));
    it.raw.set("behaviorHints", std::move(bh));
    return it;
}

void LibraryItem::touch() {
    raw.set("_mtime", mj::Value::string(nowIso()));
}

void LibraryItem::setProgress(const std::string& videoId, int64_t offsetMs, int64_t durationMs,
                              int64_t watchedDeltaMs) {
    mj::Value& s = stateObj();
    if (!videoId.empty()) s.set("video_id", mj::Value::string(videoId));
    s.set("timeOffset", mj::Value::number((double)offsetMs));
    if (durationMs > 0) s.set("duration", mj::Value::number((double)durationMs));
    if (watchedDeltaMs > 0) {
        s.set("timeWatched", mj::Value::number((double)(s.getInt("timeWatched") + watchedDeltaMs)));
        s.set("overallTimeWatched",
              mj::Value::number((double)(s.getInt("overallTimeWatched") + watchedDeltaMs)));
    }
    s.set("lastWatched", mj::Value::string(nowIso()));
    touch();
}

void LibraryItem::markWatched() {
    mj::Value& s = stateObj();
    s.set("timesWatched", mj::Value::number((double)(s.getInt("timesWatched") + 1)));
    s.set("flaggedWatched", mj::Value::number(1));
    s.set("timeOffset", mj::Value::number(0));
    s.set("lastWatched", mj::Value::string(nowIso()));
    touch();
}

void LibraryItem::setInLibrary(bool inLib) {
    raw.set("removed", mj::Value::boolean(!inLib));
    if (inLib) raw.set("temp", mj::Value::boolean(false));
    touch();
}

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------
Account::Account(std::string configDir, HttpPostFn post) : dir_(std::move(configDir)), post_(std::move(post)) {
    while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
    #ifdef __SWITCH__
    if (!post_) post_ = makeCurlPost("romfs:/cacert.pem");
#else
    if (!post_) post_ = makeCurlPost("romfs/cacert.pem");
#endif
}

bool Account::readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool Account::writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());  // sdmc rename() won't overwrite
    return rename(tmp.c_str(), path.c_str()) == 0;
}

void Account::saveSession() {
    std::string key, mail;
    {
        std::lock_guard<std::mutex> lk(mu_);
        key = authKey_;
        mail = email_;
    }
    std::string path = dir_ + "/" + kSessionFile;
    if (key.empty()) { remove(path.c_str()); return; }
    mj::Value v = mj::Value::object();
    v.set("authKey", mj::Value::string(key));
    v.set("email", mj::Value::string(mail));
    writeFileAtomic(path, v.dump());
}

void Account::savePending() {
    std::string path = dir_ + "/" + kPendingFile;
    if (pending_.empty()) { remove(path.c_str()); return; }
    mj::Value arr = mj::Value::array();
    for (auto& kv : pending_) arr.push(kv.second.raw);
    writeFileAtomic(path, arr.dump());
}

bool Account::load() {
    std::string txt;
    mj::Value v;
    if (readFile(dir_ + "/" + kSessionFile, txt) && mj::parse(txt, v)) {
        std::lock_guard<std::mutex> lk(mu_);
        authKey_ = v.getStr("authKey");
        email_ = v.getStr("email");
    }
    if (readFile(dir_ + "/" + kPendingFile, txt) && mj::parse(txt, v) && v.isArray()) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& it : v.items()) {
            LibraryItem li;
            li.raw = it;
            if (!li.id().empty()) pending_[li.id()] = std::move(li);
        }
    }
    return isLoggedIn();
}

bool Account::isLoggedIn() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !authKey_.empty();
}

std::string Account::email() const {
    std::lock_guard<std::mutex> lk(mu_);
    return email_;
}

std::string Account::authKey() const {
    std::lock_guard<std::mutex> lk(mu_);
    return authKey_;
}

bool Account::sessionExpired() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessionExpired_;
}

bool Account::apiCall(const char* method, mj::Value body, mj::Value& result, std::string& err) {
    std::string resp, terr;
    if (!post_(std::string(kApiBase) + method, body.dump(), resp, terr)) {
        err = terr.empty() ? "network error" : terr;
        return false;
    }
    mj::Value v;
    std::string perr;
    if (!mj::parse(resp, v, &perr) || !v.isObject()) {
        err = "bad response from Stremio (" + perr + ")";
        return false;
    }
    if (const mj::Value* e = v.find("error")) {
        std::string msg = e->isObject() ? e->getStr("message") : e->asStr();
        if (msg.empty()) msg = "Stremio API error";
        // Stremio reports invalid/expired auth keys as a "session" error.
        std::string low;
        for (char c : msg) low += (char)tolower((unsigned char)c);
        if (low.find("session") != std::string::npos || low.find("auth") != std::string::npos) {
            std::lock_guard<std::mutex> lk(mu_);
            if (!authKey_.empty()) sessionExpired_ = true;
        }
        err = msg;
        return false;
    }
    const mj::Value* r = v.find("result");
    result = r ? *r : mj::Value();
    return true;
}

bool Account::login(const std::string& email, const std::string& password, std::string& err) {
    mj::Value body = mj::Value::object();
    body.set("type", mj::Value::string("Login"));
    body.set("email", mj::Value::string(email));
    body.set("password", mj::Value::string(password));
    body.set("facebook", mj::Value::boolean(false));

    mj::Value res;
    if (!apiCall("login", std::move(body), res, err)) return false;
    std::string key = res.getStr("authKey");
    if (key.empty()) { err = "login succeeded but no auth key was returned"; return false; }

    std::string mail = email;
    if (const mj::Value* u = res.find("user")) {
        std::string m = u->getStr("email");
        if (!m.empty()) mail = m;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        authKey_ = key;
        email_ = mail;
        sessionExpired_ = false;
    }
    saveSession();
    return true;
}

void Account::logout() {
    std::string key = authKey();
    if (!key.empty()) {
        mj::Value body = mj::Value::object();
        body.set("type", mj::Value::string("Logout"));
        body.set("authKey", mj::Value::string(key));
        mj::Value res;
        std::string e;
        apiCall("logout", std::move(body), res, e);  // best effort
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        authKey_.clear();
        email_.clear();
        sessionExpired_ = false;
        pending_.clear();
        savePending();
    }
    saveSession();
}

bool Account::pullLibrary(std::vector<LibraryItem>& out, std::string& err) {
    std::string key = authKey();
    if (key.empty()) { err = "not logged in"; return false; }

    mj::Value body = mj::Value::object();
    body.set("authKey", mj::Value::string(key));
    body.set("collection", mj::Value::string("libraryItem"));
    body.set("ids", mj::Value::array());
    body.set("all", mj::Value::boolean(true));

    mj::Value res;
    if (!apiCall("datastoreGet", std::move(body), res, err)) return false;
    if (!res.isArray()) { err = "unexpected library format"; return false; }

    out.clear();
    out.reserve(res.items().size());
    for (auto& it : res.items()) {
        if (!it.isObject()) continue;
        LibraryItem li;
        li.raw = std::move(it);
        if (!li.id().empty()) out.push_back(std::move(li));
    }
    return true;
}

bool Account::pushItems(const std::vector<LibraryItem>& items, std::string& err) {
    std::string key = authKey();
    if (key.empty()) { err = "not logged in"; return false; }

    const size_t kChunk = 50;
    for (size_t i = 0; i < items.size(); i += kChunk) {
        mj::Value changes = mj::Value::array();
        for (size_t j = i; j < items.size() && j < i + kChunk; j++) changes.push(items[j].raw);

        mj::Value body = mj::Value::object();
        body.set("authKey", mj::Value::string(key));
        body.set("collection", mj::Value::string("libraryItem"));
        body.set("changes", std::move(changes));

        mj::Value res;
        if (!apiCall("datastorePut", std::move(body), res, err)) return false;
    }
    return true;
}

void Account::queuePush(const LibraryItem& item) {
    if (item.id().empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    pending_[item.id()] = item;  // newer state for the same id replaces older
    savePending();
}

size_t Account::pendingCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_.size();
}

bool Account::flushPending(std::string& err) {
    std::vector<LibraryItem> batch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : pending_) batch.push_back(kv.second);
    }
    if (batch.empty()) return true;
    if (!pushItems(batch, err)) return false;

    std::lock_guard<std::mutex> lk(mu_);
    for (auto& sent : batch) {
        auto it = pending_.find(sent.id());
        // Only drop it if it wasn't updated again while we were uploading.
        if (it != pending_.end() && it->second.mtime() == sent.mtime()) pending_.erase(it);
    }
    savePending();
    return true;
}

// ---------------------------------------------------------------------------
// SyncWorker
// ---------------------------------------------------------------------------
void SyncWorker::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
    stop_ = false;
    th_ = std::thread(&SyncWorker::run, this);
}

void SyncWorker::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;
}

void SyncWorker::requestPull(PullCb cb) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb_ = std::move(cb);
        pullReq_ = true;
    }
    cv_.notify_all();
}

void SyncWorker::reportProgress(const LibraryItem& item) {
    acct_.queuePush(item);
    {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_ = true;
    }
    cv_.notify_all();
}

void SyncWorker::run() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        // Wake on work, or every 30 s to retry anything that failed (e.g. offline).
        cv_.wait_for(lk, std::chrono::seconds(30), [&] { return stop_ || pullReq_ || dirty_; });
        if (stop_) break;

        bool doPull = pullReq_;
        PullCb cb = cb_;
        pullReq_ = false;
        dirty_ = false;
        lk.unlock();

        if (acct_.isLoggedIn()) {
            std::string err;
            if (acct_.pendingCount() > 0) acct_.flushPending(err);  // failures stay queued
            if (doPull) {
                std::vector<LibraryItem> items;
                std::string perr;
                bool ok = acct_.pullLibrary(items, perr);
                if (cb) cb(ok ? std::move(items) : std::vector<LibraryItem>(), ok ? "" : perr);
            }
        } else if (doPull && cb) {
            cb({}, "not logged in");
        }

        lk.lock();
    }
}

}  // namespace stremio
