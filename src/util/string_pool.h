#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstddef>

namespace tinyizer {

// Efficient string interning — all identifiers live here.
// Single allocation, O(1) equality via pointer comparison.
class StringPool {
public:
    // Intern a string. Returns a pointer that stays valid for the pool's lifetime.
    const char* intern(std::string_view sv);

    // Get total bytes stored (for stats)
    size_t total_bytes() const { return total_bytes_; }

    // Clear all interned strings
    void clear();

private:
    // Use raw pointers as keys. Store the string data in blocks.
    std::unordered_map<const char*, size_t> map_;
    std::vector<std::unique_ptr<char[]>> blocks_;
    size_t total_bytes_ = 0;
    size_t current_block_used_ = 0;

    static constexpr size_t BLOCK_SIZE = 65536; // 64 KB blocks
};

} // namespace tinyizer
