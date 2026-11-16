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
// Text utilities, part 2: sort, pr, base64/base32/basenc, od, printf, csplit, split, ptx, shuf, tsort, numfmt.
#include "commands.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <limits>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <regex>
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

uint64_t random_u64()
{
    uint64_t v = 0;
    if (::getrandom(&v, sizeof v, 0) != static_cast<ssize_t>(sizeof v)) {
        v = static_cast<uint64_t>(std::rand()) * 6364136223846793005ULL + static_cast<uint64_t>(std::time(nullptr));
    }
    return v;
}

uint64_t random_below(uint64_t n)
{
    if (n <= 1) return 0;
    const uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    uint64_t v;
    do {
        v = random_u64();
    } while (v >= limit);
    return v % n;
}

} // namespace

// =============================================================================================
// sort
// =============================================================================================
namespace {

struct KeyDef {
    size_t f1 = 1, c1 = 0, f2 = 0, c2 = 0; // f2 == 0: to end of line
    bool skip_b1 = false, skip_b2 = false;
    bool numeric = false, general = false, human = false, month = false, version = false, random = false, dict = false,
         fold = false, nonprint = false, reverse = false;
    bool own_flags = false; // flags given on the key itself (otherwise the global ones apply)
};

struct SortCtx {
    std::vector<KeyDef> keys;
    KeyDef global;
    char tsep = 0;
    bool stable = false, unique = false, bytewise = false;
    uint64_t salt = 0;
};

size_t field_begin(const std::string &s, size_t fnum, char tsep)
{
    size_t pos = 0;
    for (size_t f = 1; f < fnum; ++f) {
        if (tsep) {
            const size_t e = s.find(tsep, pos);
            if (e == std::string::npos) return s.size();
            pos = e + 1;
        } else {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') ++pos;
        }
    }
    return pos;
}

std::string extract_key(const SortCtx &c, const KeyDef &k, const std::string &line)
{
    size_t b = field_begin(line, k.f1, c.tsep);
    if (k.skip_b1) while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
    b = std::min(line.size(), b + (k.c1 ? k.c1 - 1 : 0));
    size_t e = line.size();
    if (k.f2 != 0) {
        const size_t fe = field_begin(line, k.f2 + 1, c.tsep);
        size_t fb = field_begin(line, k.f2, c.tsep);
        if (k.c2) {
            if (k.skip_b2) while (fb < line.size() && (line[fb] == ' ' || line[fb] == '\t')) ++fb;
            e = std::min(line.size(), fb + k.c2);
        } else {
            e = fe;
            if (!c.tsep && e > fb && e < line.size() + 1) { /* field end already stops before the next field's blanks */ }
            if (c.tsep && e > 0 && e <= line.size() && fe != line.size() && line[fe - 1] == c.tsep) --e;
        }
    }
    if (e < b) e = b;
    return line.substr(b, e - b);
}

long double parse_human(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    char *end = nullptr;
    long double v = std::strtold(s.c_str() + i, &end);
    if (end == s.c_str() + i) return 0;
    if (*end) {
        static const char *sfx = "KMGTPEZY";
        const char *p = std::strchr(sfx, std::toupper(static_cast<unsigned char>(*end)));
        if (p && *p) for (int n = 0; n <= p - sfx; ++n) v *= 1024;
    }
    return v;
}

int month_of(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    static const char *m[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    std::string t = s.substr(i, 3);
    for (char &ch : t) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (int k = 0; k < 12; ++k) if (t == m[k]) return k + 1;
    return 0;
}

int version_cmp(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
            if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
            const int c = na.compare(nb);
            if (c) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]) ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

int compare_key(const SortCtx &c, const KeyDef &k, const std::string &la, const std::string &lb)
{
    std::string a = extract_key(c, k, la), b = extract_key(c, k, lb);
    int r = 0;
    if (k.numeric || k.general || k.human) {
        long double x, y;
        if (k.human) { x = parse_human(a); y = parse_human(b); }
        else if (k.general) {
            char *e1, *e2;
            const char *pa = a.c_str(), *pb = b.c_str();
            while (*pa == ' ' || *pa == '\t') ++pa;
            while (*pb == ' ' || *pb == '\t') ++pb;
            x = std::strtold(pa, &e1);
            y = std::strtold(pb, &e2);
            if (e1 == pa) x = -std::numeric_limits<long double>::infinity();
            if (e2 == pb) y = -std::numeric_limits<long double>::infinity();
        } else {
            // plain -n: digits, one decimal point, optional minus; thousands separators are not special
            auto num = [](const std::string &s) {
                size_t i = 0;
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                size_t j = i;
                if (j < s.size() && s[j] == '-') ++j;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
                if (j < s.size() && s[j] == '.') { ++j; while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j; }
                if (j == i || (j == i + 1 && s[i] == '-')) return 0.0L;
                return std::strtold(s.substr(i, j - i).c_str(), nullptr);
            };
            x = num(a);
            y = num(b);
        }
        r = x < y ? -1 : (x > y ? 1 : 0);
    } else if (k.month) {
        const int x = month_of(a), y = month_of(b);
        r = x < y ? -1 : (x > y ? 1 : 0);
    } else if (k.version) {
        r = version_cmp(a, b);
    } else if (k.random) {
        const uint64_t x = std::hash<std::string>{}(a) ^ c.salt, y = std::hash<std::string>{}(b) ^ c.salt;
        r = x < y ? -1 : (x > y ? 1 : 0);
        if (r == 0) r = a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
    } else {
        if (k.dict || k.nonprint || k.fold) {
            auto prep = [&](std::string &s) {
                std::string o;
                for (unsigned char ch : s) {
                    if (k.dict && !(std::isalnum(ch) || ch == ' ' || ch == '\t')) continue;
                    if (k.nonprint && !std::isprint(ch)) continue;
                    o += static_cast<char>(k.fold ? std::toupper(ch) : ch);
                }
                s = std::move(o);
            };
            prep(a);
            prep(b);
        }
        r = c.bytewise ? a.compare(b) : std::strcoll(a.c_str(), b.c_str());
        r = r < 0 ? -1 : (r > 0 ? 1 : 0);
    }
    return k.reverse ? -r : r;
}

int compare_lines(const SortCtx &c, const std::string &a, const std::string &b, bool last_resort)
{
    if (c.keys.empty()) {
        const int r = compare_key(c, c.global, a, b);
        if (r != 0 || c.stable || !last_resort) return r;
        return 0;
    }
    for (const KeyDef &k : c.keys) {
        const int r = compare_key(c, k, a, b);
        if (r != 0) return r;
    }
    if (c.stable || !last_resort) return 0;
    KeyDef whole;
    whole.reverse = c.global.reverse;
    const int r = c.bytewise ? a.compare(b) : std::strcoll(a.c_str(), b.c_str());
    return whole.reverse ? (r > 0 ? -1 : (r < 0 ? 1 : 0)) : (r < 0 ? -1 : (r > 0 ? 1 : 0));
}

bool parse_key(const std::string &spec, KeyDef &k, const KeyDef &global)
{
    k = KeyDef();
    const size_t comma = spec.find(',');
    const std::string a = spec.substr(0, comma), b = comma == std::string::npos ? "" : spec.substr(comma + 1);
    auto parse_pos = [&](const std::string &s, size_t &f, size_t &ch, bool &skip, bool is_end) {
        size_t i = 0;
        std::string n;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n += s[i++];
        if (n.empty()) return false;
        f = static_cast<size_t>(std::strtoul(n.c_str(), nullptr, 10));
        if (i < s.size() && s[i] == '.') {
            ++i;
            std::string m;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) m += s[i++];
            ch = static_cast<size_t>(std::strtoul(m.c_str(), nullptr, 10));
        }
        for (; i < s.size(); ++i) {
            switch (s[i]) {
            case 'b': skip = true; break;
            case 'n': k.numeric = true; k.own_flags = true; break;
            case 'g': k.general = true; k.own_flags = true; break;
            case 'h': k.human = true; k.own_flags = true; break;
            case 'M': k.month = true; k.own_flags = true; break;
            case 'V': k.version = true; k.own_flags = true; break;
            case 'R': k.random = true; k.own_flags = true; break;
            case 'd': k.dict = true; k.own_flags = true; break;
            case 'f': k.fold = true; k.own_flags = true; break;
            case 'i': k.nonprint = true; k.own_flags = true; break;
            case 'r': k.reverse = true; k.own_flags = true; break;
            default: return false;
            }
        }
        (void)is_end;
        return true;
    };
    if (!parse_pos(a, k.f1, k.c1, k.skip_b1, false) || k.f1 == 0) return false;
    if (!b.empty()) {
        size_t f2 = 0, c2 = 0;
        if (!parse_pos(b, f2, c2, k.skip_b2, true)) return false;
        k.f2 = f2;
        k.c2 = c2;
    }
    if (!k.own_flags) {
        const bool b1 = k.skip_b1, b2 = k.skip_b2;
        const size_t f1 = k.f1, c1 = k.c1, f2 = k.f2, c2 = k.c2;
        k = global;
        k.f1 = f1; k.c1 = c1; k.f2 = f2; k.c2 = c2; k.skip_b1 = b1 || global.skip_b1; k.skip_b2 = b2 || global.skip_b2;
    }
    return true;
}

} // namespace

int cmd_sort(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Write sorted concatenation of all FILE(s) to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"ignore-leading-blanks", no_argument, nullptr, 'b'}, {"dictionary-order", no_argument, nullptr, 'd'},
                                {"ignore-case", no_argument, nullptr, 'f'}, {"general-numeric-sort", no_argument, nullptr, 'g'},
                                {"human-numeric-sort", no_argument, nullptr, 'h'}, {"ignore-nonprinting", no_argument, nullptr, 'i'},
                                {"key", required_argument, nullptr, 'k'}, {"month-sort", no_argument, nullptr, 'M'},
                                {"numeric-sort", no_argument, nullptr, 'n'}, {"random-sort", no_argument, nullptr, 'R'},
                                {"reverse", no_argument, nullptr, 'r'}, {"version-sort", no_argument, nullptr, 'V'},
                                {"check", optional_argument, nullptr, 'c'}, {"merge", no_argument, nullptr, 'm'},
                                {"output", required_argument, nullptr, 'o'}, {"stable", no_argument, nullptr, 's'},
                                {"field-separator", required_argument, nullptr, 't'}, {"unique", no_argument, nullptr, 'u'},
                                {"zero-terminated", no_argument, nullptr, 'z'}, {"buffer-size", required_argument, nullptr, 'S'},
                                {"temporary-directory", required_argument, nullptr, 'T'}, {"parallel", required_argument, nullptr, 1},
                                {"random-source", required_argument, nullptr, 2}, {"batch-size", required_argument, nullptr, 3}, kEnd};
    SortCtx ctx;
    std::vector<std::string> key_specs;
    std::string output;
    bool check = false, quiet_check = false;
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "bdfghik:MnRrVc::Cmo:st:uzS:T:", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': ctx.global.skip_b1 = ctx.global.skip_b2 = true; break;
        case 'd': ctx.global.dict = true; break;
        case 'f': ctx.global.fold = true; break;
        case 'g': ctx.global.general = true; break;
        case 'h': ctx.global.human = true; break;
        case 'i': ctx.global.nonprint = true; break;
        case 'k': key_specs.push_back(optarg); break;
        case 'M': ctx.global.month = true; break;
        case 'n': ctx.global.numeric = true; break;
        case 'R': ctx.global.random = true; break;
        case 'r': ctx.global.reverse = true; break;
        case 'V': ctx.global.version = true; break;
        case 'c': check = true; quiet_check = optarg && std::strcmp(optarg, "quiet") == 0; break;
        case 'C': check = quiet_check = true; break;
        case 'm': break;
        case 'o': output = optarg; break;
        case 's': ctx.stable = true; break;
        case 't': ctx.tsep = std::strcmp(optarg, "\\0") == 0 ? '\0' : optarg[0]; break;
        case 'u': ctx.unique = true; break;
        case 'z': eol = '\0'; break;
        case 'S': case 'T': case 1: case 2: case 3: break;
        default: return 2;
        }
    }
    const char *lc = std::getenv("LC_ALL");
    if (!lc || !*lc) lc = std::getenv("LC_COLLATE");
    if (!lc || !*lc) lc = std::getenv("LANG");
    ctx.bytewise = lc && (std::strcmp(lc, "C") == 0 || std::strcmp(lc, "POSIX") == 0);
    ctx.salt = random_u64();
    for (const std::string &s : key_specs) {
        KeyDef k;
        if (!parse_key(s, k, ctx.global)) {
            err("invalid number at field start: invalid count at start of '" + s + "'");
            return 2;
        }
        ctx.keys.push_back(k);
    }
    std::string data;
    int rc = 0;
    const auto files = operands(argc, argv);
    for (const std::string &f : files) {
        if (!read_input(f, data)) rc = 2;
    }
    if (rc) return rc;
    if (!data.empty() && data.back() != eol) data += eol;
    std::vector<std::string> lines = split_lines(data, eol);

    if (check) {
        for (size_t i = 1; i < lines.size(); ++i) {
            const int r = compare_lines(ctx, lines[i - 1], lines[i], true);
            if (r > 0 || (ctx.unique && r == 0 && compare_lines(ctx, lines[i - 1], lines[i], false) == 0)) {
                if (!quiet_check) err((files[0] == "-" ? std::string("-") : files[0]) + ":" + std::to_string(i + 1) + ": disorder: " + lines[i]);
                return 1;
            }
        }
        return 0;
    }
    std::stable_sort(lines.begin(), lines.end(), [&](const std::string &a, const std::string &b) { return compare_lines(ctx, a, b, true) < 0; });
    std::string result;
    const std::string *prev = nullptr;
    for (const std::string &l : lines) {
        if (ctx.unique && prev && compare_lines(ctx, *prev, l, false) == 0) continue;
        result += l;
        result += eol;
        prev = &l;
    }
    if (!output.empty()) {
        const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !write_all(fd, result)) { err_errno("open failed: " + output); return 2; }
        ::close(fd);
    } else {
        out(result);
    }
    return 0;
}

// =============================================================================================
// pr
// =============================================================================================
int cmd_pr(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Paginate or columnate FILE(s) for printing.")) {
        return 0;
    }
    std::vector<char *> av(argv, argv + argc);
    std::vector<std::string> storage;
    size_t columns = 1, page_len = 66, width = 72, indent = 0;
    bool omit_header = false, number = false, merge = false, double_space = false, across = false, form_feed = false;
    std::string header;
    long first_page = 1, last_page = 0;
    // -COLUMN is a numeric option
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[i][1])) && std::strlen(argv[i]) > 1) {
            columns = static_cast<size_t>(std::atoi(argv[i] + 1));
            av[i] = const_cast<char *>("--");
            storage.push_back("");
        }
    }
    std::vector<char *> av2;
    for (char *p : av) if (std::strcmp(p, "--") != 0 || p == av[0]) av2.push_back(p);
    int c;
    optind = 1;
    int ac = static_cast<int>(av2.size());
    while ((c = getopt(ac, av2.data(), "adFfh:l:mn::o:tTw:+:")) != -1) {
        switch (c) {
        case 'a': across = true; break;
        case 'd': double_space = true; break;
        case 'F': case 'f': form_feed = true; break;
        case 'h': header = optarg; break;
        case 'l': page_len = static_cast<size_t>(std::atoi(optarg)); break;
        case 'm': merge = true; break;
        case 'n': number = true; break;
        case 'o': indent = static_cast<size_t>(std::atoi(optarg)); break;
        case 't': omit_header = true; break;
        case 'T': omit_header = true; break;
        case 'w': width = static_cast<size_t>(std::atoi(optarg)); break;
        case '+': first_page = std::atol(optarg); { const char *d = std::strchr(optarg, ':'); if (d) last_page = std::atol(d + 1); } break;
        default: return 1;
        }
    }
    std::vector<std::string> files(av2.begin() + optind, av2.end());
    if (files.empty()) files.push_back("-");
    int rc = 0;
    auto render = [&](const std::string &title, std::vector<std::string> lines_in) {
        std::vector<std::string> lines;
        if (number) {
            size_t n = 1;
            for (auto &l : lines_in) { char b[32]; std::snprintf(b, sizeof b, "%5zu\t", n++); lines.push_back(b + l); }
        } else lines = std::move(lines_in);
        const size_t body_lines = omit_header ? page_len : (page_len > 10 ? page_len - 10 : page_len);
        const size_t per_page = std::max<size_t>(1, body_lines / (double_space ? 2 : 1)) * columns;
        const size_t colw = columns > 1 ? (width - (columns - 1)) / columns : width;
        const std::string pad(indent, ' ');
        const std::string stamp = format_time(std::time(nullptr), "%Y-%m-%d %H:%M", false);
        long page = 1;
        for (size_t base = 0; base < lines.size(); base += per_page, ++page) {
            if (page < first_page || (last_page && page > last_page)) continue;
            std::string o;
            if (!omit_header) {
                o += "\n\n";
                const std::string pg = "Page " + std::to_string(page);
                const std::string center = header.empty() ? title : header;
                const size_t space = width > stamp.size() + center.size() + pg.size() ? width - stamp.size() - center.size() - pg.size() : 2;
                o += pad + stamp + std::string(space / 2, ' ') + center + std::string(space - space / 2, ' ') + pg + "\n\n\n";
            }
            const size_t nrows = (std::min(per_page, lines.size() - base) + columns - 1) / columns;
            size_t emitted = 0;
            for (size_t r = 0; r < nrows; ++r) {
                std::string row;
                for (size_t cidx = 0; cidx < columns; ++cidx) {
                    const size_t idx = base + (across ? r * columns + cidx : cidx * nrows + r);
                    if (idx >= lines.size() || idx >= base + per_page) continue;
                    std::string cell = lines[idx].substr(0, colw);
                    if (columns > 1 && cidx + 1 < columns) cell += std::string(colw - cell.size() + 1, ' ');
                    row += cell;
                }
                o += pad + row + "\n";
                ++emitted;
                if (double_space) o += "\n";
            }
            if (!omit_header) {
                const size_t used = emitted * (double_space ? 2 : 1);
                if (body_lines > used) o += std::string(body_lines - used, '\n');
                o += "\n\n\n\n\n";
            }
            if (form_feed) o += "\f";
            out(o);
        }
    };
    if (merge) {
        std::vector<std::vector<std::string>> cols;
        for (const std::string &f : files) {
            std::string data;
            if (!read_input(f, data)) { rc = 1; continue; }
            cols.push_back(split_lines(data));
        }
        columns = cols.size();
        size_t rows = 0;
        for (auto &cl : cols) rows = std::max(rows, cl.size());
        std::vector<std::string> merged;
        for (size_t r = 0; r < rows; ++r) for (auto &cl : cols) merged.push_back(r < cl.size() ? cl[r] : "");
        across = true;
        render(files[0], merged);
        return rc;
    }
    for (const std::string &f : files) {
        std::string data;
        if (!read_input(f, data)) { rc = 1; continue; }
        render(f == "-" ? "" : f, split_lines(data));
    }
    return rc;
}

// =============================================================================================
// base64 / base32 / basenc
// =============================================================================================
namespace {

enum class Enc { B64, B64URL, B32, B32HEX, B16, B2MSB, B2LSB, Z85 };

const char *alphabet(Enc e)
{
    switch (e) {
    case Enc::B64: return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    case Enc::B64URL: return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    case Enc::B32: return "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    case Enc::B32HEX: return "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    case Enc::B16: return "0123456789ABCDEF";
    case Enc::Z85: return "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";
    default: return "01";
    }
}

std::string encode(Enc e, const std::string &in)
{
    std::string o;
    const char *al = alphabet(e);
    if (e == Enc::B64 || e == Enc::B64URL) {
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
            o += al[v >> 18]; o += al[(v >> 12) & 63]; o += al[(v >> 6) & 63]; o += al[v & 63];
        }
        if (i + 1 == in.size()) {
            const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
            o += al[v >> 18]; o += al[(v >> 12) & 63]; o += "==";
        } else if (i + 2 == in.size()) {
            const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
            o += al[v >> 18]; o += al[(v >> 12) & 63]; o += al[(v >> 6) & 63]; o += '=';
        }
    } else if (e == Enc::B32 || e == Enc::B32HEX) {
        for (size_t i = 0; i < in.size(); i += 5) {
            uint64_t v = 0;
            const size_t n = std::min<size_t>(5, in.size() - i);
            for (size_t k = 0; k < 5; ++k) v = (v << 8) | (k < n ? static_cast<uint8_t>(in[i + k]) : 0);
            const size_t chars = (n * 8 + 4) / 5;
            for (size_t k = 0; k < 8; ++k) o += k < chars ? al[(v >> (35 - 5 * k)) & 31] : '=';
        }
    } else if (e == Enc::B16) {
        for (unsigned char ch : in) { o += al[ch >> 4]; o += al[ch & 15]; }
    } else if (e == Enc::B2MSB || e == Enc::B2LSB) {
        for (unsigned char ch : in) for (int k = 0; k < 8; ++k) o += (e == Enc::B2MSB ? (ch >> (7 - k)) : (ch >> k)) & 1 ? '1' : '0';
    } else { // Z85
        for (size_t i = 0; i + 3 < in.size(); i += 4) {
            uint32_t v = (static_cast<uint8_t>(in[i]) << 24) | (static_cast<uint8_t>(in[i + 1]) << 16) | (static_cast<uint8_t>(in[i + 2]) << 8) | static_cast<uint8_t>(in[i + 3]);
            char tmp[5];
            for (int k = 4; k >= 0; --k) { tmp[k] = al[v % 85]; v /= 85; }
            o.append(tmp, 5);
        }
    }
    return o;
}

bool decode(Enc e, const std::string &in, std::string &o, bool ignore_garbage)
{
    const char *al = alphabet(e);
    int map[256];
    std::fill(std::begin(map), std::end(map), -1);
    for (int i = 0; al[i]; ++i) map[static_cast<unsigned char>(al[i])] = i;
    if (e == Enc::B64 || e == Enc::B64URL || e == Enc::B32 || e == Enc::B32HEX) {
        const bool is64 = (e == Enc::B64 || e == Enc::B64URL);
        const int bits = is64 ? 6 : 5, quantum = is64 ? 4 : 8;
        std::vector<int> vals;
        size_t pad = 0;
        for (unsigned char ch : in) {
            if (ch == '\n' || ch == '\r') continue;
            if (ch == '=') { ++pad; continue; }
            if (pad) return false;
            if (map[ch] < 0) { if (ignore_garbage) continue; return false; }
            vals.push_back(map[ch]);
        }
        if ((vals.size() + pad) % quantum != 0) return false;
        uint32_t acc = 0;
        int nb = 0;
        for (int v : vals) {
            acc = (acc << bits) | static_cast<uint32_t>(v);
            nb += bits;
            if (nb >= 8) { nb -= 8; o += static_cast<char>((acc >> nb) & 0xFF); }
        }
        return true;
    }
    if (e == Enc::B16) {
        int hi = -1;
        for (unsigned char ch : in) {
            if (ch == '\n') continue;
            const int v = map[static_cast<unsigned char>(std::toupper(ch))];
            if (v < 0) { if (ignore_garbage) continue; return false; }
            if (hi < 0) hi = v; else { o += static_cast<char>(hi * 16 + v); hi = -1; }
        }
        return hi < 0;
    }
    if (e == Enc::B2MSB || e == Enc::B2LSB) {
        int acc = 0, n = 0;
        for (unsigned char ch : in) {
            if (ch == '\n') continue;
            if (ch != '0' && ch != '1') { if (ignore_garbage) continue; return false; }
            const int bit = ch - '0';
            if (e == Enc::B2MSB) acc = (acc << 1) | bit; else acc |= bit << n;
            if (++n == 8) { o += static_cast<char>(acc); acc = n = 0; }
        }
        return n == 0;
    }
    // Z85
    std::vector<int> vals;
    for (unsigned char ch : in) {
        if (ch == '\n') continue;
        if (map[ch] < 0) { if (ignore_garbage) continue; return false; }
        vals.push_back(map[ch]);
    }
    if (vals.size() % 5) return false;
    for (size_t i = 0; i < vals.size(); i += 5) {
        uint32_t v = 0;
        for (int k = 0; k < 5; ++k) v = v * 85 + static_cast<uint32_t>(vals[i + k]);
        for (int k = 3; k >= 0; --k) o += static_cast<char>((v >> (8 * k)) & 0xFF);
    }
    return true;
}

int encoder_main(int argc, char **argv, Enc e, bool is_basenc)
{
    static const option lo[] = {{"decode", no_argument, nullptr, 'd'}, {"ignore-garbage", no_argument, nullptr, 'i'}, {"wrap", required_argument, nullptr, 'w'},
                                {"base64", no_argument, nullptr, 1}, {"base64url", no_argument, nullptr, 2}, {"base32", no_argument, nullptr, 3},
                                {"base32hex", no_argument, nullptr, 4}, {"base16", no_argument, nullptr, 5}, {"base2msbf", no_argument, nullptr, 6},
                                {"base2lsbf", no_argument, nullptr, 7}, {"z85", no_argument, nullptr, 8}, kEnd};
    bool decode_mode = false, ignore = false;
    size_t wrap = 76;
    bool wrap_set = false;
    int c;
    while ((c = getopt_long(argc, argv, "diw:", lo, nullptr)) != -1) {
        switch (c) {
        case 'd': decode_mode = true; break;
        case 'i': ignore = true; break;
        case 'w': { uint64_t v; if (!parse_u64(optarg, v)) { err(std::string("invalid wrap size: '") + optarg + "'"); return 1; } wrap = static_cast<size_t>(v); wrap_set = true; break; }
        case 1: e = Enc::B64; break; case 2: e = Enc::B64URL; break; case 3: e = Enc::B32; break; case 4: e = Enc::B32HEX; break;
        case 5: e = Enc::B16; break; case 6: e = Enc::B2MSB; break; case 7: e = Enc::B2LSB; break; case 8: e = Enc::Z85; break;
        default: return 1;
        }
    }
    if (is_basenc && argc == 1) { usage_error("missing encoding type"); return 1; }
    (void)wrap_set;
    std::string data;
    const std::string f = optind < argc ? argv[optind] : "-";
    if (!read_input(f, data)) return 1;
    if (decode_mode) {
        std::string o;
        if (!decode(e, data, o, ignore)) { err("invalid input"); return 1; }
        out(o);
        return 0;
    }
    const std::string enc = encode(e, data);
    if (wrap == 0) { out(enc); if (!enc.empty()) out("\n"); return 0; }
    for (size_t i = 0; i < enc.size(); i += wrap) out(enc.substr(i, wrap) + "\n");
    return 0;
}

} // namespace

int cmd_base64(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]", "Base64 encode or decode FILE, or standard input, to standard output.")) return 0;
    return encoder_main(argc, argv, Enc::B64, false);
}
int cmd_base32(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]", "Base32 encode or decode FILE, or standard input, to standard output.")) return 0;
    return encoder_main(argc, argv, Enc::B32, false);
}
int cmd_basenc(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]", "basenc encode or decode FILE (--base64 --base64url --base32 --base32hex --base16 --base2msbf --base2lsbf --z85).")) return 0;
    return encoder_main(argc, argv, Enc::B64, true);
}

// =============================================================================================
// od
// =============================================================================================
int cmd_od(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Write an unambiguous representation, octal bytes by default, of FILE to standard output.")) {
        return 0;
    }
    struct Fmt { char type; int size; };
    std::vector<Fmt> fmts;
    char addr = 'o';
    uint64_t skip = 0, limit = UINT64_MAX, width = 16;
    bool verbose = false;
    int c;
    auto add_type = [&](const std::string &t) -> bool {
        for (size_t i = 0; i < t.size();) {
            const char ty = t[i++];
            int size = 0;
            std::string n;
            while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) n += t[i++];
            if (!n.empty()) size = std::atoi(n.c_str());
            if (ty == 'a' || ty == 'c') fmts.push_back({ty, 1});
            else if (ty == 'f') fmts.push_back({ty, size ? size : 8});
            else if (ty == 'd' || ty == 'o' || ty == 'u' || ty == 'x') fmts.push_back({ty, size ? size : 4});
            else return false;
        }
        return true;
    };
    optind = 1;
    while ((c = getopt(argc, argv, "A:j:N:t:vw::abcdfhilosxD")) != -1) {
        switch (c) {
        case 'A': addr = optarg[0]; break;
        case 'j': if (!parse_size(optarg, skip)) { err(std::string("invalid skip argument '") + optarg + "'"); return 1; } break;
        case 'N': if (!parse_size(optarg, limit)) { err(std::string("invalid byte count argument '") + optarg + "'"); return 1; } break;
        case 't': if (!add_type(optarg)) { err(std::string("invalid type string '") + optarg + "'"); return 1; } break;
        case 'v': verbose = true; break;
        case 'w': width = optarg ? static_cast<uint64_t>(std::atoi(optarg)) : 32; break;
        case 'a': fmts.push_back({'a', 1}); break;
        case 'b': fmts.push_back({'o', 1}); break;
        case 'c': fmts.push_back({'c', 1}); break;
        case 'd': fmts.push_back({'u', 2}); break;
        case 'D': fmts.push_back({'u', 4}); break;
        case 'f': fmts.push_back({'f', 4}); break;
        case 'h': fmts.push_back({'x', 2}); break;
        case 'i': fmts.push_back({'d', 4}); break;
        case 'l': fmts.push_back({'d', 8}); break;
        case 'o': fmts.push_back({'o', 2}); break;
        case 's': fmts.push_back({'d', 2}); break;
        case 'x': fmts.push_back({'x', 2}); break;
        default: return 1;
        }
    }
    if (fmts.empty()) fmts.push_back({'o', 2});
    if (width == 0) width = 16;
    std::string data;
    for (const std::string &f : operands(argc, argv)) {
        if (!read_input(f, data)) return 1;
    }
    if (skip < data.size()) data = data.substr(skip); else data.clear();
    if (data.size() > limit) data.resize(limit);
    auto addr_str = [&](uint64_t a) {
        char b[32];
        if (addr == 'n') return std::string();
        if (addr == 'd') std::snprintf(b, sizeof b, "%07llu", (unsigned long long)a);
        else if (addr == 'x') std::snprintf(b, sizeof b, "%06llx", (unsigned long long)a);
        else std::snprintf(b, sizeof b, "%07llo", (unsigned long long)a);
        return std::string(b);
    };
    std::string prev_chunk;
    bool starred = false;
    uint64_t off = 0;
    for (; off < data.size(); off += width) {
        const std::string chunk = data.substr(off, width);
        if (!verbose && off > 0 && chunk == prev_chunk && chunk.size() == width && off + width < data.size()) {
            if (!starred) { out("*\n"); starred = true; }
            continue;
        }
        starred = false;
        prev_chunk = chunk;
        for (size_t fi = 0; fi < fmts.size(); ++fi) {
            std::string line = fi == 0 ? addr_str(off) : (addr == 'n' ? "" : std::string(7, ' '));
            const Fmt &f = fmts[fi];
            const size_t step = static_cast<size_t>(f.size);
            for (size_t i = 0; i < chunk.size(); i += step) {
                uint64_t v = 0;
                for (size_t k = 0; k < step && i + k < chunk.size(); ++k) v |= static_cast<uint64_t>(static_cast<uint8_t>(chunk[i + k])) << (8 * k);
                char b[64];
                switch (f.type) {
                case 'a': { static const char *names[] = {"nul","soh","stx","etx","eot","enq","ack","bel"," bs"," ht"," nl"," vt"," ff"," cr"," so"," si","dle","dc1","dc2","dc3","dc4","nak","syn","etb","can"," em","sub","esc"," fs"," gs"," rs"," us"}; const unsigned char ch = v & 0x7F; if (ch < 32) std::snprintf(b, sizeof b, " %3s", names[ch]); else if (ch == 127) std::snprintf(b, sizeof b, " del"); else std::snprintf(b, sizeof b, "   %c", ch); break; }
                case 'c': { const unsigned char ch = static_cast<unsigned char>(v); switch (ch) { case '\n': std::snprintf(b, sizeof b, "  \\n"); break; case '\t': std::snprintf(b, sizeof b, "  \\t"); break; case '\r': std::snprintf(b, sizeof b, "  \\r"); break; case 0: std::snprintf(b, sizeof b, "  \\0"); break; default: if (std::isprint(ch)) std::snprintf(b, sizeof b, "   %c", ch); else std::snprintf(b, sizeof b, " %03o", ch); } break; }
                case 'o': std::snprintf(b, sizeof b, " %0*llo", f.size == 1 ? 3 : f.size == 2 ? 6 : f.size == 4 ? 11 : 22, (unsigned long long)v); break;
                case 'u': std::snprintf(b, sizeof b, " %*llu", f.size == 1 ? 3 : f.size == 2 ? 5 : f.size == 4 ? 10 : 20, (unsigned long long)v); break;
                case 'x': std::snprintf(b, sizeof b, " %0*llx", f.size * 2, (unsigned long long)v); break;
                case 'd': { long long sv = f.size == 1 ? (int8_t)v : f.size == 2 ? (int16_t)v : f.size == 4 ? (int32_t)v : (long long)v; std::snprintf(b, sizeof b, " %*lld", f.size == 1 ? 4 : f.size == 2 ? 6 : f.size == 4 ? 11 : 20, sv); break; }
                default: { if (f.size == 4) { float fl; uint32_t u = (uint32_t)v; std::memcpy(&fl, &u, 4); std::snprintf(b, sizeof b, " %15.8g", fl); } else { double d; std::memcpy(&d, &v, 8); std::snprintf(b, sizeof b, " %24.17g", d); } }
                }
                line += b;
            }
            out(line + "\n");
        }
    }
    if (addr != 'n') out(addr_str(data.size()) + "\n");
    return 0;
}

// =============================================================================================
// printf
// =============================================================================================
namespace {

bool parse_num_arg(const std::string &s, long double &v, bool &bad)
{
    if (s.empty()) { v = 0; return true; }
    if ((s[0] == '\'' || s[0] == '"') && s.size() >= 2) { v = static_cast<unsigned char>(s[1]); return true; }
    char *end = nullptr;
    errno = 0;
    v = std::strtold(s.c_str(), &end);
    if (end == s.c_str() || *end) { bad = true; v = end == s.c_str() ? 0 : v; return false; }
    return true;
}

} // namespace

int cmd_printf(int argc, char **argv)
{
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "--version") == 0)) {
        handle_help_version(argc, argv, "FORMAT [ARGUMENT]...", "Print ARGUMENT(s) according to FORMAT.");
        return 0;
    }
    if (argc < 2) {
        usage_error("missing operand");
        return 1;
    }
    const std::string fmt = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    size_t ai = 0;
    int rc = 0;
    auto next_arg = [&](bool &have) -> std::string { have = ai < args.size(); return have ? args[ai++] : std::string(); };
    bool stop = false;
    for (;;) {
        const size_t start_ai = ai;
        for (size_t i = 0; i < fmt.size() && !stop;) {
            const char ch = fmt[i];
            if (ch == '\\') {
                size_t j = i + 1;
                std::string esc = "\\";
                if (j < fmt.size()) {
                    esc += fmt[j++];
                    // collect following digits for octal / hex / unicode escapes
                    if (std::string("01234567").find(fmt[j - 1]) != std::string::npos) while (j < fmt.size() && esc.size() < 4 && fmt[j] >= '0' && fmt[j] <= '7') esc += fmt[j++];
                    else if (fmt[j - 1] == 'x') while (j < fmt.size() && esc.size() < 4 && std::isxdigit(static_cast<unsigned char>(fmt[j]))) esc += fmt[j++];
                    else if (fmt[j - 1] == 'u' || fmt[j - 1] == 'U') { const size_t lim = fmt[j - 1] == 'u' ? 6 : 10; while (j < fmt.size() && esc.size() < lim && std::isxdigit(static_cast<unsigned char>(fmt[j]))) esc += fmt[j++]; }
                }
                bool st = false;
                out(unescape(esc, &st));
                if (st) { stop = true; break; }
                i = j;
                continue;
            }
            if (ch != '%') { out(std::string(1, ch)); ++i; continue; }
            if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out("%"); i += 2; continue; }
            size_t j = i + 1;
            std::string spec = "%";
            while (j < fmt.size() && std::strchr("-+ #0'", fmt[j])) spec += fmt[j++];
            // width
            if (j < fmt.size() && fmt[j] == '*') { bool h; spec += std::to_string(std::atoi(next_arg(h).c_str())); ++j; }
            else while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec += fmt[j++];
            if (j < fmt.size() && fmt[j] == '.') {
                spec += fmt[j++];
                if (j < fmt.size() && fmt[j] == '*') { bool h; spec += std::to_string(std::atoi(next_arg(h).c_str())); ++j; }
                else while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec += fmt[j++];
            }
            if (j >= fmt.size()) { err("%" + fmt.substr(i + 1) + ": invalid conversion specification"); return 1; }
            const char conv = fmt[j++];
            bool have;
            const std::string a = next_arg(have);
            char buf[1024];
            switch (conv) {
            case 'd': case 'i': {
                long double v; bool bad = false; parse_num_arg(a, v, bad);
                if (bad) { err("'" + a + "': expected a numeric value"); rc = 1; }
                std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(v));
                out(buf); break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                long double v; bool bad = false; parse_num_arg(a, v, bad);
                if (bad) { err("'" + a + "': expected a numeric value"); rc = 1; }
                std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), static_cast<unsigned long long>(static_cast<long long>(v)));
                out(buf); break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                long double v; bool bad = false; parse_num_arg(a, v, bad);
                if (bad) { err("'" + a + "': expected a numeric value"); rc = 1; }
                std::snprintf(buf, sizeof buf, (spec + "L" + conv).c_str(), v);
                out(buf); break;
            }
            case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), a.empty() ? '\0' : a[0]); out(a.empty() ? std::string() : std::string(buf)); break;
            case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), a.c_str()); out(buf); break;
            case 'b': { bool st = false; out(unescape(a, &st)); if (st) stop = true; break; }
            case 'q': out(quote_shell(a)); break;
            default: err(std::string("%") + conv + ": invalid conversion specification"); return 1;
            }
            i = j;
        }
        if (stop || ai >= args.size() || ai == start_ai) break; // reuse the format while arguments remain
    }
    return rc;
}

// =============================================================================================
// split / csplit
// =============================================================================================
namespace {

std::string suffix_for(size_t idx, size_t len, bool numeric, bool hex)
{
    std::string s(len, 'a');
    const size_t base = numeric ? 10 : (hex ? 16 : 26);
    for (size_t k = len; k > 0; --k) {
        const size_t d = idx % base;
        idx /= base;
        if (numeric) s[k - 1] = static_cast<char>('0' + d);
        else if (hex) s[k - 1] = "0123456789abcdef"[d];
        else s[k - 1] = static_cast<char>('a' + d);
    }
    return s;
}

} // namespace

int cmd_split(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE [PREFIX]]", "Output pieces of FILE to PREFIXaa, PREFIXab, ...; default size is 1000 lines, and default PREFIX is 'x'.")) {
        return 0;
    }
    static const option lo[] = {{"lines", required_argument, nullptr, 'l'}, {"bytes", required_argument, nullptr, 'b'},
                                {"line-bytes", required_argument, nullptr, 'C'}, {"number", required_argument, nullptr, 'n'},
                                {"numeric-suffixes", optional_argument, nullptr, 'd'}, {"hex-suffixes", optional_argument, nullptr, 'x'},
                                {"suffix-length", required_argument, nullptr, 'a'}, {"additional-suffix", required_argument, nullptr, 1},
                                {"verbose", no_argument, nullptr, 2}, {"elide-empty-files", no_argument, nullptr, 'e'}, kEnd};
    uint64_t lines = 1000, bytes = 0, linebytes = 0, chunks = 0;
    bool numeric = false, hex = false, verbose = false, elide = false;
    size_t slen = 2;
    bool slen_set = false;
    std::string extra;
    int c;
    std::vector<char *> av(argv, argv + argc);
    std::string legacy;
    if (argc > 1 && argv[1][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[1][1]))) { legacy = std::string("-l") + (argv[1] + 1); av[1] = legacy.data(); }
    while ((c = getopt_long(argc, av.data(), "l:b:C:n:d::x::a:eu", lo, nullptr)) != -1) {
        switch (c) {
        case 'l': if (!parse_u64(optarg, lines) || !lines) { err(std::string("invalid number of lines: '") + optarg + "'"); return 1; } break;
        case 'b': if (!parse_size(optarg, bytes) || !bytes) { err(std::string("invalid number of bytes: '") + optarg + "'"); return 1; } break;
        case 'C': if (!parse_size(optarg, linebytes) || !linebytes) { err(std::string("invalid number of bytes: '") + optarg + "'"); return 1; } break;
        case 'n': { std::string v = optarg; const size_t sl = v.find('/'); if (sl != std::string::npos) v = v.substr(sl + 1); if (!parse_u64(v, chunks) || !chunks) { err(std::string("invalid number of chunks: '") + optarg + "'"); return 1; } break; }
        case 'd': numeric = true; break;
        case 'x': hex = true; break;
        case 'a': slen = static_cast<size_t>(std::atoi(optarg)); slen_set = true; break;
        case 1: extra = optarg; break;
        case 2: verbose = true; break;
        case 'e': elide = true; break;
        case 'u': break;
        default: return 1;
        }
    }
    (void)slen_set;
    const std::string input = optind < argc ? av[optind] : "-";
    const std::string prefix = optind + 1 < argc ? av[optind + 1] : "x";
    std::string data;
    if (!read_input(input, data)) return 1;
    std::vector<std::string> pieces;
    if (chunks) {
        const size_t per = (data.size() + chunks - 1) / chunks;
        for (size_t i = 0; i < chunks; ++i) pieces.push_back(i * per < data.size() ? data.substr(i * per, per) : std::string());
    } else if (bytes) {
        for (size_t i = 0; i < data.size(); i += bytes) pieces.push_back(data.substr(i, bytes));
    } else if (linebytes) {
        std::string cur;
        size_t pos = 0;
        while (pos < data.size()) {
            size_t e = data.find('\n', pos);
            const size_t len = (e == std::string::npos ? data.size() : e + 1) - pos;
            if (!cur.empty() && cur.size() + len > linebytes) { pieces.push_back(cur); cur.clear(); }
            cur += data.substr(pos, len);
            pos += len;
        }
        if (!cur.empty()) pieces.push_back(cur);
    } else {
        size_t pos = 0, n = 0;
        std::string cur;
        while (pos < data.size()) {
            const size_t e = data.find('\n', pos);
            const size_t len = (e == std::string::npos ? data.size() : e + 1) - pos;
            cur += data.substr(pos, len);
            pos += len;
            if (++n == lines) { pieces.push_back(cur); cur.clear(); n = 0; }
        }
        if (!cur.empty()) pieces.push_back(cur);
    }
    size_t base = numeric ? 10 : (hex ? 16 : 26), cap = 1;
    for (size_t k = 0; k < slen; ++k) cap *= base;
    int rc = 0;
    size_t idx = 0;
    for (const std::string &p : pieces) {
        if (elide && p.empty()) continue;
        if (idx >= cap) { err("output file suffixes exhausted"); return 1; }
        const std::string name = prefix + suffix_for(idx++, slen, numeric, hex) + extra;
        const int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !write_all(fd, p)) { err_errno(name); rc = 1; if (fd >= 0) ::close(fd); continue; }
        ::close(fd);
        if (verbose) out("creating file '" + name + "'\n");
    }
    return rc;
}

int cmd_csplit(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE PATTERN...", "Output pieces of FILE separated by PATTERN(s) to files 'xx00', 'xx01', ...")) {
        return 0;
    }
    static const option lo[] = {{"prefix", required_argument, nullptr, 'f'}, {"suffix-format", required_argument, nullptr, 'b'},
                                {"digits", required_argument, nullptr, 'n'}, {"quiet", no_argument, nullptr, 's'},
                                {"silent", no_argument, nullptr, 's'}, {"keep-files", no_argument, nullptr, 'k'},
                                {"elide-empty-files", no_argument, nullptr, 'z'}, kEnd};
    std::string prefix = "xx", sufmt;
    int digits = 2;
    bool quiet = false, keep = false, elide = false;
    int c;
    while ((c = getopt_long(argc, argv, "f:b:n:skz", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': prefix = optarg; break;
        case 'b': sufmt = optarg; break;
        case 'n': digits = std::atoi(optarg); break;
        case 's': quiet = true; break;
        case 'k': keep = true; break;
        case 'z': elide = true; break;
        default: return 1;
        }
    }
    if (argc - optind < 2) { usage_error(argc - optind < 1 ? "missing operand" : "missing operand after '" + std::string(argv[optind]) + "'"); return 1; }
    std::string data;
    if (!read_input(argv[optind], data)) return 1;
    const auto lines = split_lines(data);
    std::vector<std::string> pieces;
    size_t pos = 0;
    auto take = [&](size_t upto) {
        std::string s;
        for (size_t i = pos; i < upto && i < lines.size(); ++i) s += lines[i] + "\n";
        pieces.push_back(s);
        pos = std::max(pos, std::min(upto, lines.size()));
    };
    for (int pi = optind + 1; pi < argc; ++pi) {
        const std::string pat = argv[pi];
        if (!pat.empty() && pat[0] == '{') continue; // repeat counts are consumed together with their pattern
        long repeat = 1;
        if (pi + 1 < argc && argv[pi + 1][0] == '{') {
            const std::string r = argv[pi + 1];
            repeat = r.find('*') != std::string::npos ? LONG_MAX : std::atol(r.c_str() + 1) + 1;
        }
        for (long rep = 0; rep < repeat && pos < lines.size(); ++rep) {
            if (std::isdigit(static_cast<unsigned char>(pat[0]))) {
                const size_t line_no = static_cast<size_t>(std::atol(pat.c_str()));
                if (rep > 0) { take(pos + line_no); } else { if (line_no <= pos + 0 && line_no != 0) { err("'" + pat + "': line number out of range"); return 1; } take(line_no - 1); }
                continue;
            }
            const char delim = pat[0];
            const bool skip = delim == '%';
            const size_t close = pat.rfind(delim);
            if ((delim != '/' && delim != '%') || close == 0) { err(pat + ": invalid pattern"); return 1; }
            const std::string re = pat.substr(1, close - 1);
            long off = close + 1 < pat.size() ? std::atol(pat.c_str() + close + 1) : 0;
            std::regex rx;
            try { rx = std::regex(re); } catch (const std::regex_error &) { err(pat + ": invalid regular expression"); return 1; }
            size_t found = lines.size();
            for (size_t i = pos + (rep > 0 || true ? 1 : 0) - (pos == 0 ? 1 : 0); i < lines.size(); ++i) {
                if (std::regex_search(lines[i], rx)) { found = i; break; }
            }
            if (found == lines.size()) { if (rep == 0) { err("'" + pat + "': match not found"); return 1; } break; }
            const size_t cut = static_cast<size_t>(std::max<long>(0, static_cast<long>(found) + off));
            if (skip) pos = cut; else take(cut);
        }
        if (pi + 1 < argc && argv[pi + 1][0] == '{') ++pi;
    }
    {
        std::string s;
        for (size_t i = pos; i < lines.size(); ++i) s += lines[i] + "\n";
        pieces.push_back(s);
    }
    int rc = 0;
    size_t idx = 0;
    for (const std::string &p : pieces) {
        if (elide && p.empty()) continue;
        char sfx[64];
        if (sufmt.empty()) std::snprintf(sfx, sizeof sfx, "%0*zu", digits, idx); else std::snprintf(sfx, sizeof sfx, sufmt.c_str(), static_cast<int>(idx));
        const std::string name = prefix + sfx;
        const int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !write_all(fd, p)) { err_errno(name); rc = 1; if (fd >= 0) ::close(fd); continue; }
        ::close(fd);
        if (!quiet) out(std::to_string(p.size()) + "\n");
        ++idx;
    }
    (void)keep;
    return rc;
}

// =============================================================================================
// shuf / tsort / ptx / numfmt
// =============================================================================================
int cmd_shuf(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE] | -e [ARG]... | -i LO-HI", "Write a random permutation of the input lines to standard output.")) {
        return 0;
    }
    static const option lo[] = {{"echo", no_argument, nullptr, 'e'}, {"input-range", required_argument, nullptr, 'i'},
                                {"head-count", required_argument, nullptr, 'n'}, {"output", required_argument, nullptr, 'o'},
                                {"random-source", required_argument, nullptr, 1}, {"repeat", no_argument, nullptr, 'r'},
                                {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    bool echo = false, repeat = false;
    std::string range, output;
    uint64_t count = UINT64_MAX;
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "ei:n:o:rz", lo, nullptr)) != -1) {
        switch (c) {
        case 'e': echo = true; break;
        case 'i': range = optarg; break;
        case 'n': if (!parse_u64(optarg, count)) { err(std::string("invalid line count: '") + optarg + "'"); return 1; } break;
        case 'o': output = optarg; break;
        case 1: break;
        case 'r': repeat = true; break;
        case 'z': eol = '\0'; break;
        default: return 1;
        }
    }
    std::vector<std::string> items;
    if (!range.empty()) {
        const size_t dash = range.find('-');
        uint64_t lo_v, hi_v;
        if (dash == std::string::npos || !parse_u64(range.substr(0, dash), lo_v) || !parse_u64(range.substr(dash + 1), hi_v) || hi_v < lo_v) {
            err("invalid input range: '" + range + "'");
            return 1;
        }
        for (uint64_t v = lo_v; v <= hi_v; ++v) items.push_back(std::to_string(v));
    } else if (echo) {
        items.assign(argv + optind, argv + argc);
    } else {
        std::string data;
        const std::string f = optind < argc ? argv[optind] : "-";
        if (!read_input(f, data)) return 1;
        items = split_lines(data, eol);
    }
    std::string result;
    if (repeat) {
        if (items.empty()) return 1;
        for (uint64_t k = 0; k < (count == UINT64_MAX ? 1000000ULL : count); ++k) result += items[random_below(items.size())] + eol;
    } else {
        for (size_t i = items.size(); i > 1; --i) std::swap(items[i - 1], items[random_below(i)]);
        for (size_t i = 0; i < items.size() && i < count; ++i) result += items[i] + eol;
    }
    if (!output.empty()) {
        const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || !write_all(fd, result)) { err_errno(output); return 1; }
        ::close(fd);
    } else {
        out(result);
    }
    return 0;
}

int cmd_tsort(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] [FILE]", "Write totally ordered list consistent with the partial ordering in FILE.")) {
        return 0;
    }
    std::string data;
    const std::string f = optind < argc ? argv[optind] : "-";
    if (!read_input(f, data)) return 1;
    std::istringstream ss(data);
    std::vector<std::string> tokens;
    std::string w;
    while (ss >> w) tokens.push_back(w);
    if (tokens.size() % 2) { err(f + ": input contains an odd number of tokens"); return 1; }
    std::vector<std::string> order;
    std::map<std::string, size_t> id;
    auto node = [&](const std::string &n) {
        auto it = id.find(n);
        if (it != id.end()) return it->second;
        id[n] = order.size();
        order.push_back(n);
        return order.size() - 1;
    };
    std::vector<std::set<size_t>> succ;
    std::vector<size_t> indeg;
    for (size_t i = 0; i < tokens.size(); i += 2) {
        const size_t a = node(tokens[i]), b = node(tokens[i + 1]);
        if (succ.size() < order.size()) { succ.resize(order.size()); indeg.resize(order.size(), 0); }
        if (a != b && succ[a].insert(b).second) ++indeg[b];
    }
    succ.resize(order.size());
    indeg.resize(order.size(), 0);
    std::vector<bool> done(order.size(), false);
    size_t emitted = 0;
    int rc = 0;
    while (emitted < order.size()) {
        bool progress = false;
        for (size_t i = 0; i < order.size(); ++i) {
            if (!done[i] && indeg[i] == 0) {
                out(order[i] + "\n");
                done[i] = true;
                ++emitted;
                for (size_t s : succ[i]) --indeg[s];
                progress = true;
                break;
            }
        }
        if (!progress) { // cycle: report the remaining nodes, then break the loop at the first one
            err(f + ": input contains a loop:");
            for (size_t i = 0; i < order.size(); ++i) if (!done[i]) std::fprintf(stderr, "%s: %s\n", prog.c_str(), order[i].c_str());
            rc = 1;
            for (size_t i = 0; i < order.size(); ++i) {
                if (!done[i]) { indeg[i] = 0; break; }
            }
        }
    }
    return rc;
}

int cmd_ptx(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [INPUT]...", "Output a permuted index, including context, of the words in the input files.")) {
        return 0;
    }
    static const option lo[] = {{"width", required_argument, nullptr, 'w'}, {"ignore-case", no_argument, nullptr, 'f'},
                                {"ignore-file", required_argument, nullptr, 'i'}, {"only-file", required_argument, nullptr, 'o'},
                                {"auto-reference", no_argument, nullptr, 'A'}, kEnd};
    size_t width = 72;
    bool fold = false;
    std::set<std::string> ignore, only;
    bool use_only = false;
    int c;
    auto load = [&](const char *f, std::set<std::string> &s) {
        std::string d;
        if (!read_input(f, d)) return false;
        for (const std::string &w : split_lines(d)) s.insert(w);
        return true;
    };
    while ((c = getopt_long(argc, argv, "w:fi:o:AGr", lo, nullptr)) != -1) {
        switch (c) {
        case 'w': width = static_cast<size_t>(std::atoi(optarg)); break;
        case 'f': fold = true; break;
        case 'i': if (!load(optarg, ignore)) return 1; break;
        case 'o': use_only = true; if (!load(optarg, only)) return 1; break;
        case 'A': case 'G': case 'r': break;
        default: return 1;
        }
    }
    if (!use_only && ignore.empty()) for (const char *w : {"a", "an", "the", "of", "and", "or", "to", "in", "is"}) ignore.insert(w);
    struct Hit { std::string key, before, after; };
    std::vector<Hit> hits;
    for (const std::string &f : operands(argc, argv)) {
        std::string data;
        if (!read_input(f, data)) return 1;
        for (const std::string &line : split_lines(data)) {
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && !std::isalnum(static_cast<unsigned char>(line[i]))) ++i;
                size_t j = i;
                while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
                if (j > i) {
                    std::string word = line.substr(i, j - i), cmp = word;
                    if (fold) for (char &ch : cmp) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    const bool ok = use_only ? only.count(cmp) > 0 : !ignore.count(cmp);
                    if (ok) hits.push_back({word, line.substr(0, i), line.substr(j)});
                }
                i = j;
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [&](const Hit &a, const Hit &b) {
        std::string x = a.key, y = b.key;
        if (fold) { for (char &ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); for (char &ch : y) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        return std::strcoll(x.c_str(), y.c_str()) < 0;
    });
    const size_t half = width / 2;
    for (const Hit &h : hits) {
        std::string left = h.before;
        if (left.size() > half) left = left.substr(left.size() - half);
        std::string right = h.key + h.after;
        if (right.size() > width - half) right = right.substr(0, width - half);
        out(std::string(half > left.size() ? half - left.size() : 0, ' ') + left + "   " + right + "\n");
    }
    return 0;
}

namespace {

bool numfmt_parse(const std::string &s, const std::string &from, long double &v)
{
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    char *end = nullptr;
    v = std::strtold(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    std::string sfx = end;
    if (sfx.empty() || from == "none") return sfx.empty();
    const bool iec = from == "iec" || from == "iec-i" || (from == "auto" && sfx.size() >= 2 && sfx.back() == 'i');
    const long double base = (from == "si" || (from == "auto" && !iec)) ? 1000.0L : 1024.0L;
    if (!sfx.empty() && sfx.back() == 'i') sfx.pop_back();
    static const char *letters = "KMGTPEZY";
    const char *p = std::strchr(letters, std::toupper(static_cast<unsigned char>(sfx[0])));
    if (!p || sfx.size() != 1) return false;
    for (int n = 0; n <= p - letters; ++n) v *= base;
    return true;
}

std::string numfmt_format(long double v, const std::string &to, const std::string &round, const std::string &fmt)
{
    std::string suffix;
    if (to != "none") {
        const bool iec = to == "iec" || to == "iec-i";
        const long double base = iec ? 1024.0L : 1000.0L;
        static const char *letters = "KMGTPEZY";
        int n = 0;
        while (std::fabs(v) >= base && n < 8) { v /= base; ++n; }
        if (n) { suffix = std::string(1, n == 1 && !iec ? 'k' : letters[n - 1]); if (to == "iec-i") suffix += "i"; }
        if (n) {
            if (round == "down") v = std::floor(v * 10) / 10; else if (round == "nearest") v = std::round(v * 10) / 10; else if (round == "towards-zero") v = std::trunc(v * 10) / 10; else v = std::ceil(v * 10) / 10;
            if (std::fabs(v) >= 10) v = (round == "down") ? std::floor(v) : (round == "nearest" ? std::round(v) : std::ceil(v));
        }
    }
    char b[128];
    if (!fmt.empty()) std::snprintf(b, sizeof b, fmt.c_str(), static_cast<double>(v));
    else if (suffix.empty() || std::fabs(v - std::trunc(v)) < 1e-9) std::snprintf(b, sizeof b, "%.0Lf", v);
    else std::snprintf(b, sizeof b, "%.1Lf", v);
    return std::string(b) + suffix;
}

} // namespace

int cmd_numfmt(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [NUMBER]...", "Reformat NUMBER(s), or the numbers from standard input if none are specified.")) {
        return 0;
    }
    static const option lo[] = {{"from", required_argument, nullptr, 1}, {"to", required_argument, nullptr, 2}, {"field", required_argument, nullptr, 3},
                                {"delimiter", required_argument, nullptr, 'd'}, {"padding", required_argument, nullptr, 4}, {"suffix", required_argument, nullptr, 5},
                                {"round", required_argument, nullptr, 6}, {"format", required_argument, nullptr, 7}, {"header", optional_argument, nullptr, 8},
                                {"invalid", required_argument, nullptr, 9}, {"grouping", no_argument, nullptr, 'g'}, {"zero-terminated", no_argument, nullptr, 'z'}, kEnd};
    std::string from = "none", to = "none", round = "from-zero", fmt, delim, suffix, invalid = "abort";
    int field = 1, padding = 0, header = 0;
    char eol = '\n';
    int c;
    while ((c = getopt_long(argc, argv, "d:z", lo, nullptr)) != -1) {
        switch (c) {
        case 1: from = optarg; break;
        case 2: to = optarg; break;
        case 3: field = std::atoi(optarg); break;
        case 'd': delim = optarg; break;
        case 4: padding = std::atoi(optarg); break;
        case 5: suffix = optarg; break;
        case 6: round = optarg; break;
        case 7: fmt = optarg; break;
        case 8: header = optarg ? std::atoi(optarg) : 1; break;
        case 9: invalid = optarg; break;
        case 'g': break;
        case 'z': eol = '\0'; break;
        default: return 1;
        }
    }
    std::vector<std::string> inputs(argv + optind, argv + argc);
    if (inputs.empty()) {
        std::string data;
        if (!read_input("-", data)) return 1;
        inputs = split_lines(data, eol);
    }
    int rc = 0, lineno = 0;
    for (std::string line : inputs) {
        if (++lineno <= header) { out(line + eol); continue; }
        // split into fields
        std::vector<std::string> f;
        std::string sep = delim.empty() ? " " : delim;
        if (delim.empty()) { std::istringstream ss(line); std::string w; while (ss >> w) f.push_back(w); }
        else { size_t pos = 0; while (pos <= line.size()) { size_t e = line.find(delim[0], pos); if (e == std::string::npos) e = line.size(); f.push_back(line.substr(pos, e - pos)); pos = e + 1; } }
        if (field < 1 || static_cast<size_t>(field) > f.size()) { out(line + eol); continue; }
        std::string token = f[field - 1];
        if (!suffix.empty() && token.size() >= suffix.size() && token.compare(token.size() - suffix.size(), suffix.size(), suffix) == 0) token.resize(token.size() - suffix.size());
        long double v;
        if (!numfmt_parse(token, from, v)) {
            err("invalid number: '" + token + "'");
            if (invalid == "abort") return 2;
            rc = 2;
            out(line + eol);
            continue;
        }
        std::string r = numfmt_format(v, to, round, fmt) + suffix;
        if (padding > 0 && static_cast<int>(r.size()) < padding) r = std::string(static_cast<size_t>(padding) - r.size(), ' ') + r;
        else if (padding < 0 && static_cast<int>(r.size()) < -padding) r += std::string(static_cast<size_t>(-padding) - r.size(), ' ');
        f[field - 1] = r;
        out(join(f, sep) + eol);
    }
    return rc;
}

} // namespace cu
