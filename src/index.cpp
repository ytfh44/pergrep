#include "internal.hpp"
#include "platform.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <system_error>
namespace pergrep::detail {
std::uint8_t lg_for(std::size_t n){std::size_t want=std::clamp<std::size_t>(n*2,512,65536);std::uint8_t lg=9;for(std::size_t b=512;b<want&&lg<16;b<<=1)++lg;return lg;}
QueryDesc compile_qgram_query(std::string_view q){QueryDesc d;if(q.size()>=4)for(size_t i=0;i+4<=q.size();++i)d.hashes.push_back(hash4((const unsigned char*)q.data()+i));for(uint8_t lg=9;lg<=16;++lg){uint32_t mask=(1u<<lg)-1;std::vector<uint16_t>b;for(uint32_t h:d.hashes)b.push_back(h&mask);std::sort(b.begin(),b.end());b.erase(std::unique(b.begin(),b.end()),b.end());auto&v=d.classes[lg-9];for(auto x:b){uint16_t w=x>>6;uint64_t m=1ull<<(x&63);if(!v.empty()&&v.back().first==w)v.back().second|=m;else v.push_back({w,m});}}return d;}
}

namespace pergrep {
namespace fs=std::filesystem;
using detail::Chunk;using detail::LoadedFile;


static int64_t mtime_ns(const fs::path&p){std::error_code ec;auto t=fs::last_write_time(p,ec);if(ec)return 0;return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();}
static bool looks_binary(std::string_view s){auto n=std::min<size_t>(s.size(),8192);return std::memchr(s.data(),'\0',n)!=nullptr;}

Index::Index() = default;
Index::Index(std::shared_ptr<Impl> i) : impl_(std::move(i)) {}

Index Index::build(const fs::path& root, IndexOptions opt) {
    if (opt.chunk_bytes < 64) throw std::runtime_error("pergrep: chunk_bytes too small (minimum 64)");
    if (opt.positional_block_bytes < 16) throw std::runtime_error("pergrep: positional_block_bytes too small (minimum 16)");
    if (opt.planned_qgrams < 1) throw std::runtime_error("pergrep: planned_qgrams must be at least 1");
    if (opt.positional_budget_ratio < 0.0) throw std::runtime_error("pergrep: positional_budget_ratio cannot be negative");

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
        for (uint64_t b = 0; b < n; b += core) {
            uint64_t e = std::min(n, b + core), x = std::min(n, e + over);
            I->chunks.push_back({fid, b, e, x});
        }
    }

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
    if (opt.chunk_bytes < 64) throw std::runtime_error("pergrep: chunk_bytes too small (minimum 64)");
    if (opt.positional_block_bytes < 16) throw std::runtime_error("pergrep: positional_block_bytes too small (minimum 16)");
    if (opt.planned_qgrams < 1) throw std::runtime_error("pergrep: planned_qgrams must be at least 1");
    if (opt.positional_budget_ratio < 0.0) throw std::runtime_error("pergrep: positional_budget_ratio cannot be negative");

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
        for (uint64_t b = 0; b < n; b += core) {
            uint64_t e = std::min(n, b + core), x = std::min(n, e + over);
            I->chunks.push_back({fid, b, e, x});
        }
    }

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
template<class T>void put(std::ostream&o,const T&x){o.write((const char*)&x,sizeof x);if(!o)throw std::runtime_error("index write failed");}template<class T>T get(std::istream&i){T x{};i.read((char*)&x,sizeof x);if(!i)throw std::runtime_error("index read failed");return x;}void puts(std::ostream&o,std::string_view s){uint64_t n=s.size();put(o,n);o.write(s.data(),n);}std::string gets(std::istream&i){auto n=get<uint64_t>(i);std::string s(n,'\0');i.read(s.data(),n);if(!i)throw std::runtime_error("index read failed");return s;}template<class T>void putv(std::ostream&o,const std::vector<T>&v){uint64_t n=v.size();put(o,n);if(n)o.write((const char*)v.data(),sizeof(T)*n);}template<class T>std::vector<T>getv(std::istream&i){auto n=get<uint64_t>(i);std::vector<T>v(n);if(n)i.read((char*)v.data(),sizeof(T)*n);if(!i)throw std::runtime_error("index read failed");return v;}
}
void Index::save(const fs::path& file) const {
    if (!impl_) throw std::runtime_error("cannot persist an uninitialized pergrep index");
    if (impl_->ephemeral) throw std::runtime_error("cannot persist an in-memory pergrep index");
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path());
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    if (!o) throw std::runtime_error("cannot create index: " + file.string());
    o.write("PERGREP\0", 8);
    put(o, uint32_t(4));
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
    o.write(reinterpret_cast<const char*>(impl_->qgram_freq.data()), sizeof(impl_->qgram_freq));
    put(o, uint32_t(impl_->pos_block));
    uint64_t nf = impl_->infos.size();
    put(o, nf);
    for (auto& f : impl_->infos) {
        puts(o, f.path);
        put(o, uint64_t(f.size));
        put(o, int64_t(f.mtime_ns));
        put(o, uint8_t(f.binary ? 1 : 0));
    }
    putv(o, impl_->chunks);
    for (auto& g : impl_->groups) {
        put(o, uint8_t(g.lg));
        put(o, uint32_t(g.m));
        put(o, uint32_t(g.words));
        putv(o, g.gids);
        putv(o, g.bits);
    }
    putv(o, impl_->pos_desc);
    putv(o, impl_->pos);
    if (!o) throw std::runtime_error("index write failed");
}
Index Index::load(const fs::path& file) {
    std::ifstream i(file, std::ios::binary);
    if (!i) throw std::runtime_error("cannot open index: " + file.string());
    char magic[8];
    i.read(magic, 8);
    if (std::memcmp(magic, "PERGREP\0", 8)) throw std::runtime_error("not a pergrep index");
    auto ver = get<uint32_t>(i);
    if (ver != 4) throw std::runtime_error("unsupported pergrep index version");
    auto I = std::make_shared<Impl>();
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
    i.read(reinterpret_cast<char*>(I->byte_freq.data()), sizeof(I->byte_freq));
    i.read(reinterpret_cast<char*>(I->qgram_freq.data()), sizeof(I->qgram_freq));
    if (!i) throw std::runtime_error("index read failed");
    I->pos_block = get<uint32_t>(i);
    auto nf = get<uint64_t>(i);
    I->infos.reserve(nf);
    I->loaded.reserve(nf);
    for (uint64_t k = 0; k < nf; ++k) {
        FileInfo f;
        f.path = gets(i);
        f.size = get<uint64_t>(i);
        f.mtime_ns = get<int64_t>(i);
        f.binary = get<uint8_t>(i) != 0;
        I->infos.push_back(f);
#ifdef _WIN32
        std::ifstream src(I->root / fs::path(std::u8string(f.path.begin(), f.path.end())), std::ios::binary);
#else
        std::ifstream src(I->root / f.path, std::ios::binary);
#endif
        if (!src) throw std::runtime_error("indexed source disappeared: " + f.path);
        std::string data((std::istreambuf_iterator<char>(src)), {});
        I->loaded.push_back({f, std::move(data)});
    }
    I->chunks = getv<Chunk>(i);
    for (auto& g : I->groups) {
        g.lg = get<uint8_t>(i);
        g.m = get<uint32_t>(i);
        g.words = get<uint32_t>(i);
        g.gids = getv<uint32_t>(i);
        g.bits = getv<uint64_t>(i);
    }
    I->pos_desc = getv<detail::PosDesc>(i);
    I->pos = getv<uint8_t>(i);
    return Index(I);
}
std::string version(){return "0.1.0";}
} // namespace pergrep
