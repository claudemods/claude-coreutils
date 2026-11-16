#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "common.hpp"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace cu {

std::string prog = "claude-coreutils";

void err(const std::string &msg)
{
    flush_out();
    std::fprintf(stderr, "%s: %s\n", prog.c_str(), msg.c_str());
}

void err_errno(const std::string &what, int e)
{
    if (e < 0) {
        e = errno;
    }
    err(what + ": " + std::strerror(e));
}

void usage_error(const std::string &msg)
{
    err(msg);
    std::fprintf(stderr, "Try '%s --help' for more information.\n", prog.c_str());
}

bool handle_help_version(int argc, char **argv, const char *usage, const char *summary)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("Usage: %s %s\n%s\n", prog.c_str(), usage, summary);
        return true;
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s (claude-coreutils) %s\n", prog.c_str(), CU_VERSION);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------------------------

bool read_fd(int fd, std::string &out)
{
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return true;
        }
        out.append(buf, static_cast<size_t>(n));
    }
}

bool read_file(const std::string &path, std::string &out)
{
    if (path == "-") {
        return read_fd(0, out);
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = read_fd(fd, out);
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return ok;
}

bool write_all(int fd, const char *data, size_t n)
{
    while (n > 0) {
        const ssize_t w = ::write(fd, data, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

void out(const std::string &s)
{
    std::fwrite(s.data(), 1, s.size(), stdout);
}

bool flush_out()
{
    if (std::fflush(stdout) != 0 && errno != EPIPE) {
        std::fprintf(stderr, "%s: write error: %s\n", prog.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

std::vector<std::string> split_lines(const std::string &data, char sep)
{
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t e = data.find(sep, pos);
        if (e == std::string::npos) {
            lines.push_back(data.substr(pos));
            break;
        }
        lines.push_back(data.substr(pos, e - pos));
        pos = e + 1;
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------
// strings / numbers
// ---------------------------------------------------------------------------------------------

std::string strip_trailing_slashes(std::string s)
{
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool parse_u64(const std::string &s, uint64_t &out, int base)
{
    if (s.empty() || s[0] == '-' || s[0] == '+') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, base);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

bool parse_i64(const std::string &s, int64_t &out)
{
    if (s.empty()) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

bool parse_double(const std::string &s, double &out)
{
    if (s.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    out = v;
    return true;
}

bool parse_size(const std::string &s, uint64_t &out)
{
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (i == 0) {
        return false;
    }
    uint64_t v = 0;
    if (!parse_u64(s.substr(0, i), v)) {
        return false;
    }
    std::string suf = s.substr(i);
    uint64_t mult = 1;
    if (!suf.empty()) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(suf[0])));
        const bool iec = suf.size() >= 2 && (suf[1] == 'i' || suf[1] == 'I'); // KiB
        const bool si = suf.size() == 2 && (suf[1] == 'B') && !iec;           // kB
        const uint64_t base = (si ? 1000 : 1024);
        switch (c) {
        case 'K': mult = base; break;
        case 'M': mult = base * base; break;
        case 'G': mult = base * base * base; break;
        case 'T': mult = base * base * base * base; break;
        case 'P': mult = base * base * base * base * base; break;
        case 'E': mult = base * base * base * base * base * base; break;
        case 'B': mult = 1; break;
        default: return false;
        }
    }
    out = v * mult;
    return true;
}

std::string human_size(double bytes, bool si)
{
    const double base = si ? 1000.0 : 1024.0;
    static const char *units[] = {"", "K", "M", "G", "T", "P", "E"};
    int u = 0;
    while (bytes >= base && u < 6) {
        bytes /= base;
        ++u;
    }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof buf, "%.0f", bytes);
    } else if (bytes < 10) {
        std::snprintf(buf, sizeof buf, "%.1f%s", std::ceil(bytes * 10) / 10, units[u]);
    } else {
        std::snprintf(buf, sizeof buf, "%.0f%s", std::ceil(bytes), units[u]);
    }
    return buf;
}

std::string join(const std::vector<std::string> &v, const std::string &sep)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            r += sep;
        }
        r += v[i];
    }
    return r;
}

static void append_utf8(std::string &o, uint32_t cp)
{
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string unescape(const std::string &s, bool *stop)
{
    std::string o;
    if (stop) {
        *stop = false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            o += s[i];
            continue;
        }
        const char c = s[++i];
        switch (c) {
        case 'a': o += '\a'; break;
        case 'b': o += '\b'; break;
        case 'e': o += '\x1b'; break;
        case 'f': o += '\f'; break;
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'v': o += '\v'; break;
        case '\\': o += '\\'; break;
        case 'c':
            if (stop) {
                *stop = true;
            }
            return o;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            // \0NNN (echo) or \NNN (printf): up to 3 octal digits after an optional leading 0
            size_t j = i;
            if (c == '0') {
                ++j;
            }
            int v = 0, n = 0;
            while (j < s.size() && n < 3 && s[j] >= '0' && s[j] <= '7') {
                v = v * 8 + (s[j] - '0');
                ++j;
                ++n;
            }
            if (c != '0') {
                v = 0, n = 0, j = i;
                while (j < s.size() && n < 3 && s[j] >= '0' && s[j] <= '7') {
                    v = v * 8 + (s[j] - '0');
                    ++j;
                    ++n;
                }
            }
            o += static_cast<char>(v);
            i = j - 1;
            break;
        }
        case 'x': {
            int v = 0, n = 0;
            size_t j = i + 1;
            while (j < s.size() && n < 2 && std::isxdigit(static_cast<unsigned char>(s[j]))) {
                v = v * 16 + std::stoi(std::string(1, s[j]), nullptr, 16);
                ++j;
                ++n;
            }
            if (n == 0) {
                o += "\\x";
            } else {
                o += static_cast<char>(v);
                i = j - 1;
            }
            break;
        }
        case 'u': case 'U': {
            const int digits = (c == 'u') ? 4 : 8;
            uint32_t v = 0;
            int n = 0;
            size_t j = i + 1;
            while (j < s.size() && n < digits && std::isxdigit(static_cast<unsigned char>(s[j]))) {
                v = v * 16 + static_cast<uint32_t>(std::stoi(std::string(1, s[j]), nullptr, 16));
                ++j;
                ++n;
            }
            if (n == 0) {
                o += '\\';
                o += c;
            } else {
                append_utf8(o, v);
                i = j - 1;
            }
            break;
        }
        default:
            o += '\\';
            o += c;
        }
    }
    return o;
}

std::string quote_shell(const std::string &s)
{
    if (!s.empty() && s.find_first_of(" \t\n'\"\\$`*?[]{}()<>|&;!#~") == std::string::npos) {
        return s;
    }
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') {
            r += "'\\''";
        } else {
            r += c;
        }
    }
    return r + "'";
}

std::string format_time(time_t t, const std::string &fmt, bool utc)
{
    std::tm tmv{};
    if (utc) {
        gmtime_r(&t, &tmv);
    } else {
        localtime_r(&t, &tmv);
    }
    char buf[512];
    const size_t n = std::strftime(buf, sizeof buf, fmt.c_str(), &tmv);
    return std::string(buf, n);
}

// ---------------------------------------------------------------------------------------------
// users / groups / modes
// ---------------------------------------------------------------------------------------------

bool lookup_user(const std::string &name, uid_t &uid)
{
    if (name.empty()) {
        return false;
    }
    if (const passwd *p = getpwnam(name.c_str())) {
        uid = p->pw_uid;
        return true;
    }
    uint64_t v;
    if (parse_u64(name, v) && v <= UINT32_MAX) {
        uid = static_cast<uid_t>(v);
        return true;
    }
    return false;
}

bool lookup_group(const std::string &name, gid_t &gid)
{
    if (name.empty()) {
        return false;
    }
    if (const group *g = getgrnam(name.c_str())) {
        gid = g->gr_gid;
        return true;
    }
    uint64_t v;
    if (parse_u64(name, v) && v <= UINT32_MAX) {
        gid = static_cast<gid_t>(v);
        return true;
    }
    return false;
}

std::string user_name(uid_t uid)
{
    if (const passwd *p = getpwuid(uid)) {
        return p->pw_name;
    }
    return std::to_string(uid);
}

std::string group_name(gid_t gid)
{
    if (const group *g = getgrgid(gid)) {
        return g->gr_name;
    }
    return std::to_string(gid);
}

bool parse_mode(const std::string &spec, mode_t cur, bool is_dir, mode_t &out)
{
    if (spec.empty()) {
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(spec[0]))) {
        uint64_t v;
        if (!parse_u64(spec, v, 8) || v > 07777) {
            return false;
        }
        out = static_cast<mode_t>(v);
        return true;
    }
    mode_t mode = cur & 07777;
    const mode_t umask_bits = [] { const mode_t m = ::umask(0); ::umask(m); return m; }();
    size_t i = 0;
    while (i < spec.size()) {
        mode_t who = 0;
        bool explicit_who = false;
        while (i < spec.size() && std::strchr("ugoa", spec[i])) {
            explicit_who = true;
            if (spec[i] == 'u') who |= S_ISUID | S_IRWXU;
            if (spec[i] == 'g') who |= S_ISGID | S_IRWXG;
            if (spec[i] == 'o') who |= S_IRWXO;
            if (spec[i] == 'a') who |= S_ISUID | S_ISGID | S_IRWXU | S_IRWXG | S_IRWXO;
            ++i;
        }
        if (!explicit_who) {
            who = S_IRWXU | S_IRWXG | S_IRWXO;
        }
        if (i >= spec.size() || !std::strchr("+-=", spec[i])) {
            return false;
        }
        while (i < spec.size() && std::strchr("+-=", spec[i])) {
            const char op = spec[i++];
            mode_t bits = 0;
            // permission letters or a copy of another class (u/g/o)
            if (i < spec.size() && std::strchr("ugo", spec[i])) {
                const char src = spec[i++];
                const mode_t m = mode;
                mode_t perm = (src == 'u') ? (m >> 6) & 7 : (src == 'g') ? (m >> 3) & 7 : m & 7;
                bits = (perm << 6) | (perm << 3) | perm;
            } else {
                while (i < spec.size() && std::strchr("rwxXst", spec[i])) {
                    switch (spec[i]) {
                    case 'r': bits |= S_IRUSR | S_IRGRP | S_IROTH; break;
                    case 'w': bits |= S_IWUSR | S_IWGRP | S_IWOTH; break;
                    case 'x': bits |= S_IXUSR | S_IXGRP | S_IXOTH; break;
                    case 'X':
                        if (is_dir || (cur & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                            bits |= S_IXUSR | S_IXGRP | S_IXOTH;
                        }
                        break;
                    case 's': bits |= S_ISUID | S_ISGID; break;
                    case 't': bits |= S_ISVTX; break;
                    }
                    ++i;
                }
            }
            mode_t mask = who;
            if (!explicit_who) {
                mask &= ~umask_bits;
            }
            const mode_t apply = bits & (mask | S_ISVTX);
            if (op == '+') {
                mode |= apply;
            } else if (op == '-') {
                mode &= ~apply;
            } else {
                mode = (mode & ~who) | apply;
            }
        }
        if (i < spec.size()) {
            if (spec[i] != ',') {
                return false;
            }
            ++i;
        }
    }
    out = mode;
    return true;
}

std::string mode_string(mode_t m)
{
    std::string s(10, '-');
    if (S_ISDIR(m)) s[0] = 'd';
    else if (S_ISLNK(m)) s[0] = 'l';
    else if (S_ISCHR(m)) s[0] = 'c';
    else if (S_ISBLK(m)) s[0] = 'b';
    else if (S_ISFIFO(m)) s[0] = 'p';
    else if (S_ISSOCK(m)) s[0] = 's';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        if (m & (0400 >> i)) {
            s[1 + i] = rwx[i];
        }
    }
    if (m & S_ISUID) s[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID) s[6] = (m & S_IXGRP) ? 's' : 'S';
    if (m & S_ISVTX) s[9] = (m & S_IXOTH) ? 't' : 'T';
    return s;
}

// ---------------------------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------------------------

std::string readlink_str(const std::string &path)
{
    std::string buf(256, '\0');
    for (;;) {
        const ssize_t n = ::readlink(path.c_str(), buf.data(), buf.size());
        if (n < 0) {
            return {};
        }
        if (static_cast<size_t>(n) < buf.size()) {
            buf.resize(static_cast<size_t>(n));
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::string canonicalize(const std::string &path, bool must_exist)
{
    char *r = ::realpath(path.c_str(), nullptr);
    if (r) {
        std::string s = r;
        std::free(r);
        return s;
    }
    if (must_exist) {
        return {};
    }
    // missing tail: canonicalize the longest existing prefix and append the rest lexically
    std::string head = path;
    std::string tail;
    while (!head.empty() && head != "/") {
        const size_t slash = head.find_last_of('/');
        tail = (slash == std::string::npos ? head : head.substr(slash)) + tail;
        head = (slash == std::string::npos) ? "" : (slash == 0 ? "/" : head.substr(0, slash));
        char *rh = ::realpath(head.empty() ? "." : head.c_str(), nullptr);
        if (rh) {
            std::string s = rh;
            std::free(rh);
            if (tail.front() != '/') {
                tail = "/" + tail;
            }
            return (s == "/" ? "" : s) + tail;
        }
    }
    return path;
}

} // namespace cu

// ---------------------------------------------------------------------------------------------
// date parsing
// ---------------------------------------------------------------------------------------------

namespace cu {

namespace {

bool add_relative(std::tm &tmv, long n, const std::string &unit_in)
{
    std::string u = unit_in;
    for (char &c : u) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!u.empty() && u.back() == 's') {
        u.pop_back();
    }
    if (u == "sec" || u == "second") tmv.tm_sec += static_cast<int>(n);
    else if (u == "min" || u == "minute") tmv.tm_min += static_cast<int>(n);
    else if (u == "hour") tmv.tm_hour += static_cast<int>(n);
    else if (u == "day") tmv.tm_mday += static_cast<int>(n);
    else if (u == "week") tmv.tm_mday += static_cast<int>(7 * n);
    else if (u == "fortnight") tmv.tm_mday += static_cast<int>(14 * n);
    else if (u == "month") tmv.tm_mon += static_cast<int>(n);
    else if (u == "year") tmv.tm_year += static_cast<int>(n);
    else return false;
    return true;
}

} // namespace

bool parse_date(const std::string &in, time_t now, time_t &out)
{
    std::string s = in;
    const size_t b = s.find_first_not_of(" \t");
    s = (b == std::string::npos) ? "" : s.substr(b, s.find_last_not_of(" \t") - b + 1);
    if (s.empty()) {
        return false;
    }
    if (s[0] == '@') {
        int64_t v;
        if (!parse_i64(s.substr(1), v)) {
            return false;
        }
        out = static_cast<time_t>(v);
        return true;
    }

    std::tm tmv{};
    localtime_r(&now, &tmv);
    tmv.tm_isdst = -1;
    bool utc = false;
    long tz_offset = 0;
    bool have_tz_offset = false;

    // tokenise
    std::vector<std::string> tok;
    {
        std::string cur;
        for (char c : s) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) {
                    tok.push_back(cur);
                    cur.clear();
                }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) {
            tok.push_back(cur);
        }
    }

    bool time_set = false;
    for (size_t i = 0; i < tok.size(); ++i) {
        std::string t = tok[i];
        std::string lower = t;
        for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        int y, mo, d, h, mi, sec = 0;
        char sep = 0;
        char zone[16] = {0};
        if (lower == "now") {
            continue;
        } else if (lower == "today") {
            tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
            time_set = true;
        } else if (lower == "yesterday") {
            tmv.tm_mday -= 1;
            tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
            time_set = true;
        } else if (lower == "tomorrow") {
            tmv.tm_mday += 1;
            tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
            time_set = true;
        } else if (lower == "midnight") {
            tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
            time_set = true;
        } else if (lower == "noon") {
            tmv.tm_hour = 12;
            tmv.tm_min = tmv.tm_sec = 0;
            time_set = true;
        } else if (lower == "utc" || lower == "gmt" || lower == "z") {
            utc = true;
        } else if (std::sscanf(t.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d%15s", &y, &mo, &d, &sep, &h, &mi, &sec, zone) >= 6 &&
                   (sep == 'T' || sep == 't')) {
            tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
            tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
            time_set = true;
            const std::string z = zone;
            if (z == "Z" || z == "z") {
                utc = true;
            } else if (!z.empty() && (z[0] == '+' || z[0] == '-')) {
                int zh = 0, zm = 0;
                if (std::sscanf(z.c_str() + 1, "%2d:%2d", &zh, &zm) >= 1 || std::sscanf(z.c_str() + 1, "%2d%2d", &zh, &zm) >= 1) {
                    tz_offset = (z[0] == '-' ? -1 : 1) * (zh * 3600L + zm * 60L);
                    have_tz_offset = true;
                }
            }
        } else if (std::sscanf(t.c_str(), "%4d-%2d-%2d", &y, &mo, &d) == 3 && t.size() == 10) {
            tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
            if (!time_set) {
                tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
            }
        } else if (std::sscanf(t.c_str(), "%2d:%2d:%2d", &h, &mi, &sec) == 3 || std::sscanf(t.c_str(), "%2d:%2d", &h, &mi) == 2) {
            if (std::sscanf(t.c_str(), "%2d:%2d:%2d", &h, &mi, &sec) != 3) {
                sec = 0;
            }
            tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
            time_set = true;
        } else if (lower == "ago") {
            return false; // handled with the preceding "N unit"
        } else {
            // "[+-]N unit [ago]"
            int64_t n;
            if (parse_i64(t[0] == '+' ? t.substr(1) : t, n) && i + 1 < tok.size()) {
                const bool ago = (i + 2 < tok.size() && tok[i + 2] == "ago");
                if (!add_relative(tmv, static_cast<long>(ago ? -n : n), tok[i + 1])) {
                    return false;
                }
                i += ago ? 2 : 1;
            } else if (lower.size() > 2 && (lower.back() == 'z') && std::sscanf(t.c_str(), "%4d%2d%2d", &y, &mo, &d) == 3) {
                return false;
            } else {
                return false;
            }
        }
    }

    time_t r;
    if (utc || have_tz_offset) {
        r = timegm(&tmv) - tz_offset;
    } else {
        r = mktime(&tmv);
    }
    if (r == static_cast<time_t>(-1)) {
        return false;
    }
    out = r;
    return true;
}

} // namespace cu
