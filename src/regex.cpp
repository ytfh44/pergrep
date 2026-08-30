#include "internal.hpp"

#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/utf8.h>

#include <deque>
#include <unordered_set>

// QO-4 cost model integration: Regex flavour uses QueryIR branch_mandatory / mandatory
// for chunk-level pruning. The cheapest verifier is chosen by estimateCost() in
// search.cpp, which estimates selectivity via qgram_freq / byte_freq. For
// branch_mandatory patterns (e.g. "foo|bar"), the cost model picks the rarest
// literal across all branches (pick_rarest_branch_literal) for logging and
// verifies via SearchStats::verifier. This file keeps the pure extraction
// conservative (no false negatives) and documents the cost hooks.

namespace pergrep::detail {
namespace {

struct Rune { UChar32 cp = U_SENTINEL; std::size_t next = 0; bool ok = false; };
Rune rune_at(std::string_view s, std::size_t pos) {
    if (pos >= s.size()) return {};
    int32_t i = static_cast<int32_t>(pos), n = static_cast<int32_t>(s.size());
    UChar32 cp; U8_NEXT(s.data(), i, n, cp);
    if (cp < 0) return {static_cast<unsigned char>(s[pos]), pos + 1, true};
    return {cp, static_cast<std::size_t>(i), true};
}
Rune rune_before(std::string_view s, std::size_t pos) {
    if (!pos) return {};
    int32_t i = static_cast<int32_t>(pos); UChar32 cp; U8_PREV(s.data(), 0, i, cp);
    if (cp < 0) return {static_cast<unsigned char>(s[pos - 1]), pos - 1, true};
    return {cp, static_cast<std::size_t>(i), true};
}
UChar32 fold(UChar32 cp) { return u_foldCase(cp, U_FOLD_CASE_DEFAULT); }
bool cp_eq(UChar32 a, UChar32 b, bool icase) { return icase ? fold(a) == fold(b) : a == b; }
bool unicode_word(UChar32 cp) {
    return u_isalnum(cp) ||
           u_charType(cp) == U_CONNECTOR_PUNCTUATION ||
           u_hasBinaryProperty(cp, UCHAR_JOIN_CONTROL) ||
           u_charType(cp) == U_NON_SPACING_MARK ||
           u_charType(cp) == U_COMBINING_SPACING_MARK ||
           u_charType(cp) == U_ENCLOSING_MARK;
}
bool ascii_word(unsigned char c) { return std::isalnum(c) || c == '_'; }

UnicodeProperty property_from_name(std::string name, bool negated) {
    UnicodeProperty p; p.negated = negated;
    // Trim leading/trailing whitespace from name for key=value handling
    auto trim = [](std::string s){
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if(a==std::string::npos) return std::string{};
        return s.substr(a, b-a+1);
    };
    name = trim(name);
    auto eq_pos = name.find_first_of("=:");
    if (eq_pos != std::string::npos) {
        std::string raw_key = trim(name.substr(0, eq_pos));
        std::string raw_val = trim(name.substr(eq_pos + 1));
        std::string k_norm, v_norm;
        for (char c : raw_key) if (c != '_' && c != '-' && c != ' ') k_norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char c : raw_val) if (c != '_' && c != '-' && c != ' ') v_norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (k_norm == "sc" || k_norm == "script" || k_norm == "scx" || k_norm == "scriptextensions") {
            UErrorCode ec = U_ZERO_ERROR; UScriptCode codes[8]; int32_t count = uscript_getCode(raw_val.c_str(), codes, 8, &ec);
            if (U_SUCCESS(ec) && count > 0) { p.kind = UnicodeProperty::Kind::Script; p.value = static_cast<std::int32_t>(codes[0]); return p; }
            throw std::runtime_error("pergrep regex: unknown Unicode script: " + raw_val);
        }
        if (k_norm == "gc" || k_norm == "generalcategory" || k_norm == "category") {
            return property_from_name(raw_val, negated);
        }
        name = raw_val;
    }
    std::string norm;
    for (char c : name) {
        if (c != '_' && c != '-' && c != ' ') {
            norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (norm == "any") { p.kind = UnicodeProperty::Kind::Binary; p.value = -1; return p; }
    if (norm == "ascii") { p.kind = UnicodeProperty::Kind::Binary; p.value = -2; return p; }
    if (norm == "assigned") { p.kind = UnicodeProperty::Kind::Binary; p.value = -3; return p; }
    if (norm == "word") { p.kind = UnicodeProperty::Kind::Word; return p; }

    if (norm == "l" || norm == "letter") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_L_MASK; return p; }
    if (norm == "m" || norm == "mark" || norm == "combiningmark") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_M_MASK; return p; }
    if (norm == "n" || norm == "number") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_N_MASK; return p; }
    if (norm == "p" || norm == "punctuation" || norm == "punct") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_P_MASK; return p; }
    if (norm == "s" || norm == "symbol") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_S_MASK; return p; }
    if (norm == "z" || norm == "separator") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_Z_MASK; return p; }
    if (norm == "c" || norm == "other") { p.kind = UnicodeProperty::Kind::GeneralGroup; p.value = U_GC_C_MASK; return p; }

    if (norm == "lu" || norm == "uppercaseletter") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_UPPERCASE_LETTER; return p; }
    if (norm == "ll" || norm == "lowercaseletter") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_LOWERCASE_LETTER; return p; }
    if (norm == "lt" || norm == "titlecaseletter") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_TITLECASE_LETTER; return p; }
    if (norm == "lm" || norm == "modifierletter") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_MODIFIER_LETTER; return p; }
    if (norm == "lo" || norm == "otherletter") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_OTHER_LETTER; return p; }
    if (norm == "mn" || norm == "nonspacingmark") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_NON_SPACING_MARK; return p; }
    if (norm == "mc" || norm == "spacingmark" || norm == "combiningspacingmark") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_COMBINING_SPACING_MARK; return p; }
    if (norm == "me" || norm == "enclosingmark") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_ENCLOSING_MARK; return p; }
    if (norm == "nd" || norm == "decimalnumber" || norm == "digit" || norm == "decimaldigit") { p.kind = UnicodeProperty::Kind::DecimalDigit; return p; }
    if (norm == "nl" || norm == "letternumber") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_LETTER_NUMBER; return p; }
    if (norm == "no" || norm == "othernumber") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_OTHER_NUMBER; return p; }
    if (norm == "pc" || norm == "connectorpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_CONNECTOR_PUNCTUATION; return p; }
    if (norm == "pd" || norm == "dashpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_DASH_PUNCTUATION; return p; }
    if (norm == "ps" || norm == "openpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_START_PUNCTUATION; return p; }
    if (norm == "pe" || norm == "closepunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_END_PUNCTUATION; return p; }
    if (norm == "pi" || norm == "initialpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_INITIAL_PUNCTUATION; return p; }
    if (norm == "pf" || norm == "finalpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_FINAL_PUNCTUATION; return p; }
    if (norm == "po" || norm == "otherpunctuation") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_OTHER_PUNCTUATION; return p; }
    if (norm == "sm" || norm == "mathsymbol") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_MATH_SYMBOL; return p; }
    if (norm == "sc" || norm == "currencysymbol") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_CURRENCY_SYMBOL; return p; }
    if (norm == "sk" || norm == "modifiersymbol") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_MODIFIER_SYMBOL; return p; }
    if (norm == "so" || norm == "othersymbol") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_OTHER_SYMBOL; return p; }
    if (norm == "zs" || norm == "spaceseparator") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_SPACE_SEPARATOR; return p; }
    if (norm == "zl" || norm == "lineseparator") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_LINE_SEPARATOR; return p; }
    if (norm == "zp" || norm == "paragraphseparator") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_PARAGRAPH_SEPARATOR; return p; }
    if (norm == "cc" || norm == "control" || norm == "cntrl") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_CONTROL_CHAR; return p; }
    if (norm == "cf" || norm == "format") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_FORMAT_CHAR; return p; }
    if (norm == "cs" || norm == "surrogate") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_SURROGATE; return p; }
    if (norm == "co" || norm == "privateuse") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_PRIVATE_USE_CHAR; return p; }
    if (norm == "cn" || norm == "unassigned") { p.kind = UnicodeProperty::Kind::GeneralCategory; p.value = U_UNASSIGNED; return p; }

    if (norm == "alphabetic" || norm == "alpha") { p.kind = UnicodeProperty::Kind::Alphabetic; return p; }
    if (norm == "whitespace" || norm == "space") { p.kind = UnicodeProperty::Kind::WhiteSpace; return p; }
    if (norm == "asciihexdigit" || norm == "ahex") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_ASCII_HEX_DIGIT; return p; }
    if (norm == "hexdigit" || norm == "hex") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_HEX_DIGIT; return p; }
    if (norm == "bidicontrol") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_BIDI_CONTROL; return p; }
    if (norm == "cased") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CASED; return p; }
    if (norm == "caseignorable") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CASE_IGNORABLE; return p; }
    if (norm == "changeswhencasefolded") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CHANGES_WHEN_CASEFOLDED; return p; }
    if (norm == "changeswhencasemapped") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CHANGES_WHEN_CASEMAPPED; return p; }
    if (norm == "changeswhenlowercased") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CHANGES_WHEN_LOWERCASED; return p; }
    if (norm == "changeswhentitlecased") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CHANGES_WHEN_TITLECASED; return p; }
    if (norm == "changeswhenuppercased") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_CHANGES_WHEN_UPPERCASED; return p; }
    if (norm == "defaultignorablecodepoint") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_DEFAULT_IGNORABLE_CODE_POINT; return p; }
    if (norm == "deprecated") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_DEPRECATED; return p; }
    if (norm == "diacritic") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_DIACRITIC; return p; }
    if (norm == "extender") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_EXTENDER; return p; }
    if (norm == "graphemebase") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_GRAPHEME_BASE; return p; }
    if (norm == "graphemeextend") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_GRAPHEME_EXTEND; return p; }
    if (norm == "idcontinue" || norm == "idc") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_ID_CONTINUE; return p; }
    if (norm == "idstart" || norm == "ids") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_ID_START; return p; }
    if (norm == "ideographic" || norm == "ideo") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_IDEOGRAPHIC; return p; }
    if (norm == "joincontrol") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_JOIN_CONTROL; return p; }
    if (norm == "logicalorderexception") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_LOGICAL_ORDER_EXCEPTION; return p; }
    if (norm == "lowercase" || norm == "lower") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_LOWERCASE; return p; }
    if (norm == "math") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_MATH; return p; }
    if (norm == "noncharactercodepoint") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_NONCHARACTER_CODE_POINT; return p; }
    if (norm == "quotationmark" || norm == "qmark") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_QUOTATION_MARK; return p; }
    if (norm == "radical") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_RADICAL; return p; }
    if (norm == "softdotted") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_SOFT_DOTTED; return p; }
    if (norm == "terminalpunctuation") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_TERMINAL_PUNCTUATION; return p; }
    if (norm == "unifiedideograph") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_UNIFIED_IDEOGRAPH; return p; }
    if (norm == "uppercase" || norm == "upper") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_UPPERCASE; return p; }
    if (norm == "xidcontinue" || norm == "xidc") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_XID_CONTINUE; return p; }
    if (norm == "xidstart" || norm == "xids") { p.kind = UnicodeProperty::Kind::Binary; p.value = UCHAR_XID_START; return p; }

    UErrorCode ec = U_ZERO_ERROR; UScriptCode codes[8]; int32_t count = uscript_getCode(name.c_str(), codes, 8, &ec);
    if (U_SUCCESS(ec) && count > 0) { p.kind = UnicodeProperty::Kind::Script; p.value = static_cast<std::int32_t>(codes[0]); return p; }
    throw std::runtime_error("pergrep regex: unknown Unicode property: " + name);
}
bool property_match(const UnicodeProperty& p, UChar32 cp) {
    bool hit = false;
    switch (p.kind) {
        case UnicodeProperty::Kind::Binary:
            if (p.value == -1) hit = true;
            else if (p.value == -2) hit = (cp >= 0 && cp <= 127);
            else if (p.value == -3) hit = (u_charType(cp) != U_UNASSIGNED);
            else hit = u_hasBinaryProperty(cp, static_cast<UProperty>(p.value));
            break;
        case UnicodeProperty::Kind::Alphabetic: hit = u_hasBinaryProperty(cp, UCHAR_ALPHABETIC); break;
        case UnicodeProperty::Kind::WhiteSpace: hit = u_isUWhiteSpace(cp); break;
        case UnicodeProperty::Kind::Word: hit = unicode_word(cp); break;
        case UnicodeProperty::Kind::DecimalDigit: hit = u_charType(cp) == U_DECIMAL_DIGIT_NUMBER; break;
        case UnicodeProperty::Kind::AsciiDigit: hit = (cp >= '0' && cp <= '9'); break;
        case UnicodeProperty::Kind::AsciiWord: hit = (cp < 128 && ascii_word(static_cast<unsigned char>(cp))); break;
        case UnicodeProperty::Kind::AsciiSpace: hit = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v'); break;
        case UnicodeProperty::Kind::GeneralGroup: hit = (U_GET_GC_MASK(cp) & p.value) != 0; break;
        case UnicodeProperty::Kind::GeneralCategory: hit = u_charType(cp) == p.value; break;
        case UnicodeProperty::Kind::Script: { UErrorCode ec = U_ZERO_ERROR; hit = uscript_getScript(cp, &ec) == p.value && U_SUCCESS(ec); break; }
    }
    return p.negated ? !hit : hit;
}
bool class_match(const CharClassSpec& cls, UChar32 cp, bool icase) {
    auto match_raw = [&](UChar32 x) {
        for (auto [a,b] : cls.ranges) {
            if (x >= static_cast<UChar32>(a) && x <= static_cast<UChar32>(b)) return true;
            if (icase) {
                UChar32 fa = fold(static_cast<UChar32>(a)), fb = fold(static_cast<UChar32>(b));
                UChar32 fx = fold(x);
                if (fa <= fb && fx >= fa && fx <= fb) return true;
                UChar32 la = u_tolower(static_cast<UChar32>(a)), lb = u_tolower(static_cast<UChar32>(b));
                UChar32 lx = u_tolower(x);
                if (la <= lb && lx >= la && lx <= lb) return true;
                UChar32 ua = u_toupper(static_cast<UChar32>(a)), ub = u_toupper(static_cast<UChar32>(b));
                UChar32 ux = u_toupper(x);
                if (ua <= ub && ux >= ua && ux <= ub) return true;
            }
        }
        for (const auto& p : cls.properties) {
            if (property_match(p, x)) return true;
        }
        return false;
    };
    bool hit = match_raw(cp);
    if (icase && !hit) {
        UChar32 variants[] = { fold(cp), u_tolower(cp), u_toupper(cp), u_totitle(cp) };
        for (UChar32 v : variants) {
            if (v != cp && match_raw(v)) { hit = true; break; }
        }
    }
    return cls.negated ? !hit : hit;
}

class Parser {
public:
    Parser(std::string_view s, PatternOptions o): s_(s), opt_(o) {}
    RegexProgram parse() {
        auto n = alt(); if (i_ != s_.size()) fail("unexpected trailing input");
        RegexProgram p; p.ast = std::move(n); p.groups = groups_; p.extended = extended_; p.group_names = group_names_;
        p.install_query_ir(analyze_query(p.ast, p.extended));
        return p;
    }
private:
    std::string_view s_; PatternOptions opt_; std::size_t i_ = 0; int groups_ = 0; bool extended_ = false; std::vector<std::string> group_names_{""};
    bool ignore_ws_ = false; bool swap_greed_ = false;
    [[noreturn]] void fail(const std::string& m) const { throw std::runtime_error("pergrep regex: " + m + " at byte " + std::to_string(i_)); }
    bool eat(char c) { if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
    char get() { if (i_ >= s_.size()) fail("unexpected end of pattern"); return s_[i_++]; }
    void skip_ignored() {
        if (!ignore_ws_) return;
        for (;;) {
            while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
            if (i_ < s_.size() && s_[i_] == '#') { while (i_ < s_.size() && s_[i_] != '\n') ++i_; continue; }
            break;
        }
    }
    std::shared_ptr<RegexNode> node(RegexNode::Kind k) { auto n = std::make_shared<RegexNode>(); n->kind = k; n->icase = opt_.case_mode == CaseMode::Insensitive; n->dotall = opt_.dotall; n->multiline = opt_.multiline; n->unicode = opt_.unicode; n->crlf = opt_.crlf; return n; }
    std::shared_ptr<RegexNode> alt() {
        std::vector<std::shared_ptr<RegexNode>> xs{concat()}; while (eat('|')) xs.push_back(concat());
        if (xs.size() == 1) return xs.front();
        auto n = node(RegexNode::Kind::Alt); n->children = std::move(xs); return n;
    }
    std::shared_ptr<RegexNode> concat() {
        std::vector<std::shared_ptr<RegexNode>> xs;
        for (;;) { skip_ignored(); if (i_ >= s_.size() || s_[i_] == ')' || s_[i_] == '|') break; xs.push_back(repeat()); }
        if (xs.empty()) return node(RegexNode::Kind::Empty);
        std::vector<std::shared_ptr<RegexNode>> ys;
        for (auto& x : xs) { if (!ys.empty() && ys.back()->kind == RegexNode::Kind::Literal && x->kind == RegexNode::Kind::Literal && ys.back()->icase == x->icase) ys.back()->literal += x->literal; else ys.push_back(x); }
        if (ys.size() == 1) return ys.front();
        auto n = node(RegexNode::Kind::Concat); n->children = std::move(ys); return n;
    }
    std::size_t number() { if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) fail("expected number"); std::size_t x=0; while(i_<s_.size()&&std::isdigit(static_cast<unsigned char>(s_[i_]))){ auto d=s_[i_++]-'0'; if(x>(SIZE_MAX-d)/10) fail("repetition overflow"); x=x*10+d; } return x; }
    std::shared_ptr<RegexNode> repeat() {
        auto a = atom(); if (i_ >= s_.size()) return a; std::size_t mn=0,mx=0; bool rep=false;
        if(eat('*')){mn=0;mx=SIZE_MAX;rep=true;} else if(eat('+')){mn=1;mx=SIZE_MAX;rep=true;} else if(eat('?')){mn=0;mx=1;rep=true;} else if(eat('{')){rep=true;mn=number();mx=mn;if(eat(',')){if(i_<s_.size()&&s_[i_]!='}')mx=number();else mx=SIZE_MAX;}if(!eat('}'))fail("unclosed repetition");if(mx!=SIZE_MAX&&mx<mn)fail("invalid repetition range");}
        if(!rep) return a;
        auto n=node(RegexNode::Kind::Repeat);n->children={a};n->min=mn;n->max=mx;n->greedy=!eat('?');if(swap_greed_)n->greedy=!n->greedy;return n;
    }
    UChar32 pattern_rune() {
        if(i_>=s_.size()) fail("unexpected end of pattern");
        int32_t j=static_cast<int32_t>(i_), n=static_cast<int32_t>(s_.size()); UChar32 cp; U8_NEXT(s_.data(),j,n,cp); if(cp<0){ cp=static_cast<unsigned char>(s_[i_]); ++i_; } else i_=static_cast<std::size_t>(j); return cp;
    }
    static void append_utf8(std::string& out, UChar32 cp) { char buf[U8_MAX_LENGTH]; int32_t i=0; U8_APPEND_UNSAFE(buf,i,cp); out.append(buf,buf+i); }
    std::shared_ptr<RegexNode> atom() {
        if(eat('^')) return node(RegexNode::Kind::Begin);
        if(eat('$')) return node(RegexNode::Kind::End);
        if(eat('.')) return node(RegexNode::Kind::Dot);
        if(eat('[')) return char_class();
        if(eat('(')) {
            std::string name;
            if(eat('?')) {
                if(eat('P')) { if(!eat('<')) fail("expected < after ?P"); while(i_<s_.size()&&s_[i_]!='>'){ char c=s_[i_++]; if(!(std::isalnum(static_cast<unsigned char>(c))||c=='_'))fail("bad capture name"); name.push_back(c); } if(name.empty()||!eat('>'))fail("bad named capture"); }
                else if(eat(':')) { auto x=alt(); if(!eat(')'))fail("unclosed group"); return x; }
                else if(eat('=')){extended_=true;auto n=node(RegexNode::Kind::LookAhead);n->children={alt()};if(!eat(')'))fail("unclosed lookahead");return n;}
                else if(eat('!')){extended_=true;auto n=node(RegexNode::Kind::LookAhead);n->negative=true;n->children={alt()};if(!eat(')'))fail("unclosed lookahead");return n;}
                else if(eat('<')) {
                    if(eat('=')){extended_=true;auto n=node(RegexNode::Kind::LookBehind);n->children={alt()};if(!eat(')'))fail("unclosed lookbehind");return n;}
                    if(eat('!')){extended_=true;auto n=node(RegexNode::Kind::LookBehind);n->negative=true;n->children={alt()};if(!eat(')'))fail("unclosed lookbehind");return n;}
                    while(i_<s_.size()&&s_[i_]!='>'){ char c=s_[i_++]; if(!(std::isalnum(static_cast<unsigned char>(c))||c=='_'))fail("bad capture name"); name.push_back(c); }
                    if(name.empty()||!eat('>'))fail("bad named capture");
                } else {
                    auto saved=opt_; bool saved_x=ignore_ws_, saved_U=swap_greed_; bool seen=false,unset=false;
                    while(i_<s_.size()){char f=s_[i_];if(f=='-'){unset=true;++i_;continue;}if(f==':'||f==')')break;if(f!='i'&&f!='m'&&f!='s'&&f!='U'&&f!='x'&&f!='u')fail("unsupported inline flag");seen=true;++i_;bool val=!unset;if(f=='i')opt_.case_mode=val?CaseMode::Insensitive:CaseMode::Sensitive;else if(f=='m')opt_.multiline=val;else if(f=='s')opt_.dotall=val;else if(f=='x')ignore_ws_=val;else if(f=='U')swap_greed_=val;else if(f=='u')opt_.unicode=val;}
                    if(!seen)fail("unknown group extension");
                    if(eat(')'))return node(RegexNode::Kind::Empty);
                    if(eat(':')){auto x=alt();skip_ignored();if(!eat(')'))fail("unclosed flagged group");opt_=saved;ignore_ws_=saved_x;swap_greed_=saved_U;return x;}
                    opt_=saved;ignore_ws_=saved_x;swap_greed_=saved_U;fail("malformed inline flags");
                }
            }
            int g=++groups_; if(group_names_.size()<=static_cast<std::size_t>(g))group_names_.resize(g+1);group_names_[g]=name; auto n=node(RegexNode::Kind::Group);n->group=g;n->group_name=name;n->children={alt()};if(!eat(')'))fail("unclosed group");return n;
        }
        if(eat('\\')) return escaped();
        UChar32 cp=pattern_rune(); auto n=node(RegexNode::Kind::Literal);append_utf8(n->literal,cp);return n;
    }
    std::shared_ptr<RegexNode> escaped() {
        char c=get();
        if(c>='1'&&c<='9'){extended_=true;auto n=node(RegexNode::Kind::BackRef);n->group=c-'0';return n;}
        if(c=='b') return node(RegexNode::Kind::WordBoundary);
        if(c=='B'){auto n=node(RegexNode::Kind::WordBoundary);n->negative=true;return n;}
        if(c=='A') return node(RegexNode::Kind::AbsBegin);
        if(c=='z') return node(RegexNode::Kind::AbsEnd);
        if(c=='Z') return node(RegexNode::Kind::EndNewline);
        if(c=='p'||c=='P'){ std::string name; if(eat('{')){while(i_<s_.size()&&s_[i_]!='}')name.push_back(s_[i_++]);if(!eat('}'))fail("unclosed Unicode property");}else{if(i_>=s_.size())fail("missing Unicode property");name.push_back(get());} auto n=node(RegexNode::Kind::Class);n->char_class.properties.push_back(property_from_name(name,c=='P'));return n; }
        if(c=='d'||c=='D'||c=='w'||c=='W'||c=='s'||c=='S'){
            auto n=node(RegexNode::Kind::Class);
            bool neg=(c=='D'||c=='W'||c=='S');
            char lc=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            UnicodeProperty p;
            if(!opt_.unicode){
                if(lc=='d')p.kind=UnicodeProperty::Kind::AsciiDigit;
                else if(lc=='w')p.kind=UnicodeProperty::Kind::AsciiWord;
                else p.kind=UnicodeProperty::Kind::AsciiSpace;
            } else {
                if(lc=='d')p.kind=UnicodeProperty::Kind::DecimalDigit;
                else if(lc=='w')p.kind=UnicodeProperty::Kind::Word;
                else p.kind=UnicodeProperty::Kind::WhiteSpace;
            }
            p.negated=neg;
            n->char_class.properties.push_back(p);
            return n;
        }
        if (c == 'n') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\n'); return n; }
        if (c == 'r') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\r'); return n; }
        if (c == 't') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\t'); return n; }
        if (c == 'f') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\f'); return n; }
        if (c == 'v') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\v'); return n; }
        if (c == 'a') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\a'); return n; }
        if (c == 'e') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back(27); return n; }
        if (c == '0') { auto n = node(RegexNode::Kind::Literal); n->literal.push_back('\0'); return n; }
        if (c == 'x') {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            if (i_ + 2 > s_.size()) fail("short hex escape");
            int a = hex(s_[i_++]), b = hex(s_[i_++]);
            if (a < 0 || b < 0) fail("bad hex escape");
            auto n = node(RegexNode::Kind::Literal);
            n->literal.push_back(static_cast<char>((a << 4) | b));
            return n;
        }
        if (std::isalnum(static_cast<unsigned char>(c))) {
            fail("unrecognized escape sequence: \\" + std::string(1, c));
        }
        auto n = node(RegexNode::Kind::Literal);
        n->literal.push_back(c);
        return n;
    }
    UChar32 class_rune() {
        if(eat('\\')){
            char e=get();
            if(e=='n')return '\n';
            if(e=='r')return '\r';
            if(e=='t')return '\t';
            if(e=='f')return '\f';
            if(e=='v')return '\v';
            if(e=='a')return '\a';
            if(e=='e')return 27;
            if(e=='x'){
                auto hex=[](char h)->int{if(h>='0'&&h<='9')return h-'0';if(h>='a'&&h<='f')return h-'a'+10;if(h>='A'&&h<='F')return h-'A'+10;return -1;};
                if(i_+2>s_.size())fail("short hex escape");
                int a=hex(s_[i_++]),b=hex(s_[i_++]);
                if(a<0||b<0)fail("bad hex escape");
                return static_cast<unsigned char>((a<<4)|b);
            }
            if (std::isalnum(static_cast<unsigned char>(e))) {
                fail("unrecognized escape sequence inside character class: \\" + std::string(1, e));
            }
            return static_cast<unsigned char>(e);
        }
        return pattern_rune();
    }
    std::shared_ptr<RegexNode> char_class() {
        auto n=node(RegexNode::Kind::Class);if(eat('^'))n->char_class.negated=true;bool first=true;std::optional<UChar32> prev;
        while(i_<s_.size()){
            if(s_[i_]==']'&&!first){++i_;return n;}first=false;
            if(s_[i_]=='\\'&&i_+1<s_.size()){
                char e=s_[i_+1];
                if(e=='p'||e=='P'){i_+=2;std::string name;if(eat('{')){while(i_<s_.size()&&s_[i_]!='}')name.push_back(s_[i_++]);if(!eat('}'))fail("unclosed Unicode property");}else{if(i_>=s_.size())fail("missing Unicode property");name.push_back(get());}n->char_class.properties.push_back(property_from_name(name,e=='P'));prev.reset();continue;}
                if(e=='d'||e=='D'||e=='w'||e=='W'||e=='s'||e=='S'){
                    i_+=2;UnicodeProperty p;char lc=static_cast<char>(std::tolower(static_cast<unsigned char>(e)));
                    if(!opt_.unicode){
                        if(lc=='d')p.kind=UnicodeProperty::Kind::AsciiDigit;
                        else if(lc=='w')p.kind=UnicodeProperty::Kind::AsciiWord;
                        else p.kind=UnicodeProperty::Kind::AsciiSpace;
                    } else {
                        if(lc=='d')p.kind=UnicodeProperty::Kind::DecimalDigit;
                        else if(lc=='w')p.kind=UnicodeProperty::Kind::Word;
                        else p.kind=UnicodeProperty::Kind::WhiteSpace;
                    }
                    p.negated=(e=='D'||e=='W'||e=='S');n->char_class.properties.push_back(p);prev.reset();continue;
                }
            }
            if(s_[i_]=='-'&&prev&&i_+1<s_.size()&&s_[i_+1]!=']'){++i_;UChar32 end=class_rune();if(end<*prev)fail("descending class range");n->char_class.ranges.back().second=static_cast<uint32_t>(end);prev.reset();continue;}
            UChar32 cp=class_rune();
            n->char_class.ranges.push_back({static_cast<uint32_t>(cp),static_cast<uint32_t>(cp)});prev=cp;
        }
        fail("unclosed character class");
    }
};


struct PatchRef { int pc=-1; bool y=false; };
struct Frag { int start=-1; std::vector<PatchRef> out; };

class NfaCompiler {
public:
    explicit NfaCompiler(RegexProgram& p):p_(p){}
    void compile(const std::shared_ptr<RegexNode>& root){
        auto f=build(root);NfaInst m;m.op=NfaInst::Op::Match;int mi=emit(std::move(m));patch(f.out,mi);p_.nfa_start=f.start;
    }
private:
    RegexProgram& p_;
    int emit(NfaInst i){if(p_.nfa.size()>=1000000)throw std::runtime_error("pergrep regex: compiled program too large");p_.nfa.push_back(std::move(i));return static_cast<int>(p_.nfa.size()-1);}
    void set(PatchRef r,int target){if(r.y)p_.nfa[r.pc].y=target;else p_.nfa[r.pc].x=target;}
    void patch(const std::vector<PatchRef>& v,int target){for(auto r:v)set(r,target);}
    static std::vector<PatchRef> append(std::vector<PatchRef> a,const std::vector<PatchRef>&b){a.insert(a.end(),b.begin(),b.end());return a;}
    Frag epsilon(){NfaInst i;i.op=NfaInst::Op::Jmp;int pc=emit(std::move(i));return{pc,{{pc,false}}};}
    Frag concat(Frag a,Frag b){patch(a.out,b.start);return{a.start,std::move(b.out)};}
    NfaInst base(const RegexNode&n,NfaInst::Op op){NfaInst i;i.op=op;i.icase=n.icase;i.dotall=n.dotall;i.multiline=n.multiline;i.unicode=n.unicode;i.crlf=n.crlf;return i;}
    Frag one(NfaInst i){int pc=emit(std::move(i));return{pc,{{pc,false}}};}
    Frag build_literal(const RegexNode&n){
        Frag all=epsilon();bool any=false;std::size_t p=0;
        while(p<n.literal.size()){auto r=rune_at(n.literal,p);if(!r.ok)break;NfaInst i=base(n,NfaInst::Op::Rune);i.rune=static_cast<std::uint32_t>(r.cp);auto f=one(std::move(i));all=any?concat(std::move(all),std::move(f)):std::move(f);any=true;p=r.next;}
        return any?all:epsilon();
    }
    Frag build_repeat(const std::shared_ptr<RegexNode>&n){
        const auto&child=n->children[0];Frag res=epsilon();
        for(std::size_t k=0;k<n->min;++k)res=concat(std::move(res),build(child));
        if(n->max==n->min)return res;
        if(n->max==SIZE_MAX){
            auto c=build(child);NfaInst sp=base(*n,NfaInst::Op::Split);int pc=emit(std::move(sp));
            if(n->greedy){p_.nfa[pc].x=c.start;patch(c.out,pc);patch(res.out,pc);return{res.start,{{pc,true}}};}
            p_.nfa[pc].y=c.start;patch(c.out,pc);patch(res.out,pc);return{res.start,{{pc,false}}};
        }
        std::vector<PatchRef> all_exits;
        for(std::size_t k=n->min;k<n->max;++k){
            auto c=build(child);NfaInst sp=base(*n,NfaInst::Op::Split);int pc=emit(std::move(sp));
            if(n->greedy){
                p_.nfa[pc].x=c.start;
                all_exits.push_back({pc,true});
            } else {
                p_.nfa[pc].y=c.start;
                all_exits.push_back({pc,false});
            }
            patch(res.out,pc);
            res.out=std::move(c.out);
        }
        all_exits.insert(all_exits.end(),res.out.begin(),res.out.end());
        return{res.start,std::move(all_exits)};
    }
    Frag build(const std::shared_ptr<RegexNode>&n){
        using K=RegexNode::Kind;
        switch(n->kind){
            case K::Empty:return epsilon();
            case K::Literal:return build_literal(*n);
            case K::Dot:return one(base(*n,NfaInst::Op::Any));
            case K::Class:{auto i=base(*n,NfaInst::Op::Class);i.char_class=std::make_shared<CharClassSpec>(n->char_class);return one(std::move(i));}
            case K::Begin:return one(base(*n,NfaInst::Op::AssertBegin));
            case K::End:return one(base(*n,NfaInst::Op::AssertEnd));
            case K::AbsBegin:return one(base(*n,NfaInst::Op::AssertAbsBegin));
            case K::AbsEnd:return one(base(*n,NfaInst::Op::AssertAbsEnd));
            case K::EndNewline:return one(base(*n,NfaInst::Op::AssertEndNewline));
            case K::WordBoundary:{auto i=base(*n,NfaInst::Op::AssertWord);i.negative=n->negative;return one(std::move(i));}
            case K::WordStartHalf:return one(base(*n,NfaInst::Op::AssertWordStartHalf));
            case K::WordEndHalf:return one(base(*n,NfaInst::Op::AssertWordEndHalf));
            case K::Group:{
                auto c=build(n->children[0]);auto s=base(*n,NfaInst::Op::SaveStart);s.group=n->group;int a=emit(std::move(s));p_.nfa[a].x=c.start;
                auto e=base(*n,NfaInst::Op::SaveEnd);e.group=n->group;int b=emit(std::move(e));patch(c.out,b);return{a,{{b,false}}};
            }
            case K::Concat:{Frag r=epsilon();for(auto&c:n->children)r=concat(std::move(r),build(c));return r;}
            case K::Alt:{
                if(n->children.empty()) return epsilon();
                auto r=build(n->children[0]);
                for(std::size_t k=1;k<n->children.size();++k){auto b=build(n->children[k]);NfaInst sp=base(*n,NfaInst::Op::Split);sp.x=r.start;sp.y=b.start;int pc=emit(std::move(sp));r={pc,append(std::move(r.out),b.out)};}return r;
            }
            case K::Repeat:return build_repeat(n);
            case K::BackRef:case K::LookAhead:case K::LookBehind:throw std::runtime_error("pergrep regex: non-regular construct in default NFA");
        }
        return epsilon();
    }
};

struct NfaThread { int pc=-1; std::size_t start=0; std::vector<std::pair<std::size_t,std::size_t>> caps; };

bool assert_begin(const NfaInst&i,std::string_view t,std::size_t pos,unsigned char sep){return pos==0||(i.multiline&&pos>0&&static_cast<unsigned char>(t[pos-1])==sep);}
bool assert_end(const NfaInst&i,std::string_view t,std::size_t pos,unsigned char sep){
    if(i.multiline){
        bool ok=pos==t.size()||(pos<t.size()&&static_cast<unsigned char>(t[pos])==sep);
        if(!ok&&i.crlf&&sep=='\n'&&pos+1<t.size()&&t[pos]=='\r'&&pos+1<t.size()&&t[pos+1]=='\n') ok=true;
        return ok;
    }
    bool ok=pos==t.size()||(pos+1==t.size()&&static_cast<unsigned char>(t[pos])==sep);
    if(!ok&&i.crlf&&sep=='\n'&&pos+2==t.size()&&t[pos]=='\r'&&pos+1<t.size()&&t[pos+1]=='\n') ok=true;
    return ok;
}
bool assert_abs_begin(const NfaInst&,std::string_view,std::size_t pos){return pos==0;}
bool assert_abs_end(const NfaInst&,std::string_view t,std::size_t pos){return pos==t.size();}
bool assert_end_newline(const NfaInst&i,std::string_view t,std::size_t pos,unsigned char sep){
    bool ok=pos==t.size()||(pos+1==t.size()&&static_cast<unsigned char>(t[pos])==sep);
    if(!ok&&i.crlf&&sep=='\n'&&pos+2==t.size()&&t[pos]=='\r'&&pos+1<t.size()&&t[pos+1]=='\n') ok=true;
    return ok;
}
bool is_word_at(const NfaInst&i,const Rune&r){return r.ok&&(i.unicode?unicode_word(r.cp):(r.cp<128&&ascii_word(static_cast<unsigned char>(r.cp))));}
bool assert_word(const NfaInst&i,std::string_view t,std::size_t pos){auto l=rune_before(t,pos),r=rune_at(t,pos);bool ok=is_word_at(i,l)!=is_word_at(i,r);return i.negative?!ok:ok;}
bool assert_word_start_half(const NfaInst&i,std::string_view t,std::size_t pos){return !is_word_at(i,rune_before(t,pos));}
bool assert_word_end_half(const NfaInst&i,std::string_view t,std::size_t pos){return !is_word_at(i,rune_at(t,pos));}

void add_nfa_thread(const RegexProgram&p,std::string_view text,const PatternOptions&,unsigned char sep,std::size_t pos,NfaThread seed,std::vector<NfaThread>&list,std::vector<std::uint8_t>&seen){
    std::vector<NfaThread> stack;stack.push_back(std::move(seed));
    while(!stack.empty()){
        auto t=std::move(stack.back());stack.pop_back();if(t.pc<0||static_cast<std::size_t>(t.pc)>=p.nfa.size())continue;if(seen[t.pc])continue;seen[t.pc]=1;const auto&i=p.nfa[t.pc];
        auto push=[&](int pc,NfaThread z){z.pc=pc;stack.push_back(std::move(z));};
        switch(i.op){
            case NfaInst::Op::Jmp:push(i.x,std::move(t));break;
            case NfaInst::Op::Split:{auto y=t;push(i.y,std::move(y));push(i.x,std::move(t));break;}
            case NfaInst::Op::SaveStart:if(i.group>=0&&static_cast<std::size_t>(i.group)<t.caps.size())t.caps[i.group].first=pos;push(i.x,std::move(t));break;
            case NfaInst::Op::SaveEnd:if(i.group>=0&&static_cast<std::size_t>(i.group)<t.caps.size())t.caps[i.group].second=pos;push(i.x,std::move(t));break;
            case NfaInst::Op::AssertBegin:if(assert_begin(i,text,pos,sep))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertEnd:if(assert_end(i,text,pos,sep))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertAbsBegin:if(assert_abs_begin(i,text,pos))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertAbsEnd:if(assert_abs_end(i,text,pos))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertEndNewline:if(assert_end_newline(i,text,pos,sep))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertWord:if(assert_word(i,text,pos))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertWordStartHalf:if(assert_word_start_half(i,text,pos))push(i.x,std::move(t));break;
            case NfaInst::Op::AssertWordEndHalf:if(assert_word_end_half(i,text,pos))push(i.x,std::move(t));break;
            default:list.push_back(std::move(t));break;
        }
    }
}

bool nfa_consume(const NfaInst&i,UChar32 cp,unsigned char sep,const PatternOptions&){
    bool icase=i.icase;
    if(i.op==NfaInst::Op::Rune)return cp_eq(cp,static_cast<UChar32>(i.rune),icase);
    if(i.op==NfaInst::Op::Any)return i.dotall||cp!=sep;
    if(i.op==NfaInst::Op::Class)return i.char_class&&class_match(*i.char_class,cp,icase);
    return false;
}

bool nfa_search(const RegexProgram&p,std::string_view text,const PatternOptions&o,std::size_t from,Match*out,std::uint32_t file_id,unsigned char sep){
    if(p.nfa_start<0) return false;
    std::vector<NfaThread> cur, next;
    std::vector<std::uint8_t> seen(p.nfa.size()), seen_next(p.nfa.size());
    std::optional<NfaThread> best;
    std::size_t best_end = 0;
    std::size_t pos = from;
    for (;;) {
        if (cur.empty() && !best && !p.query_ir.prefixes.empty() && pos < text.size()) {
            std::size_t next_jump = std::string_view::npos;
            if (p.query_ir.prefixes.size() == 1) {
                next_jump = text.find(p.query_ir.prefixes[0], pos);
            } else {
                for (const auto& pref : p.query_ir.prefixes) {
                    auto cand = text.find(pref, pos);
                    if (cand != std::string_view::npos) {
                        if (next_jump == std::string_view::npos || cand < next_jump) {
                            next_jump = cand;
                        }
                    }
                }
            }
            if (next_jump == std::string_view::npos) break;
            pos = next_jump;
        }
        NfaThread start;
        start.pc = p.nfa_start;
        start.start = pos;
        start.caps.assign(static_cast<std::size_t>(p.groups) + 1, {SIZE_MAX, SIZE_MAX});
        add_nfa_thread(p, text, o, sep, pos, std::move(start), cur, seen);
        for (std::size_t k = 0; k < cur.size(); ++k) {
            if (p.nfa[cur[k].pc].op == NfaInst::Op::Match) {
                best = cur[k];
                best_end = pos;
                cur.resize(k);
                break;
            }
        }
        if (best && cur.empty()) break;
        if (pos >= text.size()) break;
        auto r = rune_at(text, pos);
        if (!r.ok) break;
        next.clear();
        std::fill(seen_next.begin(), seen_next.end(), 0);
        for (auto& t : cur) {
            const auto& i = p.nfa[t.pc];
            if (nfa_consume(i, r.cp, sep, o)) {
                auto z = t;
                z.pc = i.x;
                add_nfa_thread(p, text, o, sep, r.next, std::move(z), next, seen_next);
            }
        }
        cur.swap(next);
        seen.swap(seen_next);
        pos = r.next;
        if (cur.empty() && best) break;
    }
    if(!best) return false;
    if(out){out->file_id=file_id;out->start=best->start;out->end=best_end;out->captures.assign(static_cast<std::size_t>(p.groups)+1,{});out->captures[0]={best->start,best_end,true,""};for(int g=1;g<=p.groups;++g){if(static_cast<std::size_t>(g)<best->caps.size()){auto[a,b]=best->caps[g];if(a!=SIZE_MAX&&b!=SIZE_MAX)out->captures[g]={a,b,true,static_cast<std::size_t>(g)<p.group_names.size()?p.group_names[g]:std::string{}};}}}
    return true;
}

struct Caps { std::vector<std::pair<std::size_t,std::size_t>> g; };
struct State { std::size_t pos=0; Caps caps; };

bool literal_at(std::string_view text,std::size_t pos,std::string_view lit,bool icase,std::size_t* end) {
    std::size_t tp=pos,lp=0;while(lp<lit.size()){auto a=rune_at(text,tp),b=rune_at(lit,lp);if(!a.ok||!b.ok||!cp_eq(a.cp,b.cp,icase))return false;tp=a.next;lp=b.next;}if(end)*end=tp;return true;
}
// Extended VM (eval) resource bounds — enforced explicitly with clean throws:
// - lookbehind window: 8192 bytes/code-units before s.pos (both positive and negative branches: lo = s.pos>8192 ? s.pos-8192 : 0)
// - Repeat hard limit: capped to 10000 iterations (finite max -> min(max,10000); unbounded -> min(hard,10000))
// - recursion depth: throws if depth>10000
// - VM state limit: any Concat/Alt/Repeat intermediate expansion exceeding 50000 states throws
//   "pergrep regex: VM state limit exceeded" instead of silent truncation
std::vector<State> eval(const std::shared_ptr<RegexNode>&n,std::string_view t,const PatternOptions&o,unsigned char sep,const State&s,int depth=0) {
    if(depth>10000) throw std::runtime_error("pergrep regex: recursion depth exceeded");
    using K=RegexNode::Kind;std::vector<State>out;bool icase=n->icase;
    switch(n->kind){
        case K::Empty:out.push_back(s);break;
        case K::Literal:{std::size_t e;if(literal_at(t,s.pos,n->literal,icase,&e)){auto z=s;z.pos=e;out.push_back(std::move(z));}break;}
        case K::Dot:{auto r=rune_at(t,s.pos);if(r.ok&&(n->dotall||r.cp!=sep)){auto z=s;z.pos=r.next;out.push_back(std::move(z));}break;}
        case K::Class:{auto r=rune_at(t,s.pos);if(r.ok&&class_match(n->char_class,r.cp,icase)){auto z=s;z.pos=r.next;out.push_back(std::move(z));}break;}
        case K::Begin:{bool ok=s.pos==0||(n->multiline&&s.pos>0&&static_cast<unsigned char>(t[s.pos-1])==sep);if(ok)out.push_back(s);break;}
        case K::End:{
            bool ok=false;
            if(n->multiline){
                ok=s.pos==t.size()||(s.pos<t.size()&&static_cast<unsigned char>(t[s.pos])==sep);
                if(!ok&&n->crlf&&sep=='\n'&&s.pos+1<t.size()&&t[s.pos]=='\r'&&s.pos+1<t.size()&&t[s.pos+1]=='\n')ok=true;
            } else {
                ok=s.pos==t.size()||(s.pos+1==t.size()&&static_cast<unsigned char>(t[s.pos])==sep);
                if(!ok&&n->crlf&&sep=='\n'&&s.pos+2==t.size()&&t[s.pos]=='\r'&&s.pos+1<t.size()&&t[s.pos+1]=='\n')ok=true;
            }
            if(ok)out.push_back(s);
            break;
        }
        case K::AbsBegin:{if(s.pos==0)out.push_back(s);break;}
        case K::AbsEnd:{if(s.pos==t.size())out.push_back(s);break;}
        case K::EndNewline:{
            bool ok=s.pos==t.size()||(s.pos+1==t.size()&&static_cast<unsigned char>(t[s.pos])==sep);
            if(!ok&&n->crlf&&sep=='\n'&&s.pos+2==t.size()&&t[s.pos]=='\r'&&s.pos+1<t.size()&&t[s.pos+1]=='\n')ok=true;
            if(ok)out.push_back(s);
            break;
        }
        case K::WordBoundary:{auto l=rune_before(t,s.pos),r=rune_at(t,s.pos);bool lw=l.ok&&(n->unicode?unicode_word(l.cp):(l.cp<128&&ascii_word(static_cast<unsigned char>(l.cp))));bool rw=r.ok&&(n->unicode?unicode_word(r.cp):(r.cp<128&&ascii_word(static_cast<unsigned char>(r.cp))));bool ok=lw!=rw;if(n->negative)ok=!ok;if(ok)out.push_back(s);break;}
        case K::WordStartHalf:{auto l=rune_before(t,s.pos);bool lw=l.ok&&(n->unicode?unicode_word(l.cp):(l.cp<128&&ascii_word(static_cast<unsigned char>(l.cp))));if(!lw)out.push_back(s);break;}
        case K::WordEndHalf:{auto r=rune_at(t,s.pos);bool rw=r.ok&&(n->unicode?unicode_word(r.cp):(r.cp<128&&ascii_word(static_cast<unsigned char>(r.cp))));if(!rw)out.push_back(s);break;}
        case K::Concat:{std::vector<State>cur{s};for(auto&c:n->children){std::vector<State>next;for(auto&st:cur){auto v=eval(c,t,o,sep,st,depth+1);next.insert(next.end(),std::make_move_iterator(v.begin()),std::make_move_iterator(v.end()));if(next.size()>50000)throw std::runtime_error("pergrep regex: VM state limit exceeded");}cur.swap(next);if(cur.empty())break;}out=std::move(cur);break;}
        case K::Alt:{for(auto&c:n->children){auto v=eval(c,t,o,sep,s,depth+1);out.insert(out.end(),std::make_move_iterator(v.begin()),std::make_move_iterator(v.end()));if(out.size()>50000)throw std::runtime_error("pergrep regex: VM state limit exceeded");}break;}
        case K::Group:{auto base=s;auto v=eval(n->children[0],t,o,sep,s,depth+1);for(auto&z:v){if(static_cast<int>(z.caps.g.size())<=n->group)z.caps.g.resize(n->group+1,{SIZE_MAX,SIZE_MAX});z.caps.g[n->group]={base.pos,z.pos};}out=std::move(v);break;}
        case K::BackRef:{if(n->group>=static_cast<int>(s.caps.g.size()))break;auto[a,b]=s.caps.g[n->group];if(a==SIZE_MAX||b<a||b>t.size())break;std::size_t e;if(literal_at(t,s.pos,t.substr(a,b-a),icase,&e)){auto z=s;z.pos=e;out.push_back(std::move(z));}break;}
        case K::LookAhead:{
            auto v=eval(n->children[0],t,o,sep,s,depth+1);
            if(n->negative){if(v.empty())out.push_back(s);}
            else{for(auto&z:v){State r=z;r.pos=s.pos;out.push_back(std::move(r));}}
            break;
        }
        case K::LookBehind:{
            std::size_t lo = s.pos > 8192 ? s.pos - 8192 : 0;
            if(n->negative){
                bool ok=false;
                for(std::size_t p=lo;p<=s.pos;){
                    State q=s;q.pos=p;auto v=eval(n->children[0],t,o,sep,q,depth+1);
                    for(auto&z:v)if(z.pos==s.pos){ok=true;break;}
                    if(ok||p==s.pos) break;
                    auto rr=rune_at(t,p);p=rr.ok?rr.next:p+1;
                }
                if(!ok)out.push_back(s);
            } else {
                for(std::size_t p=lo;p<=s.pos;){
                    State q=s;q.pos=p;auto v=eval(n->children[0],t,o,sep,q,depth+1);
                    for(auto&z:v)if(z.pos==s.pos){State r=z;r.pos=s.pos;out.push_back(std::move(r));}
                    if(p==s.pos) break;
                    auto rr=rune_at(t,p);p=rr.ok?rr.next:p+1;
                }
            }
            break;
        }
        case K::Repeat:{
            std::vector<State>levels{s};std::vector<std::vector<State>>all{levels};
            std::size_t total = levels.size();
            std::size_t hard = t.size() - s.pos + 1;
            std::size_t limit = n->max == SIZE_MAX ? std::min<std::size_t>(hard, 10000) : std::min<std::size_t>(n->max, 10000);
            for(std::size_t k=1;k<=limit;++k){
                std::vector<State>next;
                for(auto&st:levels){
                    auto v=eval(n->children[0],t,o,sep,st,depth+1);
                    for(auto&z:v)if(z.pos!=st.pos)next.push_back(std::move(z));
                }
                if(next.empty())break;
                if(next.size()>50000) throw std::runtime_error("pergrep regex: VM state limit exceeded");
                total += next.size();
                if(total>50000) throw std::runtime_error("pergrep regex: VM state limit exceeded");
                all.push_back(next);levels=std::move(next);
            }
            if(n->greedy){for(std::size_t k=all.size();k-->n->min;)out.insert(out.end(),all[k].begin(),all[k].end());}
            else{for(std::size_t k=n->min;k<all.size();++k)out.insert(out.end(),all[k].begin(),all[k].end());}
            if(out.size()>50000) throw std::runtime_error("pergrep regex: VM state limit exceeded");
            break;
        }
    }
    return out;
}

} // namespace

// Canonical QueryIR extraction helpers. All optimizer metadata is built here
// and installed into RegexProgram once by Parser::parse().
std::vector<std::string> query_mandatory(const std::shared_ptr<RegexNode>& n) {
    using K = RegexNode::Kind;
    if (!n) return {};
    if (n->kind == K::Literal) return (n->literal.empty() || n->icase) ? std::vector<std::string>{} : std::vector<std::string>{n->literal};
    if (n->kind == K::Group) return query_mandatory(n->children[0]);
    if (n->kind == K::LookAhead || n->kind == K::LookBehind) {
        if (!n->negative) return query_mandatory(n->children[0]);
        return {};
    }
    if (n->kind == K::Repeat) {
        if (n->min == 0) return {};
        return query_mandatory(n->children[0]);
    }
    if (n->kind == K::Concat) {
        std::vector<std::string> out; std::string run;
        for (auto& c : n->children) {
            if (c->kind == K::Literal && !c->icase) run += c->literal;
            else { if (!run.empty()) { out.push_back(run); run.clear(); } auto m = query_mandatory(c); out.insert(out.end(), m.begin(), m.end()); }
        }
        if (!run.empty()) out.push_back(run);
        return out;
    }
    if (n->kind == K::Alt) {
        if (n->children.empty()) return {};
        auto acc = query_mandatory(n->children[0]);
        for (size_t i = 1; i < n->children.size(); ++i) {
            auto cur = query_mandatory(n->children[i]);
            std::vector<std::string> nxt;
            for (auto& a : acc) if (std::find(cur.begin(), cur.end(), a) != cur.end()) nxt.push_back(a);
            acc.swap(nxt);
        }
        return acc;
    }
    return {};
}
std::vector<std::string> query_prefixes(const std::shared_ptr<RegexNode>& n) {
    using K = RegexNode::Kind;
    if (!n) return {};
    if (n->kind == K::Literal) {
        if (!n->literal.empty() && !n->icase) return {n->literal};
        return {};
    }
    if (n->kind == K::Group) return query_prefixes(n->children[0]);
    if (n->kind == K::Repeat) {
        if (n->min > 0) return query_prefixes(n->children[0]);
        return {};
    }
    if (n->kind == K::Concat) {
        if (!n->children.empty()) return query_prefixes(n->children.front());
        return {};
    }
    if (n->kind == K::Alt) {
        std::vector<std::string> out;
        for (const auto& c : n->children) {
            auto p = query_prefixes(c);
            if (p.empty()) return {};
            out.insert(out.end(), p.begin(), p.end());
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    return {};
}
std::vector<std::vector<std::string>> query_branch_mandatory(const std::shared_ptr<RegexNode>& n) {
    using K = RegexNode::Kind;
    if (!n) return {};
    const RegexNode* cur = n.get();
    std::shared_ptr<RegexNode> cur_sp = n;
    while (cur && cur->kind == K::Group) { cur_sp = cur->children[0]; cur = cur_sp.get(); }
    if (!cur || cur->kind != K::Alt) return {};
    std::vector<std::vector<std::string>> out;
    for (const auto& c : cur_sp->children) {
        auto m = query_mandatory(c);
        if (m.empty()) return {};
        out.push_back(std::move(m));
    }
    return out;
}
bool query_is_pure_literal(const std::shared_ptr<RegexNode>& n, std::string& out) {
    using K = RegexNode::Kind;
    if (!n) return false;
    if (n->kind == K::Literal) {
        if (!n->icase) { out = n->literal; return true; }
        return false;
    }
    if (n->kind == K::Group) return query_is_pure_literal(n->children[0], out);
    if (n->kind == K::Concat) {
        std::string run;
        for (const auto& c : n->children) {
            std::string sub;
            if (!query_is_pure_literal(c, sub)) return false;
            run += sub;
        }
        out = run;
        return true;
    }
    return false;
}
QueryIR analyze_query(const std::shared_ptr<RegexNode>& ast, bool extended) {
    QueryIR ir;
    ir.mandatory = query_mandatory(ast);
    std::sort(ir.mandatory.begin(), ir.mandatory.end());
    ir.mandatory.erase(std::unique(ir.mandatory.begin(), ir.mandatory.end()), ir.mandatory.end());
    std::sort(ir.mandatory.begin(), ir.mandatory.end(), [](const auto& a, const auto& b){ return a.size() > b.size(); });
    ir.prefixes = query_prefixes(ast);
    ir.branch_mandatory = query_branch_mandatory(ast);
    std::string lit;
    bool pure = query_is_pure_literal(ast, lit);
    // Extended patterns (backref/lookaround) are never pure literals.
    if (extended) pure = false;
    ir.is_pure_literal = pure;
    if (pure) ir.exact_literal = lit;
    return ir;
}

RegexProgram parse_regex(std::string_view pattern,const PatternOptions&opt){
    auto p=Parser(pattern,opt).parse();
    if(p.extended&&opt.engine==Engine::Default)throw std::runtime_error("pergrep regex: backreferences/look-around require -P/--pcre2");
    auto mk=[&](RegexNode::Kind k,bool multiline=false){auto n=std::make_shared<RegexNode>();n->kind=k;n->icase=opt.case_mode==CaseMode::Insensitive;n->dotall=opt.dotall;n->multiline=multiline;n->unicode=opt.unicode;n->crlf=opt.crlf;return n;};
    if(opt.line){auto c=mk(RegexNode::Kind::Concat);c->children={mk(RegexNode::Kind::Begin,true),p.ast,mk(RegexNode::Kind::End,true)};p.ast=std::move(c);}
    else if(opt.word){auto c=mk(RegexNode::Kind::Concat);c->children={mk(RegexNode::Kind::WordStartHalf),p.ast,mk(RegexNode::Kind::WordEndHalf)};p.ast=std::move(c);}
    if(!p.extended){NfaCompiler c(p);c.compile(p.ast);}
    return p;
}

bool regex_search(const RegexProgram&p,std::string_view text,const PatternOptions&o,std::size_t from,Match*out,std::uint32_t file_id,unsigned char sep){
    if(!p.extended)return nfa_search(p,text,o,from,out,file_id,sep);
    std::size_t st=from;while(st<=text.size()){State s;s.pos=st;s.caps.g.resize(p.groups+1,{SIZE_MAX,SIZE_MAX});auto v=eval(p.ast,text,o,sep,s);if(!v.empty()){auto z=v.front();if(out){out->file_id=file_id;out->start=st;out->end=z.pos;out->captures.assign(p.groups+1,{});out->captures[0]={st,z.pos,true,""};for(int g=1;g<=p.groups;++g){if(g<static_cast<int>(z.caps.g.size())){auto[a,b]=z.caps.g[g];if(a!=SIZE_MAX)out->captures[g]={a,b,true,g<static_cast<int>(p.group_names.size())?p.group_names[g]:std::string{}};}}}return true;}if(st==text.size())break;auto r=rune_at(text,st);st=r.ok?r.next:st+1;}return false;
}
void test_eval_depth_guard(int depth){
    // Build a minimal AST (literal) and call eval with the given depth to verify the guard.
    auto n = std::make_shared<RegexNode>(); n->kind = RegexNode::Kind::Literal; n->literal = "a";
    State s; s.pos = 0;
    // Create a dummy text view; eval will immediately check depth before doing work.
    std::string_view t = "a";
    PatternOptions o;
    unsigned char sep = '\n';
    (void)eval(n, t, o, sep, s, depth);
}
std::vector<Match> regex_find_all(const RegexProgram&p,std::string_view text,const PatternOptions&o,bool overlapping,std::uint32_t file_id,std::uint64_t base,std::uint64_t max_matches,unsigned char sep){
    std::vector<Match>out;std::size_t pos=0;while(pos<=text.size()){Match m;if(!regex_search(p,text,o,pos,&m,file_id,sep))break;std::size_t ls=m.start,le=m.end;m.start+=base;m.end+=base;for(auto&c:m.captures)if(c.matched){c.start+=base;c.end+=base;}out.push_back(std::move(m));if(max_matches&&out.size()>=max_matches)break;if(overlapping){auto r=rune_at(text,ls);pos=r.ok?r.next:ls+1;}else if(le>ls)pos=le;else{auto r=rune_at(text,ls);pos=r.ok?r.next:ls+1;}}return out;
}

} // namespace pergrep::detail

namespace pergrep {
Pattern::Pattern()=default;Pattern::Pattern(std::shared_ptr<const Impl>i):impl_(std::move(i)){}
Pattern Pattern::compile(std::string e,PatternOptions o){
    auto i=std::make_shared<Impl>();i->expr=std::move(e);i->opt=o;
    if(o.case_mode==CaseMode::Smart){
        bool upper=false;
        if(o.unicode){int32_t p=0,n=static_cast<int32_t>(i->expr.size());while(p<n){UChar32 cp;U8_NEXT(i->expr.data(),p,n,cp);if(cp<0)continue;if(u_isupper(cp)){upper=true;break;}}}
        else for(unsigned char c:i->expr)if(std::isupper(c)){upper=true;break;}
        i->opt.case_mode=upper?CaseMode::Sensitive:CaseMode::Insensitive;
    }
    if(o.kind==PatternKind::Regex)i->re=detail::parse_regex(i->expr,i->opt);
    return Pattern(i);
}
const std::string&Pattern::expression()const noexcept{return impl_->expr;}const PatternOptions&Pattern::options()const noexcept{return impl_->opt;}bool Pattern::is_fixed()const noexcept{return impl_->opt.kind==PatternKind::Fixed;}std::vector<std::string>Pattern::mandatory_literals()const{if(is_fixed())return{impl_->expr};return impl_->re.query_ir.mandatory;}
}
