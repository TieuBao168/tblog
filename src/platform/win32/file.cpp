// Windows files, paths and directories (see platform/platform.h).
//
// Every handle is opened with FILE_SHARE_READ | FILE_SHARE_WRITE |
// FILE_SHARE_DELETE and is not inheritable, so the live log file can be
// renamed (rotation), deleted (retention, an external cleanup) or read while
// tblog holds it open, as on POSIX. Paths are UTF-8; '/' and '\' both
// separate components.
#include "platform/win32/win32.h"

#include <cerrno>
#include <cstring>
#include <new>
#include <vector>

namespace tblog {
namespace pf {

namespace win {

int errno_from(DWORD err) noexcept {
    switch (err) {
        case ERROR_SUCCESS: return 0;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_NO_MORE_FILES: return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
        case ERROR_CURRENT_DIRECTORY: return EACCES;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return EEXIST;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return ENOSPC;
        case ERROR_INVALID_HANDLE: return EBADF;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return ENOMEM;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
        case ERROR_NO_UNICODE_TRANSLATION: return EINVAL;
        case ERROR_DIRECTORY: return ENOTDIR;
        case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA: return EPIPE;
        case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
        case ERROR_NOT_SAME_DEVICE: return EXDEV;
        case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED: return ENOSYS;
        default: return EIO;
    }
}

void set_errno_from_last_error() noexcept { errno = errno_from(GetLastError()); }

bool to_wide(const std::string &s, std::wstring *out) noexcept {
    try {
        out->clear();
        if (s.empty()) return true;
        if (std::memchr(s.data(), '\0', s.size())) {
            errno = EINVAL;
            return false;
        }
        if (s.size() > 0x7fffffff) {
            errno = ENAMETOOLONG;
            return false;
        }
        const int in = static_cast<int>(s.size());
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), in, nullptr, 0);
        if (n <= 0) {
            errno = EINVAL;
            return false;
        }
        out->resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), in, &(*out)[0], n);
        return true;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return false;
    }
}

std::string to_utf8(const wchar_t *s, size_t len) {
    std::string out;
    if (len == 0 || len > 0x7fffffff) return out;
    const int in = static_cast<int>(len);
    int n = WideCharToMultiByte(CP_UTF8, 0, s, in, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, s, in, &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace win

using win::file_of;
using win::handle_of;

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;  // 1601-01-01 -> 1970-01-01

int64_t filetime_to_unix_ns(const FILETIME &ft) noexcept {
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (static_cast<int64_t>(t) - static_cast<int64_t>(kEpochDiff100ns)) * 100;
}

void fill_info(const BY_HANDLE_FILE_INFORMATION &bi, FileInfo *out) noexcept {
    out->size = (uint64_t(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow;
    out->mtime_ns = filetime_to_unix_ns(bi.ftLastWriteTime);
    out->id.dev = bi.dwVolumeSerialNumber;
    out->id.ino = (uint64_t(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
}

// Length of the root of `p` that must not be created: "C:" / "C:\" / "\" /
// "\\server\share\" / "\\?\C:\" ... (0 for a relative path).
size_t root_length(const std::string &p) noexcept {
    const size_t n = p.size();
    size_t i = 0;
    if (n >= 2 && is_path_separator(p[0]) && is_path_separator(p[1])) {
        // \\?\ or \\.\ device prefix, then a drive or UNC\server\share.
        if (n >= 4 && (p[2] == '?' || p[2] == '.') && is_path_separator(p[3])) {
            i = 4;
            if (n >= i + 4 && (p.compare(i, 3, "UNC") == 0) && is_path_separator(p[i + 3])) {
                i += 4;  // fall through to the server\share part below
            } else {
                if (n >= i + 2 && p[i + 1] == ':') i += 2;
                if (i < n && is_path_separator(p[i])) ++i;
                return i;
            }
        } else {
            i = 2;
        }
        // server \ share
        for (int part = 0; part < 2; ++part) {
            while (i < n && !is_path_separator(p[i])) ++i;
            if (i < n) ++i;
        }
        return i;
    }
    if (n >= 2 && p[1] == ':') i = 2;
    if (i < n && is_path_separator(p[i])) ++i;
    return i;
}

bool is_directory(const std::wstring &w) noexcept {
    const DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// FILE_RENAME_INFO with the Flags member of Windows 10 1607+ (declared here:
// older SDK / MinGW headers only have the BOOLEAN ReplaceIfExists form, which
// has the same layout).
struct RenameInfoEx {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength;  // bytes
    WCHAR FileName[1];
};
constexpr DWORD kRenameReplaceIfExists = 0x00000001;  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
constexpr DWORD kRenamePosixSemantics = 0x00000002;   // FILE_RENAME_FLAG_POSIX_SEMANTICS
constexpr int kFileRenameInfoEx = 22;                 // FileRenameInfoEx

// Rename with POSIX semantics (Windows 10 1607+, NTFS): works even when the
// target is open by someone else (with FILE_SHARE_DELETE), where
// MoveFileExW fails with ERROR_ACCESS_DENIED. On failure GetLastError() is set.
bool posix_rename(const std::wstring &from, const std::wstring &to) noexcept {
    // The target name must be fully qualified (NT rename semantics).
    DWORD need = GetFullPathNameW(to.c_str(), 0, nullptr, nullptr);
    if (need == 0) return false;
    std::vector<wchar_t> full;
    try {
        full.resize(need);
    } catch (const std::bad_alloc &) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    DWORD len = GetFullPathNameW(to.c_str(), need, full.data(), nullptr);
    if (len == 0 || len >= need) return false;
    // Win32 "C:\x" -> NT "\??\C:\x"; "\\server\share" -> "\??\UNC\server\share".
    std::wstring nt;
    try {
        if (len >= 2 && full[0] == L'\\' && full[1] == L'\\') {
            if (len >= 4 && (full[2] == L'?' || full[2] == L'.') && full[3] == L'\\')
                nt = L"\\??\\" + std::wstring(full.data() + 4, len - 4);
            else
                nt = L"\\??\\UNC\\" + std::wstring(full.data() + 2, len - 2);
        } else {
            nt = L"\\??\\" + std::wstring(full.data(), len);
        }
    } catch (const std::bad_alloc &) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    const size_t name_bytes = nt.size() * sizeof(wchar_t);
    const size_t total = sizeof(RenameInfoEx) + name_bytes;
    std::vector<unsigned char> buf;
    try {
        buf.assign(total, 0);
    } catch (const std::bad_alloc &) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    RenameInfoEx *info = reinterpret_cast<RenameInfoEx *>(buf.data());
    info->Flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, nt.data(), name_bytes);

    HANDLE h = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, kShareAll, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx),
                                               info, static_cast<DWORD>(total));
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    SetLastError(err);
    return ok != FALSE;
}

}  // namespace

// ------------------------------------------------------------------ files
FileHandle file_open(const std::string &path, OpenMode mode, unsigned /*perms*/) noexcept {
    std::wstring w;
    if (!win::to_wide(path, &w)) return no_file();
    DWORD access = 0, disposition = 0;
    bool no_follow = true;  // refuse a reparse point as the file (platform.h, OpenMode)
    bool truncate = false;
    switch (mode) {
        case OpenMode::Append:
            // FILE_APPEND_DATA without FILE_WRITE_DATA: every WriteFile goes
            // to the end of the file (O_APPEND).
            access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
            disposition = OPEN_ALWAYS;
            break;
        case OpenMode::Read:
            access = GENERIC_READ;
            disposition = OPEN_EXISTING;
            no_follow = false;
            break;
        case OpenMode::ReadNoFollow:
            access = GENERIC_READ;
            disposition = OPEN_EXISTING;
            break;
        case OpenMode::Truncate:
            // Not CREATE_ALWAYS: the reparse-point check must come before the
            // data is cut, so open first and truncate once it passed.
            access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
            disposition = OPEN_ALWAYS;
            truncate = true;
            break;
        case OpenMode::CreateNew:
            access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
            disposition = CREATE_NEW;  // fails if the name exists, a link included
            break;
    }
    // FILE_FLAG_OPEN_REPARSE_POINT opens a symbolic link / junction itself
    // instead of its target, so the check below sees it.
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | (no_follow ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    // Default security attributes: the handle is not inherited.
    HANDLE h = CreateFileW(w.c_str(), access, kShareAll, nullptr, disposition, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        win::set_errno_from_last_error();
        return no_file();
    }
    if (no_follow) {
        auto fail = [&](int err) -> FileHandle {
            CloseHandle(h);
            errno = err;
            return no_file();
        };
        BY_HANDLE_FILE_INFORMATION info;
        FILE_ATTRIBUTE_TAG_INFO tag;
        if (!GetFileInformationByHandle(h, &info) ||
            !GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag))
            return fail(win::errno_from(GetLastError()));
        // A hard link (second name) may be any file of the creator's choosing.
        if (info.nNumberOfLinks > 1) return fail(EMLINK);
        if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            // Links (symbolic links, junctions: name surrogates) are refused.
            if (IsReparseTagNameSurrogate(tag.ReparseTag)) return fail(ELOOP);
            // Other reparse points are ordinary files with a filter on top
            // (OneDrive / cloud-files placeholders, deduplication): open
            // them normally, so the filter serves the data, and make sure
            // the name still refers to the same file.
            CloseHandle(h);
            h = CreateFileW(w.c_str(), access, kShareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                win::set_errno_from_last_error();
                return no_file();
            }
            BY_HANDLE_FILE_INFORMATION again;
            if (!GetFileInformationByHandle(h, &again)) return fail(win::errno_from(GetLastError()));
            if (again.dwVolumeSerialNumber != info.dwVolumeSerialNumber ||
                again.nFileIndexHigh != info.nFileIndexHigh || again.nFileIndexLow != info.nFileIndexLow)
                return fail(ELOOP);  // replaced in between (e.g. by a link)
        }
    }
    if (truncate) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
            win::set_errno_from_last_error();
            CloseHandle(h);
            return no_file();
        }
    }
    return file_of(h);
}

void file_close(FileHandle h) noexcept {
    if (h.valid()) CloseHandle(handle_of(h));
}

FileHandle file_dup(FileHandle h) noexcept {
    if (!h.valid()) {
        errno = EBADF;
        return no_file();
    }
    HANDLE out = nullptr;
    const HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, handle_of(h), self, &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        win::set_errno_from_last_error();
        return no_file();
    }
    return file_of(out);
}

bool file_write_all(FileHandle h, const void *data, size_t len, size_t *written) noexcept {
    const char *p = static_cast<const char *>(data);
    size_t done = 0;
    bool ok = true;
    while (done < len) {
        const size_t left = len - done;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : static_cast<DWORD>(left);
        DWORD n = 0;
        if (!WriteFile(handle_of(h), p + done, chunk, &n, nullptr)) {
            win::set_errno_from_last_error();
            ok = false;
            break;
        }
        if (n == 0) {
            errno = EIO;
            ok = false;
            break;
        }
        done += n;
    }
    if (written) *written = done;
    return ok;
}

bool file_write_crash(FileHandle h, const void *data, size_t len) noexcept {
    const char *p = static_cast<const char *>(data);
    while (len > 0) {
        const DWORD chunk = len > 0x40000000u ? 0x40000000u : static_cast<DWORD>(len);
        DWORD n = 0;
        if (!WriteFile(handle_of(h), p, chunk, &n, nullptr) || n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

int64_t file_read(FileHandle h, void *buf, size_t len) noexcept {
    const DWORD chunk = len > 0x40000000u ? 0x40000000u : static_cast<DWORD>(len);
    DWORD n = 0;
    if (!ReadFile(handle_of(h), buf, chunk, &n, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF || err == ERROR_BROKEN_PIPE) return 0;
        errno = win::errno_from(err);
        return -1;
    }
    return static_cast<int64_t>(n);
}

bool file_sync(FileHandle h) noexcept {
    if (FlushFileBuffers(handle_of(h))) return true;
    win::set_errno_from_last_error();
    return false;
}

bool file_datasync(FileHandle h) noexcept { return file_sync(h); }

bool file_truncate(FileHandle h, uint64_t size) noexcept {
    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    // Setting the end of file needs FILE_WRITE_DATA, which an Append handle
    // must not have (it would stop appending): do it through a second handle
    // to the same file.
    HANDLE w = ReOpenFile(handle_of(h), GENERIC_WRITE, kShareAll, 0);
    if (w != INVALID_HANDLE_VALUE) {
        const BOOL ok = SetFileInformationByHandle(w, FileEndOfFileInfo, &eof, sizeof eof);
        const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(w);
        if (ok) return true;
        errno = win::errno_from(err);
        return false;
    }
    if (SetFileInformationByHandle(handle_of(h), FileEndOfFileInfo, &eof, sizeof eof)) return true;
    win::set_errno_from_last_error();
    return false;
}

bool file_info(FileHandle h, FileInfo *out) noexcept {
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(handle_of(h), &bi)) {
        win::set_errno_from_last_error();
        return false;
    }
    fill_info(bi, out);
    return true;
}

bool path_info(const std::string &path, FileInfo *out) noexcept {
    std::wstring w;
    if (!win::to_wide(path, &w)) return false;
    // FILE_FLAG_BACKUP_SEMANTICS: directories too (stat works on them).
    HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        win::set_errno_from_last_error();
        return false;
    }
    const bool ok = file_info(file_of(h), out);
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------ paths
size_t last_path_separator(const std::string &path) noexcept { return path.find_last_of("/\\"); }

bool path_exists(const std::string &path) noexcept {
    std::wstring w;
    if (!win::to_wide(path, &w)) return false;
    if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // A file in the middle of being deleted (or locked down) is still there.
    const DWORD err = GetLastError();
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION;
}

bool make_parent_dirs(const std::string &path) noexcept {
    try {
        const size_t slash = last_path_separator(path);
        if (slash == std::string::npos) return true;
        const std::string dir = path.substr(0, slash);
        const size_t root = root_length(dir);
        if (dir.size() <= root) return true;  // "C:\x", "\x", "\\server\share\x"
        std::wstring w;
        for (size_t i = root + 1; i <= dir.size(); ++i) {
            if (i != dir.size() && !is_path_separator(dir[i])) continue;
            if (is_path_separator(dir[i - 1])) continue;  // "a//b"
            if (!win::to_wide(dir.substr(0, i), &w)) return false;
            if (!CreateDirectoryW(w.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    errno = win::errno_from(err);
                    return false;
                }
            }
        }
        if (!win::to_wide(dir, &w)) return false;
        if (is_directory(w)) return true;
        errno = ENOTDIR;
        return false;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return false;
    }
}

bool rename_replace(const std::string &from, const std::string &to) noexcept {
    std::wstring wf, wt;
    if (!win::to_wide(from, &wf) || !win::to_wide(to, &wt)) return false;
    // MoveFileExW renames a file that is open elsewhere (all our handles share
    // FILE_SHARE_DELETE) and replaces an existing, closed target. It fails
    // with ERROR_ACCESS_DENIED when the target is itself open (or delete-
    // pending); POSIX-semantics rename handles that case.
    if (MoveFileExW(wf.c_str(), wt.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    const DWORD err = GetLastError();
    if ((err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) && posix_rename(wf, wt))
        return true;
    errno = win::errno_from(err);
    return false;
}

bool remove_file(const std::string &path) noexcept {
    std::wstring w;
    if (!win::to_wide(path, &w)) return false;
    // Since Windows 10 1903 DeleteFileW uses POSIX semantics on NTFS: the name
    // disappears at once even while the file is still open elsewhere.
    if (DeleteFileW(w.c_str())) return true;
    win::set_errno_from_last_error();
    return false;
}

bool list_dir(const std::string &dir, std::vector<std::string> *names) {
    std::wstring w;
    if (!win::to_wide(dir, &w)) return false;
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w += L'\\';
    w += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return true;  // empty (not even "." -- a drive root)
        errno = win::errno_from(err);
        return false;
    }
    try {
        do {
            const wchar_t *n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) continue;
            names->push_back(win::to_utf8(n, wcslen(n)));
        } while (FindNextFileW(h, &fd));
    } catch (...) {
        FindClose(h);
        throw;
    }
    FindClose(h);
    return true;
}

void sync_dir(const std::string &) noexcept {}  // NTFS metadata is journaled; nothing to flush

}  // namespace pf
}  // namespace tblog
