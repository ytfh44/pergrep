#pragma once
// Portable shim for the small set of POSIX facilities the pergrep CLI uses.
// On Unix these map 1:1 to libc; on Windows they are implemented with the
// Win32 API so the CLI has no POSIX header or function dependencies.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <filesystem>

namespace pergrep_cli::platform {

// Filesystem path to a UTF-8 narrow string with forward slashes (generic
// form), so FileInfo paths stay in UTF-8 regardless of the active ANSI code
// page and match the Unix-facing `generic_string()` convention.
inline std::string path_to_utf8(const std::filesystem::path& p) {
    std::wstring w = p.generic_wstring();
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

inline bool isatty_stdout() { return _isatty(_fileno(stdout)) != 0; }
inline bool isatty_stdin() { return _isatty(_fileno(stdin)) != 0; }

// Whether a path is a reparse point (symlink, junction, mount point). Windows
// junction points are directory reparse points that std::filesystem reports
// as plain directories; the indexer must skip them unless follow is set.
inline bool is_reparse_point(const std::filesystem::path& p) {
    DWORD attrs = ::GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

inline size_t utf8_char_len(unsigned char c) noexcept {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// fnmatch(3) subset with FNM_PATHNAME semantics: '*' stops at '/',
// '?' advances by UTF-8 code point and stops at '/', '[...]' character classes.
// '**' (double star) matches any sequence including '/' – with "**/" matching
// zero or more directory levels (so "**/*.txt" matches "a.txt" at depth 0).
inline bool fnmatch(const std::string& pat, const std::string& text) {
    const std::size_t P = pat.size(), T = text.size();
    std::vector<signed char> memo((P + 1) * (T + 1), -1);
    auto go = [&](auto&& self, std::size_t i, std::size_t j) -> bool {
        auto& mm = memo[i * (T + 1) + j];
        if (mm != -1) return mm != 0;
        bool ok = false;
        if (i == P) {
            ok = (j == T);
        } else if (pat[i] == '*') {
            bool dbl = (i + 1 < P && pat[i + 1] == '*');
            std::size_t ni = i + (dbl ? 2 : 1);
            if (dbl && ni < P && (pat[ni] == '/' || pat[ni] == '\\')) {
                // "**/" – matches zero or more directories (including zero)
                if (self(self, ni + 1, j)) ok = true;
                for (std::size_t k = j; !ok && k < T; ) {
                    if (text[k] == '/' || text[k] == '\\') {
                        if (self(self, ni + 1, k + 1)) ok = true;
                    }
                    std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[k]));
                    if (k + clen > T) clen = 1;
                    k += clen;
                }
            } else {
                if (self(self, ni, j)) ok = true;
                for (std::size_t k = j; !ok && k < T && (dbl || (text[k] != '/' && text[k] != '\\')); ) {
                    std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[k]));
                    if (k + clen > T) clen = 1;
                    k += clen;
                    if (self(self, ni, k)) ok = true;
                }
            }
        } else if (pat[i] == '?') {
            if (j < T && text[j] != '/' && text[j] != '\\') {
                std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[j]));
                if (j + clen > T) clen = 1;
                ok = self(self, i + 1, j + clen);
            }
        } else if (pat[i] == '/' || pat[i] == '\\') {
            ok = (j < T && (text[j] == '/' || text[j] == '\\')) && self(self, i + 1, j + 1);
        } else if (pat[i] == '[') {
            std::size_t e = pat.find(']', i + 1);
            if (e == std::string::npos) {
                ok = (j < T && pat[i] == text[j]) && self(self, i + 1, j + 1);
            } else if (j < T && text[j] != '/' && text[j] != '\\') {
                bool neg = (i + 1 < e && (pat[i + 1] == '!' || pat[i + 1] == '^'));
                std::size_t k = i + 1 + (neg ? 1 : 0);
                bool hit = false;
                while (k < e) {
                    if (k + 2 < e && pat[k + 1] == '-') {
                        if (static_cast<unsigned char>(text[j]) >= static_cast<unsigned char>(pat[k]) &&
                            static_cast<unsigned char>(text[j]) <= static_cast<unsigned char>(pat[k + 2])) hit = true;
                        k += 3;
                    } else {
                        if (pat[k] == text[j]) hit = true;
                        ++k;
                    }
                }
                if (neg) hit = !hit;
                ok = hit && self(self, e + 1, j + 1);
            }
        } else {
            ok = (j < T && pat[i] == text[j]) && self(self, i + 1, j + 1);
        }
        mm = ok ? 1 : 0;
        return ok;
    };
    return go(go, 0, 0);
}


// UTF-8 narrow string to UTF-16, for Win32 APIs that require wide strings.
inline std::wstring utf8_to_wide(std::string_view in) {
    if (in.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()),
                                  nullptr, 0);
    if (n <= 0) return std::wstring(in.begin(), in.end());
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()),
                          w.data(), n);
    return w;
}

// A minimal recoding from an arbitrary 8-bit encoding to UTF-8. Windows has no
// iconv; this maps through the ANSI code page via MultiByteToWideChar.
inline bool ansi_to_utf8(std::string_view in, std::string& out) {
    if (in.empty()) { out.clear(); return true; }
    int n = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                                  in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                              in.data(), static_cast<int>(in.size()), w.data(), n) != n)
        return false;
    int m = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return false;
    out.resize(static_cast<std::size_t>(m));
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), n, out.data(), m, nullptr, nullptr);
    return true;
}

// File timestamps in nanoseconds since the Unix epoch. `which` is
// "accessed" or "created". Takes a filesystem path so Windows can use the
// native wide encoding directly.
inline std::int64_t file_time_ns(const std::filesystem::path& path, std::string_view which) {
    const std::wstring& w = path.native();
    HANDLE h = ::CreateFileW(w.c_str(),
                             FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME ft{};
    BOOL ok = which == "accessed"
                  ? ::GetFileTime(h, nullptr, &ft, nullptr)
                  : ::GetFileTime(h, nullptr, nullptr, &ft);
    ::CloseHandle(h);
    if (!ok) return 0;
    // FILETIME is 100 ns intervals since 1601-01-01; Unix epoch is 1970-01-01.
    constexpr std::int64_t UNIX_EPOCH_100NS = 116444736000000000LL;
    std::int64_t t = (std::int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    t -= UNIX_EPOCH_100NS;
    return t * 100;
}

// Whether two paths live on the same filesystem (volume). Used for
// --one-file-system.
inline bool same_device(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto vol = [](const std::filesystem::path& p) -> std::wstring {
        const std::wstring& w = p.native();
        if (w.empty()) return {};
        DWORD n = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
        if (n == 0) return {};
        std::wstring full(n, L'\0');
        DWORD ret = ::GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
        if (ret == 0 || ret >= n) return {};
        full.resize(ret);
        for (auto& c : full) {
            if (c == L'/') c = L'\\';
        }
        // Extended UNC path \\?\UNC\server\share\...
        if (full.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            auto pos1 = full.find(L'\\', 8);
            if (pos1 == std::wstring::npos) return full;
            auto pos2 = full.find(L'\\', pos1 + 1);
            if (pos2 != std::wstring::npos) full.resize(pos2);
            for (auto& c : full) c = static_cast<wchar_t>(::towlower(c));
            return full;
        }
        // Extended drive path \\?\C:\...
        if (full.rfind(L"\\\\?\\", 0) == 0 && full.size() >= 6 && full[5] == L':') {
            std::wstring root = { static_cast<wchar_t>(::towupper(full[4])), L':', L'\\' };
            return root;
        }
        // Standard drive path C:\...
        if (full.size() >= 2 && full[1] == L':') {
            std::wstring root = { static_cast<wchar_t>(::towupper(full[0])), L':', L'\\' };
            return root;
        }
        // Standard UNC path \\server\share\...
        if (full.rfind(L"\\\\", 0) == 0) {
            auto pos1 = full.find(L'\\', 2);
            if (pos1 == std::wstring::npos) return full;
            auto pos2 = full.find(L'\\', pos1 + 1);
            if (pos2 != std::wstring::npos) full.resize(pos2);
            for (auto& c : full) c = static_cast<wchar_t>(::towlower(c));
            return full;
        }
        return full;
    };
    return vol(a) == vol(b);
}


// Runs a command with the given argv, feeding `input` on stdin, and captures
// stdout. Returns false if the process could not be started or exited nonzero.
inline bool run_capture(const std::vector<std::string>& argv, std::string_view input, std::string& output) {
    if (argv.empty()) return false;
    // Build a Windows command line (UTF-16) with quoting rules compatible with
    // CommandLineToArgvW: quote arguments containing spaces/tabs/quotes and
    // escape embedded quotes with backslashes.
    std::wstring cmdline;
    for (const auto& a : argv) {
        if (!cmdline.empty()) cmdline += L' ';
        std::wstring w = utf8_to_wide(a);
        bool need_quote = a.empty() || a.find_first_of(" \t\"") != std::string::npos;
        if (need_quote) cmdline += L'"';
        std::size_t backslashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') { ++backslashes; continue; }
            if (c == L'"') {
                cmdline.append(backslashes * 2 + 1, L'\\');
                cmdline += L'"';
                backslashes = 0;
            } else {
                cmdline.append(backslashes, L'\\');
                backslashes = 0;
                cmdline += c;
            }
        }
        if (need_quote) {
            cmdline.append(backslashes * 2, L'\\');
            cmdline += L'"';
        } else {
            cmdline.append(backslashes, L'\\');
        }
    }

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!::CreatePipe(&in_r, &in_w, &sa, 0) || !::CreatePipe(&out_r, &out_w, &sa, 0)) {
        if (in_r) ::CloseHandle(in_r);
        if (in_w) ::CloseHandle(in_w);
        if (out_r) ::CloseHandle(out_r);
        if (out_w) ::CloseHandle(out_w);
        return false;
    }
    ::SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(in_r);
    ::CloseHandle(out_w);
    if (!ok) {
        ::CloseHandle(in_w);
        ::CloseHandle(out_r);
        return false;
    }

    // Feed stdin on a thread to avoid deadlock on large inputs.
    struct StdinWriter {
        HANDLE h = nullptr;
        std::string d;
    };
    auto* ctx = new StdinWriter{in_w, std::string(input)};
    DWORD tid = 0;
    HANDLE writer = ::CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* w = static_cast<StdinWriter*>(p);
        const char* ptr = w->d.data();
        size_t rem = w->d.size();
        while (rem > 0) {
            DWORD to_write = static_cast<DWORD>(std::min<size_t>(rem, 65536));
            DWORD written = 0;
            if (!::WriteFile(w->h, ptr, to_write, &written, nullptr) || written == 0) {
                break;
            }
            ptr += written;
            rem -= written;
        }
        ::CloseHandle(w->h);
        delete w;
        return 0;
    }, ctx, 0, &tid);

    if (!writer) {
        ::CloseHandle(in_w);
        delete ctx;
    }

    char buf[16384];
    DWORD n = 0;
    for (;;) {
        if (!::ReadFile(out_r, buf, sizeof buf, &n, nullptr) || n == 0) break;
        output.append(buf, n);
    }
    ::CloseHandle(out_r);
    if (writer) {
        ::WaitForSingleObject(writer, INFINITE);
        ::CloseHandle(writer);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return code == 0;
}

inline std::string utf8_from_argv(const char* s) {
    if (!s) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return s;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    int m = ::WideCharToMultiByte(CP_ACP, 0, w.data(), n - 1, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return s;
    std::string out(static_cast<std::size_t>(m), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, w.data(), n - 1, out.data(), m, nullptr, nullptr);
    return out;
}

} // namespace pergrep_cli::platform

#else // !_WIN32

#include <fnmatch.h>
#include <iconv.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>

namespace pergrep_cli::platform {

inline std::string path_to_utf8(const std::filesystem::path& p) {
    return p.generic_string();
}

inline bool isatty_stdout() { return ::isatty(STDOUT_FILENO) != 0; }
inline bool isatty_stdin() { return ::isatty(STDIN_FILENO) != 0; }

inline bool is_reparse_point(const std::filesystem::path& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

inline size_t utf8_char_len(unsigned char c) noexcept {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

inline bool fnmatch(const std::string& pat, const std::string& text) {
    const std::size_t P = pat.size(), T = text.size();
    std::vector<signed char> memo((P + 1) * (T + 1), -1);
    auto go = [&](auto&& self, std::size_t i, std::size_t j) -> bool {
        auto& mm = memo[i * (T + 1) + j];
        if (mm != -1) return mm != 0;
        bool ok = false;
        if (i == P) {
            ok = (j == T);
        } else if (pat[i] == '*') {
            bool dbl = (i + 1 < P && pat[i + 1] == '*');
            std::size_t ni = i + (dbl ? 2 : 1);
            if (dbl && ni < P && (pat[ni] == '/' || pat[ni] == '\\')) {
                if (self(self, ni + 1, j)) ok = true;
                for (std::size_t k = j; !ok && k < T; ) {
                    if (text[k] == '/' || text[k] == '\\') {
                        if (self(self, ni + 1, k + 1)) ok = true;
                    }
                    std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[k]));
                    if (k + clen > T) clen = 1;
                    k += clen;
                }
            } else {
                if (self(self, ni, j)) ok = true;
                for (std::size_t k = j; !ok && k < T && (dbl || (text[k] != '/' && text[k] != '\\')); ) {
                    std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[k]));
                    if (k + clen > T) clen = 1;
                    k += clen;
                    if (self(self, ni, k)) ok = true;
                }
            }
        } else if (pat[i] == '?') {
            if (j < T && text[j] != '/' && text[j] != '\\') {
                std::size_t clen = utf8_char_len(static_cast<unsigned char>(text[j]));
                if (j + clen > T) clen = 1;
                ok = self(self, i + 1, j + clen);
            }
        } else if (pat[i] == '/' || pat[i] == '\\') {
            ok = (j < T && (text[j] == '/' || text[j] == '\\')) && self(self, i + 1, j + 1);
        } else if (pat[i] == '[') {
            std::size_t e = pat.find(']', i + 1);
            if (e == std::string::npos) {
                ok = (j < T && pat[i] == text[j]) && self(self, i + 1, j + 1);
            } else if (j < T && text[j] != '/' && text[j] != '\\') {
                bool neg = (i + 1 < e && (pat[i + 1] == '!' || pat[i + 1] == '^'));
                std::size_t k = i + 1 + (neg ? 1 : 0);
                bool hit = false;
                while (k < e) {
                    if (k + 2 < e && pat[k + 1] == '-') {
                        if (static_cast<unsigned char>(text[j]) >= static_cast<unsigned char>(pat[k]) &&
                            static_cast<unsigned char>(text[j]) <= static_cast<unsigned char>(pat[k + 2])) hit = true;
                        k += 3;
                    } else {
                        if (pat[k] == text[j]) hit = true;
                        ++k;
                    }
                }
                if (neg) hit = !hit;
                ok = hit && self(self, e + 1, j + 1);
            }
        } else {
            ok = (j < T && pat[i] == text[j]) && self(self, i + 1, j + 1);
        }
        mm = ok ? 1 : 0;
        return ok;
    };
    return go(go, 0, 0);
}

inline bool ansi_to_utf8(std::string_view in, std::string& out) {
    // Non-Windows iconv path lives in cli.cpp (iconv_to_utf8).
    (void)in; (void)out;
    return false;
}

inline std::int64_t file_time_ns(const std::filesystem::path& path, std::string_view which) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    if (which == "accessed")
        return (std::int64_t)st.st_atim.tv_sec * 1000000000LL + st.st_atim.tv_nsec;
    return (std::int64_t)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
}

inline bool same_device(const std::filesystem::path& a, const std::filesystem::path& b) {
    struct stat sa{}, sb{};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 &&
           sa.st_dev == sb.st_dev;
}

inline bool run_capture(const std::vector<std::string>& argv, std::string_view input, std::string& output) {
    (void)argv; (void)input; (void)output;
    return false;
}

} // namespace pergrep_cli::platform

#endif
