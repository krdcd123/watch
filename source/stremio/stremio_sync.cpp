#include "stremio_sync.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace ss {

namespace {

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm); avoids needing timegm().
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// "2026-10-29T18:30:00.000Z" -> unix seconds (0 if unparsable)
int64_t isoToEpoch(const std::string& iso) {
    int Y, M, D, h, m;
    double s;
    if (iso.size() < 19 || sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &s) != 6) return 0;
    return daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + (int64_t)s;
}

bool supportedType(const std::string& t) { return t == "movie" || t == "series"; }

} // namespace

// ───────────────────────────── lifecycle ─────────────────────────────

void StremioSync::init(Library& lib, const std::string& dir, stremio::HttpPostFn post) {
    if (m_initialized) return;
    m_lib = &lib;
    m_dir = dir;
    m_post = post;
    m_acct = new stremio::Account(dir, post);
    m_acct->load();
    m_worker = new stremio::SyncWorker(*m_acct);
    m_worker->start();
    m_initialized = true;

    lib.setListener([this](LibraryItem& item, const LibraryEvent& ev) { onLibraryEvent(item, ev); });

    if (m_acct->isLoggedIn()) {
        setStatus("Signed in as " + m_acct->email() + " - syncing...");
        requestPull();
    } else {
        setStatus("Sync watch history with your Stremio account");
    }
}

void StremioSync::shutdown() {
    if (!m_initialized) return;
    stremio::cancelPendingRequests();           // don't wait on slow networks at exit
    if (m_loginThread.joinable()) m_loginThread.join();
    flushTrailing(true);                        // unsent progress -> disk, retried next launch
    if (m_worker) { m_worker->stop(); delete m_worker; m_worker = nullptr; }
    if (m_lib) m_lib->setListener(nullptr);
    delete m_acct;
    m_acct = nullptr;
    m_initialized = false;
}

bool StremioSync::loggedIn() const { return m_acct && m_acct->isLoggedIn(); }
std::string StremioSync::email() const { return m_acct ? m_acct->email() : std::string(); }

void StremioSync::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    m_status = s;
}

std::string StremioSync::statusText() const {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    return m_status;
}

void StremioSync::requestPull() {
    if (!m_worker || m_pulling.exchange(true)) return;
    m_worker->requestPull([this](std::vector<stremio::LibraryItem>&& items, const std::string& err) {
        std::lock_guard<std::mutex> lk(m_inboxMutex);
        m_inbox = std::move(items);
        m_pullError = err;
        m_inboxReady = true;
    });
}

void StremioSync::syncNow() {
    if (loggedIn() && !busy()) {
        setStatus("Syncing...");
        requestPull();
    }
}

// ───────────────────────────── login / logout ─────────────────────────────

void StremioSync::beginLogin(const std::string& email, const std::string& password) {
    if (!m_acct || busy()) return;
    if (m_loginThread.joinable()) m_loginThread.join();
    setStatus("Signing in...");
    m_loginIsLogout = false;
    m_loginEmail = email;
    m_loginState.store(1);
    m_loginThread = std::thread([this, email, password]() {
        std::string err;
        m_loginOk = m_acct->login(email, password, err);
        m_loginErr = err;
        m_loginState.store(2);
    });
}

void StremioSync::logout() {
    if (!m_acct || busy() || !m_acct->isLoggedIn()) return;
    if (m_loginThread.joinable()) m_loginThread.join();
    setStatus("Signing out...");
    m_trailing.clear();
    m_loginIsLogout = true;
    m_loginState.store(1);
    m_loginThread = std::thread([this]() {
        m_acct->logout();   // best-effort server logout, always clears the local session
        m_loginOk = true;
        m_loginErr.clear();
        m_loginState.store(2);
    });
}

// ───────────────────────────── per-frame work ─────────────────────────────

bool StremioSync::pump() {
    if (!m_initialized) return false;
    bool changed = false;

    // 1) login / logout finished?
    if (m_loginState.load() == 2) {
        if (m_loginThread.joinable()) m_loginThread.join();
        if (m_loginIsLogout) {
            if (m_lib) m_lib->clearSyncData();
            setStatus("Signed out. Local history kept on this Switch.");
            changed = true;  // syncRaw cleared -> save
        } else if (m_loginOk) {
            setStatus("Signed in as " + m_acct->email() + " - syncing...");
            requestPull();
        } else {
            setStatus("Sign-in failed: " + m_loginErr);
        }
        m_loginState.store(0);
    }

    // 2) pulled library arrived?
    std::vector<stremio::LibraryItem> items;
    std::string err;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lk(m_inboxMutex);
        if (m_inboxReady) {
            items.swap(m_inbox);
            err = m_pullError;
            m_inboxReady = false;
            ready = true;
        }
    }
    if (ready) {
        m_pulling.store(false);
        if (!err.empty()) {
            if (m_acct->sessionExpired()) setStatus("Session expired - sign out and sign in again");
            else setStatus("Sync failed: " + err);
        } else {
            size_t n = mergeRemote(items);
            char buf[64];
            snprintf(buf, sizeof buf, " - synced");
            setStatus("Signed in as " + m_acct->email() + buf);
            if (n > 0) changed = true;
        }
    }

    // 3) trailing (throttled) progress push
    if (!m_trailing.empty() && steadyMs() - m_lastPushMs >= 25000) flushTrailing(false);

    return changed;
}

// ───────────────────────────── local -> remote ─────────────────────────────

void StremioSync::flushTrailing(bool persistOnly) {
    for (auto& kv : m_trailing) {
        if (persistOnly) m_acct->queuePush(kv.second);
        else if (m_worker) m_worker->reportProgress(kv.second);
    }
    m_trailing.clear();
    m_lastPushMs = steadyMs();
}

void StremioSync::onLibraryEvent(LibraryItem& item, const LibraryEvent& ev) {
    if (!m_acct || !m_acct->isLoggedIn()) return;
    if (!supportedType(item.type) || item.id.empty()) return;

    // Start from the last known remote object so fields we don't model survive.
    stremio::LibraryItem s;
    if (!item.syncRaw.empty()) {
        mj::Value v;
        if (mj::parse(item.syncRaw, v) && v.isObject()) s.raw = std::move(v);
    }
    if (s.id().empty()) s = stremio::LibraryItem::create(item.id, item.name, item.type, item.poster);

    bool immediate = false;
    const std::string vid = item.videoId.empty() ? item.id : item.videoId;

    if (ev.type == LibraryEventType::Progress) {
        if (ev.positionMs >= 0) {
            // Real playback tick.
            int64_t delta = 0;
            if (s.videoId() == vid) {
                delta = ev.positionMs - s.timeOffset();
                if (delta < 0 || delta > 60000) delta = 0;
            }
            s.setProgress(vid, ev.positionMs, ev.durationMs, delta);
        } else if (item.progress >= 0.95) {
            // Finished playback.
            bool alreadyDone = (s.timeOffset() == 0 && s.timesWatched() > 0 && s.videoId() == vid);
            if (alreadyDone) return;
            s.setProgress(vid, s.timeOffset(), 0, 0);
            s.markWatched();
            immediate = true;
        } else {
            return;  // "playback started" placeholder, no resume point yet
        }
    } else {
        s.setInLibrary(item.bookmarked);
        immediate = true;
    }

    item.syncRaw = s.raw.dump();
    m_trailing[item.id] = std::move(s);
    if (immediate || steadyMs() - m_lastPushMs >= 25000) flushTrailing(false);
}

// ───────────────────────────── remote -> local ─────────────────────────────

size_t StremioSync::mergeRemote(std::vector<stremio::LibraryItem>& items) {
    if (!m_lib) return 0;
    size_t applied = 0;

    for (auto& r : items) {
        const std::string id = r.id();
        if (id.empty() || !supportedType(r.type())) continue;

        const LibraryItem* local = m_lib->getItem(id);
        int64_t rEpoch = isoToEpoch(r.mtime());
        if (rEpoch == 0) rEpoch = isoToEpoch(r.lastWatched());

        // Local copy is same age or newer: keep its data (its edits were already queued for push),
        // but adopt the remote object so later pushes preserve fields we don't model.
        if (local && local->lastWatched >= rEpoch) {
            if (local->syncRaw.empty()) {
                LibraryItem li = *local;
                li.syncRaw = r.raw.dump();
                m_lib->setItem(li);
                applied++;
            }
            continue;
        }

        const int64_t off = r.timeOffset();
        const int64_t dur = r.duration();
        const bool hasWatch = off > 0 || r.timesWatched() > 0;
        const bool dead = r.removed() && !r.temp();   // deleted from library, not just history

        if (dead && !hasWatch) {
            // Un-bookmarked remotely and never watched: just clear a stale local bookmark.
            if (local && local->bookmarked) {
                LibraryItem li = *local;
                li.bookmarked = false;
                li.syncRaw = r.raw.dump();
                m_lib->setItem(li);
                applied++;
            }
            continue;
        }

        LibraryItem li = local ? *local : LibraryItem();
        li.id = id;
        li.type = r.type();
        li.name = r.name().empty() ? li.name : r.name();
        li.poster = r.poster().empty() ? li.poster : r.poster();
        li.videoId = r.videoId().empty() ? id : r.videoId();
        li.bookmarked = r.inLibrary();
        li.lastWatched = rEpoch > 0 ? rEpoch : (int64_t)std::time(nullptr);
        li.syncRaw = r.raw.dump();

        if (off > 0 && dur > 0) li.progress = std::clamp((double)off / (double)dur, 0.011, 1.0);
        else if (off > 0)       li.progress = 0.02;
        else                    li.progress = r.timesWatched() > 0 ? 1.0 : 0.0;

        m_lib->setItem(li);
        applied++;
    }
    return applied;
}

} // namespace ss
