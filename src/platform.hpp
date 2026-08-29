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

// fnmatch(3) subset sufficient for pergrep: '*' (crosses '/'), '?',
// '[...]' character classes. No FNM_PATHNAME semantics needed.
inline bool fnmatch(const std::string& pat, const std::string& text) {
    const std::size_t P = pat.size(), T = text.size();
    std::vector<signed char> memo((P + 1) * (T + 1), -1);
    auto go = [&](auto&& self, std::size_t i, std::size_t j) -> bool {
        auto& mm = memo[i * (T + 1) + j];
        if (mm != -1) return mm != 0;
        bool ok = false;
        if (i == P) ok = j == T;
        else if (pat[i] == '*') {
            if (self(self, i + 1, j)) ok = true;
            for (std::size_t k = j; !ok && k < T; ++k)
                if (self(self, i + 1, k + 1)) ok = true;
        } else if (pat[i] == '?') {
            ok = j < T && self(self, i + 1, j + 1);
        } else if (pat[i] == '[') {
            std::size_t e = pat.find(']', i + 1);
            if (e == std::string::npos) ok = j < T && pat[i] == text[j] && self(self, i + 1, j + 1);
            else if (j < T) {
                bool neg = i + 1 < e && (pat[i + 1] == '!' || pat[i + 1] == '^');
                std::size_t k = i + 1 + (neg ? 1 : 0);
                bool hit = false;
                while (k < e) {
                    if (k + 2 < e && pat[k + 1] == '-') {
                        if (text[j] >= pat[k] && text[j] <= pat[k + 2]) hit = true;
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
            ok = j < T && pat[i] == text[j] && self(self, i + 1, j + 1);
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
    auto vol = [](const std::wstring& w) {
        wchar_t root[4] = {0, 0, 0, 0};
        if (w.size() >= 2 && w[1] == L':') {
            root[0] = w[0]; root[1] = L':'; root[2] = L'\\';
        } else {
            // UNC path; use its server/share prefix via GetFullPathNameW.
            DWORD n = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
            if (n == 0) return std::wstring{};
            std::wstring full(n, L'\0');
            ::GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
            auto pos = full.find_first_of(L"\\/", 2);
            if (pos != std::wstring::npos) full.resize(pos);
            return full;
        }
        return std::wstring(root);
    };
    return vol(a.native()) == vol(b.native());
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
        cmdline.append(backslashes, L'\\');
        if (need_quote) cmdline += L'"';
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
    struct StdinWriter { HANDLE h; std::string d; };
    std::string iv(input);
    HANDLE hIn = in_w;
    DWORD tid = 0;
    HANDLE writer = ::CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* w = static_cast<StdinWriter*>(p);
        DWORD written = 0;
        ::WriteFile(w->h, w->d.data(), static_cast<DWORD>(w->d.size()), &written, nullptr);
        ::CloseHandle(w->h);
        delete w;
        return 0;
    }, new StdinWriter{hIn, std::move(iv)}, 0, &tid);
    if (writer) ::CloseHandle(writer);

    char buf[16384];
    DWORD n = 0;
    for (;;) {
        if (!::ReadFile(out_r, buf, sizeof buf, &n, nullptr) || n == 0) break;
        output.append(buf, n);
    }
    ::CloseHandle(out_r);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(in_w);
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

inline bool fnmatch(const std::string& pat, const std::string& text) {
    return ::fnmatch(pat.c_str(), text.c_str(), FNM_PATHNAME) == 0;
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
