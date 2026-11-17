#include <algorithm>
#include <cerrno>
#include <csignal>
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
// Text utilities, part 1: cat head tail tee tac nl wc cut paste join comm uniq tr expand unexpand fold fmt.
// (sort and pr are in cmd_text2.cpp together with the encoders.)
#include "commands.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace cu {

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

bool read_input(const std::string &name, std::string &data)
{
    if (!read_file(name, data)) {
        err_errno(name == "-" ? "standard input" : name);
        return false;
    }
    return true;
}

std::vector<std::string> operands(int argc, char **argv)
{
    std::vector<std::string> v(argv + optind, argv + argc);
    if (v.empty()) v.push_back("-");
    return v;
}

/// Lines with their terminators kept (so files without a final newline survive intact).
std::vector<std::string> lines_keep(const std::string &data, char sep = '\n')
{
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t e = data.find(sep, pos);
        if (e == std::string::npos) {
            v.push_back(data.substr(pos));
            break;
        }
        v.push_back(data.substr(pos, e - pos + 1));
        pos = e + 1;
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------------------------
int cmd_cat(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Concatenate FILE(s) to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"show-all", no_argument, nullptr, 'A'}, {"number-nonblank", no_argument, nullptr, 'b'},
                                {"show-ends", no_argument, nullptr, 'E'}, {"number", no_argument, nullptr, 'n'},
                                {"squeeze-blank", no_argument, nullptr, 's'}, {"show-tabs", no_argument, nullptr, 'T'},
                                {"show-nonprinting", no_argument, nullptr, 'v'}, kEnd};
    bool number = false, nonblank = false, ends = false, squeeze = false, tabs = false, nonprint = false;
    int c;
    while ((c = getopt_long(argc, argv, "AbeEnstTuv", lo, nullptr)) != -1) {
        switch (c) {
        case 'A': nonprint = ends = tabs = true; break;
        case 'b': nonblank = number = true; break;
        case 'e': nonprint = ends = true; break;
        case 'E': ends = true; break;
        case 'n': number = true; break;
        case 's': squeeze = true; break;
        case 't': nonprint = tabs = true; break;
        case 'T': tabs = true; break;
        case 'u': break;
        case 'v': nonprint = true; break;
        default: return 1;
        }
    }
    const bool plain = !(number || ends || squeeze || tabs || nonprint);
    int rc = 0;
    uint64_t lineno = 0;
    bool prev_blank = false;
    for (const std::string &f : operands(argc, argv)) {
        const int fd = f == "-" ? 0 : ::open(f.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            err_errno(f);
            rc = 1;
            continue;
        }
        if (plain) {
            char buf[1 << 16];
            for (;;) {
                const ssize_t n = ::read(fd, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { err_errno(f); rc = 1; break; }
                if (n == 0) break;
                flush_out();
                if (!write_all(1, buf, static_cast<size_t>(n))) {
                    if (errno != EPIPE) err_errno("write error");
                    if (fd) ::close(fd);
                    return 1;
                }
            }
        } else {
            std::string data;
            if (!read_fd(fd, data)) { err_errno(f); rc = 1; }
            for (const std::string &line : lines_keep(data)) {
                const bool nl = !line.empty() && line.back() == '\n';
                std::string body = nl ? line.substr(0, line.size() - 1) : line;
                const bool blank = body.empty();
                if (squeeze && blank && prev_blank) continue;
                prev_blank = blank;
                std::string o;
                if (number && !(nonblank && blank)) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%6llu\t", static_cast<unsigned long long>(++lineno));
                    o += b;
                }
                for (unsigned char ch : body) {
                    if (ch == '\t' && tabs) o += "^I";
                    else if (nonprint && ch != '\t' && (ch < 32 || ch == 127)) { o += '^'; o += static_cast<char>(ch == 127 ? '?' : ch + 64); }
                    else if (nonprint && ch >= 128) {
                        o += "M-";
                        const unsigned char l = ch - 128;
                        if (l < 32) { o += '^'; o += static_cast<char>(l + 64); }
                        else if (l == 127) o += "^?";
                        else o += static_cast<char>(l);
                    } else o += static_cast<char>(ch);
                }
                if (ends && nl) o += '$';
                if (nl) o += '\n';
                out(o);
            }
        }
        if (fd) ::close(fd);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct Count {
    bool from_end = false; // head -n -N / tail -n +N
    bool plus = false;
    uint64_t n = 10;
};

bool parse_count(const std::string &s, Count &c, bool allow_sign)
{
    std::string v = s;
    if (allow_sign && !v.empty() && (v[0] == '-' || v[0] == '+')) {
        c.from_end = v[0] == '-';
        c.plus = v[0] == '+';
        v = v.substr(1);
    }
    return parse_size(v, c.n);
}

} // namespace

int cmd_head(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Print the first 10 lines of each FILE to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"lines", required_argument, nullptr, 'n'}, {"bytes", required_argument, nullptr, 'c'},
                                {"quiet", no_argument, nullptr, 'q'}, {"silent", no_argument, nullptr, 'q'},
                                {"verbose", no_argument, nullptr, 'v'}, {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    Count cnt;
    bool bytes = false, quiet = false, verbose = false;
    char sep = '\n';
    int c;
    // "head -5 file" is the traditional spelling of -n 5
    std::vector<char *> av(argv, argv + argc);
    std::string legacy;
    if (argc > 1 && argv[1][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[1][1]))) {
        legacy = std::string("-n") + (argv[1] + 1);
        av[1] = legacy.data();
    }
    while ((c = getopt_long(argc, av.data(), "n:c:qvz", lo, nullptr)) != -1) {
        switch (c) {
        case 'n': bytes = false; if (!parse_count(optarg, cnt, true)) { err(std::string("invalid number of lines: '") + optarg + "'"); return 1; } break;
        case 'c': bytes = true; if (!parse_count(optarg, cnt, true)) { err(std::string("invalid number of bytes: '") + optarg + "'"); return 1; } break;
        case 'q': quiet = true; break;
        case 'v': verbose = true; break;
        case 'z': sep = '\0'; break;
        default: return 1;
        }
    }
    const auto files = [&] { std::vector<std::string> v(av.begin() + optind, av.end()); if (v.empty()) v.push_back("-"); return v; }();
    int rc = 0;
    bool first = true;
    for (const std::string &f : files) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        if ((files.size() > 1 && !quiet) || verbose) {
            out(std::string(first ? "" : "\n") + "==> " + (f == "-" ? "standard input" : f) + " <==\n");
        }
        first = false;
        if (bytes) {
            if (cnt.from_end) out(data.substr(0, data.size() > cnt.n ? data.size() - cnt.n : 0));
            else out(data.substr(0, cnt.n));
        } else {
            const auto lines = lines_keep(data, sep);
            size_t take = cnt.from_end ? (lines.size() > cnt.n ? lines.size() - cnt.n : 0) : std::min<size_t>(cnt.n, lines.size());
            for (size_t i = 0; i < take; ++i) out(lines[i]);
        }
    }
    return rc;
}

int cmd_tail(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Print the last 10 lines of each FILE to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"lines", required_argument, nullptr, 'n'}, {"bytes", required_argument, nullptr, 'c'},
                                {"quiet", no_argument, nullptr, 'q'}, {"silent", no_argument, nullptr, 'q'},
                                {"verbose", no_argument, nullptr, 'v'}, {"follow", optional_argument, nullptr, 'f'},
                                {"retry", no_argument, nullptr, 3}, {"sleep-interval", required_argument, nullptr, 's'},
                                {"pid", required_argument, nullptr, 4}, {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    Count cnt;
    bool bytes = false, quiet = false, verbose = false, follow = false, follow_name = false;
    double interval = 1.0;
    long pid = 0;
    char sep = '\n';
    std::vector<char *> av(argv, argv + argc);
    std::string legacy;
    if (argc > 1 && (argv[1][0] == '-' || argv[1][0] == '+') && std::isdigit(static_cast<unsigned char>(argv[1][1]))) {
        legacy = std::string("-n") + argv[1];
        av[1] = legacy.data();
    }
    int c;
    while ((c = getopt_long(argc, av.data(), "n:c:qvfFs:z", lo, nullptr)) != -1) {
        switch (c) {
        case 'n': bytes = false; if (!parse_count(optarg, cnt, true)) { err(std::string("invalid number of lines: '") + optarg + "'"); return 1; } cnt.from_end = false; break;
        case 'c': bytes = true; if (!parse_count(optarg, cnt, true)) { err(std::string("invalid number of bytes: '") + optarg + "'"); return 1; } cnt.from_end = false; break;
        case 'q': quiet = true; break;
        case 'v': verbose = true; break;
        case 'f': follow = true; if (optarg && std::string(optarg) == "name") follow_name = true; break;
        case 'F': follow = follow_name = true; break;
        case 3: break;
        case 's': parse_double(optarg, interval); break;
        case 4: pid = std::atol(optarg); break;
        case 'z': sep = '\0'; break;
        default: return 1;
        }
    }
    const auto files = [&] { std::vector<std::string> v(av.begin() + optind, av.end()); if (v.empty()) v.push_back("-"); return v; }();
    int rc = 0;
    bool first = true;
    std::vector<std::pair<std::string, off_t>> follow_state;
    for (const std::string &f : files) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        if ((files.size() > 1 && !quiet) || verbose) {
            out(std::string(first ? "" : "\n") + "==> " + (f == "-" ? "standard input" : f) + " <==\n");
        }
        first = false;
        if (bytes) {
            if (cnt.plus) out(cnt.n > 0 ? data.substr(std::min<size_t>(cnt.n - 1, data.size())) : data);
            else out(data.substr(data.size() > cnt.n ? data.size() - cnt.n : 0));
        } else {
            const auto lines = lines_keep(data, sep);
            size_t start = cnt.plus ? (cnt.n > 0 ? std::min<size_t>(cnt.n - 1, lines.size()) : 0)
                                    : (lines.size() > cnt.n ? lines.size() - cnt.n : 0);
            for (size_t i = start; i < lines.size(); ++i) out(lines[i]);
        }
        if (f != "-") follow_state.emplace_back(f, static_cast<off_t>(data.size()));
    }
    if (!follow || follow_state.empty()) return rc;

    flush_out();
    std::signal(SIGPIPE, SIG_DFL);
    for (;;) {
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return rc;
        for (auto &[name, offset] : follow_state) {
            struct stat st;
            if (::stat(name.c_str(), &st) != 0) continue;
            if (st.st_size < offset) offset = 0; // truncated or rotated (journald, logrotate)
            if (st.st_size == offset) continue;
            const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ::lseek(fd, offset, SEEK_SET);
            std::string more;
            read_fd(fd, more);
            ::close(fd);
            offset += static_cast<off_t>(more.size());
            if (follow_state.size() > 1 && !quiet) out("\n==> " + name + " <==\n");
            out(more);
        }
        flush_out();
        ::usleep(static_cast<useconds_t>(interval * 1e6));
    }
}

int cmd_tee(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Copy standard input to each FILE, and also to standard output.")) {
        return 0;
    }
    bool append = false;
    int c;
    static const option lo[] = {{"append", no_argument, nullptr, 'a'}, {"ignore-interrupts", no_argument, nullptr, 'i'},
                                {"output-error", optional_argument, nullptr, 1}, kEnd};
    while ((c = getopt_long(argc, argv, "aip", lo, nullptr)) != -1) {
        if (c == 'a') append = true;
        else if (c == 'i') std::signal(SIGINT, SIG_IGN);
        else if (c == 'p' || c == 1) {}
        else return 1;
    }
    std::vector<int> fds;
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "-") continue;
        const int fd = ::open(f.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) { err_errno(f); rc = 1; continue; }
        fds.push_back(fd);
    }
    char buf[1 << 16];
    for (;;) {
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (!write_all(1, buf, static_cast<size_t>(n))) { if (errno != EPIPE) err_errno("write error"); rc = 1; }
        for (int fd : fds) {
            if (!write_all(fd, buf, static_cast<size_t>(n))) { err_errno("write error"); rc = 1; }
        }
    }
    for (int fd : fds) ::close(fd);
    return rc;
}

int cmd_tac(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Write each FILE to standard output, last line first.")) {
        return 0;
    }
    static const option lo[] = {{"before", no_argument, nullptr, 'b'}, {"separator", required_argument, nullptr, 's'}, kEnd};
    bool before = false;
    std::string sep = "\n";
    int c;
    while ((c = getopt_long(argc, argv, "brs:", lo, nullptr)) != -1) {
        if (c == 'b') before = true;
        else if (c == 'r') {}
        else if (c == 's') sep = optarg;
        else return 1;
    }
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t e = data.find(sep, pos);
            if (e == std::string::npos) { parts.push_back(data.substr(pos)); break; }
            if (before) {
                if (e > pos) parts.push_back(data.substr(pos, e - pos));
                pos = e;
                const size_t n = data.find(sep, pos + sep.size());
                parts.push_back(data.substr(pos, (n == std::string::npos ? data.size() : n) - pos));
                pos = (n == std::string::npos ? data.size() : n);
            } else {
                parts.push_back(data.substr(pos, e - pos + sep.size()));
                pos = e + sep.size();
            }
        }
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) out(*it);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_nl(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Write each FILE to standard output, with line numbers added.")) {
        return 0;
    }
    static const option lo[] = {{"body-numbering", required_argument, nullptr, 'b'}, {"number-format", required_argument, nullptr, 'n'},
                                {"number-width", required_argument, nullptr, 'w'}, {"number-separator", required_argument, nullptr, 's'},
                                {"starting-line-number", required_argument, nullptr, 'v'}, {"line-increment", required_argument, nullptr, 'i'},
                                {"join-blank-lines", required_argument, nullptr, 'l'}, kEnd};
    std::string style = "t", format = "rn", sepstr = "\t";
    int width = 6;
    long long start = 1, inc = 1, join_blank = 1;
    int c;
    while ((c = getopt_long(argc, argv, "b:n:w:s:v:i:l:p", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': style = optarg; break;
        case 'n': format = optarg; break;
        case 'w': width = std::atoi(optarg); break;
        case 's': sepstr = optarg; break;
        case 'v': start = std::atoll(optarg); break;
        case 'i': inc = std::atoll(optarg); break;
        case 'l': join_blank = std::atoll(optarg); break;
        case 'p': break;
        default: return 1;
        }
    }
    long long n = start;
    int rc = 0, blank_run = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        for (const std::string &line : lines_keep(data)) {
            const bool has_nl = !line.empty() && line.back() == '\n';
            const std::string body = has_nl ? line.substr(0, line.size() - 1) : line;
            const bool blank = body.find_first_not_of(" \t") == std::string::npos && body.empty();
            bool number = false;
            if (style == "a") number = true;
            else if (style == "t") number = !blank;
            else if (style == "n") number = false;
            if (blank) {
                ++blank_run;
                if (style == "a" && join_blank > 1 && blank_run % join_blank != 0) number = false;
            } else {
                blank_run = 0;
            }
            if (number) {
                char b[64];
                if (format == "ln") std::snprintf(b, sizeof b, "%-*lld", width, n);
                else if (format == "rz") std::snprintf(b, sizeof b, "%0*lld", width, n);
                else std::snprintf(b, sizeof b, "%*lld", width, n);
                out(std::string(b) + sepstr + body + (has_nl ? "\n" : ""));
                n += inc;
            } else {
                out(std::string(static_cast<size_t>(width), ' ') + sepstr.substr(0, 0) + (blank ? "" : sepstr) + body + (has_nl ? "\n" : ""));
            }
        }
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_wc(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Print newline, word, and byte counts for each FILE.")) {
        return 0;
    }
    static const option lo[] = {{"bytes", no_argument, nullptr, 'c'}, {"chars", no_argument, nullptr, 'm'}, {"lines", no_argument, nullptr, 'l'},
                                {"words", no_argument, nullptr, 'w'}, {"max-line-length", no_argument, nullptr, 'L'},
                                {"files0-from", required_argument, nullptr, 1}, {"total", required_argument, nullptr, 2}, kEnd};
    bool bytes = false, chars = false, lines = false, words = false, maxl = false;
    std::string files0, total_mode = "auto";
    int c;
    while ((c = getopt_long(argc, argv, "cmlwL", lo, nullptr)) != -1) {
        switch (c) {
        case 'c': bytes = true; break;
        case 'm': chars = true; break;
        case 'l': lines = true; break;
        case 'w': words = true; break;
        case 'L': maxl = true; break;
        case 1: files0 = optarg; break;
        case 2: total_mode = optarg; break;
        default: return 1;
        }
    }
    if (!(bytes || chars || lines || words || maxl)) lines = words = bytes = true;
    std::vector<std::string> files;
    if (!files0.empty()) {
        std::string list;
        if (!read_input(files0, list)) return 1;
        for (const std::string &s : split_lines(list, '\0')) if (!s.empty()) files.push_back(s);
    } else {
        files = operands(argc, argv);
    }
    struct Totals { unsigned long long l = 0, w = 0, m = 0, b = 0, L = 0; };
    std::vector<std::pair<std::string, Totals>> results;
    Totals sum;
    int rc = 0;
    for (const std::string &f : files) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        Totals t;
        t.b = data.size();
        bool in_word = false;
        unsigned long long cur = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(data[i]);
            if (ch == '\n') { ++t.l; t.L = std::max(t.L, cur); cur = 0; }
            else if (ch == '\t') cur = (cur / 8 + 1) * 8;
            else if ((ch & 0xC0) != 0x80) ++cur;
            if ((ch & 0xC0) != 0x80) ++t.m;
            const bool space = std::isspace(ch) != 0;
            if (space) in_word = false;
            else if (!in_word) { in_word = true; ++t.w; }
        }
        t.L = std::max(t.L, cur);
        results.emplace_back(f, t);
        sum.l += t.l; sum.w += t.w; sum.m += t.m; sum.b += t.b; sum.L = std::max(sum.L, t.L);
    }
    const bool show_total = total_mode == "always" || total_mode == "only" || (total_mode == "auto" && results.size() > 1);
    unsigned long long biggest = 0;
    for (const auto &r : results) biggest = std::max({biggest, r.second.l, r.second.w, r.second.m, r.second.b, r.second.L});
    biggest = std::max(biggest, std::max(sum.b, sum.l));
    size_t width = std::to_string(biggest).size();
    const int columns = static_cast<int>(lines) + words + chars + bytes + maxl;
    if (columns == 1 && results.size() <= 1 && !show_total) width = 0;
    auto fmt = [&](const Totals &t, const std::string &name) {
        std::string s;
        auto add = [&](unsigned long long v) {
            std::string n = std::to_string(v);
            if (!s.empty()) s += ' ';
            s += std::string(width > n.size() ? width - n.size() : 0, ' ') + n;
        };
        if (lines) add(t.l);
        if (words) add(t.w);
        if (chars) add(t.m);
        if (bytes) add(t.b);
        if (maxl) add(t.L);
        if (!name.empty()) s += " " + name;
        return s + "\n";
    };
    if (total_mode != "only") {
        for (const auto &r : results) out(fmt(r.second, files0.empty() && r.first == "-" && files.size() == 1 ? "" : r.first));
    }
    if (show_total) out(fmt(sum, total_mode == "only" ? "" : "total"));
    return rc;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct Range {
    size_t lo, hi; // 1-based inclusive; hi == SIZE_MAX means "to the end"
};

bool parse_ranges(const std::string &s, std::vector<Range> &out_r)
{
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(',', pos);
        if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(pos, e - pos);
        if (item.empty()) return false;
        const size_t dash = item.find('-');
        Range r{1, SIZE_MAX};
        uint64_t v;
        if (dash == std::string::npos) {
            if (!parse_u64(item, v) || v == 0) return false;
            r = {v, v};
        } else {
            const std::string a = item.substr(0, dash), b = item.substr(dash + 1);
            if (!a.empty()) { if (!parse_u64(a, v) || v == 0) return false; r.lo = v; }
            if (!b.empty()) { if (!parse_u64(b, v) || v == 0) return false; r.hi = v; }
            if (a.empty() && b.empty()) return false;
            if (r.lo > r.hi) return false;
        }
        out_r.push_back(r);
        pos = e + 1;
    }
    return true;
}

bool in_ranges(const std::vector<Range> &rs, size_t idx, bool complement)
{
    bool in = false;
    for (const Range &r : rs) if (idx >= r.lo && idx <= r.hi) { in = true; break; }
    return in != complement;
}

} // namespace

int cmd_cut(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "OPTION... [FILE]...", "Print selected parts of lines from each FILE to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"bytes", required_argument, nullptr, 'b'}, {"characters", required_argument, nullptr, 'c'},
                                {"fields", required_argument, nullptr, 'f'}, {"delimiter", required_argument, nullptr, 'd'},
                                {"only-delimited", no_argument, nullptr, 's'}, {"output-delimiter", required_argument, nullptr, 1},
                                {"complement", no_argument, nullptr, 2}, {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    std::string list, delim = "\t", odelim;
    bool odelim_set = false, only_delim = false, complement = false;
    char mode = 0, eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "b:c:f:d:nsz", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': case 'c': case 'f': mode = static_cast<char>(c); list = optarg; break;
        case 'd': delim = optarg; if (delim.size() != 1) { err("the delimiter must be a single character"); return 1; } break;
        case 'n': break;
        case 's': only_delim = true; break;
        case 1: odelim = optarg; odelim_set = true; break;
        case 2: complement = true; break;
        case 'z': eol = '\0'; break;
        default: return 1;
        }
    }
    if (!mode) {
        err("you must specify a list of bytes, characters, or fields");
        return 1;
    }
    std::vector<Range> ranges;
    if (!parse_ranges(list, ranges)) {
        err("invalid byte, character or field list");
        return 1;
    }
    if (!odelim_set) odelim = mode == 'f' ? delim : "";
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        for (const std::string &line : split_lines(data, eol)) {
            if (mode == 'f') {
                if (line.find(delim[0]) == std::string::npos) {
                    if (!only_delim) out(line + eol);
                    continue;
                }
                std::string o;
                size_t idx = 1, pos = 0;
                bool first = true;
                while (pos <= line.size()) {
                    size_t e = line.find(delim[0], pos);
                    if (e == std::string::npos) e = line.size();
                    if (in_ranges(ranges, idx, complement)) {
                        if (!first) o += odelim;
                        o += line.substr(pos, e - pos);
                        first = false;
                    }
                    ++idx;
                    pos = e + 1;
                }
                out(o + eol);
            } else {
                std::string o;
                size_t idx = 1;
                bool prev_in = false;
                for (size_t i = 0; i < line.size(); ++idx) {
                    size_t len = 1;
                    if (mode == 'c') {
                        const unsigned char ch = static_cast<unsigned char>(line[i]);
                        len = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
                        len = std::min(len, line.size() - i);
                    }
                    const bool in = in_ranges(ranges, idx, complement);
                    if (in) {
                        if (odelim_set && !prev_in && !o.empty()) o += odelim;
                        o += line.substr(i, len);
                    }
                    prev_in = in;
                    i += len;
                }
                out(o + eol);
            }
        }
    }
    return rc;
}

int cmd_paste(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Write lines consisting of the sequentially corresponding lines from each FILE, separated by TABs.")) {
        return 0;
    }
    static const option lo[] = {{"delimiters", required_argument, nullptr, 'd'}, {"serial", no_argument, nullptr, 's'},
                                {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    std::string delims = "\t";
    bool serial = false;
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "d:sz", lo, nullptr)) != -1) {
        if (c == 'd') delims = unescape(optarg);
        else if (c == 's') serial = true;
        else if (c == 'z') eol = '\0';
        else return 1;
    }
    if (delims.empty()) delims = std::string(1, '\0');
    std::vector<std::vector<std::string>> cols;
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; cols.emplace_back(); continue; }
        cols.push_back(split_lines(data, eol));
    }
    if (serial) {
        for (const auto &col : cols) {
            std::string o;
            for (size_t i = 0; i < col.size(); ++i) {
                if (i) o += delims[(i - 1) % delims.size()];
                o += col[i];
            }
            out(o + eol);
        }
        return rc;
    }
    size_t rows = 0;
    for (const auto &col : cols) rows = std::max(rows, col.size());
    for (size_t r = 0; r < rows; ++r) {
        std::string o;
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) o += delims[(i - 1) % delims.size()];
            if (r < cols[i].size()) o += cols[i][r];
        }
        out(o + eol);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_join(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE1 FILE2", "For each pair of input lines with identical join fields, write a line to standard output.")) {
        return 0;
    }
    int f1 = 1, f2 = 1;
    std::string tsep, empty, format;
    bool icase = false, print1 = false, print2 = false, only1 = false, only2 = false;
    int c;
    while ((c = getopt(argc, argv, "1:2:a:e:ij:o:t:v:")) != -1) {
        switch (c) {
        case '1': f1 = std::atoi(optarg); break;
        case '2': f2 = std::atoi(optarg); break;
        case 'j': f1 = f2 = std::atoi(optarg); break;
        case 'a': (std::atoi(optarg) == 1 ? print1 : print2) = true; break;
        case 'v': (std::atoi(optarg) == 1 ? only1 : only2) = true; break;
        case 'e': empty = optarg; break;
        case 'i': icase = true; break;
        case 'o': format = optarg; break;
        case 't': tsep = optarg; break;
        default: return 1;
        }
    }
    if (argc - optind != 2) {
        usage_error(argc - optind < 2 ? "missing operand" : "extra operand");
        return 1;
    }
    std::string d1, d2;
    if (!read_input(argv[optind], d1) || !read_input(argv[optind + 1], d2)) return 1;
    auto fields = [&](const std::string &line) {
        std::vector<std::string> v;
        if (!tsep.empty()) {
            size_t pos = 0;
            while (pos <= line.size()) {
                size_t e = line.find(tsep[0], pos);
                if (e == std::string::npos) e = line.size();
                v.push_back(line.substr(pos, e - pos));
                pos = e + 1;
            }
        } else {
            std::istringstream ss(line);
            std::string w;
            while (ss >> w) v.push_back(w);
        }
        return v;
    };
    auto key_of = [&](const std::vector<std::string> &f, int idx) {
        std::string k = (idx >= 1 && static_cast<size_t>(idx) <= f.size()) ? f[idx - 1] : "";
        if (icase) for (char &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return k;
    };
    const std::string ofs = tsep.empty() ? " " : tsep;
    std::multimap<std::string, std::vector<std::string>> right;
    std::vector<std::pair<std::string, std::vector<std::string>>> right_order;
    for (const std::string &l : split_lines(d2)) {
        auto f = fields(l);
        right.emplace(key_of(f, f2), f);
        right_order.emplace_back(key_of(f, f2), f);
    }
    std::set<std::string> matched_keys;
    std::vector<std::vector<std::string>> unmatched2;
    auto emit = [&](const std::vector<std::string> &a, const std::vector<std::string> &b) {
        std::vector<std::string> o;
        if (!format.empty()) {
            std::istringstream ss(format);
            std::string tok;
            std::string spec = format;
            std::replace(spec.begin(), spec.end(), ',', ' ');
            std::istringstream sp(spec);
            while (sp >> tok) {
                if (tok == "0") { o.push_back(key_of(!a.empty() ? a : b, !a.empty() ? f1 : f2)); continue; }
                const size_t dot = tok.find('.');
                const int file = std::atoi(tok.substr(0, dot).c_str());
                const size_t idx = static_cast<size_t>(std::atoi(tok.substr(dot + 1).c_str()));
                const auto &src = file == 1 ? a : b;
                o.push_back(idx >= 1 && idx <= src.size() ? src[idx - 1] : empty);
            }
        } else {
            const auto &base = !a.empty() ? a : b;
            const int bi = !a.empty() ? f1 : f2;
            o.push_back(bi >= 1 && static_cast<size_t>(bi) <= base.size() ? base[bi - 1] : empty);
            for (size_t i = 0; i < a.size(); ++i) if (static_cast<int>(i) + 1 != f1) o.push_back(a[i]);
            for (size_t i = 0; i < b.size(); ++i) if (static_cast<int>(i) + 1 != f2) o.push_back(b[i]);
        }
        out(join(o, ofs) + "\n");
    };
    for (const std::string &l : split_lines(d1)) {
        const auto a = fields(l);
        const std::string k = key_of(a, f1);
        const auto range = right.equal_range(k);
        if (range.first == range.second) {
            if (print1 || only1) emit(a, {});
            continue;
        }
        matched_keys.insert(k);
        if (only1 || only2) continue;
        for (auto it = range.first; it != range.second; ++it) emit(a, it->second);
    }
    if (print2 || only2) {
        for (const auto &r : right_order) {
            if (!matched_keys.count(r.first)) emit({}, r.second);
        }
    }
    return 0;
}

int cmd_comm(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE1 FILE2", "Compare sorted files FILE1 and FILE2 line by line.")) {
        return 0;
    }
    static const option lo[] = {{"output-delimiter", required_argument, nullptr, 1}, {"total", no_argument, nullptr, 2},
                                {"zero-terminated", no_argument, nullptr, 'z'}, {"check-order", no_argument, nullptr, 3},
                                {"nocheck-order", no_argument, nullptr, 4}, kEnd};
    bool s1 = false, s2 = false, s3 = false, total = false;
    std::string delim = "\t";
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "123z", lo, nullptr)) != -1) {
        switch (c) {
        case '1': s1 = true; break;
        case '2': s2 = true; break;
        case '3': s3 = true; break;
        case 1: delim = optarg; break;
        case 2: total = true; break;
        case 'z': eol = '\0'; break;
        case 3: case 4: break;
        default: return 1;
        }
    }
    if (argc - optind != 2) {
        usage_error(argc - optind < 2 ? "missing operand" : "extra operand");
        return 1;
    }
    std::string d1, d2;
    if (!read_input(argv[optind], d1) || !read_input(argv[optind + 1], d2)) return 1;
    const auto a = split_lines(d1, eol), b = split_lines(d2, eol);
    size_t i = 0, j = 0, c1 = 0, c2 = 0, c3 = 0;
    auto line = [&](int col, const std::string &t) {
        std::string pre;
        if (col >= 2 && !s1) pre += delim;
        if (col >= 3 && !s2) pre += delim;
        out(pre + t + eol);
    };
    while (i < a.size() || j < b.size()) {
        const int cmp = i >= a.size() ? 1 : j >= b.size() ? -1 : std::strcoll(a[i].c_str(), b[j].c_str());
        if (cmp < 0) { ++c1; if (!s1) line(1, a[i]); ++i; }
        else if (cmp > 0) { ++c2; if (!s2) line(2, b[j]); ++j; }
        else { ++c3; if (!s3) line(3, a[i]); ++i; ++j; }
    }
    if (total) out(std::to_string(c1) + delim + std::to_string(c2) + delim + std::to_string(c3) + delim + "total" + eol);
    return 0;
}

int cmd_uniq(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [INPUT [OUTPUT]]", "Filter adjacent matching lines from INPUT, writing to OUTPUT.")) {
        return 0;
    }
    static const option lo[] = {{"count", no_argument, nullptr, 'c'}, {"repeated", no_argument, nullptr, 'd'},
                                {"all-repeated", optional_argument, nullptr, 'D'}, {"ignore-case", no_argument, nullptr, 'i'},
                                {"skip-fields", required_argument, nullptr, 'f'}, {"skip-chars", required_argument, nullptr, 's'},
                                {"check-chars", required_argument, nullptr, 'w'}, {"unique", no_argument, nullptr, 'u'},
                                {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    bool count = false, dup_only = false, all_dup = false, uniq_only = false, icase = false;
    size_t skip_f = 0, skip_c = 0, check = SIZE_MAX;
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "cdDf:ius:w:z", lo, nullptr)) != -1) {
        switch (c) {
        case 'c': count = true; break;
        case 'd': dup_only = true; break;
        case 'D': all_dup = true; break;
        case 'f': skip_f = static_cast<size_t>(std::atol(optarg)); break;
        case 'i': icase = true; break;
        case 's': skip_c = static_cast<size_t>(std::atol(optarg)); break;
        case 'u': uniq_only = true; break;
        case 'w': check = static_cast<size_t>(std::atol(optarg)); break;
        case 'z': eol = '\0'; break;
        default: return 1;
        }
    }
    std::string data;
    const std::string in = optind < argc ? argv[optind] : "-";
    if (!read_input(in, data)) return 1;
    auto key = [&](const std::string &l) {
        size_t p = 0;
        for (size_t f = 0; f < skip_f; ++f) {
            while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) ++p;
            while (p < l.size() && l[p] != ' ' && l[p] != '\t') ++p;
        }
        p = std::min(l.size(), p + skip_c);
        std::string k = l.substr(p, check == SIZE_MAX ? std::string::npos : check);
        if (icase) for (char &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return k;
    };
    std::string result;
    const auto lines = split_lines(data, eol);
    size_t i = 0;
    while (i < lines.size()) {
        size_t j = i + 1;
        const std::string k = key(lines[i]);
        while (j < lines.size() && key(lines[j]) == k) ++j;
        const size_t n = j - i;
        const bool show = all_dup ? n > 1 : dup_only ? n > 1 : uniq_only ? n == 1 : true;
        if (show) {
            if (all_dup) {
                for (size_t m = i; m < j; ++m) result += lines[m] + eol;
            } else if (count) {
                char b[32];
                std::snprintf(b, sizeof b, "%7zu ", n);
                result += b + lines[i] + eol;
            } else {
                result += lines[i] + eol;
            }
        }
        i = j;
    }
    if (optind + 1 < argc) {
        const int fd = ::open(argv[optind + 1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !write_all(fd, result)) { err_errno(argv[optind + 1]); return 1; }
        ::close(fd);
    } else {
        out(result);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
namespace {

std::string expand_set(const std::string &s_in, bool for_set2, size_t target_len, std::vector<unsigned char> &outv)
{
    (void)for_set2;
    (void)target_len;
    const std::string s = s_in;
    size_t i = 0;
    auto next_char = [&](int &ch) -> bool {
        if (i >= s.size()) return false;
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'a': ch = '\a'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'v': ch = '\v'; break;
            case '\\': ch = '\\'; break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                int v = 0, n = 0;
                while (i < s.size() && n < 3 && s[i] >= '0' && s[i] <= '7') { v = v * 8 + (s[i] - '0'); ++i; ++n; }
                --i;
                ch = v;
                break;
            }
            default: ch = static_cast<unsigned char>(s[i]);
            }
            ++i;
            return true;
        }
        ch = static_cast<unsigned char>(s[i++]);
        return true;
    };
    while (i < s.size()) {
        if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == ':') {
            const size_t e = s.find(":]", i + 2);
            if (e != std::string::npos) {
                const std::string cls = s.substr(i + 2, e - i - 2);
                for (int ch = 0; ch < 256; ++ch) {
                    bool in = false;
                    if (cls == "alpha") in = std::isalpha(ch); else if (cls == "digit") in = std::isdigit(ch);
                    else if (cls == "alnum") in = std::isalnum(ch); else if (cls == "upper") in = std::isupper(ch);
                    else if (cls == "lower") in = std::islower(ch); else if (cls == "space") in = std::isspace(ch);
                    else if (cls == "blank") in = (ch == ' ' || ch == '\t'); else if (cls == "punct") in = std::ispunct(ch);
                    else if (cls == "print") in = std::isprint(ch); else if (cls == "graph") in = std::isgraph(ch);
                    else if (cls == "cntrl") in = std::iscntrl(ch); else if (cls == "xdigit") in = std::isxdigit(ch);
                    else return "invalid character class '" + cls + "'";
                    if (in) outv.push_back(static_cast<unsigned char>(ch));
                }
                i = e + 2;
                continue;
            }
        }
        if (s[i] == '[' && i + 3 < s.size() && s[i + 1] == '=' ) {
            const size_t e = s.find("=]", i + 2);
            if (e != std::string::npos) {
                outv.push_back(static_cast<unsigned char>(s[i + 2]));
                i = e + 2;
                continue;
            }
        }
        if (s[i] == '[' && i + 1 < s.size()) {
            // [c*n] repeat
            const size_t star = s.find('*', i + 1);
            const size_t close = s.find(']', i + 1);
            if (star != std::string::npos && close != std::string::npos && star < close && star == i + 2) {
                const unsigned char ch = static_cast<unsigned char>(s[i + 1]);
                const std::string n = s.substr(star + 1, close - star - 1);
                size_t count = n.empty() ? (target_len > outv.size() ? target_len - outv.size() : 0) : static_cast<size_t>(std::strtoul(n.c_str(), nullptr, n.size() > 1 && n[0] == '0' ? 8 : 10));
                outv.insert(outv.end(), count, ch);
                i = close + 1;
                continue;
            }
        }
        int a;
        if (!next_char(a)) break;
        if (i + 1 < s.size() && s[i] == '-') {
            const size_t save = i;
            ++i;
            int b;
            if (next_char(b)) {
                if (b < a) return std::string("range-endpoints of '") + static_cast<char>(a) + "-" + static_cast<char>(b) + "' are in reverse collating sequence order";
                for (int ch = a; ch <= b; ++ch) outv.push_back(static_cast<unsigned char>(ch));
                continue;
            }
            i = save;
        }
        outv.push_back(static_cast<unsigned char>(a));
    }
    return "";
}

} // namespace

int cmd_tr(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... SET1 [SET2]", "Translate, squeeze, and/or delete characters from standard input, writing to standard output.")) {
        return 0;
    }
    bool complement = false, del = false, squeeze = false, truncate = false;
    int c;
    while ((c = getopt(argc, argv, "cCdst")) != -1) {
        switch (c) {
        case 'c': case 'C': complement = true; break;
        case 'd': del = true; break;
        case 's': squeeze = true; break;
        case 't': truncate = true; break;
        default: return 1;
        }
    }
    const int nsets = argc - optind;
    if (nsets < 1) { usage_error("missing operand"); return 1; }
    if (nsets > 2 || (nsets == 2 && del && !squeeze)) { usage_error("extra operand '" + std::string(argv[optind + 2 > argc - 1 ? argc - 1 : optind + 1]) + "'"); return 1; }
    if (nsets == 1 && !del && !squeeze) { usage_error("missing operand after '" + std::string(argv[optind]) + "'"); return 1; }
    std::vector<unsigned char> set1, set2;
    std::string e = expand_set(argv[optind], false, 0, set1);
    if (!e.empty()) { err(e); return 1; }
    if (nsets == 2) {
        e = expand_set(argv[optind + 1], true, set1.size(), set2);
        if (!e.empty()) { err(e); return 1; }
    }
    if (complement) {
        std::set<unsigned char> in(set1.begin(), set1.end());
        set1.clear();
        for (int ch = 0; ch < 256; ++ch) if (!in.count(static_cast<unsigned char>(ch))) set1.push_back(static_cast<unsigned char>(ch));
    }
    unsigned char map[256];
    for (int ch = 0; ch < 256; ++ch) map[ch] = static_cast<unsigned char>(ch);
    bool in1[256] = {false}, in2[256] = {false};
    for (unsigned char ch : set1) in1[ch] = true;
    if (nsets == 2 && !del) {
        if (set2.empty()) { err("when not truncating set1, string2 must be non-empty"); return 1; }
        std::vector<unsigned char> s1 = set1;
        if (truncate && s1.size() > set2.size()) s1.resize(set2.size());
        for (size_t k = 0; k < s1.size(); ++k) {
            map[s1[k]] = k < set2.size() ? set2[k] : set2.back();
        }
        for (unsigned char ch : set2) in2[ch] = true;
    }
    if (nsets == 1 && squeeze && !del) for (unsigned char ch : set1) in2[ch] = true;
    if (nsets == 2 && del && squeeze) for (unsigned char ch : set2) in2[ch] = true;
    std::string data;
    if (!read_fd(0, data)) { err_errno("read error"); return 1; }
    std::string o;
    o.reserve(data.size());
    int last = -1;
    for (unsigned char ch : data) {
        if (del && in1[ch]) continue;
        unsigned char t = (nsets == 2 && !del) ? map[ch] : ch;
        if (squeeze && in2[t] && last == t) continue;
        o += static_cast<char>(t);
        last = t;
    }
    out(o);
    return 0;
}

// ---------------------------------------------------------------------------------------------
namespace {

std::vector<size_t> parse_tabs(const std::string &s, bool &ok)
{
    std::vector<size_t> t;
    ok = true;
    std::string tok;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',' || s[i] == ' ') {
            if (!tok.empty()) {
                uint64_t v;
                if (!parse_u64(tok, v)) { ok = false; return t; }
                t.push_back(static_cast<size_t>(v));
                tok.clear();
            }
        } else tok += s[i];
    }
    return t;
}

size_t next_tab(const std::vector<size_t> &tabs, size_t col)
{
    if (tabs.size() == 1) return (col / tabs[0] + 1) * tabs[0];
    for (size_t t : tabs) if (t > col) return t;
    return col + 1;
}

} // namespace

int cmd_expand(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Convert tabs in each FILE to spaces, writing to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"initial", no_argument, nullptr, 'i'}, {"tabs", required_argument, nullptr, 't'}, kEnd};
    std::vector<size_t> tabs = {8};
    bool initial = false;
    int c;
    while ((c = getopt_long(argc, argv, "it:0123456789,", lo, nullptr)) != -1) {
        if (c == 'i') initial = true;
        else if (c == 't') { bool ok; tabs = parse_tabs(optarg, ok); if (!ok || tabs.empty() || tabs[0] == 0) { err(std::string("tab size invalid: '") + optarg + "'"); return 1; } }
        else if (std::isdigit(c) || c == ',') {}
        else return 1;
    }
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        std::string o;
        size_t col = 0;
        bool leading = true;
        for (char ch : data) {
            if (ch == '\t' && (!initial || leading)) {
                const size_t n = next_tab(tabs, col);
                o += std::string(n - col, ' ');
                col = n;
            } else if (ch == '\n') {
                o += ch; col = 0; leading = true;
            } else if (ch == '\b') {
                o += ch; if (col) --col;
            } else {
                if (ch != ' ') leading = false;
                o += ch; ++col;
            }
        }
        out(o);
    }
    return rc;
}

int cmd_unexpand(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Convert blanks in each FILE to tabs, writing to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"all", no_argument, nullptr, 'a'}, {"first-only", no_argument, nullptr, 1}, {"tabs", required_argument, nullptr, 't'}, kEnd};
    std::vector<size_t> tabs = {8};
    bool all = false;
    int c;
    while ((c = getopt_long(argc, argv, "at:", lo, nullptr)) != -1) {
        if (c == 'a') all = true;
        else if (c == 1) all = false;
        else if (c == 't') { bool ok; tabs = parse_tabs(optarg, ok); all = true; if (!ok || tabs.empty() || tabs[0] == 0) { err(std::string("tab size invalid: '") + optarg + "'"); return 1; } }
        else return 1;
    }
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        std::string o;
        for (const std::string &line : lines_keep(data)) {
            size_t col = 0, i = 0;
            std::string cur;
            size_t run_start = 0;
            bool in_run = false, leading = true, run_tab = false;
            auto flush_run = [&](size_t end_col) {
                if (!in_run) return;
                size_t pos = run_start;
                while (pos < end_col) {
                    const size_t nt = next_tab(tabs, pos);
                    if (nt <= end_col && (nt - pos >= 2 || run_tab)) {
                        cur += '	';
                        pos = nt;
                    } else {
                        cur += std::string(end_col - pos, ' ');
                        pos = end_col;
                    }
                }
                in_run = false;
                run_tab = false;
            };
            for (; i < line.size(); ++i) {
                const char ch = line[i];
                if (ch == ' ' && (all || leading)) {
                    if (!in_run) { in_run = true; run_start = col; }
                    ++col;
                } else if (ch == '\t' && (all || leading)) {
                    if (!in_run) { in_run = true; run_start = col; }
                    run_tab = true;
                    col = next_tab(tabs, col);
                } else {
                    flush_run(col);
                    if (ch != ' ' && ch != '\t') leading = false;
                    cur += ch;
                    if (ch == '\n') col = 0; else ++col;
                }
            }
            flush_run(col);
            out(cur);
        }
    }
    return rc;
}

int cmd_fold(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Wrap input lines in each FILE, writing to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"bytes", no_argument, nullptr, 'b'}, {"spaces", no_argument, nullptr, 's'}, {"width", required_argument, nullptr, 'w'}, kEnd};
    size_t width = 80;
    bool spaces = false;
    int c;
    while ((c = getopt_long(argc, argv, "bsw:", lo, nullptr)) != -1) {
        if (c == 'b') {}
        else if (c == 's') spaces = true;
        else if (c == 'w') { uint64_t v; if (!parse_u64(optarg, v) || v == 0) { err(std::string("invalid number of columns: '") + optarg + "'"); return 1; } width = static_cast<size_t>(v); }
        else return 1;
    }
    int rc = 0;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        std::string o, line;
        size_t col = 0;
        auto flush_line = [&] { o += line; line.clear(); col = 0; };
        for (char ch : data) {
            if (ch == '\n') { line += ch; flush_line(); continue; }
            size_t w = 1;
            if (ch == '\t') w = 8 - (col % 8);
            else if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80) w = 0;
            if (col + w > width) {
                if (spaces) {
                    const size_t sp = line.find_last_of(" \t");
                    if (sp != std::string::npos) {
                        const std::string rest = line.substr(sp + 1);
                        o += line.substr(0, sp + 1) + "\n";
                        line = rest;
                        col = 0;
                        for (char r : line) col += (r == '\t') ? 8 - (col % 8) : 1;
                    } else {
                        o += line + "\n";
                        line.clear();
                        col = 0;
                    }
                } else {
                    o += line + "\n";
                    line.clear();
                    col = 0;
                }
            }
            line += ch;
            col += w;
        }
        o += line;
        out(o);
    }
    return rc;
}

int cmd_fmt(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-WIDTH] [OPTION]... [FILE]...", "Reformat each paragraph in the FILE(s), writing to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"width", required_argument, nullptr, 'w'}, {"split-only", no_argument, nullptr, 's'},
                                {"uniform-spacing", no_argument, nullptr, 'u'}, {"prefix", required_argument, nullptr, 'p'},
                                {"goal", required_argument, nullptr, 'g'}, {"crown-margin", no_argument, nullptr, 'c'}, {"tagged-paragraph", no_argument, nullptr, 't'}, kEnd};
    size_t width = 75;
    std::string prefix;
    bool split_only = false, uniform = false;
    std::vector<char *> av(argv, argv + argc);
    std::string legacy;
    if (argc > 1 && argv[1][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[1][1]))) {
        legacy = std::string("-w") + (argv[1] + 1);
        av[1] = legacy.data();
    }
    int c;
    while ((c = getopt_long(argc, av.data(), "w:suctp:g:", lo, nullptr)) != -1) {
        switch (c) {
        case 'w': width = static_cast<size_t>(std::atol(optarg)); break;
        case 's': split_only = true; break;
        case 'u': uniform = true; break;
        case 'p': prefix = optarg; break;
        case 'c': case 't': case 'g': break;
        default: return 1;
        }
    }
    int rc = 0;
    std::vector<std::string> files(av.begin() + optind, av.end());
    if (files.empty()) files.push_back("-");
    for (const std::string &f : files) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        std::vector<std::string> para;
        std::string indent;
        auto flush_para = [&] {
            if (para.empty()) return;
            std::string cur = prefix + indent;
            bool at_start = true;
            for (const std::string &w : para) {
                if (!at_start && cur.size() + 1 + w.size() > width) {
                    out(cur + "\n");
                    cur = prefix + indent;
                    at_start = true;
                }
                if (!at_start) cur += uniform && !cur.empty() && (cur.back() == '.' || cur.back() == '!' || cur.back() == '?') ? "  " : " ";
                cur += w;
                at_start = false;
            }
            out(cur + "\n");
            para.clear();
        };
        for (const std::string &raw : split_lines(data)) {
            std::string line = raw;
            if (!prefix.empty()) {
                if (line.rfind(prefix, 0) != 0) { flush_para(); out(raw + "\n"); continue; }
                line = line.substr(prefix.size());
            }
            if (line.find_first_not_of(" \t") == std::string::npos) { flush_para(); out(raw + "\n"); continue; }
            if (split_only) {
                // only split long lines, never join
                flush_para();
                std::string ind = line.substr(0, line.find_first_not_of(" \t"));
                std::istringstream ss(line);
                std::string w, cur = ind;
                bool at_start = true;
                while (ss >> w) {
                    if (!at_start && cur.size() + 1 + w.size() > width) { out(prefix + cur + "\n"); cur = ind; at_start = true; }
                    if (!at_start) cur += ' ';
                    cur += w;
                    at_start = false;
                }
                out(prefix + cur + "\n");
                continue;
            }
            if (para.empty()) indent = line.substr(0, line.find_first_not_of(" \t"));
            std::istringstream ss(line);
            std::string w;
            while (ss >> w) para.push_back(w);
        }
        flush_para();
    }
    return rc;
}

} // namespace cu
