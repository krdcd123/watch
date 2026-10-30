// Host test: g++ -std=c++17 -Isource/stremio tests/stremio/test_stremio.cpp source/stremio/stremio_account.cpp -pthread
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

#include "stremio_account.h"

namespace stremio {
HttpPostFn makeCurlPost(const std::string&) { return nullptr; }  // unused in host test
}

using namespace stremio;

static std::vector<std::string> g_calls;
static bool g_offline = false;
static mj::Value g_lastPut;

static bool fakePost(const std::string& url, const std::string& body, std::string& resp, std::string& err) {
    if (g_offline) { err = "offline"; return false; }
    std::string method = url.substr(url.rfind('/') + 1);
    g_calls.push_back(method);
    mj::Value req;
    assert(mj::parse(body, req));
    if (method == "login") {
        if (req.getStr("password") != "hunter2") { resp = R"({"error":{"message":"User not found","code":2}})"; return true; }
        resp = R"({"result":{"authKey":"KEY123","user":{"email":"me@x.com"}}})";
    } else if (method == "datastoreGet") {
        if (req.getStr("authKey") != "KEY123") { resp = R"({"error":{"message":"Session does not exist","code":1}})"; return true; }
        resp = R"({"result":[
          {"_id":"tt0903747","name":"Caf\u00e9 \ud83d\ude00 Show","type":"series","removed":false,"temp":false,
           "_mtime":"2026-10-01T10:00:00.000Z","futureField":{"keep":[1,2,3]},
           "state":{"timeOffset":1234000,"duration":2880000,"video_id":"tt0903747:1:2","watched":"abc:def","timesWatched":2}},
          {"_id":"tt1375666","name":"Inception","type":"movie","removed":true,"temp":true,"_mtime":"2026-10-02T10:00:00.000Z",
           "state":{"timeOffset":0,"duration":8880000}},
          {"name":"no id"}]})";
    } else if (method == "datastorePut") {
        g_lastPut = req;
        resp = R"({"result":{"success":true}})";
    } else if (method == "logout") {
        resp = R"({"result":{"success":true}})";
    }
    return true;
}

int main() {
    system("rm -rf /tmp/sst && mkdir -p /tmp/sst");
    {
        Account a("/tmp/sst", fakePost);
        std::string err;
        assert(!a.isLoggedIn());
        assert(!a.login("me@x.com", "wrong", err) && err == "User not found");
        assert(a.login("me@x.com", "hunter2", err));
        assert(a.isLoggedIn() && a.email() == "me@x.com");

        std::vector<LibraryItem> lib;
        assert(a.pullLibrary(lib, err));
        assert(lib.size() == 2);  // id-less entry skipped
        assert(lib[0].name() == "Caf\xc3\xa9 \xf0\x9f\x98\x80 Show");  // \u escapes + surrogates -> UTF-8
        assert(lib[0].inContinueWatching());
        assert(lib[0].videoId() == "tt0903747:1:2");
        assert(!lib[1].inContinueWatching());
        assert(lib[1].inLibrary() == false);

        // Edit progress; unknown fields must survive the round trip.
        LibraryItem it = lib[0];
        it.setProgress("tt0903747:1:3", 600000, 2900000, 30000);
        assert(it.timeOffset() == 600000 && it.videoId() == "tt0903747:1:3");
        assert(isNewer(it, &lib[0]));
        assert(!isNewer(lib[0], &it));

        // Offline: queue persists, flush fails, items stay queued.
        g_offline = true;
        a.queuePush(it);
        assert(a.pendingCount() == 1);
        assert(!a.flushPending(err));
        assert(a.pendingCount() == 1);
    }
    {
        // "Restart": pending queue + session reload from disk.
        Account a("/tmp/sst", fakePost);
        assert(a.load());
        assert(a.pendingCount() == 1);
        g_offline = false;
        std::string err;
        assert(a.flushPending(err));
        assert(a.pendingCount() == 0);

        const mj::Value* ch = g_lastPut.find("changes");
        assert(ch && ch->items().size() == 1);
        const mj::Value& sent = ch->items()[0];
        assert(sent.find("futureField") != nullptr);                     // unknown field kept
        assert(sent.find("state")->getStr("watched") == "abc:def");      // bitfield kept
        assert(sent.find("state")->getInt("timeOffset") == 600000);
        assert(sent.find("state")->getInt("overallTimeWatched") == 30000);
        assert(g_lastPut.getStr("collection") == "libraryItem");

        // New watch-only item.
        LibraryItem n = LibraryItem::create("tt1375666", "Inception", "movie", "http://p");
        n.setProgress("tt1375666", 5000, 8880000, 5000);
        assert(n.inContinueWatching() && !n.inLibrary());
        n.setInLibrary(true);
        assert(n.inLibrary());
        n.markWatched();
        assert(n.timesWatched() == 1 && n.timeOffset() == 0);

        // Worker: progress report gets pushed in background; pull callback fires.
        SyncWorker w(a);
        w.start();
        bool pulled = false;
        size_t count = 0;
        w.requestPull([&](std::vector<LibraryItem>&& v, const std::string& e) { count = v.size(); pulled = e.empty(); });
        w.reportProgress(n);
        for (int i = 0; i < 100 && (!pulled || a.pendingCount()); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(pulled && count == 2);
        assert(a.pendingCount() == 0);
        w.stop();

        // Logout clears everything.
        a.logout();
        assert(!a.isLoggedIn());
    }
    {
        Account a("/tmp/sst", fakePost);
        assert(!a.load());
    }
    // JSON edge cases
    mj::Value v;
    std::string e;
    assert(!mj::parse("{\"a\":", v, &e));
    assert(!mj::parse("[1,2,] x", v, &e));
    assert(mj::parse(" {\"a\":[1,2.5,-3e2,true,null,\"x\\n\"]} ", v));
    assert(v.dump() == "{\"a\":[1,2.5,-300,true,null,\"x\\n\"]}");
    puts("ALL TESTS PASSED");
    return 0;
}
