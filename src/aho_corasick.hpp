#pragma once
// M7.2: guarded Aho-Corasick automaton for fixed-pattern sets.
// Byte-wise trie + failure links + merged outputs. Reports every pattern
// occurrence (overlap-capable); callers filter non-overlapping matches per
// pattern when needed. Deterministic: nodes in insertion order, outputs in
// pattern-index order, matches in scan order.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pergrep {
namespace detail {

struct AhoCorasick {
    struct Node {
        std::array<int, 256> next;
        int link = -1;
        std::vector<std::uint32_t> out; // pattern indices ending here (ascending)
        Node() { next.fill(-1); }
    };
    std::vector<Node> nodes;
    std::vector<std::size_t> pattern_lengths;

    AhoCorasick() { nodes.emplace_back(); } // root = 0

    // Returns false when limits would be exceeded (caller falls back).
    bool build(const std::vector<std::string>& literals, std::size_t max_nodes, std::size_t max_bytes) {
        nodes.clear();
        nodes.emplace_back();
        pattern_lengths.clear();
        pattern_lengths.reserve(literals.size());
        std::size_t total_bytes = 0;
        for (std::uint32_t pi = 0; pi < literals.size(); ++pi) {
            const auto& lit = literals[pi];
            if (lit.empty()) return false;
            total_bytes += lit.size();
            if (total_bytes > max_bytes) return false;
            int curr = 0;
            for (unsigned char c : lit) {
                if (nodes[curr].next[c] == -1) {
                    if (nodes.size() >= max_nodes) return false;
                    nodes[curr].next[c] = static_cast<int>(nodes.size());
                    nodes.emplace_back();
                }
                curr = nodes[curr].next[c];
            }
            pattern_lengths.push_back(lit.size());
            // Keep outputs ascending for deterministic merge order.
            auto& o = nodes[curr].out;
            if (o.empty() || o.back() < pi) o.push_back(pi);
            else o.insert(std::lower_bound(o.begin(), o.end(), pi), pi);
        }
        // Failure links (BFS) + output merge.
        std::vector<int> queue;
        for (int c = 0; c < 256; ++c) {
            int nxt = nodes[0].next[c];
            if (nxt != -1) {
                nodes[nxt].link = 0;
                queue.push_back(nxt);
            } else {
                nodes[0].next[c] = 0;
            }
        }
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            int v = queue[qi];
            for (int c = 0; c < 256; ++c) {
                int nxt = nodes[v].next[c];
                if (nxt != -1) {
                    nodes[nxt].link = nodes[nodes[v].link].next[c];
                    // Merge fail-target outputs (kept ascending).
                    for (auto pi : nodes[nodes[nxt].link].out) {
                        auto& o = nodes[nxt].out;
                        if (o.empty() || o.back() < pi) o.push_back(pi);
                        else if (!std::binary_search(o.begin(), o.end(), pi))
                            o.insert(std::lower_bound(o.begin(), o.end(), pi), pi);
                    }
                    queue.push_back(nxt);
                } else {
                    nodes[v].next[c] = nodes[nodes[v].link].next[c];
                }
            }
        }
        return true;
    }

    // All (pattern_index, end_offset_exclusive) pairs in scan order.
    // end_offset is relative to text start.
    std::vector<std::pair<std::uint32_t, std::size_t>> search_all(std::string_view text) const {
        std::vector<std::pair<std::uint32_t, std::size_t>> out;
        int state = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            state = nodes[state].next[static_cast<unsigned char>(text[i])];
            for (auto pi : nodes[state].out) out.push_back({pi, i + 1});
        }
        return out;
    }

    std::size_t node_count() const noexcept { return nodes.size(); }
};

} // namespace detail
} // namespace pergrep
