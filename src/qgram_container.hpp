#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace pergrep::detail {

// M8.2: Dense bitmap row representation.
// Bit vector over `universe` elements, storing `words = (universe + 63) / 64` 64-bit words.
// Constant-time bit test O(1), linear SIMD-parallelizable bitwise intersection O(words).
struct DenseBitmapRow {
    std::uint32_t universe = 0;
    std::uint32_t words = 0;
    std::vector<std::uint64_t> bits;

    DenseBitmapRow() = default;
    explicit DenseBitmapRow(std::uint32_t univ)
        : universe(univ), words((univ + 63) / 64), bits(words, 0) {}

    void set(std::uint32_t id) noexcept {
        if (id < universe) {
            bits[id >> 6] |= (1ULL << (id & 63));
        }
    }

    bool test(std::uint32_t id) const noexcept {
        if (id >= universe) return false;
        return (bits[id >> 6] & (1ULL << (id & 63))) != 0;
    }

    std::uint32_t count() const noexcept {
        std::uint32_t c = 0;
        for (auto w : bits) c += static_cast<std::uint32_t>(std::popcount(w));
        return c;
    }

    std::vector<std::uint32_t> to_ids() const {
        std::vector<std::uint32_t> out;
        out.reserve(count());
        for (std::uint32_t w = 0; w < words; ++w) {
            std::uint64_t z = bits[w];
            while (z) {
                unsigned b = std::countr_zero(z);
                std::uint32_t id = w * 64 + b;
                if (id < universe) out.push_back(id);
                z &= (z - 1);
            }
        }
        return out;
    }

    std::size_t memory_bytes() const noexcept {
        return bits.size() * sizeof(std::uint64_t) + sizeof(DenseBitmapRow);
    }

    std::size_t serialization_bytes() const noexcept {
        return sizeof(std::uint32_t) * 2 + bits.size() * sizeof(std::uint64_t);
    }

    void intersect_with(const DenseBitmapRow& other, DenseBitmapRow& out) const {
        std::uint32_t u = std::min(universe, other.universe);
        out = DenseBitmapRow(u);
        std::uint32_t min_w = std::min(words, other.words);
        for (std::uint32_t i = 0; i < min_w; ++i) {
            out.bits[i] = bits[i] & other.bits[i];
        }
    }
};

// M8.2: Sparse postings row representation.
// Sorted list of unique chunk IDs.
// O(log k) binary-search membership, O(k1 + k2) sorted list intersection.
struct SparsePostingsRow {
    std::uint32_t universe = 0;
    std::vector<std::uint32_t> ids;

    SparsePostingsRow() = default;
    explicit SparsePostingsRow(std::uint32_t univ) : universe(univ) {}

    void add(std::uint32_t id) {
        if (id < universe) {
            if (ids.empty() || id > ids.back()) {
                ids.push_back(id);
            } else if (id < ids.back()) {
                auto it = std::lower_bound(ids.begin(), ids.end(), id);
                if (it == ids.end() || *it != id) {
                    ids.insert(it, id);
                }
            }
        }
    }

    bool test(std::uint32_t id) const noexcept {
        return std::binary_search(ids.begin(), ids.end(), id);
    }

    std::uint32_t count() const noexcept {
        return static_cast<std::uint32_t>(ids.size());
    }

    std::size_t memory_bytes() const noexcept {
        return ids.size() * sizeof(std::uint32_t) + sizeof(SparsePostingsRow);
    }

    std::size_t serialization_bytes() const noexcept {
        return sizeof(std::uint32_t) * 2 + ids.size() * sizeof(std::uint32_t);
    }

    void intersect_with(const SparsePostingsRow& other, SparsePostingsRow& out) const {
        out = SparsePostingsRow(std::min(universe, other.universe));
        std::set_intersection(ids.begin(), ids.end(),
                              other.ids.begin(), other.ids.end(),
                              std::back_inserter(out.ids));
    }
};

// M8.2: Hybrid layout that dynamically selects between sparse and dense.
// The frequency threshold determines whether sparse or dense layout is chosen.
// Theoretical break-even:
// Dense bytes: ceil(universe / 64) * 8
// Sparse bytes: k * 4
// Break-even: k * 4 = universe / 8 => k = universe / 32 (~3.125% density).
enum class ContainerLayout : std::uint8_t {
    Sparse = 0,
    Dense = 1
};

struct HybridQgramRow {
    std::uint32_t universe = 0;
    ContainerLayout layout = ContainerLayout::Sparse;
    SparsePostingsRow sparse;
    DenseBitmapRow dense;

    HybridQgramRow() = default;
    explicit HybridQgramRow(std::uint32_t univ) : universe(univ), sparse(univ) {}

    // Default break-even threshold: universe / 32 (~3.125% density).
    static std::uint32_t default_threshold(std::uint32_t univ) noexcept {
        return std::max<std::uint32_t>(1, univ / 32);
    }

    void add(std::uint32_t id, std::uint32_t threshold = 0) {
        if (threshold == 0) threshold = default_threshold(universe);
        if (layout == ContainerLayout::Sparse) {
            sparse.add(id);
            if (sparse.count() > threshold) {
                // Convert to dense
                layout = ContainerLayout::Dense;
                dense = DenseBitmapRow(universe);
                for (auto cid : sparse.ids) dense.set(cid);
                sparse = SparsePostingsRow(universe); // free sparse memory
            }
        } else {
            dense.set(id);
        }
    }

    bool test(std::uint32_t id) const noexcept {
        if (layout == ContainerLayout::Sparse) return sparse.test(id);
        return dense.test(id);
    }

    std::uint32_t count() const noexcept {
        return layout == ContainerLayout::Sparse ? sparse.count() : dense.count();
    }

    std::vector<std::uint32_t> to_ids() const {
        if (layout == ContainerLayout::Sparse) return sparse.ids;
        return dense.to_ids();
    }

    std::size_t memory_bytes() const noexcept {
        return sizeof(HybridQgramRow) +
               (layout == ContainerLayout::Sparse
                    ? sparse.ids.size() * sizeof(std::uint32_t)
                    : dense.bits.size() * sizeof(std::uint64_t));
    }

    std::size_t serialization_bytes() const noexcept {
        return sizeof(std::uint32_t) * 2 + sizeof(std::uint8_t) +
               (layout == ContainerLayout::Sparse
                    ? sparse.ids.size() * sizeof(std::uint32_t)
                    : dense.bits.size() * sizeof(std::uint64_t));
    }

    // Intersect two hybrid rows regardless of their representations.
    void intersect_with(const HybridQgramRow& other, HybridQgramRow& out) const {
        std::uint32_t u = std::min(universe, other.universe);
        out = HybridQgramRow(u);
        if (layout == ContainerLayout::Sparse && other.layout == ContainerLayout::Sparse) {
            out.layout = ContainerLayout::Sparse;
            sparse.intersect_with(other.sparse, out.sparse);
        } else if (layout == ContainerLayout::Dense && other.layout == ContainerLayout::Dense) {
            out.layout = ContainerLayout::Dense;
            dense.intersect_with(other.dense, out.dense);
        } else if (layout == ContainerLayout::Sparse && other.layout == ContainerLayout::Dense) {
            // Sparse & Dense: filter sparse by dense bit-test
            out.layout = ContainerLayout::Sparse;
            for (auto id : sparse.ids) {
                if (other.dense.test(id)) out.sparse.ids.push_back(id);
            }
        } else {
            // Dense & Sparse: filter sparse by dense bit-test
            out.layout = ContainerLayout::Sparse;
            for (auto id : other.sparse.ids) {
                if (dense.test(id)) out.sparse.ids.push_back(id);
            }
        }
    }
};

} // namespace pergrep::detail
