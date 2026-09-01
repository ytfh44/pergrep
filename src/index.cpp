#include "internal.hpp"
#include "platform.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <limits>
#include <tuple>
#include <type_traits>
#ifdef _WIN32
#include <process.h>
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

namespace pergrep::detail {
static std::int64_t provider_mtime_ns(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}
std::shared_ptr<const CorpusProvider> CorpusProvider::resident(std::string data) {
    auto p = std::shared_ptr<CorpusProvider>(new CorpusProvider());
    p->resident_ = std::move(data);
    p->data_ = p->resident_.data();
    p->size_ = p->resident_.size();
    return p;
}

std::shared_ptr<const CorpusProvider> CorpusProvider::mapped(const std::filesystem::path& path,
                                                              std::uint64_t expected_size,
                                                              std::int64_t expected_mtime_ns) {
    std::error_code ec;
    const auto actual_size = std::filesystem::file_size(path, ec);
    if (ec || actual_size != expected_size)
        throw std::runtime_error("indexed source changed or disappeared: " + path.string());
    if (expected_mtime_ns != 0 && provider_mtime_ns(path) != expected_mtime_ns)
        throw std::runtime_error("indexed source changed: " + path.string());
    auto p = std::shared_ptr<CorpusProvider>(new CorpusProvider());
    if (expected_size == 0) return p;
    if (expected_size > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("indexed source is too large for this platform");
#ifdef _WIN32
    const std::wstring wpath = path.wstring();
    HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li{};
        if (GetFileSizeEx(file, &li) && li.QuadPart >= 0 && static_cast<std::uint64_t>(li.QuadPart) == expected_size) {
            const DWORD high = static_cast<DWORD>(expected_size >> 32);
            const DWORD low = static_cast<DWORD>(expected_size & 0xffffffffu);
            // Windows keeps source files with active section objects undeletable even
            // when the original handle grants FILE_SHARE_DELETE. Populate a private
            // pagefile-backed section, then release the source handle; the exposed
            // bytes remain immutable and demand-paged without pinning the source path.
            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, nullptr);
            if (mapping) {
                auto* writable = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0));
                bool copied = writable != nullptr;
                std::uint64_t offset = 0;
                while (copied && offset < expected_size) {
                    const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(expected_size - offset, 1u << 20));
                    DWORD got = 0;
                    copied = ReadFile(file, writable + offset, want, &got, nullptr) && got == want;
                    offset += got;
                }
                char extra = 0;
                DWORD extra_bytes = 0;
                copied = copied && ReadFile(file, &extra, 1, &extra_bytes, nullptr) && extra_bytes == 0;
                LARGE_INTEGER final_size{};
                copied = copied && GetFileSizeEx(file, &final_size) && final_size.QuadPart >= 0 &&
                    static_cast<std::uint64_t>(final_size.QuadPart) == expected_size;
                if (writable) UnmapViewOfFile(writable);
                if (copied) {
                    auto* data = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
                    if (data) {
                        CloseHandle(file);
                        p->mapping_ = mapping; p->data_ = data;
                        p->size_ = static_cast<std::size_t>(expected_size); return p;
                    }
                }
                CloseHandle(mapping);
            }
        }
        CloseHandle(file);
    }
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        struct stat st{};
        if (fstat(fd, &st) == 0 && static_cast<std::uint64_t>(st.st_size) == expected_size &&
            (expected_mtime_ns == 0 || static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec == expected_mtime_ns)) {
            // A file-backed mapping can SIGBUS after a concurrent truncation. Copy
            // through an anonymous mapping, then revoke write access before exposing
            // it; the provider remains immutable and independent of the source inode.
            void* data = mmap(nullptr, static_cast<std::size_t>(expected_size), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            bool copied = data != MAP_FAILED;
            std::uint64_t offset = 0;
            while (copied && offset < expected_size) {
                const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(expected_size - offset, 1u << 20));
                const auto got = ::pread(fd, static_cast<char*>(data) + offset, want, static_cast<off_t>(offset));
                copied = got == static_cast<ssize_t>(want);
                offset += got > 0 ? static_cast<std::uint64_t>(got) : 0;
            }
            char extra = 0;
            copied = copied && ::pread(fd, &extra, 1, static_cast<off_t>(expected_size)) == 0;
            struct stat final_st{};
            copied = copied && fstat(fd, &final_st) == 0 && static_cast<std::uint64_t>(final_st.st_size) == expected_size &&
                (expected_mtime_ns == 0 || static_cast<std::int64_t>(final_st.st_mtim.tv_sec) * 1'000'000'000LL + final_st.st_mtim.tv_nsec == expected_mtime_ns);
            if (copied && mprotect(data, static_cast<std::size_t>(expected_size), PROT_READ) == 0) {
                p->fd_ = fd; p->mapped_ = true; p->data_ = static_cast<const char*>(data);
                p->size_ = static_cast<std::size_t>(expected_size); return p;
            }
            if (data != MAP_FAILED) munmap(data, static_cast<std::size_t>(expected_size));
        }
        close(fd);
    }
#endif
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("indexed source disappeared: " + path.string());
        std::string data;
        data.resize(static_cast<std::size_t>(expected_size));
        if (expected_size) in.read(data.data(), static_cast<std::streamsize>(expected_size));
        if (!in || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("indexed source changed: " + path.string());
        p->resident_ = std::move(data); p->data_ = p->resident_.data(); p->size_ = p->resident_.size();
        return p;
    }
}

CorpusProvider::~CorpusProvider() {
#ifdef _WIN32
    if (data_ && mapping_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (mapped_ && data_ && size_) munmap(const_cast<char*>(data_), size_);
    if (fd_ >= 0) close(fd_);
#endif
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
inline constexpr std::uint64_t kMaxV6PayloadBytes = 1ULL << 30; // legacy name retained for compatibility
inline constexpr std::uint64_t kMaxCorpusBytes = 1ULL << 30;
inline constexpr std::uint64_t kMaxSnapshotBytes = kMaxCorpusBytes + 256ULL * 1024 * 1024;
inline constexpr std::size_t kMaxManifestFiles = 1'000'000;
inline constexpr std::uint64_t kMaxManifestMetadataBytes = 256ULL * 1024 * 1024;

static bool unsafe_file_info_path(std::string_view path) noexcept {
    if (path.empty()) return false;
    if (path.front() == '/' || path.front() == '\\' ||
        (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':'))
        return true;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find_first_of("/\\", begin);
        const auto component = path.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (component == "..") return true;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}

static void rebuild_planner_stats(detail::IndexData& I) {
    I.exact_qgrams.clear();
    I.hash_chunk_freq.fill(0);
    for (auto& ids : I.hash_chunk_ids) ids.clear();
    I.planner_stats_ready = false;
    if (I.corp_bytes > kPlannerCorpusCap) return;
    for (std::uint32_t fid = 0; fid < I.loaded.size(); ++fid) {
        const auto& data = I.loaded[fid].view();
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
        const auto data_size = static_cast<std::uint64_t>(I.loaded[c.file_id].view().size());
        if (c.core_begin > data_size || c.ext_end < c.core_begin || c.ext_end > data_size)
            continue;
        const auto view = std::string_view(I.loaded[c.file_id].view()).substr(
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
        if (s.size() > kMaxCorpusBytes - std::min(I->corp_bytes, kMaxCorpusBytes))
            throw std::runtime_error("index corpus exceeds 1 GiB limit");
        I->corp_bytes += s.size();
        for (unsigned char c : s) ++I->byte_freq[c];
        if (s.size() >= 4) {
            for (size_t q = 0; q + 4 <= s.size(); ++q) {
                auto h = detail::hash4((const unsigned char*)s.data() + q) & 65535u;
                if (I->qgram_freq[h] != UINT32_MAX) ++I->qgram_freq[h];
            }
        }
        I->infos.push_back(fi);
        I->loaded.push_back({fi, detail::CorpusProvider::resident(std::move(s))});
    }

    uint64_t core = opt.chunk_bytes, over = opt.chunk_overlap;
    for (uint32_t fid = 0; fid < I->loaded.size(); ++fid) {
        uint64_t n = I->loaded[fid].view().size();
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
        auto v = std::string_view(I->loaded[c.file_id].view()).substr(c.core_begin, c.ext_end - c.core_begin);
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
        auto z = I->chunks[ci]; auto d = I->pos_desc[ci]; auto whole = std::string_view(I->loaded[z.file_id].view());
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
    if (!opt.persist_corpus) {
        for (auto& lf : I->loaded) {
#ifdef _WIN32
            const auto source_path = I->root / fs::path(std::u8string(lf.info.path.begin(), lf.info.path.end()));
#else
            const auto source_path = I->root / lf.info.path;
#endif
            try {
                lf.provider = detail::CorpusProvider::mapped(source_path, lf.info.size, lf.info.mtime_ns);
            } catch (const std::exception&) {
                // The resident bytes collected during indexing are a safe fallback
                // when the source disappears between scan and provider attachment.
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
        if (d.content.size() > kMaxCorpusBytes - std::min(I->corp_bytes, kMaxCorpusBytes))
            throw std::runtime_error("index corpus exceeds 1 GiB limit");
        I->corp_bytes += d.content.size();
        for (unsigned char c : d.content) ++I->byte_freq[c];
        if (d.content.size() >= 4) {
            for (size_t q = 0; q + 4 <= d.content.size(); ++q) {
                auto h = detail::hash4((const unsigned char*)d.content.data() + q) & 65535u;
                if (I->qgram_freq[h] != UINT32_MAX) ++I->qgram_freq[h];
            }
        }
        I->infos.push_back(fi);
        I->loaded.push_back({fi, detail::CorpusProvider::resident(std::move(d.content))});
    }

    uint64_t core = opt.chunk_bytes, over = opt.chunk_overlap;
    for (uint32_t fid = 0; fid < I->loaded.size(); ++fid) {
        uint64_t n = I->loaded[fid].view().size();
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
        auto v = std::string_view(I->loaded[c.file_id].view()).substr(c.core_begin, c.ext_end - c.core_begin);
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
        auto z = I->chunks[ci]; auto d = I->pos_desc[ci]; auto whole = std::string_view(I->loaded[z.file_id].view());
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
    return impl_->loaded[file_id].view();
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
constexpr std::uint32_t kManifestSchema = kIndexFormatSchema;
constexpr std::uint32_t kPortableIndexVersion = kIndexFormatVersion;
constexpr std::uint32_t kFeaturePersistedCorpus = 1u;
constexpr std::uint32_t kFeatureIntegrityChecksum = 2u;

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
    std::uint64_t metadata_bytes = 0;
    std::uint64_t corpus_bytes = 0;
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
            // A manifest may contain an untrusted source_root. Bound metadata
            // collection so validation cannot traverse an oversized tree or
            // accumulate an attacker-controlled vector before payload checks.
            if (files.size() >= kMaxManifestFiles ||
                metadata_bytes > kMaxManifestMetadataBytes ||
                rel.size() > kMaxManifestMetadataBytes - metadata_bytes ||
                size > kMaxCorpusBytes - std::min(corpus_bytes, kMaxCorpusBytes))
                return 0;
            metadata_bytes += rel.size();
            corpus_bytes += size;
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
static bool valid_index_options(const IndexOptions& o) noexcept {
    return o.chunk_bytes >= 64 && o.chunk_bytes <= (1ULL << 30) &&
        o.positional_block_bytes >= 16 && o.positional_block_bytes <= (1ULL << 20) &&
        o.chunk_overlap <= o.chunk_bytes / 2 && o.planned_qgrams <= 64 &&
        std::isfinite(o.positional_budget_ratio) &&
        o.positional_budget_ratio >= 0.0 && o.positional_budget_ratio <= 10.0;
}

// v5/v6 were intentionally emitted as raw host-byte fields. Keep this codec
// solely for reading those historical files; never use it for new snapshots.
template<class T> void put_raw(std::ostream& o, const T& x) {
    o.write(reinterpret_cast<const char*>(&x), sizeof x);
    if (!o) throw std::runtime_error("index write failed");
}
template<class T> T get_raw(std::istream& i) {
    T x{};
    i.read(reinterpret_cast<char*>(&x), sizeof x);
    if (!i) throw std::runtime_error("pergrep index: truncated");
    return x;
}
void put_u8(std::ostream& o, std::uint8_t x) { put_raw(o, x); }
std::uint8_t get_u8(std::istream& i) { return get_raw<std::uint8_t>(i); }
void put_le16(std::ostream& o, std::uint16_t x) { char b[2] = {char(x), char(x >> 8)}; o.write(b, 2); if (!o) throw std::runtime_error("index write failed"); }
void put_le32(std::ostream& o, std::uint32_t x) { char b[4] = {char(x), char(x >> 8), char(x >> 16), char(x >> 24)}; o.write(b, 4); if (!o) throw std::runtime_error("index write failed"); }
void put_le64(std::ostream& o, std::uint64_t x) { char b[8]; for (unsigned k=0;k<8;++k) b[k]=char(x>>(8*k)); o.write(b, 8); if (!o) throw std::runtime_error("index write failed"); }
std::uint16_t get_le16(std::istream& i) { unsigned char b[2]; i.read(reinterpret_cast<char*>(b),2); if (!i) throw std::runtime_error("pergrep index: truncated"); return std::uint16_t(b[0]) | (std::uint16_t(b[1])<<8); }
std::uint32_t get_le32(std::istream& i) { unsigned char b[4]; i.read(reinterpret_cast<char*>(b),4); if (!i) throw std::runtime_error("pergrep index: truncated"); return std::uint32_t(b[0]) | (std::uint32_t(b[1])<<8) | (std::uint32_t(b[2])<<16) | (std::uint32_t(b[3])<<24); }
std::uint64_t get_le64(std::istream& i) { unsigned char b[8]; i.read(reinterpret_cast<char*>(b),8); if (!i) throw std::runtime_error("pergrep index: truncated"); std::uint64_t x=0; for(unsigned k=0;k<8;++k) x |= std::uint64_t(b[k]) << (8*k); return x; }
void put_le_i64(std::ostream& o, std::int64_t x) { put_le64(o, static_cast<std::uint64_t>(x)); }
std::int64_t get_le_i64(std::istream& i) { return static_cast<std::int64_t>(get_le64(i)); }
void put_le_double(std::ostream& o, double x) { put_le64(o, std::bit_cast<std::uint64_t>(x)); }
double get_le_double(std::istream& i) { return std::bit_cast<double>(get_le64(i)); }
// Max sizes to avoid OOM / "string too long" on corrupted input.
constexpr uint64_t kMaxString = 16 * 1024 * 1024;
constexpr uint64_t kMaxFiles = 10'000'000;
constexpr uint64_t kMaxChunks = 100'000'000;
constexpr uint64_t kMaxPosDesc = 100'000'000;
constexpr uint64_t kMaxVectorElems = 200'000'000;
// Keep reader and writer limits identical for every index generation.
// Reader/Writer make the v7 snapshot contract explicit: every scalar has a
// fixed width and is encoded little-endian. Legacy mode is used only for v5/v6.
struct Writer {
    std::ostream& o; bool portable;
    template<class T> void scalar(T x) {
        if (!portable) { put_raw(o, x); return; }
        if constexpr (std::is_same_v<T, std::uint8_t>) put_u8(o, x);
        else if constexpr (std::is_same_v<T, std::uint16_t>) put_le16(o, x);
        else if constexpr (std::is_same_v<T, std::uint32_t>) put_le32(o, x);
        else if constexpr (std::is_same_v<T, std::uint64_t>) put_le64(o, x);
        else if constexpr (std::is_same_v<T, std::int64_t>) put_le_i64(o, x);
        else if constexpr (std::is_same_v<T, double>) put_le_double(o, x);
        else static_assert(std::is_same_v<T, void>, "unsupported index scalar");
    }
    void string(std::string_view s) { scalar<std::uint64_t>(s.size()); if (!s.empty()) { o.write(s.data(), static_cast<std::streamsize>(s.size())); if (!o) throw std::runtime_error("index write failed"); } }
    template<class T> void vector(const std::vector<T>& v) { scalar<std::uint64_t>(v.size()); if (!portable && !v.empty()) { o.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(sizeof(T)*v.size())); if (!o) throw std::runtime_error("index write failed"); } else for (const auto x : v) scalar<T>(x); }
};
struct Reader {
    std::istream& i; bool portable; std::uint64_t stream_size;
    std::uint64_t remaining() {
        const auto p = i.tellg();
        if (p < 0 || static_cast<std::uint64_t>(p) > stream_size)
            throw std::runtime_error("pergrep index: truncated");
        return stream_size - static_cast<std::uint64_t>(p);
    }
    template<class T> T scalar() {
        if (!portable) return get_raw<T>(i);
        if constexpr (std::is_same_v<T, std::uint8_t>) return get_u8(i);
        else if constexpr (std::is_same_v<T, std::uint16_t>) return get_le16(i);
        else if constexpr (std::is_same_v<T, std::uint32_t>) return get_le32(i);
        else if constexpr (std::is_same_v<T, std::uint64_t>) return get_le64(i);
        else if constexpr (std::is_same_v<T, std::int64_t>) return get_le_i64(i);
        else if constexpr (std::is_same_v<T, double>) return get_le_double(i);
        else static_assert(std::is_same_v<T, void>, "unsupported index scalar");
    }
    std::string string() {
        const auto n = scalar<std::uint64_t>();
        if (n > kMaxString || n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) || n > remaining())
            throw std::runtime_error("pergrep index: truncated");
        std::string s(static_cast<size_t>(n), '\0');
        if (n) { i.read(s.data(), static_cast<std::streamsize>(n)); if (!i) throw std::runtime_error("pergrep index: truncated"); }
        return s;
    }
    template<class T> std::vector<T> vector() {
        const auto n = scalar<std::uint64_t>();
        if (n > kMaxVectorElems || n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) throw std::runtime_error("pergrep index: truncated");
        if (n > 0 && n > UINT64_MAX / sizeof(T)) throw std::runtime_error("pergrep index: truncated");
        if (n > remaining() / sizeof(T)) throw std::runtime_error("pergrep index: truncated");
        std::vector<T> v(static_cast<size_t>(n));
        if (!portable && n) i.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(sizeof(T)*n));
        else for (auto& x : v) x = scalar<T>();
        if (!i) throw std::runtime_error("pergrep index: truncated");
        return v;
    }
};
static std::uint64_t hash_file_range(const fs::path& file, std::uint64_t offset, std::uint64_t bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open index for integrity check: " + file.string());
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) throw std::runtime_error("pergrep index: truncated");
    std::uint64_t h = 14695981039346656037ULL;
    char buf[64 * 1024];
    while (bytes) {
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(bytes, sizeof(buf)));
        in.read(buf, want);
        if (in.gcount() != want) throw std::runtime_error("pergrep index: truncated");
        for (std::streamsize k = 0; k < want; ++k) {
            h ^= static_cast<unsigned char>(buf[k]);
            h *= 1099511628211ULL;
        }
        bytes -= static_cast<std::uint64_t>(want);
    }
    return h;
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
    if (impl_->corp_bytes > kMaxCorpusBytes)
        throw std::runtime_error("index corpus exceeds 1 GiB limit");
    if (impl_->opt.persist_corpus) {
        if (impl_->loaded.size() != impl_->infos.size())
            throw std::runtime_error("invalid v6 corpus payload");
        std::uint64_t payload_total = 0;
        for (std::size_t k = 0; k < impl_->infos.size(); ++k) {
            const auto bytes = static_cast<std::uint64_t>(impl_->loaded[k].view().size());
            if (bytes > kMaxCorpusBytes - std::min(payload_total, kMaxCorpusBytes))
                throw std::runtime_error("index corpus exceeds 1 GiB limit");
            payload_total += bytes;
        }
    }
    CacheManifest manifest = requested;
    if (manifest.schema_version.has_value() && *manifest.schema_version != kManifestSchema)
        throw std::runtime_error("unsupported cache manifest schema");
    manifest.schema_version = manifest.schema_version.value_or(kManifestSchema);
    if (manifest.index_options.has_value() && !same_options(*manifest.index_options, impl_->opt))
        throw std::runtime_error("cache manifest index options do not match index");
    if (manifest.selector_identity.has_value() && *manifest.selector_identity == 0)
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
            // v7 is the current portable snapshot format. Every scalar below is
            // fixed-width little-endian; optional sections are advertised by flags.
            Writer w{o, true};
            w.scalar<std::uint32_t>(kPortableIndexVersion);
            w.scalar<std::uint64_t>(kManifestMagic);
            w.scalar<std::uint32_t>(*manifest.schema_version);
            w.scalar<std::uint32_t>((impl_->opt.persist_corpus ? kFeaturePersistedCorpus : 0u) | kFeatureIntegrityChecksum);
            w.scalar<std::uint64_t>(*manifest.source_identity);
            w.string(*manifest.source_root);
            w.scalar<std::uint64_t>(*manifest.selector_identity);
            const auto& mo = *manifest.index_options;
            w.scalar<std::uint64_t>(mo.chunk_bytes);
            w.scalar<std::uint64_t>(mo.chunk_overlap);
            w.scalar<std::uint64_t>(mo.positional_block_bytes);
            w.scalar<double>(mo.positional_budget_ratio);
            w.scalar<std::uint64_t>(mo.planned_qgrams);
            w.scalar<std::uint8_t>(mo.include_hidden ? 1 : 0);
            w.scalar<std::uint8_t>(mo.follow_symlinks ? 1 : 0);
            w.scalar<std::uint8_t>(mo.persist_corpus ? 1 : 0);
            w.scalar<std::uint64_t>(*manifest.transform_identity);
            w.scalar<std::uint64_t>(*manifest.corpus_files);
            w.scalar<std::uint64_t>(*manifest.corpus_bytes);
            w.scalar<std::uint64_t>(*manifest.generation);
            w.string(pergrep_cli::platform::path_to_utf8(impl_->root));
            w.scalar<std::uint64_t>(impl_->opt.chunk_bytes);
            w.scalar<std::uint64_t>(impl_->opt.chunk_overlap);
            w.scalar<std::uint64_t>(impl_->opt.positional_block_bytes);
            w.scalar<double>(impl_->opt.positional_budget_ratio);
            w.scalar<std::uint64_t>(impl_->opt.planned_qgrams);
            w.scalar<std::uint8_t>(impl_->opt.include_hidden ? 1 : 0);
            w.scalar<std::uint8_t>(impl_->opt.follow_symlinks ? 1 : 0);
            w.scalar<std::uint64_t>(impl_->corp_bytes);
            w.scalar<std::int64_t>(impl_->root_mtime_ns);
            for (const auto x : impl_->byte_freq) w.scalar<std::uint64_t>(x);
            for (const auto x : impl_->qgram_freq) w.scalar<std::uint32_t>(x);
            w.scalar<std::uint32_t>(impl_->pos_block);
            w.scalar<std::uint64_t>(impl_->infos.size());
            for (const auto& f : impl_->infos) {
                w.string(f.path);
                w.scalar<std::uint64_t>(f.size);
                w.scalar<std::int64_t>(f.mtime_ns);
                w.scalar<std::uint8_t>(f.binary ? 1 : 0);
            }
            w.scalar<std::uint64_t>(impl_->chunks.size());
            for (const auto& c : impl_->chunks) {
                w.scalar<std::uint32_t>(c.file_id);
                w.scalar<std::uint64_t>(c.core_begin);
                w.scalar<std::uint64_t>(c.core_end);
                w.scalar<std::uint64_t>(c.ext_end);
            }
            for (const auto& g : impl_->groups) {
                w.scalar<std::uint8_t>(g.lg);
                w.scalar<std::uint32_t>(g.m);
                w.scalar<std::uint32_t>(g.words);
                w.vector(g.gids);
                w.vector(g.bits);
            }
            w.scalar<std::uint64_t>(impl_->pos_desc.size());
            for (const auto& d : impl_->pos_desc) {
                w.scalar<std::uint64_t>(d.off);
                w.scalar<std::uint16_t>(d.m);
                w.scalar<std::uint32_t>(d.mask_bytes);
                w.scalar<std::uint32_t>(d.blocks);
            }
            w.vector(impl_->pos);
            if (impl_->opt.persist_corpus) for (const auto& lf : impl_->loaded) w.string(lf.view());
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
        std::error_code size_ec;
        const auto body_end = fs::file_size(tmp, size_ec);
        if (size_ec || body_end < 12) throw std::runtime_error("index write failed");
        const auto checksum = hash_file_range(tmp, 12, body_end - 12);
        std::ofstream tail(tmp, std::ios::binary | std::ios::app);
        if (!tail) throw std::runtime_error("index write failed");
        put_le64(tail, checksum);
        tail.flush();
        tail.close();
        if (!tail) throw std::runtime_error("index write failed");
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
    return load_impl(file, CacheManifest{}, false);
}

Index Index::load(const fs::path& file, const CacheManifest& expected) {
    return load_impl(file, expected, true);
}

Index Index::load_impl(const fs::path& file, const CacheManifest& expected, bool manifest_aware) {
    // Truncation guard: check file size is at least header before reading.
    std::uint64_t snapshot_size = 0;
    {
        std::error_code ec;
        auto sz = fs::file_size(file, ec);
        if (ec) throw std::runtime_error("cannot stat index: " + file.string());
        snapshot_size = sz;
        constexpr uint64_t kMinHeader = 8 + 4; // magic + version
        if (sz < kMinHeader) throw std::runtime_error("pergrep index: truncated");
        if (sz > kMaxSnapshotBytes) throw std::runtime_error("pergrep index: exceeds size limit");
    }
    std::ifstream i(file, std::ios::binary);
    if (!i) throw std::runtime_error("cannot open index: " + file.string());
    char magic[8];
    i.read(magic, 8);
    if (!i || std::memcmp(magic, "PERGREP\0", 8) != 0) throw std::runtime_error("pergrep index: truncated");
    unsigned char version_bytes[4];
    i.read(reinterpret_cast<char*>(version_bytes), sizeof(version_bytes));
    if (!i) throw std::runtime_error("pergrep index: truncated");
    std::uint32_t raw_ver = 0;
    std::memcpy(&raw_ver, version_bytes, sizeof(raw_ver));
    const auto le_ver = static_cast<std::uint32_t>(version_bytes[0]) |
                        (static_cast<std::uint32_t>(version_bytes[1]) << 8) |
                        (static_cast<std::uint32_t>(version_bytes[2]) << 16) |
                        (static_cast<std::uint32_t>(version_bytes[3]) << 24);
    const bool portable = le_ver == kPortableIndexVersion;
    const auto ver = portable ? kPortableIndexVersion : raw_ver;
    if (!portable && ver != 5 && ver != 6) throw std::runtime_error("unsupported pergrep index version");
    if (portable && snapshot_size < 12 + 8) throw std::runtime_error("pergrep index: truncated");
    Reader r{i, portable, snapshot_size};
    // Verify the v7 checksum before reading any attacker-controlled lengths or
    // allocating vectors/corpus payloads. The legacy formats have no trailer.
    if (portable) {
        if (snapshot_size < 20) throw std::runtime_error("pergrep index: truncated");
        std::ifstream tail(file, std::ios::binary);
        if (!tail) throw std::runtime_error("cannot open index for integrity check: " + file.string());
        tail.seekg(static_cast<std::streamoff>(snapshot_size - 8), std::ios::beg);
        if (!tail) throw std::runtime_error("pergrep index: truncated");
        const auto expected_checksum = get_le64(tail);
        if (expected_checksum != hash_file_range(file, 12, snapshot_size - 20))
            throw std::runtime_error("pergrep index: checksum mismatch");
    }
    std::uint32_t feature_flags = 0;
    bool persisted_corpus = ver == 6;

    CacheManifest stored;
    bool has_manifest = false;
    std::uint64_t marker = r.scalar<std::uint64_t>();
    if (portable && marker != kManifestMagic) throw std::runtime_error("incompatible pergrep index schema: missing manifest");
    if (marker == kManifestMagic) {
        has_manifest = true;
        stored.schema_version = r.scalar<std::uint32_t>();
        if ((portable && stored.schema_version != kManifestSchema) || (!portable && stored.schema_version != 1))
            throw std::runtime_error("incompatible pergrep index schema: unsupported manifest schema");
        if (portable) {
            feature_flags = r.scalar<std::uint32_t>();
            if (feature_flags & ~(kFeaturePersistedCorpus | kFeatureIntegrityChecksum)) throw std::runtime_error("incompatible pergrep index schema: unknown feature section");
            if ((feature_flags & kFeatureIntegrityChecksum) == 0) throw std::runtime_error("incompatible pergrep index schema: missing integrity checksum");
            persisted_corpus = (feature_flags & kFeaturePersistedCorpus) != 0;
        }
        stored.source_identity = r.scalar<std::uint64_t>();
        stored.source_root = r.string();
        stored.selector_identity = r.scalar<std::uint64_t>();
        IndexOptions mo;
        mo.chunk_bytes = r.scalar<std::uint64_t>();
        mo.chunk_overlap = r.scalar<std::uint64_t>();
        mo.positional_block_bytes = r.scalar<std::uint64_t>();
        mo.positional_budget_ratio = r.scalar<double>();
        mo.planned_qgrams = r.scalar<std::uint64_t>();
        mo.include_hidden = r.scalar<std::uint8_t>() != 0;
        mo.follow_symlinks = r.scalar<std::uint8_t>() != 0;
        mo.persist_corpus = r.scalar<std::uint8_t>() != 0;
        if (!valid_index_options(mo))
            throw std::runtime_error("pergrep index: invalid index options");
        stored.index_options = mo;
        stored.transform_identity = r.scalar<std::uint64_t>();
        stored.corpus_files = r.scalar<std::uint64_t>();
        stored.corpus_bytes = r.scalar<std::uint64_t>();
        stored.generation = r.scalar<std::uint64_t>();

        auto mismatch = [&](bool bad, const char* what) {
            if (bad) throw std::runtime_error(std::string("stale cache manifest: ") + what);
        };
        mismatch(expected.schema_version.has_value() && *expected.schema_version != *stored.schema_version, "schema");
        if (expected.source_identity.has_value()) mismatch(*expected.source_identity != *stored.source_identity, "source identity");
        if (expected.source_root.has_value()) mismatch(*expected.source_root != *stored.source_root, "source root");
        if (expected.selector_identity.has_value()) mismatch(*expected.selector_identity != *stored.selector_identity, "selector");
        if (expected.index_options.has_value()) mismatch(!same_options(*expected.index_options, *stored.index_options), "index options");
        if (expected.transform_identity.has_value()) mismatch(*expected.transform_identity != *stored.transform_identity, "transform");
        if (expected.corpus_files.has_value()) mismatch(*expected.corpus_files != *stored.corpus_files, "corpus files");
        if (expected.corpus_bytes.has_value()) mismatch(*expected.corpus_bytes != *stored.corpus_bytes, "corpus bytes");
        if (expected.generation.has_value()) mismatch(*expected.generation != *stored.generation, "generation");

#ifdef _WIN32
        auto root_path = fs::path(std::u8string(stored.source_root->begin(), stored.source_root->end()));
#else
        auto root_path = fs::path(*stored.source_root);
#endif
        // v5 is source-backed, so metadata validation happens while only the
        // manifest is resident. v6 remains loadable after its source disappears.
        if (!persisted_corpus) {
            const auto actual_source_identity = source_identity(root_path, *stored.index_options);
            if (actual_source_identity == 0 || actual_source_identity != *stored.source_identity)
                throw std::runtime_error("stale cache manifest: source changed or disappeared");
        }
    } else {
        // Existing v5/v6 files predate the manifest. Keep their documented
        // compatibility for the unqualified API, but never use one to satisfy
        // an explicit manifest-aware request.
        if (portable) throw std::runtime_error("incompatible pergrep index schema: missing manifest");
        i.seekg(-static_cast<std::streamoff>(sizeof marker), std::ios::cur);
        if (!i) throw std::runtime_error("pergrep index: truncated");
        if (manifest_aware)
            throw std::runtime_error("cache manifest missing");
    }

    auto I = std::make_shared<Impl>();
    if (portable) persisted_corpus = (feature_flags & kFeaturePersistedCorpus) != 0;
    I->opt.persist_corpus = persisted_corpus;
#ifdef _WIN32
    auto root_str = r.string();
    I->root = fs::path(std::u8string(root_str.begin(), root_str.end()));
#else
    I->root = r.string();
#endif
    I->opt.chunk_bytes = r.scalar<std::uint64_t>();
    I->opt.chunk_overlap = r.scalar<std::uint64_t>();
    I->opt.positional_block_bytes = r.scalar<std::uint64_t>();
    I->opt.positional_budget_ratio = r.scalar<double>();
    I->opt.planned_qgrams = r.scalar<std::uint64_t>();
    I->opt.include_hidden = r.scalar<std::uint8_t>() != 0;
    I->opt.follow_symlinks = r.scalar<std::uint8_t>() != 0;
    I->corp_bytes = r.scalar<std::uint64_t>();
    I->root_mtime_ns = r.scalar<std::int64_t>();
    if (!valid_index_options(I->opt))
        throw std::runtime_error("pergrep index: invalid index options");
    if (has_manifest) {
        if (!same_options(I->opt, *stored.index_options) || I->opt.persist_corpus != persisted_corpus)
            throw std::runtime_error("stale cache manifest: index options");
        if (pergrep_cli::platform::path_to_utf8(I->root) != *stored.source_root)
            throw std::runtime_error("stale cache manifest: source root");
        if (I->corp_bytes != *stored.corpus_bytes)
            throw std::runtime_error("stale cache manifest: corpus bytes");
    }
    if (portable) {
        for (auto& x : I->byte_freq) x = r.scalar<std::uint64_t>();
        for (auto& x : I->qgram_freq) x = r.scalar<std::uint32_t>();
    } else {
        i.read(reinterpret_cast<char*>(I->byte_freq.data()), sizeof(I->byte_freq));
        i.read(reinterpret_cast<char*>(I->qgram_freq.data()), sizeof(I->qgram_freq));
        if (!i) throw std::runtime_error("pergrep index: truncated");
    }
    I->pos_block = r.scalar<std::uint32_t>();
    if (I->pos_block != I->opt.positional_block_bytes)
        throw std::runtime_error("pergrep index: positional block mismatch");
    auto nf = r.scalar<std::uint64_t>();
    if (nf > kMaxFiles) throw std::runtime_error("pergrep index: truncated");
    // Each file record needs at least a uint64 length plus fixed metadata.
    if (nf > r.remaining() / 25) throw std::runtime_error("pergrep index: truncated");
    if (has_manifest && nf != *stored.corpus_files)
        throw std::runtime_error("stale cache manifest: corpus files");
    I->infos.reserve(static_cast<size_t>(nf));
    I->loaded.reserve(static_cast<size_t>(nf));
    for (uint64_t k = 0; k < nf; ++k) {
        FileInfo f;
        f.path = r.string();
        f.size = r.scalar<std::uint64_t>();
        f.mtime_ns = r.scalar<std::int64_t>();
        f.binary = r.scalar<std::uint8_t>() != 0;
        if (unsafe_file_info_path(f.path))
            throw std::runtime_error("pergrep index: invalid FileInfo path");
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
    if (I->corp_bytes > kMaxCorpusBytes)
        throw std::runtime_error("index corpus exceeds 1 GiB limit");
    auto nc = r.scalar<std::uint64_t>();
    if (nc > kMaxChunks) throw std::runtime_error("pergrep index: truncated");
    // Each chunk record is four fixed-width fields. Check before reserve.
    if (nc > r.remaining() / 28) throw std::runtime_error("pergrep index: truncated");
    I->chunks.reserve(static_cast<size_t>(nc));
    std::vector<std::uint64_t> next_core(static_cast<size_t>(nf), 0);
    std::vector<std::uint8_t> file_seen(static_cast<size_t>(nf), 0);
    std::uint32_t previous_file = 0;
    for (uint64_t k = 0; k < nc; ++k) {
        Chunk c;
        c.file_id = r.scalar<std::uint32_t>();
        c.core_begin = r.scalar<std::uint64_t>();
        c.core_end = r.scalar<std::uint64_t>();
        c.ext_end = r.scalar<std::uint64_t>();
        if (c.file_id >= nf || c.core_begin > c.core_end || c.core_end > c.ext_end ||
            c.ext_end > I->infos[c.file_id].size ||
            c.core_end - c.core_begin > I->opt.chunk_bytes ||
            (c.core_end > UINT64_MAX - I->opt.chunk_overlap ? true :
             c.ext_end > std::min(I->infos[c.file_id].size, c.core_end + I->opt.chunk_overlap)) ||
            (k != 0 && c.file_id < previous_file) ||
            c.core_begin != next_core[c.file_id] ||
            (c.core_begin == c.core_end && I->infos[c.file_id].size != 0))
            throw std::runtime_error("pergrep index: invalid chunk geometry");
        if (k != 0 && c.file_id > previous_file && next_core[previous_file] != I->infos[previous_file].size)
            throw std::runtime_error("pergrep index: invalid chunk relationships");
        next_core[c.file_id] = c.core_end;
        file_seen[c.file_id] = 1;
        previous_file = c.file_id;
        I->chunks.push_back(c);
    }
    for (std::uint64_t fid = 0; fid < nf; ++fid)
        if (!file_seen[static_cast<size_t>(fid)] || next_core[static_cast<size_t>(fid)] != I->infos[static_cast<size_t>(fid)].size)
            throw std::runtime_error("pergrep index: invalid chunk relationships");
    std::vector<std::uint8_t> chunk_grouped(static_cast<size_t>(nc), 0);
    for (auto& g : I->groups) {
        g.lg = r.scalar<std::uint8_t>();
        g.m = r.scalar<std::uint32_t>();
        g.words = r.scalar<std::uint32_t>();
        g.gids = r.vector<std::uint32_t>();
        g.bits = r.vector<std::uint64_t>();
        if (g.lg < 9 || g.lg > 16 || g.m != (1u << g.lg) ||
            g.words != (g.gids.size() + 63) / 64 ||
            static_cast<std::uint64_t>(g.words) > UINT64_MAX / (1u << g.lg) ||
            g.bits.size() != static_cast<std::uint64_t>(1u << g.lg) * g.words)
            throw std::runtime_error("pergrep index: invalid qgram group");
        for (auto gid : g.gids) {
            if (gid >= nc || chunk_grouped[gid] != 0 ||
                detail::lg_for(static_cast<size_t>(I->chunks[gid].ext_end - I->chunks[gid].core_begin)) != g.lg)
                throw std::runtime_error("pergrep index: invalid qgram group reference");
            chunk_grouped[gid] = 1;
        }
    }
    for (auto grouped : chunk_grouped)
        if (grouped == 0) throw std::runtime_error("pergrep index: incomplete qgram groups");
    auto npd = r.scalar<std::uint64_t>();
    if (npd != nc || npd > kMaxPosDesc) throw std::runtime_error("pergrep index: invalid positional descriptors");
    if (npd > r.remaining() / 18) throw std::runtime_error("pergrep index: invalid positional descriptors");
    I->pos_desc.reserve(static_cast<size_t>(npd));
    for (uint64_t k = 0; k < npd; ++k) {
        detail::PosDesc d;
        d.off = r.scalar<std::uint64_t>();
        d.m = r.scalar<std::uint16_t>();
        d.mask_bytes = r.scalar<std::uint32_t>();
        d.blocks = r.scalar<std::uint32_t>();
        I->pos_desc.push_back(d);
    }
    I->pos = r.vector<std::uint8_t>();
    std::uint64_t pos_end = 0;
    for (std::size_t k = 0; k < I->pos_desc.size(); ++k) {
        const auto& d = I->pos_desc[k];
        const auto& c = I->chunks[k];
        const auto core_len = c.core_end - c.core_begin;
        const auto expected_blocks = std::max<std::uint64_t>(1, (core_len + I->pos_block - 1) / I->pos_block);
        const auto expected_mask_bytes = (expected_blocks + 7) / 8;
        if (d.m < 64 || d.m > 1024 || (d.m & (d.m - 1)) != 0 ||
            d.blocks != expected_blocks || d.mask_bytes != expected_mask_bytes ||
            d.off != pos_end || d.mask_bytes >
                (I->pos.size() - std::min<std::uint64_t>(pos_end, I->pos.size())) / d.m ||
            pos_end > UINT64_MAX - static_cast<std::uint64_t>(d.m) * d.mask_bytes)
            throw std::runtime_error("pergrep index: invalid positional descriptors");
        pos_end += static_cast<std::uint64_t>(d.m) * d.mask_bytes;
    }
    if (pos_end != I->pos.size()) throw std::runtime_error("pergrep index: invalid positional data");
    // QO-5/M3.5: decouple filter persistence from corpus materialization.
    // Source-backed snapshots attach immutable provider handles backed by read-only
    // mappings; a provider transparently falls back to a resident read when mapping fails.
    // v6 (persist_corpus==true): persisted corpus bytes follow the filter;
    // restore I->loaded directly from the index without touching the filesystem.
    // v7 integrity was checked before any sections were deserialized.
    if (!persisted_corpus) {
        std::uint64_t source_total = 0;
        for (const auto& f : I->infos) {
            if (f.size > kMaxCorpusBytes - source_total)
                throw std::runtime_error("indexed source corpus exceeds 1 GiB limit");
            source_total += f.size;
        }
    }
    if (persisted_corpus) {
        std::uint64_t payload_total = 0;
        for (size_t k = 0; k < I->infos.size(); ++k) {
            auto n = r.scalar<std::uint64_t>();
            if (n > kMaxV6PayloadBytes - payload_total)
                throw std::runtime_error("v6 corpus payload exceeds 1 GiB limit");
            if (n != I->infos[k].size || n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                n > UINT64_MAX - payload_total)
                throw std::runtime_error("pergrep index: invalid corpus payload");
            if (n > r.remaining()) throw std::runtime_error("pergrep index: invalid corpus payload");
            payload_total += n;
            std::string data;
            data.resize(static_cast<size_t>(n));
            if (n) {
                i.read(data.data(), static_cast<std::streamsize>(n));
                if (!i) throw std::runtime_error("pergrep index: truncated");
            }
            I->loaded.push_back({I->infos[k], detail::CorpusProvider::resident(std::move(data))});
        }
        if (payload_total != I->corp_bytes || (has_manifest && payload_total != *stored.corpus_bytes))
            throw std::runtime_error("pergrep index: invalid corpus payload totals");
        (void)0; // checksum already verified before allocation
    } else {
        (void)0; // checksum already verified before allocation
        for (auto& f : I->infos) {
#ifdef _WIN32
            const auto source_path = I->root / fs::path(std::u8string(f.path.begin(), f.path.end()));
#else
            const auto source_path = I->root / f.path;
#endif
            I->loaded.push_back({f, detail::CorpusProvider::mapped(source_path, f.size, f.mtime_ns)});
        }
    }
    if (portable) {
        const auto body_end = i.tellg();
        if (body_end < 0 || static_cast<std::uint64_t>(body_end) != snapshot_size - 8)
            throw std::runtime_error("pergrep index: unexpected trailing data");
    } else if (i.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("pergrep index: unexpected trailing data");
    }
    // Legacy v5/v6 and portable v7 do not contain planner statistics. Recompute them from the loaded
    // corpus rather than interpreting legacy qgram_freq bytes as exact stats.
    rebuild_planner_stats(*I);
    return Index(I);
}

std::string version(){return "0.1.0";}
} // namespace pergrep
