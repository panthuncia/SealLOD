#pragma once

struct MemoryBlock {
    size_t offset; // Start offset within the buffer
    size_t size;   // Size of the block
    bool isFree;   // Whether the block is free or allocated
};