#pragma once
#include "pergrep/pergrep.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pergrep::detail {

inline std::uint32_t hash4(const unsigned char* p) noexcept {
    std::uint32_t x = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                      (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline unsigned char fold_ascii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

struct QueryDesc {
    std::vector<std::uint32_t> hashes;
    std::array<std::vector<std::pair<std::uint16_t,std::uint64_t>>,8> classes;
};
std::uint8_t lg_for(std::size_t n);
QueryDesc compile_qgram_query(std::string_view q);
struct PosDesc {
    std::uint64_t off = 0;
    std::uint16_t m = 0;
    std::uint32_t mask_bytes = 0;
    std::uint32_t blocks = 0;
};

struct Chunk {
    std::uint32_t file_id = 0;
    std::uint64_t core_begin = 0;
    std::uint64_t core_end = 0;
    std::uint64_t ext_end = 0;
};
struct LoadedFile { FileInfo info; std::string data; };

struct UnicodeProperty {
    enum class Kind : std::uint8_t { GeneralCategory, GeneralGroup, Script, Binary, Alphabetic, WhiteSpace, Word, DecimalDigit };
    Kind kind = Kind::Binary;
    std::int32_t value = 0;
    bool negated = false;
};
struct CharClassSpec {
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges;
    std::vector<UnicodeProperty> properties;
    bool negated = false;
};
struct RegexNode {
    enum class Kind { Empty, Literal, Dot, Class, Begin, End, WordBoundary, WordStartHalf, WordEndHalf, Concat, Alt, Repeat, Group, BackRef, LookAhead, LookBehind };
    Kind kind = Kind::Empty;
    std::string literal;
    CharClassSpec char_class;
    std::array<std::uint64_t,4> cls{}; // fast ASCII class path
    bool cls_neg = false;
    std::string group_name;
    bool negative = false;
    bool greedy = true;
    bool icase = false;
    bool dotall = false;
    bool multiline = false;
    bool unicode = true;
    bool crlf = false;
    std::size_t min = 0, max = 0;
    int group = 0;
    std::vector<std::shared_ptr<RegexNode>> children;
};
struct NfaInst {
    enum class Op : std::uint8_t { Rune, Any, Class, Split, Jmp, SaveStart, SaveEnd, AssertBegin, AssertEnd, AssertWord, AssertWordStartHalf, AssertWordEndHalf, Match };
    Op op = Op::Match;
    std::uint32_t rune = 0;
    std::shared_ptr<const CharClassSpec> char_class;
    std::int32_t x = -1, y = -1;
    std::int32_t group = 0;
    bool negative = false;
    bool icase = false;
    bool dotall = false;
    bool multiline = false;
    bool unicode = true;
    bool crlf = false;
};
struct RegexProgram {
    std::shared_ptr<RegexNode> ast;
    int groups = 0;
    std::vector<std::string> group_names;
    bool extended = false;
    std::vector<std::string> mandatory;
    std::vector<NfaInst> nfa;
    std::int32_t nfa_start = -1;
};
RegexProgram parse_regex(std::string_view pattern, const PatternOptions& opt);
bool regex_search(const RegexProgram&, std::string_view, const PatternOptions&, std::size_t, Match*, std::uint32_t, unsigned char);
std::vector<Match> regex_find_all(const RegexProgram&, std::string_view, const PatternOptions&, bool, std::uint32_t, std::uint64_t, std::uint64_t, unsigned char);

struct IndexData {
    struct Group {
        std::uint8_t lg = 9;
        std::uint32_t m = 512;
        std::uint32_t words = 0;
        std::vector<std::uint32_t> gids;
        std::vector<std::uint64_t> bits;
    };
    struct PosDesc {
        std::uint64_t off = 0;
        std::uint16_t m = 0;
        std::uint32_t mask_bytes = 0;
        std::uint32_t blocks = 0;
    };

    std::filesystem::path root;
    IndexOptions opt;
    std::vector<FileInfo> infos;
    std::vector<LoadedFile> loaded;
    std::vector<Chunk> chunks;
    std::array<Group,8> groups;
    std::vector<PosDesc> pos_desc;
    std::vector<std::uint8_t> pos;
    std::array<std::uint64_t,256> byte_freq{};
    std::array<std::uint32_t,65536> qgram_freq{};
    std::uint64_t corp_bytes = 0;
    std::int64_t root_mtime_ns = 0;
    std::uint32_t pos_block = 256;
    bool ephemeral = false;

    std::uint64_t bytes() const noexcept {
        std::uint64_t n = pos.size() + pos_desc.size()*sizeof(PosDesc) + chunks.size()*sizeof(Chunk);
        for (const auto& g : groups) n += g.bits.size()*sizeof(std::uint64_t) + g.gids.size()*sizeof(std::uint32_t);
        n += infos.size()*sizeof(FileInfo) + byte_freq.size()*sizeof(std::uint64_t) + qgram_freq.size()*sizeof(std::uint32_t);
        for (const auto& f : infos) n += f.path.size();
        return n;
    }
};

} // namespace pergrep::detail

namespace pergrep {
struct Pattern::Impl { std::string expr; PatternOptions opt; detail::RegexProgram re; };
struct Index::Impl : detail::IndexData {};
} // namespace pergrep
