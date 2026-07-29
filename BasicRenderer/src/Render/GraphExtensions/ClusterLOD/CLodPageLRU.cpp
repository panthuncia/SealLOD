#include "Render/GraphExtensions/ClusterLOD/CLodPageLRU.h"

CLodPageLRU::~CLodPageLRU() {
}

void CLodPageLRU::Insert(uint32_t pageID) {
    if (pageID >= m_nodes.size()) {
        m_nodes.resize(static_cast<size_t>(pageID) + 1u);
    }
    if (m_nodes[pageID].present) {
        // Already present - move to MRU position.
        Unlink(pageID);
        PushBack(pageID);
        return;
    }

    m_nodes[pageID].present = true;
    ++m_size;
    PushBack(pageID);
}

void CLodPageLRU::Remove(uint32_t pageID) {
    if (!Contains(pageID)) return;

    Unlink(pageID);
    m_nodes[pageID].present = false;
    --m_size;
}

void CLodPageLRU::Touch(uint32_t pageID) {
    if (!Contains(pageID) || pageID == m_tail) return;

    Unlink(pageID);
    PushBack(pageID);
}

uint32_t CLodPageLRU::PopOldest() {
    if (m_head == ~0u) {
        return ~0u;
    }

    const uint32_t pageID = m_head;
    Unlink(pageID);
    PushBack(pageID);
    return pageID;
}

bool CLodPageLRU::Contains(uint32_t pageID) const {
    return pageID < m_nodes.size() && m_nodes[pageID].present;
}

void CLodPageLRU::Clear() {
    m_head = ~0u;
    m_tail = ~0u;
    m_size = 0u;
    m_nodes.clear();
}

// Pinned page tracking is disabled for the simplified streaming experiment.

void CLodPageLRU::Pin(uint32_t pageID) {
    Insert(pageID);
}

void CLodPageLRU::Unpin(uint32_t) {
}

bool CLodPageLRU::IsPinned(uint32_t) const {
    return false;
}

// list helpers

void CLodPageLRU::Unlink(uint32_t pageID) {
    Node& node = m_nodes[pageID];
    if (node.prev != ~0u) {
        m_nodes[node.prev].next = node.next;
    } else {
        m_head = node.next;
    }

    if (node.next != ~0u) {
        m_nodes[node.next].prev = node.prev;
    } else {
        m_tail = node.prev;
    }

    node.prev = ~0u;
    node.next = ~0u;
}

void CLodPageLRU::PushBack(uint32_t pageID) {
    Node& node = m_nodes[pageID];
    node.prev = m_tail;
    node.next = ~0u;

    if (m_tail != ~0u) {
        m_nodes[m_tail].next = pageID;
    } else {
        m_head = pageID;
    }

    m_tail = pageID;
}
