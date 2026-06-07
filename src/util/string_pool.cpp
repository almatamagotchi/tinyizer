#include "string_pool.h"
#include <cstring>
#include <algorithm>
#include <memory>

namespace tinyizer {

const char* StringPool::intern(std::string_view sv) {
    // Need to allocate
    size_t len = sv.size();
    total_bytes_ += len + 1;

    // Simple approach: allocate a new block for each string that doesn't fit
    // (Production code would use a bump allocator within blocks for efficiency)

    if (current_block_used_ + len + 1 > BLOCK_SIZE || blocks_.empty()) {
        blocks_.push_back(std::make_unique<char[]>(BLOCK_SIZE));
        current_block_used_ = 0;
    }

    char* dest = blocks_.back().get() + current_block_used_;
    std::memcpy(dest, sv.data(), len);
    dest[len] = '\0';
    current_block_used_ += len + 1;

    map_[dest] = total_bytes_;
    return dest;
}

void StringPool::clear() {
    map_.clear();
    blocks_.clear();
    total_bytes_ = 0;
    current_block_used_ = 0;
}

} // namespace tinyizer
