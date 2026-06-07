#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <algorithm>

namespace tinyizer {

// Tracks identifier frequencies and computes optimal short names.
// Uses a frequency-weighted greedy assignment — most frequent identifiers
// get the shortest possible names.
template <typename Key = std::string_view>
class FrequencyMap {
public:
    void record(const Key& id) { freq_[id]++; }

    void record_n(const Key& id, uint32_t n) { freq_[id] += n; }

    uint32_t count(const Key& id) const {
        auto it = freq_.find(id);
        return it != freq_.end() ? it->second : 0;
    }

    // Return identifiers sorted by frequency (descending)
    std::vector<std::pair<Key, uint32_t>> sorted() const {
        std::vector<std::pair<Key, uint32_t>> result(freq_.begin(), freq_.end());
        std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        return result;
    }

    // Generate the optimal short name for a given rank (0 = most frequent).
    // Uses: a-z, A-Z, a0-z9, aa-zz, Aa-Zz, aa0-zz9, ...
    // This is like base-52 but excluding digits in first position.
    static std::string name_for_rank(size_t rank) {
        if (rank < 26) {
            // a-z
            return std::string(1, static_cast<char>('a' + rank));
        }
        rank -= 26;
        if (rank < 26) {
            // A-Z
            return std::string(1, static_cast<char>('A' + rank));
        }

        // Multi-character: first char from [a-zA-Z_], rest from [a-zA-Z0-9_]
        std::string result;
        constexpr const char* FIRST_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
        constexpr size_t FIRST_COUNT = 53;
        constexpr const char* CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789";
        constexpr size_t CHAR_COUNT = 63;

        rank -= 52; // account for single-char names

        // Determine length: names of length L have FIRST_COUNT * CHAR_COUNT^(L-1) possibilities
        size_t len = 2;
        size_t count = FIRST_COUNT * CHAR_COUNT;
        while (rank >= count) {
            rank -= count;
            len++;
            count = FIRST_COUNT;
            for (size_t i = 1; i < len; i++) count *= CHAR_COUNT;
        }

        // Build the name
        // First character
        size_t first_idx = rank / (count / FIRST_COUNT);
        result += FIRST_CHARS[first_idx];
        rank %= (count / FIRST_COUNT);

        // Remaining characters
        size_t remaining = len - 1;
        size_t per_char = 1;
        for (size_t i = 0; i < remaining - 1; i++) per_char *= CHAR_COUNT;

        for (size_t i = 0; i < remaining; i++) {
            size_t idx;
            if (per_char > 0) {
                idx = rank / per_char;
                rank %= per_char;
                if (i < remaining - 1) per_char /= CHAR_COUNT;
            } else {
                idx = rank;
            }
            result += CHARS[idx];
        }

        return result;
    }

    // Build a mapping from original names → optimal short names
    std::unordered_map<Key, std::string> build_squeeze_map() const {
        auto by_freq = sorted();
        std::unordered_map<Key, std::string> mapping;
        for (size_t i = 0; i < by_freq.size(); i++) {
            mapping[by_freq[i].first] = name_for_rank(i);
        }
        return mapping;
    }

    size_t size() const { return freq_.size(); }

private:
    std::unordered_map<Key, uint32_t> freq_;
};

} // namespace tinyizer
