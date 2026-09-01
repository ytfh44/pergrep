#include "internal.hpp"
#include "platform.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <limits>
#include <tuple>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
namespace pergrep::detail {
std::uint8_t lg_for(std::size_t n){std::size_t want=std::clamp<std::size_t>(n*2,512,65536);std::uint8_t lg=9;for(std::size_t b=512;b<want&&lg<16;b<<=1)++lg;return lg;}
QueryDesc compile_qgram_query(std::string_view q){
    QueryDesc d;
    if(q.size()>=4) for(size_t i=0;i+4<=q.size();++i) d.hashes.push_back(hash4((const unsigned char*)q.data()+i));
    for(uint8_t lg=9;lg<=16;++lg){
        uint32_t mask=(1u<<lg)-1; std::vector<uint16_t>b;
        for(uint32_t h:d.hashes) b.push_back(h&mask);
        std::sort(b.begin(),b.end()); b.erase(std::unique(b.begin(),b.end()),b.end());
        auto&v=d.classes[lg-9];
        for(auto x:b){ uint16_t w=x>>6; uint64_t m=1ull<<(x&63);
            if(!v.empty()&&v.back().first==w) v.back().second|=m; else v.push_back({w,m});
        }
    }
    return d;
}
QueryDesc compile_qgram_query(std::string_view q, std::span<const std::uint32_t> selected_hashes){
    QueryDesc d;
    d.hashes.assign(selected_hashes.begin(), selected_hashes.end());
    for(uint8_t lg=9;lg<=16;++lg){
        uint32_t mask=(1u<<lg)-1; std::vector<uint16_t>b;
        for(uint32_t h:d.hashes) b.push_back(static_cast<uint16_t>(h&mask));
        std::sort(b.begin(),b.end()); b.erase(std::unique(b.begin(),b.end()),b.end());
        auto&v=d.classes[lg-9];
        for(auto x:b){ uint16_t w=x>>6; uint64_t m=1ull<<(x&63);
            if(!v.empty()&&v.back().first==w) v.back().second|=m; else v.push_back({w,m});
        }
    }
    (void)q;
    return d;
}
}

namespace pergrep {
namespace fs=std::filesystem;
using detail::Chunk;using detail::LoadedFile;


static int64_t mtime_ns(const fs::path&p){std::error_code ec;auto t=fs::last_write_time(p,ec);if(ec)return 0;return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();}
static bool looks_binary(std::string_view s){auto n=std::min<size_t>(s.size(),8192);return std::memchr(s.data(),'\0',n)!=nullptr;}
// Exact planner tables are intentionally bounded: they are transient
// calibration data, while legacy filter arrays remain available at any scale.
inline constexpr std::uint64_t kPlannerCorpusCap = 128ULL * 1024 * 1024;
inline constexpr std::size_t kPlannerDistinctQgramCap = 1ULL << 20;
static void rebuild_planner_stats(detail::IndexData& I) {
    I.exact_qgrams.clear();
    I.hash_chunk_freq.fill(0);
    for (auto& ids : I.hash_chunk_ids) ids.clear();
    I.planner_stats_ready = false;
    if (I.corp_bytes > kPlannerCorpusCap) return;
    for (std::uint32_t fid = 0; fid < I.loaded.size(); ++fid) {
        const auto& data = I.loaded[fid].data;
        std::unordered_set<std::uint32_t> document_grams;
        if (data.size() >= 4) {
            document_grams.reserve(std::min<std::size_t>(
                data.size() / 4, kPlannerDistinctQgramCap));
            for (std::size_t q = 0; q + 4 <= data.size(); ++q) {
                const auto key = detail::qgram4_key(
                    reinterpret_cast<const unsigned char*>(data.data() + q));
                auto it = I.exact_qgrams.find(key);
                if (it == I.exact_qgrams.end()) {
                    if (I.exact_qgrams.size() >= kPlannerDistinctQgramCap) {
                        I.exact_qgrams.clear();
                        return;
                    }
                    it = I.exact_qgrams.emplace(key, detail::IndexData::QgramStats{}).first;
                }
                ++it->second.occurrence_frequency;
                document_grams.insert(key);
            }
        }
        for (const auto key : document_grams) {
            auto it = I.exact_qgrams.find(key);
            if (it == I.exact_qgrams.end()) continue;
            ++it->second.document_frequency;
            it->second.document_ids.push_back(fid);
        }
    }
    for (std::uint32_t ci = 0; ci < I.chunks.size(); ++ci) {
        const auto& c = I.chunks[ci];
        // v5/BF-4 fixtures may intentionally contain serialized offset
        // values beyond the restored corpus. Do not let transient planner
        // recomputation turn such a load into an exception.
        if (c.file_id >= I.loaded.size()) continue;
        const auto data_size = static_cast<std::uint64_t>(I.loaded[c.file_id].data.size());
        if (c.core_begin > data_size || c.ext_end < c.core_begin || c.ext_end > data_size)
            continue;
        const auto view = std::string_view(I.loaded[c.file_id].data).substr(
            c.core_begin, c.ext_end - c.core_begin);
        std::unordered_set<std::uint32_t> exact_in_chunk;
        std::unordered_set<std::uint16_t> hash_in_chunk;
        if (view.size() >= 4) {
            exact_in_chunk.reserve(std::min<std::size_t>(
                view.size() / 4, kPlannerDistinctQgramCap));
            hash_in_chunk.reserve(std::min<std::size_t>(view.size() / 4, 65536));
            for (std::size_t q = 0; q + 4 <= view.size(); ++q) {
                const auto* p = reinterpret_cast<const unsigned char*>(view.data() + q);
                exact_in_chunk.insert(detail::qgram4_key(p));
                hash_in_chunk.insert(static_cast<std::uint16_t>(detail::hash4(p) & 65535u));
            }
        }
        for (const auto key : exact_in_chunk) {
            auto it = I.exact_qgrams.find(key);
            if (it == I.exact_qgrams.end()) continue;
            ++it->second.chunk_frequency;
            it->second.chunk_ids.push_back(ci);
        }
        for (const auto bucket : hash_in_chunk) {
            ++I.hash_chunk_freq[bucket];
            I.hash_chunk_ids[bucket].push_back(ci);
        }
    }
    I.planner_stats_ready = true;
}

Index::Index() = default;
Index::Index(std::shared_ptr<Impl> i) : impl_(std::move(i)) {}

Index Index::build(const fs::path& root, IndexOptions opt) {
    if (opt.chunk_bytes < 64 || opt.chunk_bytes > (1ULL << 30))
        throw std::runtime_error("pergrep: chunk_bytes out of range [64, 1073741824]");
    if (opt.positional_block_bytes < 16 || opt.positional_block_bytes > (1ULL << 20))
        throw std::runtime_error("pergrep: positional_block_bytes out of range [16, 1048576]");
    if (opt.chunk_overlap > opt.chunk_bytes / 2)
        throw std::runtime_error("pergrep: chunk_overlap must be <= chunk_bytes / 2");
    if (opt.planned_qgrams > 64)
        throw std::runtime_error("pergrep: planned_qgrams out of range [0, 64]");
    if (opt.positional_budget_ratio < 0.0 || opt.positional_budget_ratio > 10.0)
        throw std::runtime_error("pergrep: positional_budget_ratio out of range [0.0, 10.0]");
    auto I = std::make_shared<Impl>();
    I->root = fs::weakly_canonical(root);
    std::error_code ec_root;
    if (!fs::exists(I->root, ec_root) || ec_root) throw std::runtime_error("pergrep: root path does not exist: " + root.string());
    if (!fs::is_directory(I->root, ec_root) || ec_root) throw std::runtime_error("pergrep: root path is not a directory: " + root.string());
    I->opt = opt;
    I->pos_block = (uint32_t)opt.positional_block_bytes;
    I->root_mtime_ns = mtime_ns(I->root);

    std::vector<fs::path> ps;
    std::unordered_set<std::string> visited_dirs;
    if (opt.follow_symlinks) {
        std::error_code ec;
        auto rc = fs::canonical(I->root, ec);
        if (!ec) visited_dirs.insert(rc.generic_string());
    }
    auto dop = fs::directory_options::skip_permission_denied | (opt.follow_symlinks ? fs::directory_options::follow_directory_symlink : fs::directory_options::none);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(I->root, dop, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& e = *it;
        if (!opt.include_hidden) {
            auto fn = e.path().filename().string();
            if (!fn.empty() && fn[0] == '.' && fn != "." && fn != "..") {
                if (e.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
        }
        if (!opt.follow_symlinks && pergrep_cli::platform::is_reparse_point(e.path())) {
            if (e.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (opt.follow_symlinks && e.is_directory(ec)) {
            auto canon = fs::canonical(e.path(), ec);
            if (!ec && !visited_dirs.insert(canon.generic_string()).second) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (e.is_regular_file(ec)) ps.push_back(e.path());
    }
    std::sort(ps.begin(), ps.end());

    for (auto const& p : ps) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        std::string s((std::istreambuf_iterator<char>(f)), {});
        FileInfo fi;
        fi.path = pergrep_cli::platform::path_to_utf8(p.lexically_relative(I->root));
        fi.size = s.size();
        fi.mtime_ns = mtime_ns(p);
        fi.binary = looks_binary(s);
        I->corp_bytes += s.size();
        for (unsigned char c : s) ++I->byte_freq[c];
        if (s.size() >= 4) {
            for (size_t q = 0; q + 4 <= s.size(); ++q) {
                auto h = detail::hash4((const unsigned char*)s.data() + q) & 65535u;
                if (I->qgram_freq[h] != UINT32_MAX) ++I->qgram_freq[h];
            }
        }
        I->infos.push_back(fi);
        I->loaded.push_back({fi, std::move(s)});
    }

    uint64_t core = opt.chunk_bytes, over = opt.chunk_overlap;
    for (uint32_t fid = 0; fid < I->loaded.size(); ++fid) {
        uint64_t n = I->loaded[fid].data.size();
        if (n == 0) {
            I->chunks.push_back({fid, 0, 0, 0});
            continue;
        }
        for (uint64_t b = 0; b < n; b += core) {
            uint64_t e = std::min(n, b + core), x = std::min(n, e + over);
            I->chunks.push_back({fid, b, e, x});
        }
    }
    rebuild_planner_stats(*I);

    std::array<uint32_t, 8> cnt{};
    for (auto const& c : I->chunks) ++cnt[detail::lg_for(size_t(c.ext_end - c.core_begin)) - 9];
    for (int k = 0; k < 8; ++k) {
        auto& g = I->groups[k];
        g.lg = k + 9;
        g.m = 1u << g.lg;
        g.words = (cnt[k] + 63) / 64;
        g.gids.reserve(cnt[k]);
        g.bits.assign((size_t)g.m * g.words, 0);
    }
    std::array<uint32_t, 8> local{};
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto c = I->chunks[ci];
        auto& g = I->groups[detail::lg_for(size_t(c.ext_end - c.core_begin)) - 9];
        uint32_t li = local[g.lg - 9]++;
        g.gids.push_back(ci);
        auto v = std::string_view(I->loaded[c.file_id].data).substr(c.core_begin, c.ext_end - c.core_begin);
        uint32_t mask = g.m - 1;
        if (v.size() >= 4) {
            for (size_t j = 0; j + 4 <= v.size(); ++j) {
                uint32_t b = detail::hash4((const unsigned char*)v.data() + j) & mask;
                g.bits[(size_t)b * g.words + (li >> 6)] |= 1ull << (li & 63);
            }
        }
    }

    // Positional Bloom construction — per-chunk block-level q-gram filter.
    // For each chunk we build a Bloom matrix `pos` of size `m * mask_bytes` bytes:
    // - `blocks = ceil(core_len / pos_block)` — number of positional blocks in the chunk.
    //   When core_len is not divisible by pos_block, the last block is smaller but still
    //   represented; mask_bytes = ceil(blocks/8) ensures one bit per block, with trailing
    //   bits in the last byte masked off (see fixed_candidate_blocks).
    // - `mask_bytes = (blocks+7)/8` — bytes needed for one Bloom row's block mask.
    // - `choose_m(core_len, mask_bytes)` — selects number of Bloom rows `m` (power of two
    //   in [64,1024]) based on budget = core_len * positional_budget_ratio. `want` is the
    //   desired total bytes per row budget, and `m` is capped to keep `m * mask_bytes`
    //   within budget while keeping per-chunk overhead bounded. Larger `m` reduces collisions
    //   but increases memory; 64 is minimum for reasonable selectivity.
    // - `PO = 64` — positional overlap: each block's Bloom window extends 64 bytes beyond
    //   the block boundary to capture q-grams that straddle block edges (conservative).
    const uint32_t PO = 64;
    I->pos_desc.resize(I->chunks.size());
    size_t pos_total = 0;
    auto choose_m = [&](uint64_t core_len, uint32_t mask_bytes) {
        size_t budget = size_t(double(core_len) * I->opt.positional_budget_ratio);
        size_t want = std::max<size_t>(64, budget / std::max<uint32_t>(1, mask_bytes));
        uint16_t m = 64; while (m < 1024 && static_cast<size_t>(m << 1) <= want) m <<= 1; return m;
    };
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto z = I->chunks[ci]; uint64_t core_len = z.core_end - z.core_begin;
        uint32_t blocks = std::max<uint32_t>(1, (uint32_t)((core_len + I->pos_block - 1) / I->pos_block));
        uint32_t mask_bytes = (blocks + 7) / 8; uint16_t m = choose_m(core_len, mask_bytes);
        I->pos_desc[ci] = {uint64_t(pos_total), m, mask_bytes, blocks};
        pos_total += size_t(m) * mask_bytes;
    }
    I->pos.assign(pos_total, 0);
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto z = I->chunks[ci]; auto d = I->pos_desc[ci]; auto whole = std::string_view(I->loaded[z.file_id].data);
        for (uint32_t bi = 0; bi < d.blocks; ++bi) {
            uint64_t rb = uint64_t(bi) * I->pos_block;
            uint64_t chunk_len = z.ext_end - z.core_begin;
            if (rb >= chunk_len) continue;
            uint64_t re = std::min<uint64_t>(chunk_len, rb + I->pos_block + PO);
            if (re <= rb || re - rb < 4) continue;
            auto base = (const unsigned char*)whole.data() + z.core_begin + rb;
            for (uint64_t j = 0; j + 4 <= re - rb; ++j) {
                uint32_t row = detail::hash4(base + j) & (d.m - 1);
                I->pos[d.off + size_t(row) * d.mask_bytes + (bi >> 3)] |= uint8_t(1u << (bi & 7));
            }
        }
    }
    return Index(I);
}

Index Index::from_documents(std::vector<Document> documents, IndexOptions opt) {
    if (opt.chunk_bytes < 64 || opt.chunk_bytes > (1ULL << 30))
        throw std::runtime_error("pergrep: chunk_bytes out of range [64, 1073741824]");
    if (opt.positional_block_bytes < 16 || opt.positional_block_bytes > (1ULL << 20))
        throw std::runtime_error("pergrep: positional_block_bytes out of range [16, 1048576]");
    if (opt.chunk_overlap > opt.chunk_bytes / 2)
        throw std::runtime_error("pergrep: chunk_overlap must be <= chunk_bytes / 2");
    if (opt.planned_qgrams > 64)
        throw std::runtime_error("pergrep: planned_qgrams out of range [0, 64]");
    if (opt.positional_budget_ratio < 0.0 || opt.positional_budget_ratio > 10.0)
        throw std::runtime_error("pergrep: positional_budget_ratio out of range [0.0, 10.0]");

    auto I = std::make_shared<Impl>();
    I->opt = opt;
    I->pos_block = (uint32_t)opt.positional_block_bytes;
    I->ephemeral = true;

    std::sort(documents.begin(), documents.end(), [](const Document& a, const Document& b) { return a.path < b.path; });
    for (auto& d : documents) {
        FileInfo fi;
        fi.path = d.path;
        fi.size = d.content.size();
        fi.binary = looks_binary(d.content);
        I->corp_bytes += d.content.size();
        for (unsigned char c : d.content) ++I->byte_freq[c];
        if (d.content.size() >= 4) {
            for (size_t q = 0; q + 4 <= d.content.size(); ++q) {
                auto h = detail::hash4((const unsigned char*)d.content.data() + q) & 65535u;
                if (I->qgram_freq[h] != UINT32_MAX) ++I->qgram_freq[h];
            }
        }
        I->infos.push_back(fi);
        I->loaded.push_back({fi, std::move(d.content)});
    }

    uint64_t core = opt.chunk_bytes, over = opt.chunk_overlap;
    for (uint32_t fid = 0; fid < I->loaded.size(); ++fid) {
        uint64_t n = I->loaded[fid].data.size();
        if (n == 0) {
            I->chunks.push_back({fid, 0, 0, 0});
            continue;
        }
        for (uint64_t b = 0; b < n; b += core) {
            uint64_t e = std::min(n, b + core), x = std::min(n, e + over);
            I->chunks.push_back({fid, b, e, x});
        }
    }
    rebuild_planner_stats(*I);

    std::array<uint32_t, 8> cnt{};
    for (auto const& c : I->chunks) ++cnt[detail::lg_for(size_t(c.ext_end - c.core_begin)) - 9];
    for (int k = 0; k < 8; ++k) {
        auto& g = I->groups[k];
        g.lg = k + 9;
        g.m = 1u << g.lg;
        g.words = (cnt[k] + 63) / 64;
        g.gids.reserve(cnt[k]);
        g.bits.assign((size_t)g.m * g.words, 0);
    }
    std::array<uint32_t, 8> local{};
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto c = I->chunks[ci];
        auto& g = I->groups[detail::lg_for(size_t(c.ext_end - c.core_begin)) - 9];
        uint32_t li = local[g.lg - 9]++;
        g.gids.push_back(ci);
        auto v = std::string_view(I->loaded[c.file_id].data).substr(c.core_begin, c.ext_end - c.core_begin);
        uint32_t mask = g.m - 1;
        if (v.size() >= 4) {
            for (size_t j = 0; j + 4 <= v.size(); ++j) {
                uint32_t b = detail::hash4((const unsigned char*)v.data() + j) & mask;
                g.bits[(size_t)b * g.words + (li >> 6)] |= 1ull << (li & 63);
            }
        }
    }

    // Positional Bloom — same construction as in Index::build (see comment above).
    // Blocks = ceil(core_len / pos_block), mask_bytes = ceil(blocks/8), choose_m as above.
    const uint32_t PO = 64;
    I->pos_desc.resize(I->chunks.size());
    size_t pos_total = 0;
    auto choose_m = [&](uint64_t core_len, uint32_t mask_bytes) {
        size_t budget = size_t(double(core_len) * I->opt.positional_budget_ratio);
        size_t want = std::max<size_t>(64, budget / std::max<uint32_t>(1, mask_bytes));
        uint16_t m = 64; while (m < 1024 && static_cast<size_t>(m << 1) <= want) m <<= 1; return m;
    };
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto z = I->chunks[ci]; uint64_t core_len = z.core_end - z.core_begin;
        uint32_t blocks = std::max<uint32_t>(1, (uint32_t)((core_len + I->pos_block - 1) / I->pos_block));
        uint32_t mask_bytes = (blocks + 7) / 8; uint16_t m = choose_m(core_len, mask_bytes);
        I->pos_desc[ci] = {uint64_t(pos_total), m, mask_bytes, blocks};
        pos_total += size_t(m) * mask_bytes;
    }
    I->pos.assign(pos_total, 0);
    for (uint32_t ci = 0; ci < I->chunks.size(); ++ci) {
        auto z = I->chunks[ci]; auto d = I->pos_desc[ci]; auto whole = std::string_view(I->loaded[z.file_id].data);
        for (uint32_t bi = 0; bi < d.blocks; ++bi) {
            uint64_t rb = uint64_t(bi) * I->pos_block;
            uint64_t chunk_len = z.ext_end - z.core_begin;
            if (rb >= chunk_len) continue;
            uint64_t re = std::min<uint64_t>(chunk_len, rb + I->pos_block + PO);
            if (re <= rb || re - rb < 4) continue;
            auto base = (const unsigned char*)whole.data() + z.core_begin + rb;
            for (uint64_t j = 0; j + 4 <= re - rb; ++j) {
                uint32_t row = detail::hash4(base + j) & (d.m - 1);
                I->pos[d.off + size_t(row) * d.mask_bytes + (bi >> 3)] |= uint8_t(1u << (bi & 7));
            }
        }
    }
    return Index(I);
}

const fs::path& Index::root() const noexcept {
    static const fs::path empty_path;
    return impl_ ? impl_->root : empty_path;
}
const IndexOptions& Index::options() const noexcept {
    static const IndexOptions default_opt;
    return impl_ ? impl_->opt : default_opt;
}
std::span<const FileInfo> Index::files() const noexcept {
    return impl_ ? std::span<const FileInfo>(impl_->infos) : std::span<const FileInfo>{};
}
std::string_view Index::content(std::size_t file_id) const {
    if (!impl_ || file_id >= impl_->loaded.size()) throw std::out_of_range("pergrep: file_id out of range");
    return impl_->loaded[file_id].data;
}
uint64_t Index::corpus_bytes() const noexcept {
    return impl_ ? impl_->corp_bytes : 0;
}
uint64_t Index::index_bytes() const noexcept {
    return impl_ ? impl_->bytes() : 0;
}
bool Index::is_snapshot() const noexcept {
    return impl_ ? impl_->opt.persist_corpus : false;
}
const void* Index::debug_index_data() const noexcept {
    return impl_ ? static_cast<const void*>(impl_.get()) : nullptr;
}

// QO-5: Freshness check is O(files) — re-traverses the directory tree and compares
// path/size/mtime per file using std::error_code overloads (no exceptions) and
// lexically_relative (no weakly_canonical per file) with skip_permission_denied.
// Cost is proportional to file count; cheap on stable trees, linear on high-churn trees.
// No per-file weakly_canonical is needed because paths are already normalized via
// lexically_relative against the canonical root. Suitable for stable-tree fast-path.
bool Index::fresh() const {
    if (!impl_ || impl_->ephemeral) return false;
    std::vector<std::string> current;
    std::error_code ec;
    std::unordered_set<std::string> visited_dirs;
    if (impl_->opt.follow_symlinks) {
        auto rc = fs::canonical(impl_->root, ec);
        if (!ec) visited_dirs.insert(rc.generic_string());
    }
    auto dop = fs::directory_options::skip_permission_denied | (impl_->opt.follow_symlinks ? fs::directory_options::follow_directory_symlink : fs::directory_options::none);
    for (auto it = fs::recursive_directory_iterator(impl_->root, dop, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& e = *it;
        if (!impl_->opt.include_hidden) {
            auto fn = e.path().filename().string();
            if (!fn.empty() && fn[0] == '.' && fn != "." && fn != "..") {
                if (e.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
        }
        if (!impl_->opt.follow_symlinks && pergrep_cli::platform::is_reparse_point(e.path())) {
            if (e.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (impl_->opt.follow_symlinks && e.is_directory(ec)) {
            auto canon = fs::canonical(e.path(), ec);
            if (!ec && !visited_dirs.insert(canon.generic_string()).second) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (e.is_regular_file(ec)) {
            current.push_back(pergrep_cli::platform::path_to_utf8(e.path().lexically_relative(impl_->root)));
        }
    }
    std::sort(current.begin(), current.end());
    if (current.size() != impl_->infos.size()) return false;
    for (size_t k = 0; k < current.size(); ++k) {
        if (current[k] != impl_->infos[k].path) return false;
#ifdef _WIN32
        auto p = impl_->root / fs::path(std::u8string(impl_->infos[k].path.begin(), impl_->infos[k].path.end()));
#else
        auto p = impl_->root / impl_->infos[k].path;
#endif
        std::error_code x;
        if ((uint64_t)fs::file_size(p, x) != impl_->infos[k].size || x) return false;
        if (mtime_ns(p) != impl_->infos[k].mtime_ns) return false;
    }
    return true;
}

namespace {
constexpr std::uint64_t kManifestMagic = 0x4d414e4946455354ULL; // MANIFEST
constexpr std::uint32_t kManifestSchema = 1;

static std::uint64_t fnv_mix(std::uint64_t h, std::string_view s) noexcept {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) noexcept {
    for (unsigned i = 0; i < 8; ++i) { h ^= static_cast<unsigned char>(v); h *= 1099511628211ULL; v >>= 8; }
    return h;
}

// Compute a source fingerprint from metadata only. This is intentionally free
// of file-content reads so stale v5 entries can be rejected before filters or
// resident corpus bytes are materialized.
static std::uint64_t source_identity(const fs::path& root, const IndexOptions& opt) {
    std::vector<std::tuple<std::string, std::uint64_t, std::int64_t>> files;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return 0;
    std::unordered_set<std::string> visited_dirs;
    if (opt.follow_symlinks) {
        auto rc = fs::canonical(root, ec);
        if (!ec) visited_dirs.insert(rc.generic_string());
    }
    auto dop = fs::directory_options::skip_permission_denied |
        (opt.follow_symlinks ? fs::directory_options::follow_directory_symlink : fs::directory_options::none);
    for (auto it = fs::recursive_directory_iterator(root, dop, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& e = *it;
        if (!opt.include_hidden) {
            auto name = e.path().filename().string();
            if (!name.empty() && name[0] == '.' && name != "." && name != "..") {
                if (e.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
        }
        if (!opt.follow_symlinks && pergrep_cli::platform::is_reparse_point(e.path())) {
            if (e.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (opt.follow_symlinks && e.is_directory(ec)) {
            auto canon = fs::canonical(e.path(), ec);
            if (!ec && !visited_dirs.insert(canon.generic_string()).second) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (e.is_regular_file(ec)) {
            auto rel = pergrep_cli::platform::path_to_utf8(e.path().lexically_relative(root));
            std::error_code x;
            auto size = fs::file_size(e.path(), x);
            if (x) return 0;
            files.emplace_back(std::move(rel), size, mtime_ns(e.path()));
        }
    }
    std::sort(files.begin(), files.end());
    auto h = fnv_mix(1469598103934665603ULL, pergrep_cli::platform::path_to_utf8(root));
    for (const auto& [path, size, mtime] : files) {
        h = fnv_mix(h, path); h = fnv_mix(h, size); h = fnv_mix(h, static_cast<std::uint64_t>(mtime));
    }
    return h;
}

static bool same_options(const IndexOptions& a, const IndexOptions& b) noexcept {
    return a.chunk_bytes == b.chunk_bytes && a.chunk_overlap == b.chunk_overlap &&
        a.positional_block_bytes == b.positional_block_bytes &&
        a.positional_budget_ratio == b.positional_budget_ratio &&
        a.planned_qgrams == b.planned_qgrams && a.include_hidden == b.include_hidden &&
        a.follow_symlinks == b.follow_symlinks && a.persist_corpus == b.persist_corpus;
}

// Serialization is host-endian (little-endian on x86_64) and not portable across
// architectures. All scalar fields are written as raw host bytes via put<T> and
// read via get<T>. This is intentional for speed; an index built on one
// endianness cannot be loaded on another without conversion. Field-by-field
// encoding is used for Chunk and PosDesc (no putv<Chunk>/putv<PosDesc>) to
// avoid padding divergence across compilers/platforms.
template<class T> void put(std::ostream& o, const T& x) {
    o.write(reinterpret_cast<const char*>(&x), sizeof x);
    if (!o) throw std::runtime_error("index write failed");
}
template<class T> T get(std::istream& i) {
    T x{};
    i.read(reinterpret_cast<char*>(&x), sizeof x);
    if (!i) throw std::runtime_error("pergrep index: truncated");
    return x;
}
void puts(std::ostream& o, std::string_view s) {
    uint64_t n = s.size();
    put(o, n);
    if (n) {
        o.write(s.data(), static_cast<std::streamsize>(n));
        if (!o) throw std::runtime_error("index write failed");
    }
}
// Max sizes to avoid OOM / "string too long" on corrupted input.
constexpr uint64_t kMaxString = 16 * 1024 * 1024; // 16 MiB per string (path/root)
constexpr uint64_t kMaxFiles = 10'000'000;
constexpr uint64_t kMaxChunks = 100'000'000;
constexpr uint64_t kMaxPosDesc = 100'000'000;
constexpr uint64_t kMaxVectorElems = 200'000'000; // for gids/bits/pos
std::string gets(std::istream& i) {
    auto n = get<uint64_t>(i);
    if (n > kMaxString) throw std::runtime_error("pergrep index: truncated");
    std::string s;
    s.resize(static_cast<size_t>(n));
    if (n) {
        i.read(s.data(), static_cast<std::streamsize>(n));
        if (!i) throw std::runtime_error("pergrep index: truncated");
    }
    return s;
}
template<class T> void putv(std::ostream& o, const std::vector<T>& v) {
    uint64_t n = v.size();
    put(o, n);
    if (n) {
        o.write(reinterpret_cast<const char*>(v.data()), sizeof(T) * n);
        if (!o) throw std::runtime_error("index write failed");
    }
}
template<class T> std::vector<T> getv(std::istream& i) {
    auto n = get<uint64_t>(i);
    if (n > kMaxVectorElems) throw std::runtime_error("pergrep index: truncated");
    // Guard against overflow in sizeof(T)*n allocation check
    if (n > 0 && n > (UINT64_MAX / sizeof(T))) throw std::runtime_error("pergrep index: truncated");
    std::vector<T> v;
    v.resize(static_cast<size_t>(n));
    if (n) {
        i.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(sizeof(T) * n));
        if (!i) throw std::runtime_error("pergrep index: truncated");
    }
    return v;
}
inline std::string pid_suffix() {
#ifdef _WIN32
    return std::to_string(_getpid());
#else
    return std::to_string(::getpid());
#endif
}
}
void Index::save(const fs::path& file) const {
    save(file, CacheManifest{});
}

void Index::save(const fs::path& file, const CacheManifest& requested) const {
    if (!impl_) throw std::runtime_error("cannot persist an uninitialized pergrep index");
    if (impl_->ephemeral) throw std::runtime_error("cannot persist an in-memory pergrep index");
    CacheManifest manifest = requested;
    if (manifest.schema_version && *manifest.schema_version != kManifestSchema)
        throw std::runtime_error("unsupported cache manifest schema");
    manifest.schema_version = manifest.schema_version.value_or(kManifestSchema);
    if (manifest.index_options && !same_options(*manifest.index_options, impl_->opt))
        throw std::runtime_error("cache manifest index options do not match index");
    if (manifest.selector_identity && *manifest.selector_identity == 0)
        manifest.selector_identity = 0;
    manifest.source_root = manifest.source_root.value_or(pergrep_cli::platform::path_to_utf8(impl_->root));
    manifest.source_identity = manifest.source_identity.value_or(source_identity(impl_->root, impl_->opt));
    manifest.selector_identity = manifest.selector_identity.value_or(0);
    manifest.index_options = manifest.index_options.value_or(impl_->opt);
    manifest.transform_identity = manifest.transform_identity.value_or(0);
    manifest.corpus_files = manifest.corpus_files.value_or(impl_->infos.size());
    manifest.corpus_bytes = manifest.corpus_bytes.value_or(impl_->corp_bytes);
    manifest.generation = manifest.generation.value_or(static_cast<std::uint64_t>(impl_->root_mtime_ns));
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path());
    // Crash-safe: write to temp file then atomic rename. fs::rename is atomic on
    // POSIX and uses MoveFileExW on Windows (atomic when on same volume).
    fs::path tmp = file;
    tmp += ".tmp." + pid_suffix();
    // Ensure any stale temp is removed before writing
    std::error_code ec_rm;
    fs::remove(tmp, ec_rm);
    try {
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) throw std::runtime_error("cannot create index: " + tmp.string());
            o.write("PERGREP\0", 8);
            // QO-5: v5 = filter-only (legacy, re-read corpus on load, O(corpus) I/O).
            // v6 = filter + persisted corpus bytes (prototype on-disk corpus) so
            // load can restore I->loaded without touching the filesystem.
            // Default persist_corpus=false keeps v5 for backward compat and small files;
            // persist_corpus=true emits v6 and appends raw corpus after pos vector.
            uint32_t ver = impl_->opt.persist_corpus ? 6 : 5;
            put(o, ver);
            // Manifest is a fixed, bounded preamble. It is validated before the
            // legacy filter sections or v6 corpus payload are read.
            put(o, kManifestMagic);
            put(o, *manifest.schema_version);
            put(o, *manifest.source_identity);
            puts(o, *manifest.source_root);
            put(o, *manifest.selector_identity);
            const auto& mo = *manifest.index_options;
            put(o, uint64_t(mo.chunk_bytes));
            put(o, uint64_t(mo.chunk_overlap));
            put(o, uint64_t(mo.positional_block_bytes));
            put(o, mo.positional_budget_ratio);
            put(o, uint64_t(mo.planned_qgrams));
            put(o, uint8_t(mo.include_hidden ? 1 : 0));
            put(o, uint8_t(mo.follow_symlinks ? 1 : 0));
            put(o, uint8_t(mo.persist_corpus ? 1 : 0));
            put(o, *manifest.transform_identity);
            put(o, *manifest.corpus_files);
            put(o, *manifest.corpus_bytes);
            put(o, *manifest.generation);
            puts(o, pergrep_cli::platform::path_to_utf8(impl_->root));
            put(o, uint64_t(impl_->opt.chunk_bytes));
            put(o, uint64_t(impl_->opt.chunk_overlap));
            put(o, uint64_t(impl_->opt.positional_block_bytes));
            put(o, impl_->opt.positional_budget_ratio);
            put(o, uint64_t(impl_->opt.planned_qgrams));
            put(o, uint8_t(impl_->opt.include_hidden ? 1 : 0));
            put(o, uint8_t(impl_->opt.follow_symlinks ? 1 : 0));
            put(o, uint64_t(impl_->corp_bytes));
            put(o, int64_t(impl_->root_mtime_ns));
            o.write(reinterpret_cast<const char*>(impl_->byte_freq.data()), sizeof(impl_->byte_freq));
            // v5/v6 retain qgram_freq as the legacy hash-bucket occurrence
            // array. M1.5 planner tables are recomputed on load from corpus
            // bytes, so these formats remain byte-for-byte compatible.
            o.write(reinterpret_cast<const char*>(impl_->qgram_freq.data()), sizeof(impl_->qgram_freq));
            if (!o) throw std::runtime_error("index write failed");
            put(o, uint32_t(impl_->pos_block));
            uint64_t nf = impl_->infos.size();
            put(o, nf);
            for (auto& f : impl_->infos) {
                puts(o, f.path);
                put(o, uint64_t(f.size));
                put(o, int64_t(f.mtime_ns));
                put(o, uint8_t(f.binary ? 1 : 0));
            }
            put(o, uint64_t(impl_->chunks.size()));
            // Field-by-field, not putv<Chunk>, to avoid padding.
            for (auto const& c : impl_->chunks) {
                put(o, uint32_t(c.file_id));
                put(o, uint64_t(c.core_begin));
                put(o, uint64_t(c.core_end));
                put(o, uint64_t(c.ext_end));
            }
            for (auto& g : impl_->groups) {
                put(o, uint8_t(g.lg));
                put(o, uint32_t(g.m));
                put(o, uint32_t(g.words));
                putv(o, g.gids);
                putv(o, g.bits);
            }
            put(o, uint64_t(impl_->pos_desc.size()));
            // Field-by-field for PosDesc as well.
            for (auto const& d : impl_->pos_desc) {
                put(o, uint64_t(d.off));
                put(o, uint16_t(d.m));
                put(o, uint32_t(d.mask_bytes));
                put(o, uint32_t(d.blocks));
            }
            putv(o, impl_->pos);
            // QO-5 prototype on-disk corpus: when persist_corpus is true, also
            // persist the raw file contents after the filter. This decouples filter
            // persistence from corpus re-read: load no longer needs O(corpus) I/O.
            // Documented as prototype — index files become larger by corpus_bytes.
            if (impl_->opt.persist_corpus) {
                for (auto& lf : impl_->loaded) {
                    puts(o, lf.data);
                }
            }
            o.flush();
            if (!o) {
                o.close();
                fs::remove(tmp, ec_rm);
                throw std::runtime_error("index write failed");
            }
            o.close();
            if (!o) {
                fs::remove(tmp, ec_rm);
                throw std::runtime_error("index write failed");
            }
        }
        std::error_code ec;
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec_rm);
            throw std::runtime_error("cannot finalize index: " + ec.message());
        }
    } catch (...) {
        std::error_code ec2;
        // Best-effort cleanup; do not hide original exception.
        // If tmp still exists, remove it. If rename already succeeded, tmp is gone.
        fs::remove(tmp, ec2);
        throw;
    }
}
Index Index::load(const fs::path& file) {
    return load(file, CacheManifest{});
}

Index Index::load(const fs::path& file, const CacheManifest& expected) {
    // Truncation guard: check file size is at least header before reading.
    {
        std::error_code ec;
        auto sz = fs::file_size(file, ec);
        if (!ec) {
            constexpr uint64_t kMinHeader = 8 + 4; // magic + version
            if (sz < kMinHeader) throw std::runtime_error("pergrep index: truncated");
        }
    }
    std::ifstream i(file, std::ios::binary);
    if (!i) throw std::runtime_error("cannot open index: " + file.string());
    char magic[8];
    i.read(magic, 8);
    if (!i || std::memcmp(magic, "PERGREP\0", 8) != 0) throw std::runtime_error("pergrep index: truncated");
    auto ver = get<uint32_t>(i);
    if (ver != 5 && ver != 6) throw std::runtime_error("unsupported pergrep index version");

    CacheManifest stored;
    bool has_manifest = false;
    std::uint64_t marker = 0;
    i.read(reinterpret_cast<char*>(&marker), sizeof marker);
    if (!i) throw std::runtime_error("pergrep index: truncated");
    if (marker == kManifestMagic) {
        has_manifest = true;
        stored.schema_version = get<std::uint32_t>(i);
        if (stored.schema_version != kManifestSchema)
            throw std::runtime_error("unsupported cache manifest schema");
        stored.source_identity = get<std::uint64_t>(i);
        stored.source_root = gets(i);
        stored.selector_identity = get<std::uint64_t>(i);
        IndexOptions mo;
        mo.chunk_bytes = get<std::uint64_t>(i);
        mo.chunk_overlap = get<std::uint64_t>(i);
        mo.positional_block_bytes = get<std::uint64_t>(i);
        mo.positional_budget_ratio = get<double>(i);
        mo.planned_qgrams = get<std::uint64_t>(i);
        mo.include_hidden = get<std::uint8_t>(i) != 0;
        mo.follow_symlinks = get<std::uint8_t>(i) != 0;
        mo.persist_corpus = get<std::uint8_t>(i) != 0;
        stored.index_options = mo;
        stored.transform_identity = get<std::uint64_t>(i);
        stored.corpus_files = get<std::uint64_t>(i);
        stored.corpus_bytes = get<std::uint64_t>(i);
        stored.generation = get<std::uint64_t>(i);

        auto mismatch = [&](bool bad, const char* what) {
            if (bad) throw std::runtime_error(std::string("stale cache manifest: ") + what);
        };
        mismatch(expected.schema_version && *expected.schema_version != *stored.schema_version, "schema");
        if (expected.source_identity) mismatch(*expected.source_identity != *stored.source_identity, "source identity");
        if (expected.source_root) mismatch(*expected.source_root != *stored.source_root, "source root");
        if (expected.selector_identity) mismatch(*expected.selector_identity != *stored.selector_identity, "selector");
        if (expected.index_options) mismatch(!same_options(*expected.index_options, *stored.index_options), "index options");
        if (expected.transform_identity) mismatch(*expected.transform_identity != *stored.transform_identity, "transform");
        if (expected.corpus_files) mismatch(*expected.corpus_files != *stored.corpus_files, "corpus files");
        if (expected.corpus_bytes) mismatch(*expected.corpus_bytes != *stored.corpus_bytes, "corpus bytes");
        if (expected.generation) mismatch(*expected.generation != *stored.generation, "generation");

#ifdef _WIN32
        auto root_path = fs::path(std::u8string(stored.source_root->begin(), stored.source_root->end()));
#else
        auto root_path = fs::path(*stored.source_root);
#endif
        // v5 is source-backed, so metadata validation happens while only the
        // manifest is resident. v6 remains loadable after its source disappears.
        if (ver == 5 && source_identity(root_path, *stored.index_options) != *stored.source_identity)
            throw std::runtime_error("stale cache manifest: source changed or disappeared");
    } else {
        // Existing v5/v6 files predate the manifest. Keep their documented
        // compatibility for the unqualified API, but never use one to satisfy
        // an explicit manifest-aware request.
        i.seekg(-static_cast<std::streamoff>(sizeof marker), std::ios::cur);
        if (!i) throw std::runtime_error("pergrep index: truncated");
        if (expected.schema_version || expected.source_identity || expected.source_root || expected.selector_identity ||
            expected.index_options || expected.transform_identity || expected.corpus_files ||
            expected.corpus_bytes || expected.generation)
            throw std::runtime_error("cache manifest missing");
    }

    auto I = std::make_shared<Impl>();
    I->opt.persist_corpus = (ver == 6);
#ifdef _WIN32
    auto root_str = gets(i);
    I->root = fs::path(std::u8string(root_str.begin(), root_str.end()));
#else
    I->root = gets(i);
#endif
    I->opt.chunk_bytes = get<uint64_t>(i);
    I->opt.chunk_overlap = get<uint64_t>(i);
    I->opt.positional_block_bytes = get<uint64_t>(i);
    I->opt.positional_budget_ratio = get<double>(i);
    I->opt.planned_qgrams = get<uint64_t>(i);
    I->opt.include_hidden = get<uint8_t>(i) != 0;
    I->opt.follow_symlinks = get<uint8_t>(i) != 0;
    I->corp_bytes = get<uint64_t>(i);
    I->root_mtime_ns = get<int64_t>(i);
    if (has_manifest) {
        if (!same_options(I->opt, *stored.index_options) || I->opt.persist_corpus != (ver == 6))
            throw std::runtime_error("stale cache manifest: index options");
        if (pergrep_cli::platform::path_to_utf8(I->root) != *stored.source_root)
            throw std::runtime_error("stale cache manifest: source root");
        if (I->corp_bytes != *stored.corpus_bytes)
            throw std::runtime_error("stale cache manifest: corpus bytes");
    }
    i.read(reinterpret_cast<char*>(I->byte_freq.data()), sizeof(I->byte_freq));
    // qgram_freq is legacy hash-bucket occurrence data; exact planner stats
    // are rebuilt below after v5/v6 corpus restoration.
    i.read(reinterpret_cast<char*>(I->qgram_freq.data()), sizeof(I->qgram_freq));
    if (!i) throw std::runtime_error("pergrep index: truncated");
    I->pos_block = get<uint32_t>(i);
    auto nf = get<uint64_t>(i);
    if (nf > kMaxFiles) throw std::runtime_error("pergrep index: truncated");
    if (has_manifest && nf != *stored.corpus_files)
        throw std::runtime_error("stale cache manifest: corpus files");
    I->infos.reserve(static_cast<size_t>(nf));
    I->loaded.reserve(static_cast<size_t>(nf));
    for (uint64_t k = 0; k < nf; ++k) {
        FileInfo f;
        f.path = gets(i);
        f.size = get<uint64_t>(i);
        f.mtime_ns = get<int64_t>(i);
        f.binary = get<uint8_t>(i) != 0;
        I->infos.push_back(std::move(f));
    }
    std::uint64_t info_total = 0;
    for (const auto& f : I->infos) {
        if (f.size > UINT64_MAX - info_total)
            throw std::runtime_error("pergrep index: invalid corpus totals");
        info_total += f.size;
    }
    if (info_total != I->corp_bytes)
        throw std::runtime_error("pergrep index: invalid corpus totals");
    if (has_manifest && info_total != *stored.corpus_bytes)
        throw std::runtime_error("stale cache manifest: corpus bytes");
    auto nc = get<uint64_t>(i);
    if (nc > kMaxChunks) throw std::runtime_error("pergrep index: truncated");
    I->chunks.reserve(static_cast<size_t>(nc));
    for (uint64_t k = 0; k < nc; ++k) {
        Chunk c;
        c.file_id = get<uint32_t>(i);
        c.core_begin = get<uint64_t>(i);
        c.core_end = get<uint64_t>(i);
        c.ext_end = get<uint64_t>(i);
        if (c.file_id >= nf || c.core_begin > c.core_end || c.core_end > c.ext_end ||
            c.ext_end > I->infos[c.file_id].size)
            throw std::runtime_error("pergrep index: invalid chunk geometry");
        I->chunks.push_back(c);
    }
    for (auto& g : I->groups) {
        g.lg = get<uint8_t>(i);
        g.m = get<uint32_t>(i);
        g.words = get<uint32_t>(i);
        g.gids = getv<uint32_t>(i);
        g.bits = getv<uint64_t>(i);
        if (g.lg < 9 || g.lg > 16 || g.m != (1u << g.lg) ||
            g.words != (g.gids.size() + 63) / 64 ||
            static_cast<std::uint64_t>(g.words) > UINT64_MAX / (1u << g.lg) ||
            g.bits.size() != static_cast<std::uint64_t>(1u << g.lg) * g.words)
            throw std::runtime_error("pergrep index: invalid qgram group");
        for (auto gid : g.gids)
            if (gid >= nc) throw std::runtime_error("pergrep index: invalid qgram group reference");
    }
    auto npd = get<uint64_t>(i);
    if (npd != nc || npd > kMaxPosDesc) throw std::runtime_error("pergrep index: invalid positional descriptors");
    I->pos_desc.reserve(static_cast<size_t>(npd));
    for (uint64_t k = 0; k < npd; ++k) {
        detail::PosDesc d;
        d.off = get<uint64_t>(i);
        d.m = get<uint16_t>(i);
        d.mask_bytes = get<uint32_t>(i);
        d.blocks = get<uint32_t>(i);
        I->pos_desc.push_back(d);
    }
    I->pos = getv<uint8_t>(i);
    std::uint64_t pos_end = 0;
    for (const auto& d : I->pos_desc) {
        if (d.m < 64 || d.m > 1024 || (d.m & (d.m - 1)) != 0 || d.blocks == 0 ||
            d.mask_bytes != (static_cast<std::uint64_t>(d.blocks) + 7) / 8 ||
            d.off != pos_end || static_cast<std::uint64_t>(d.mask_bytes) >
                (I->pos.size() - std::min<std::uint64_t>(pos_end, I->pos.size())) / d.m)
            throw std::runtime_error("pergrep index: invalid positional descriptors");
        pos_end += static_cast<std::uint64_t>(d.m) * d.mask_bytes;
    }
    if (pos_end != I->pos.size()) throw std::runtime_error("pergrep index: invalid positional data");
    // QO-5: decouple filter persistence from corpus re-read.
    // v5 (persist_corpus==false): legacy path re-reads every source file via
    // std::ifstream (O(corpus) I/O) to repopulate I->loaded. Documented as
    // prototype cost — suitable for stable trees, large corpora pay re-read.
    // v6 (persist_corpus==true): persisted corpus bytes follow the filter;
    // restore I->loaded directly from the index without touching the filesystem.
    if (ver == 6) {
        std::uint64_t payload_total = 0;
        for (size_t k = 0; k < I->infos.size(); ++k) {
            auto n = get<uint64_t>(i);
            if (n != I->infos[k].size || n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                n > UINT64_MAX - payload_total)
                throw std::runtime_error("pergrep index: invalid corpus payload");
            payload_total += n;
            std::string data;
            data.resize(static_cast<size_t>(n));
            if (n) {
                i.read(data.data(), static_cast<std::streamsize>(n));
                if (!i) throw std::runtime_error("pergrep index: truncated");
            }
            I->loaded.push_back({I->infos[k], std::move(data)});
        }
        if (payload_total != I->corp_bytes || (has_manifest && payload_total != *stored.corpus_bytes))
            throw std::runtime_error("pergrep index: invalid corpus payload totals");
    } else {
        for (auto& f : I->infos) {
#ifdef _WIN32
            std::ifstream src(I->root / fs::path(std::u8string(f.path.begin(), f.path.end())), std::ios::binary);
#else
            std::ifstream src(I->root / f.path, std::ios::binary);
#endif
            if (!src) throw std::runtime_error("indexed source disappeared: " + f.path);
            std::string data((std::istreambuf_iterator<char>(src)), {});
            if (data.size() != f.size)
                throw std::runtime_error("indexed source changed or disappeared: " + f.path);
            I->loaded.push_back({f, std::move(data)});
        }
    }
    // v5/v6 do not contain planner statistics. Recompute them from the loaded
    // corpus rather than interpreting legacy qgram_freq bytes as exact stats.
    rebuild_planner_stats(*I);
    return Index(I);
}

std::string version(){return "0.1.0";}
} // namespace pergrep
