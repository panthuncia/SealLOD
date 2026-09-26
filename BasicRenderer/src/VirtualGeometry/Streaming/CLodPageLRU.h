#pragma once

#include <cstdint>
#include <vector>

// Page-level doubly-linked list LRU for cluster-LOD streaming.
//
// - Each page in the pool occupies exactly one node in the list.
// - Front = least-recently-used, back = most-recently-used.
// - Touch() moves a node to the back in O(1).
// - PopOldest() rotates the oldest page to MRU and returns it.
//
// Thread safety: none - intended to be owned by a single thread (the worker).
class CLodPageLRU {
public:
    CLodPageLRU() = default;
    ~CLodPageLRU();

    CLodPageLRU(const CLodPageLRU&) = delete;
    CLodPageLRU& operator=(const CLodPageLRU&) = delete;

    // Insert a page into the LRU as most-recently-used.
    // If already present, moves it to the MRU position.
    void Insert(uint32_t pageID);

    // Remove a page from the LRU entirely.
    // No-op if not present.
    void Remove(uint32_t pageID);

    // Move an existing page to the back (most-recently-used).
    // No-op if not present.
    void Touch(uint32_t pageID);

    // Return the least-recently-used page and move it to MRU.
    // Returns ~0u if the LRU is empty.
    uint32_t PopOldest();

    // Is this page tracked in the LRU?
    bool Contains(uint32_t pageID) const;

    // Number of pages tracked by the LRU.
    uint32_t Size() const { return m_size; }

    // Clear all entries (LRU + pinned).
    void Clear();

    // Compatibility no-ops while the streaming experiment ignores page locks.
    void Pin(uint32_t pageID);
    void Unpin(uint32_t pageID);
    bool IsPinned(uint32_t pageID) const;
    uint32_t PinnedCount() const { return 0u; }

private:
    struct Node {
        uint32_t prev = ~0u;
        uint32_t next = ~0u;
        bool present = false;
    };

    // Unlink a node from the list without destroying it.
    void Unlink(uint32_t pageID);

    // Append a node at the back of the list.
    void PushBack(uint32_t pageID);

    uint32_t m_head = ~0u;
    uint32_t m_tail = ~0u;
    uint32_t m_size = 0u;
    // Physical page IDs are dense, so direct indexing avoids a hash lookup and
    // one heap allocation per page in this hot bookkeeping structure.
    std::vector<Node> m_nodes;
};
