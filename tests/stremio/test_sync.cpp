// Build+run (host): g++ -std=c++17 -Iinclude -Isource -Isource/stremio tests/stremio/test_sync.cpp source/stremio/stremio_sync.cpp source/stremio/stremio_account.cpp source/core/library.cpp -pthread && ./a.out
// Host test for StremioSync + patched Library against a fake Stremio server.
#include <cassert>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include "stremio/stremio_sync.h"

namespace stremio { HttpPostFn makeCurlPost(const std::string&) { return nullptr; } }

using namespace ss;

static std::mutex g_mu;
static std::vector<mj::Value> g_puts;      // every item pushed
static bool g_listFlip = false;

static bool fake(const std::string& url, const std::string& body, std::string& resp, std::string&) {
    std::string method = url.substr(url.rfind('/') + 1);
    mj::Value req; assert(mj::parse(body, req));
    std::lock_guard<std::mutex> lk(g_mu);
    if (method == "login") resp = R"({"result":{"authKey":"K","user":{"email":"me@x.com"}}})";
    else if (method == "datastoreGet") resp = R"({"result":[
        {"_id":"tt0903747","name":"Breaking Bad","type":"series","poster":"p1","removed":false,"temp":false,
         "_mtime":"2026-10-01T10:00:00.000Z","extra":{"keep":true},
         "state":{"lastWatched":"2026-10-01T10:00:00.000Z","timeOffset":1440000,"duration":2880000,"video_id":"tt0903747:1:2","watched":"bits"}},
        {"_id":"tt1375666","name":"Inception","type":"movie","removed":true,"temp":true,"_mtime":"2026-10-02T09:00:00.000Z",
         "state":{"timeOffset":4440000,"duration":8880000}},
        {"_id":"tt0000001","name":"Deleted","type":"movie","removed":true,"temp":false,"_mtime":"2026-10-02T09:00:00.000Z","state":{"timeOffset":0}},
        {"_id":"kitsu:1","name":"Ch","type":"channel","removed":false,"temp":false,"_mtime":"2026-10-02T09:00:00.000Z","state":{}}]})";
    else if (method == "datastorePut") { for (auto& c : req.find("changes")->items()) g_puts.push_back(c); resp = R"({"result":{"success":true}})"; }
    else resp = R"({"result":{"success":true}})";
    return true;
}

static bool pumpUntil(StremioSync& s, Library& lib, const std::function<bool()>& cond, int ms = 5000) {
    for (int i = 0; i < ms / 20; i++) {
        if (s.pump()) { /* caller would save */ }
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main() {
    system("rm -rf /tmp/sst2 && mkdir -p /tmp/sst2");
    Library lib;
    {
        StremioSync sync;
        sync.init(lib, "/tmp/sst2", fake);
        assert(!sync.loggedIn());

        // Events while logged out must not crash or push.
        lib.updateProgress("tt9", "movie", "Local", "", "tt9", 0.5, 100000, 200000);
        assert(g_puts.empty());

        sync.beginLogin("me@x.com", "pw");
        assert(sync.busy());
        assert(pumpUntil(sync, lib, [&] { return lib.getItem("tt0903747") != nullptr; }));
        assert(sync.loggedIn() && !sync.busy());

        // Remote -> local mapping
        const LibraryItem* bb = lib.getItem("tt0903747");
        assert(bb && bb->bookmarked && bb->videoId == "tt0903747:1:2");
        assert(bb->progress > 0.49 && bb->progress < 0.51);
        assert(!bb->syncRaw.empty());
        const LibraryItem* inc = lib.getItem("tt1375666");
        assert(inc && !inc->bookmarked && inc->progress > 0.49 && inc->progress < 0.51);  // watch-only temp item
        assert(!lib.getItem("tt0000001"));    // deleted ghost skipped
        assert(!lib.getItem("kitsu:1"));      // unsupported type skipped
        assert(lib.getContinueWatching(10).size() == 3);
        // lastWatched parsed from ISO (2026-10-01T10:00:00Z = 1790848800)
        assert(bb->lastWatched == 1790848800);
        printf("status: %s\n", sync.statusText().c_str());

        // Local progress -> pushed (first event not throttled), unknown remote fields preserved
        lib.updateProgress("tt0903747", "series", "Breaking Bad", "p1", "tt0903747:1:3", 0.25, 720000, 2880000);
        assert(pumpUntil(sync, lib, [&] { std::lock_guard<std::mutex> l(g_mu); return !g_puts.empty(); }));
        {
            std::lock_guard<std::mutex> l(g_mu);
            mj::Value& p = g_puts.back();
            assert(p.getStr("_id") == "tt0903747");
            assert(p.find("extra") != nullptr);
            assert(p.find("state")->getStr("watched") == "bits");
            assert(p.find("state")->getInt("timeOffset") == 720000);
            assert(p.find("state")->getStr("video_id") == "tt0903747:1:3");
            assert(p.getBool("removed") == false);
        }

        // Second progress within 25 s is throttled (held as trailing, not pushed yet)
        size_t before; { std::lock_guard<std::mutex> l(g_mu); before = g_puts.size(); }
        lib.updateProgress("tt0903747", "series", "Breaking Bad", "p1", "tt0903747:1:3", 0.30, 864000, 2880000);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        { std::lock_guard<std::mutex> l(g_mu); assert(g_puts.size() == before); }

        // Bookmark toggle is immediate and carries the trailing progress too
        lib.toggleBookmark("tt1375666", "movie", "Inception", "");
        // Both items are reported, but the worker may upload them in one batch or two: wait for both.
        assert(pumpUntil(sync, lib, [&] {
            std::lock_guard<std::mutex> l(g_mu);
            bool a = false, b = false;
            for (size_t i = before; i < g_puts.size(); i++) {
                if (g_puts[i].getStr("_id") == "tt1375666") a = true;
                if (g_puts[i].getStr("_id") == "tt0903747") b = true;
            }
            return a && b;
        }));
        {
            std::lock_guard<std::mutex> l(g_mu);
            bool sawInception = false, sawBB = false;
            for (size_t i = before; i < g_puts.size(); i++) {
                if (g_puts[i].getStr("_id") == "tt1375666") { sawInception = true; assert(!g_puts[i].getBool("removed")); assert(!g_puts[i].getBool("temp")); }
                if (g_puts[i].getStr("_id") == "tt0903747") { sawBB = true; assert(g_puts[i].find("state")->getInt("timeOffset") == 864000); }
            }
            assert(sawInception && sawBB);
        }

        // Brand-new local title -> created with watch-only flags
        before = g_puts.size();
        lib.updateProgress("tt7777777", "movie", "New Movie", "pp", "tt7777777", 0.1, 600000, 6000000);
        lib.toggleBookmark("tt7777777", "movie", "New Movie", "pp");  // bookmark => flush
        assert(pumpUntil(sync, lib, [&] { std::lock_guard<std::mutex> l(g_mu); return g_puts.size() > before; }));

        // Finish playback (EOF event: progress 1.0, no ms) -> timesWatched+1, offset cleared
        before = g_puts.size();
        lib.updateProgress("tt1375666", "movie", "Inception", "", "tt1375666", 1.0);
        assert(pumpUntil(sync, lib, [&] { std::lock_guard<std::mutex> l(g_mu); return g_puts.size() > before; }));
        {
            std::lock_guard<std::mutex> l(g_mu);
            mj::Value& p = g_puts.back();
            assert(p.getStr("_id") == "tt1375666");
            assert(p.find("state")->getInt("timesWatched") == 1 && p.find("state")->getInt("timeOffset") == 0);
        }
        // Duplicate EOF event doesn't double count
        before = g_puts.size();
        lib.updateProgress("tt1375666", "movie", "Inception", "", "tt1375666", 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        { std::lock_guard<std::mutex> l(g_mu); assert(g_puts.size() == before); }

        // Library persistence round trip keeps syncRaw
        assert(lib.save("/tmp/sst2/library.json"));
        Library lib2; assert(lib2.load("/tmp/sst2/library.json"));
        assert(!lib2.getItem("tt0903747")->syncRaw.empty());

        // Leave a throttled update pending, then shut down: it must be persisted for next launch.
        lib.updateProgress("tt0903747", "series", "Breaking Bad", "p1", "tt0903747:1:3", 0.40, 1152000, 2880000);
        sync.shutdown();
        assert(access("/tmp/sst2/stremio_pending.json", 0) == 0);

        // Logout path (new instance)
    }
    {
        stremio::cancelPendingRequests();   // sticky flag only affects curl impl, not the fake
        Library lib3;
        StremioSync sync;
        sync.init(lib3, "/tmp/sst2", fake);
        assert(sync.loggedIn());                          // session restored from disk
        assert(pumpUntil(sync, lib3, [&] { return lib3.getItem("tt0903747") != nullptr; }));
        sync.logout();
        assert(pumpUntil(sync, lib3, [&] { return !sync.loggedIn() && !sync.busy(); }));
        assert(lib3.getItem("tt0903747") && lib3.getItem("tt0903747")->syncRaw.empty());
        printf("status: %s\n", sync.statusText().c_str());
        // Clear keeps listener
        lib3.clear(); assert(!lib3.getItem("tt0903747"));
    }
    puts("SYNC TESTS PASSED");
    return 0;
}
