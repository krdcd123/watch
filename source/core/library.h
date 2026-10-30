#pragma once

// ─────────────────────────────────────────────
// Library — Watch history & bookmarks
// Persists to SD card as lightweight JSON
// ─────────────────────────────────────────────

#include "types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace ss {

// Change notification so an optional sync layer (Stremio account) can mirror edits.
enum class LibraryEventType { Progress, Bookmark };
struct LibraryEvent {
    LibraryEventType type = LibraryEventType::Progress;
    double prevProgress = 0.0;   // progress before this update
    int64_t positionMs = -1;     // playback position, -1 if unknown
    int64_t durationMs = -1;     // media duration, -1 if unknown
};
// Receives the stored item (mutable, so the listener can update item.syncRaw).
using LibraryListener = std::function<void(LibraryItem&, const LibraryEvent&)>;

class Library {
public:
    // Register a listener (called on the thread that edits the library).
    void setListener(LibraryListener l) { m_listener = std::move(l); }

    // Remove all items (keeps the listener, unlike assigning a fresh Library).
    void clear() { m_items.clear(); }

    // Forget cached Stremio account data on every item (used on sign-out).
    void clearSyncData() { for (auto& kv : m_items) kv.second.syncRaw.clear(); }

    // Insert/replace an item silently (no listener call). Used when merging remote data.
    void setItem(const LibraryItem& item) { m_items[item.id] = item; }

    // Load from SD card
    bool load(const std::string& path);

    // Save to SD card
    bool save(const std::string& path) const;

    // Update watch progress for an item
    void updateProgress(const std::string& id, const std::string& type,
                        const std::string& name, const std::string& poster,
                        const std::string& videoId, double progress,
                        int64_t positionMs = -1, int64_t durationMs = -1);

    // Toggle bookmark
    void toggleBookmark(const std::string& id, const std::string& type,
                        const std::string& name, const std::string& poster);

    // Get item (nullptr if not in library)
    const LibraryItem* getItem(const std::string& id) const;

    // Get recently watched (sorted by lastWatched desc)
    std::vector<LibraryItem> getRecentlyWatched(int limit = 20) const;

    // Get bookmarked items
    std::vector<LibraryItem> getBookmarked() const;

    // Get "continue watching" items (progress > 0 and < 0.95)
    std::vector<LibraryItem> getContinueWatching(int limit = 10) const;

private:
    std::unordered_map<std::string, LibraryItem> m_items;
    LibraryListener m_listener;
};

} // namespace ss
