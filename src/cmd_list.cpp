#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <sys/statvfs.h>
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
// ls, dir, vdir, df, du, stat.
#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace cu {

const char *const kDefaultLsColors =
    "rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:do=01;35:bd=40;33;01:cd=40;33;01:or=40;31;01:mi=00:"
    "su=37;41:sg=30;43:ca=00:tw=30;42:ow=34;42:st=37;44:ex=01;32:"
    "*.tar=01;31:*.tgz=01;31:*.zip=01;31:*.gz=01;31:*.bz2=01;31:*.xz=01;31:*.zst=01;31:*.7z=01;31:*.rar=01;31:"
    "*.deb=01;31:*.rpm=01;31:*.pkg.tar.zst=01;31:*.pkg.tar.xz=01;31:*.AppImage=01;32:"
    "*.jpg=01;35:*.jpeg=01;35:*.png=01;35:*.gif=01;35:*.svg=01;35:*.webp=01;35:*.avif=01;35:"
    "*.mp4=01;35:*.mkv=01;35:*.webm=01;35:*.mov=01;35:*.mp3=00;36:*.flac=00;36:*.ogg=00;36:*.opus=00;36:*.wav=00;36";

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

int term_width()
{
    winsize ws{};
    if (::ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    if (const char *c = std::getenv("COLUMNS")) {
        const int v = std::atoi(c);
        if (v > 0) return v;
    }
    return 80;
}

/// Display width of a UTF-8 string (counts code points; good enough for aligning columns).
size_t display_width(const std::string &s)
{
    size_t w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

// ---------------------------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------------------------
struct Colors {
    std::map<std::string, std::string> kind;      // "di", "ln", ...
    std::vector<std::pair<std::string, std::string>> ext; // "*.tar" -> sgr

    void load()
    {
        const char *env = std::getenv("LS_COLORS");
        parse(kDefaultLsColors);
        if (env && *env) parse(env);
    }
    void parse(const std::string &spec)
    {
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t e = spec.find(':', pos);
            if (e == std::string::npos) e = spec.size();
            const std::string item = spec.substr(pos, e - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
                if (k.size() > 1 && k[0] == '*') {
                    bool replaced = false;
                    for (auto &p : ext) if (p.first == k) { p.second = v; replaced = true; }
                    if (!replaced) ext.emplace_back(k, v);
                } else {
                    kind[k] = v;
                }
            }
            pos = e + 1;
        }
    }
    std::string get(const std::string &k) const
    {
        const auto it = kind.find(k);
        return it == kind.end() ? "" : it->second;
    }
};

struct Entry {
    std::string name;
    std::string path;
    struct stat st{};
    bool have_st = false;
    bool dangling = false;
    std::string link_target;
    struct stat target_st{};
    bool have_target = false;
};

struct LsOpts {
    bool all = false, almost_all = false, longfmt = false, human = false, si = false, one = false, columns = false, across = false,
         recursive = false, dir_as_file = false, classify = false, slash = false, inode = false, blocks = false,
         numeric = false, no_owner = false, no_group = false, deref = false, deref_cmd = false, reverse = false, comma = false,
         group_dirs = false, ignore_backups = false, escape = false, hide_ctrl = false, quote_all = false, context = false;
    char sort = 'n'; // n name, S size, t time, X extension, v version, U none
    char timesel = 'm'; // m, a, c
    std::string time_style = "locale";
    bool color = false;
    bool literal_names = false;
    std::vector<std::string> ignore;
    uint64_t block_size = 1024;
};

std::string quote_name(const std::string &n, const LsOpts &o, bool tty)
{
    if (o.quote_all) return "\"" + n + "\"";
    if (o.literal_names) return n;
    if (o.escape) {
        std::string r;
        for (unsigned char c : n) {
            if (c == '\n') r += "\\n";
            else if (c == '\t') r += "\\t";
            else if (c == '\\') r += "\\\\";
            else if (c < 32 || c == 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); r += b; }
            else r += static_cast<char>(c);
        }
        return r;
    }
    if (o.hide_ctrl) {
        std::string r = n;
        for (char &c : r) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
        return r;
    }
    if (tty && n.find_first_of(" \t\n'\"\\$`*?[]{}()<>|&;!#~") != std::string::npos) {
        return quote_shell(n);
    }
    return n;
}

std::string version_key_pad(const std::string &s)
{
    // zero-pad digit runs so a plain string compare sorts "file2" before "file10"
    std::string r;
    for (size_t i = 0; i < s.size();) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            r += std::string(20 - std::min<size_t>(20, j - i), '0') + s.substr(i, j - i);
            i = j;
        } else {
            r += s[i++];
        }
    }
    return r;
}

class Lister
{
public:
    Lister(LsOpts o, Colors c) : o_(std::move(o)), colors_(std::move(c)), tty_(::isatty(1)) {}

    int run(std::vector<std::string> args)
    {
        if (args.empty()) args.push_back(".");
        std::vector<Entry> files, dirs;
        int rc = 0;
        for (const std::string &a : args) {
            Entry e;
            e.name = a;
            e.path = a;
            const bool follow = o_.deref || o_.deref_cmd || (!o_.longfmt && !o_.classify && !o_.dir_as_file) ? true : o_.deref;
            if (!load(e, follow || (!o_.longfmt && !o_.dir_as_file))) {
                err_errno("cannot access '" + a + "'");
                rc = 2;
                continue;
            }
            // symlink to a directory on the command line is listed as a directory unless -d / -l
            bool is_dir = S_ISDIR(e.st.st_mode);
            if (S_ISLNK(e.st.st_mode) && e.have_target && S_ISDIR(e.target_st.st_mode) && !o_.longfmt && !o_.dir_as_file && !o_.classify) {
                is_dir = true;
            }
            if (is_dir && !o_.dir_as_file) dirs.push_back(e);
            else files.push_back(e);
        }
        sort_entries(files);
        sort_entries(dirs);
        const bool header = args.size() > 1 || o_.recursive;
        bool first = true;
        if (!files.empty()) {
            print(files, false);
            first = false;
        }
        for (const Entry &d : dirs) {
            rc |= list_dir(d.path, header, first, true);
            first = false;
        }
        return rc;
    }

private:
    bool load(Entry &e, bool follow)
    {
        if (::lstat(e.path.c_str(), &e.st) != 0) return false;
        e.have_st = true;
        if (S_ISLNK(e.st.st_mode)) {
            e.link_target = readlink_str(e.path);
            if (::stat(e.path.c_str(), &e.target_st) == 0) e.have_target = true;
            else e.dangling = true;
            if (follow && e.have_target) {
                e.st = e.target_st;
            }
        }
        return true;
    }

    void sort_entries(std::vector<Entry> &v) const
    {
        if (o_.sort == 'U') {
            if (o_.reverse) std::reverse(v.begin(), v.end());
            return;
        }
        auto cmp = [&](const Entry &a, const Entry &b) {
            if (o_.group_dirs) {
                const bool da = S_ISDIR(a.st.st_mode), db = S_ISDIR(b.st.st_mode);
                if (da != db) return da;
            }
            int r = 0;
            switch (o_.sort) {
            case 'S':
                if (a.st.st_size != b.st.st_size) r = a.st.st_size > b.st.st_size ? -1 : 1;
                break;
            case 't': {
                const timespec &ta = ts_of(a), &tb = ts_of(b);
                if (ta.tv_sec != tb.tv_sec) r = ta.tv_sec > tb.tv_sec ? -1 : 1;
                else if (ta.tv_nsec != tb.tv_nsec) r = ta.tv_nsec > tb.tv_nsec ? -1 : 1;
                break;
            }
            case 'X': {
                const size_t ea = a.name.rfind('.'), eb = b.name.rfind('.');
                const std::string xa = ea == std::string::npos ? "" : a.name.substr(ea);
                const std::string xb = eb == std::string::npos ? "" : b.name.substr(eb);
                r = std::strcoll(xa.c_str(), xb.c_str());
                break;
            }
            case 'v':
                r = version_key_pad(a.name).compare(version_key_pad(b.name));
                break;
            default: break;
            }
            if (r == 0) r = std::strcoll(a.name.c_str(), b.name.c_str());
            if (r == 0) r = a.name.compare(b.name);
            return o_.reverse ? r > 0 : r < 0;
        };
        std::stable_sort(v.begin(), v.end(), cmp);
    }

    const timespec &ts_of(const Entry &e) const
    {
        return o_.timesel == 'a' ? e.st.st_atim : (o_.timesel == 'c' ? e.st.st_ctim : e.st.st_mtim);
    }

    bool ignored(const std::string &n) const
    {
        if (!o_.all && !o_.almost_all && !n.empty() && n[0] == '.') return true;
        if (o_.almost_all && (n == "." || n == "..")) return true;
        if (o_.ignore_backups && !n.empty() && n.back() == '~') return true;
        for (const std::string &p : o_.ignore) {
            if (::fnmatch(p.c_str(), n.c_str(), FNM_PERIOD) == 0) return true;
        }
        return false;
    }

    int list_dir(const std::string &path, bool header, bool first, bool top)
    {
        DIR *d = ::opendir(path.c_str());
        if (!d) {
            err_errno("cannot open directory '" + path + "'");
            return 2;
        }
        std::vector<Entry> ents;
        while (const dirent *de = ::readdir(d)) {
            const std::string n = de->d_name;
            if (ignored(n)) continue;
            Entry e;
            e.name = n;
            e.path = (path == "/" ? "" : path) + "/" + n;
            if (path == ".") e.path = n; // keep short names for the common case
            if (!load(e, o_.deref)) {
                err_errno("cannot access '" + e.path + "'");
                continue;
            }
            ents.push_back(std::move(e));
        }
        ::closedir(d);
        sort_entries(ents);
        if (header) {
            if (!first) out("\n");
            out(quote_name(path, o_, tty_) + ":\n");
        }
        if (o_.longfmt || o_.blocks) {
            uint64_t total = 0;
            for (const Entry &e : ents) total += static_cast<uint64_t>(e.st.st_blocks) * 512;
            out("total " + blocks_str(total) + "\n");
        }
        print(ents, true);
        int rc = 0;
        if (o_.recursive) {
            for (const Entry &e : ents) {
                if (S_ISDIR(e.st.st_mode) && e.name != "." && e.name != ".." && !S_ISLNK(e.st.st_mode)) {
                    rc |= list_dir(e.path, true, false, false);
                }
            }
        }
        (void)top;
        return rc;
    }

    std::string blocks_str(uint64_t bytes) const
    {
        if (o_.human) return human_size(static_cast<double>(bytes), o_.si);
        return std::to_string((bytes + o_.block_size - 1) / o_.block_size);
    }

    std::string size_str(const Entry &e) const
    {
        if (S_ISCHR(e.st.st_mode) || S_ISBLK(e.st.st_mode)) {
            return std::to_string(major(e.st.st_rdev)) + ", " + std::to_string(minor(e.st.st_rdev));
        }
        if (o_.human) return human_size(static_cast<double>(e.st.st_size), o_.si);
        return std::to_string(static_cast<long long>(e.st.st_size));
    }

    std::string time_str(const Entry &e) const
    {
        const timespec &t = ts_of(e);
        const time_t now = std::time(nullptr);
        std::string style = o_.time_style;
        if (style == "full-iso") {
            char ns[16];
            std::snprintf(ns, sizeof ns, ".%09ld", static_cast<long>(t.tv_nsec));
            return format_time(t.tv_sec, "%Y-%m-%d %H:%M:%S", false) + ns + format_time(t.tv_sec, " %z", false);
        }
        if (style == "long-iso") return format_time(t.tv_sec, "%Y-%m-%d %H:%M", false);
        if (style == "iso") {
            const bool recent = t.tv_sec > now - 15778476 && t.tv_sec < now + 3600;
            return recent ? format_time(t.tv_sec, "%m-%d %H:%M", false) : format_time(t.tv_sec, "%Y-%m-%d ", false);
        }
        if (style.rfind('+', 0) == 0) return format_time(t.tv_sec, style.substr(1), false);
        const bool recent = t.tv_sec > now - 15778476 && t.tv_sec < now + 3600;
        return recent ? format_time(t.tv_sec, "%b %e %H:%M", false) : format_time(t.tv_sec, "%b %e  %Y", false);
    }

    std::string sgr_for(const Entry &e) const
    {
        const mode_t m = e.st.st_mode;
        // symlinks colour by their own type; the symlink check uses lstat data
        struct stat l{};
        const bool is_link = ::lstat(e.path.c_str(), &l) == 0 && S_ISLNK(l.st_mode);
        if (is_link) {
            if (e.dangling) { const std::string s = colors_.get("or"); return s.empty() ? colors_.get("ln") : s; }
            const std::string ln = colors_.get("ln");
            if (ln == "target") return sgr_for_mode(e.target_st.st_mode, e.name);
            return ln;
        }
        return sgr_for_mode(m, e.name);
    }

    std::string sgr_for_mode(mode_t m, const std::string &name) const
    {
        if (S_ISDIR(m)) {
            if ((m & S_ISVTX) && (m & S_IWOTH)) return colors_.get("tw");
            if (m & S_IWOTH) return colors_.get("ow");
            if (m & S_ISVTX) return colors_.get("st");
            return colors_.get("di");
        }
        if (S_ISFIFO(m)) return colors_.get("pi");
        if (S_ISSOCK(m)) return colors_.get("so");
        if (S_ISBLK(m)) return colors_.get("bd");
        if (S_ISCHR(m)) return colors_.get("cd");
        if (m & S_ISUID) return colors_.get("su");
        if (m & S_ISGID) return colors_.get("sg");
        // longest matching extension pattern wins
        std::string best;
        size_t best_len = 0;
        for (const auto &p : colors_.ext) {
            const std::string pat = p.first.substr(1);
            if (name.size() >= pat.size() && ::strcasecmp(name.c_str() + name.size() - pat.size(), pat.c_str()) == 0 && pat.size() >= best_len) {
                best = p.second;
                best_len = pat.size();
            }
        }
        if (!best.empty()) return best;
        if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) return colors_.get("ex");
        return colors_.get("fi");
    }

    std::string indicator(const Entry &e) const
    {
        const mode_t m = e.st.st_mode;
        struct stat l{};
        if (::lstat(e.path.c_str(), &l) == 0 && S_ISLNK(l.st_mode) && (o_.classify)) return "@";
        if (S_ISDIR(m)) return (o_.classify || o_.slash) ? "/" : "";
        if (!o_.classify) return "";
        if (S_ISFIFO(m)) return "|";
        if (S_ISSOCK(m)) return "=";
        if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) return "*";
        return "";
    }

    std::string colored(const Entry &e) const
    {
        std::string n = quote_name(e.name, o_, tty_);
        if (o_.color) {
            const std::string sgr = sgr_for(e);
            if (!sgr.empty() && sgr != "0" && sgr != "00") {
                n = "\x1b[" + sgr + "m" + n + "\x1b[0m";
            }
        }
        return n + indicator(e);
    }

    std::string plain_for_width(const Entry &e) const
    {
        return quote_name(e.name, o_, tty_) + indicator(e);
    }

    void print(const std::vector<Entry> &ents, bool in_dir)
    {
        (void)in_dir;
        if (o_.longfmt) {
            print_long(ents);
        } else if (o_.one || (!o_.columns && !o_.comma && !tty_)) {
            for (const Entry &e : ents) {
                out(prefix(e, 0, 0) + colored(e) + "\n");
            }
        } else if (o_.comma) {
            std::string line;
            const int w = term_width();
            for (size_t i = 0; i < ents.size(); ++i) {
                const std::string piece = plain_for_width(ents[i]) + (i + 1 < ents.size() ? ", " : "");
                if (!line.empty() && display_width(line) + piece.size() > static_cast<size_t>(w)) {
                    out(line + "\n");
                    line.clear();
                }
                line += colored(ents[i]) + (i + 1 < ents.size() ? ", " : "");
            }
            if (!line.empty()) out(line + "\n");
        } else {
            print_columns(ents);
        }
    }

    std::string prefix(const Entry &e, size_t inode_w, size_t block_w) const
    {
        std::string p;
        if (o_.inode) {
            std::string s = std::to_string(static_cast<unsigned long long>(e.st.st_ino));
            p += std::string(inode_w > s.size() ? inode_w - s.size() : 0, ' ') + s + " ";
        }
        if (o_.blocks) {
            std::string s = blocks_str(static_cast<uint64_t>(e.st.st_blocks) * 512);
            p += std::string(block_w > s.size() ? block_w - s.size() : 0, ' ') + s + " ";
        }
        return p;
    }

    void print_columns(const std::vector<Entry> &ents)
    {
        if (ents.empty()) return;
        const int width = term_width();
        std::vector<std::string> shown(ents.size());
        size_t maxw = 0, inode_w = 0, block_w = 0;
        for (const Entry &e : ents) {
            if (o_.inode) inode_w = std::max(inode_w, std::to_string(static_cast<unsigned long long>(e.st.st_ino)).size());
            if (o_.blocks) block_w = std::max(block_w, blocks_str(static_cast<uint64_t>(e.st.st_blocks) * 512).size());
        }
        for (size_t i = 0; i < ents.size(); ++i) {
            shown[i] = prefix(ents[i], inode_w, block_w);
            maxw = std::max(maxw, display_width(shown[i]) + display_width(plain_for_width(ents[i])));
        }
        const size_t colw = maxw + 2;
        size_t ncols = std::max<size_t>(1, static_cast<size_t>(width) / colw);
        ncols = std::min(ncols, ents.size());
        const size_t nrows = (ents.size() + ncols - 1) / ncols;
        for (size_t r = 0; r < nrows; ++r) {
            std::string line;
            for (size_t c = 0; c < ncols; ++c) {
                const size_t idx = o_.across ? r * ncols + c : c * nrows + r;
                if (idx >= ents.size()) continue;
                const std::string cell = shown[idx] + colored(ents[idx]);
                const size_t vis = display_width(shown[idx]) + display_width(plain_for_width(ents[idx]));
                line += cell;
                const bool last = (o_.across ? idx + 1 >= ents.size() || c + 1 == ncols : (c + 1) * nrows + r >= ents.size());
                if (!last) line += std::string(colw - vis, ' ');
            }
            out(line + "\n");
        }
    }

    void print_long(const std::vector<Entry> &ents)
    {
        struct Row {
            std::string inode, blocks, mode, nlink, owner, group, ctx, size, time, name;
        };
        std::vector<Row> rows;
        size_t wi = 0, wb = 0, wn = 0, wo = 0, wg = 0, wc = 0, ws = 0;
        for (const Entry &e : ents) {
            Row r;
            r.inode = std::to_string(static_cast<unsigned long long>(e.st.st_ino));
            r.blocks = blocks_str(static_cast<uint64_t>(e.st.st_blocks) * 512);
            std::string mode = mode_string(S_ISLNK(e.st.st_mode) || !o_.deref ? e.st.st_mode : e.st.st_mode);
            // extended attributes / ACL marker
            if (::listxattr(e.path.c_str(), nullptr, 0) > 0) {
                ssize_t len = ::listxattr(e.path.c_str(), nullptr, 0);
                std::string names(static_cast<size_t>(std::max<ssize_t>(len, 0)), '\0');
                if (::listxattr(e.path.c_str(), names.data(), names.size()) > 0) {
                    mode += (names.find("system.posix_acl") != std::string::npos) ? "+" : (names.find("security.selinux") != std::string::npos ? "." : "@");
                }
            }
            r.mode = mode;
            r.nlink = std::to_string(static_cast<unsigned long long>(e.st.st_nlink));
            r.owner = o_.numeric ? std::to_string(e.st.st_uid) : user_name(e.st.st_uid);
            r.group = o_.numeric ? std::to_string(e.st.st_gid) : group_name(e.st.st_gid);
            if (o_.context) {
                char buf[256];
                const ssize_t n = ::lgetxattr(e.path.c_str(), "security.selinux", buf, sizeof buf - 1);
                r.ctx = n > 0 ? std::string(buf, static_cast<size_t>(n)) : "?";
                while (!r.ctx.empty() && r.ctx.back() == '\0') r.ctx.pop_back();
            }
            r.size = size_str(e);
            r.time = time_str(e);
            r.name = colored(e);
            if (S_ISLNK(e.st.st_mode) || (!e.link_target.empty())) {
                struct stat l{};
                if (::lstat(e.path.c_str(), &l) == 0 && S_ISLNK(l.st_mode)) {
                    Entry t;
                    t.name = e.link_target;
                    t.path = e.link_target;
                    std::string target = quote_name(e.link_target, o_, tty_);
                    if (o_.color && e.have_target) {
                        t.st = e.target_st;
                        const std::string sgr = sgr_for_mode(e.target_st.st_mode, e.link_target);
                        if (!sgr.empty() && sgr != "0") target = "\x1b[" + sgr + "m" + target + "\x1b[0m";
                    }
                    r.name += " -> " + target;
                    if (o_.classify && e.have_target) {
                        // indicator of the target
                        Entry te;
                        te.st = e.target_st;
                        te.path = e.link_target;
                        r.name += indicator(te);
                    }
                }
            }
            wi = std::max(wi, r.inode.size());
            wb = std::max(wb, r.blocks.size());
            wn = std::max(wn, r.nlink.size());
            wo = std::max(wo, display_width(r.owner));
            wg = std::max(wg, display_width(r.group));
            wc = std::max(wc, r.ctx.size());
            ws = std::max(ws, r.size.size());
            rows.push_back(std::move(r));
        }
        auto padl = [](const std::string &s, size_t w) { return std::string(w > s.size() ? w - s.size() : 0, ' ') + s; };
        auto padr = [](const std::string &s, size_t w) { const size_t dw = display_width(s); return s + std::string(w > dw ? w - dw : 0, ' '); };
        for (const Row &r : rows) {
            std::string line;
            if (o_.inode) line += padl(r.inode, wi) + " ";
            if (o_.blocks) line += padl(r.blocks, wb) + " ";
            line += r.mode + " " + padl(r.nlink, wn) + " ";
            if (!o_.no_owner) line += padr(r.owner, wo) + " ";
            if (!o_.no_group) line += padr(r.group, wg) + " ";
            if (o_.context) line += padr(r.ctx, wc) + " ";
            line += padl(r.size, ws) + " " + r.time + " " + r.name;
            out(line + "\n");
        }
    }

    LsOpts o_;
    Colors colors_;
    bool tty_;
};

int ls_main(int argc, char **argv, bool force_columns, bool force_long, bool force_escape)
{
    static const option lo[] = {
        {"all", no_argument, nullptr, 'a'}, {"almost-all", no_argument, nullptr, 'A'}, {"human-readable", no_argument, nullptr, 'h'},
        {"si", no_argument, nullptr, 1}, {"recursive", no_argument, nullptr, 'R'}, {"directory", no_argument, nullptr, 'd'},
        {"classify", optional_argument, nullptr, 'F'}, {"reverse", no_argument, nullptr, 'r'}, {"inode", no_argument, nullptr, 'i'},
        {"size", no_argument, nullptr, 's'}, {"numeric-uid-gid", no_argument, nullptr, 'n'}, {"dereference", no_argument, nullptr, 'L'},
        {"color", optional_argument, nullptr, 2}, {"group-directories-first", no_argument, nullptr, 3},
        {"time-style", required_argument, nullptr, 4}, {"time", required_argument, nullptr, 5}, {"sort", required_argument, nullptr, 6},
        {"ignore", required_argument, nullptr, 'I'}, {"ignore-backups", no_argument, nullptr, 'B'}, {"escape", no_argument, nullptr, 'b'},
        {"hide-control-chars", no_argument, nullptr, 'q'}, {"literal", no_argument, nullptr, 'N'}, {"quote-name", no_argument, nullptr, 'Q'},
        {"context", no_argument, nullptr, 'Z'}, {"block-size", required_argument, nullptr, 7}, {"file-type", no_argument, nullptr, 8},
        {"full-time", no_argument, nullptr, 9}, kEnd};
    LsOpts o;
    bool want_color = false, color_auto = false;
    const bool tty = ::isatty(1);
    if (force_columns) o.columns = true;
    if (force_long) o.longfmt = true;
    if (force_escape) o.escape = true;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "aAbBcCdDfFghHiI:klLmnNopqQrRsStTuUvxXZ1", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': o.all = true; break;
        case 'A': o.almost_all = true; break;
        case 'b': o.escape = true; break;
        case 'B': o.ignore_backups = true; break;
        case 'c': o.timesel = 'c'; break;
        case 'C': o.columns = true; o.longfmt = false; o.one = false; break;
        case 'd': o.dir_as_file = true; break;
        case 'f': o.all = true; o.sort = 'U'; break;
        case 'F': o.classify = true; break;
        case 8: o.classify = false; o.slash = false; break;
        case 'g': o.longfmt = true; o.no_owner = true; break;
        case 'G': o.no_group = true; break;
        case 'h': o.human = true; break;
        case 'H': o.deref_cmd = true; break;
        case 'i': o.inode = true; break;
        case 'I': o.ignore.push_back(optarg); break;
        case 'k': o.block_size = 1024; break;
        case 'l': o.longfmt = true; break;
        case 'L': o.deref = true; break;
        case 'm': o.comma = true; o.longfmt = false; break;
        case 'n': o.numeric = true; o.longfmt = true; break;
        case 'N': o.literal_names = true; break;
        case 'o': o.longfmt = true; o.no_group = true; break;
        case 'p': o.slash = true; break;
        case 'q': o.hide_ctrl = true; break;
        case 'Q': o.quote_all = true; break;
        case 'r': o.reverse = true; break;
        case 'R': o.recursive = true; break;
        case 's': o.blocks = true; break;
        case 'S': o.sort = 'S'; break;
        case 't': o.sort = 't'; break;
        case 'u': o.timesel = 'a'; break;
        case 'U': o.sort = 'U'; break;
        case 'v': o.sort = 'v'; break;
        case 'x': o.columns = true; o.across = true; o.longfmt = false; break;
        case 'X': o.sort = 'X'; break;
        case 'Z': o.context = true; break;
        case '1': o.one = true; o.columns = false; break;
        case 'T': case 'D': break;
        case 1: o.human = true; o.si = true; break;
        case 2: {
            const std::string w = optarg ? optarg : "always";
            if (w == "always" || w == "yes" || w == "force") want_color = true;
            else if (w == "auto" || w == "tty" || w == "if-tty") color_auto = true;
            break;
        }
        case 3: o.group_dirs = true; break;
        case 4: o.time_style = optarg; break;
        case 5: o.timesel = optarg[0] == 'a' || optarg[0] == 'u' ? 'a' : (optarg[0] == 'c' ? 'c' : 'm'); break;
        case 6: {
            const std::string w = optarg;
            o.sort = w == "size" ? 'S' : w == "time" ? 't' : w == "extension" ? 'X' : w == "version" ? 'v' : w == "none" ? 'U' : 'n';
            break;
        }
        case 7: { uint64_t b; if (parse_size(optarg, b) && b) o.block_size = b; break; }
        case 9: o.longfmt = true; o.time_style = "full-iso"; break;
        default: return 2;
        }
    }
    if (const char *tsv = std::getenv("TIME_STYLE"); tsv && o.time_style == "locale") o.time_style = tsv;
    if (const char *bs = std::getenv("BLOCK_SIZE"); bs && !o.human) { uint64_t b; if (parse_size(bs, b) && b) o.block_size = b; }
    o.color = want_color || (color_auto && tty);
    Colors colors;
    if (o.color) colors.load();
    if (!o.columns && !o.longfmt && !o.one && !o.comma && tty) o.columns = true;
    std::vector<std::string> args(argv + optind, argv + argc);
    Lister l(o, colors);
    return l.run(args);
}

} // namespace

int cmd_ls(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "List information about the FILEs (the current directory by default).")) {
        return 0;
    }
    return ls_main(argc, argv, false, false, false);
}
int cmd_dir(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "List directory contents in columns (like ls -C -b).")) {
        return 0;
    }
    return ls_main(argc, argv, true, false, true);
}
int cmd_vdir(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "List directory contents in long format (like ls -l -b).")) {
        return 0;
    }
    return ls_main(argc, argv, false, true, true);
}

// ---------------------------------------------------------------------------------------------
// df
// ---------------------------------------------------------------------------------------------
int cmd_df(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...",
                            "Show information about the file system on which each FILE resides, or all file systems by default.\n"
                            "Arch/CachyOS: btrfs subvolume mounts of one device are listed once; pseudo file systems are hidden unless -a.")) {
        return 0;
    }
    static const option lo[] = {{"all", no_argument, nullptr, 'a'}, {"human-readable", no_argument, nullptr, 'h'},
                                {"si", no_argument, nullptr, 'H'}, {"inodes", no_argument, nullptr, 'i'}, {"local", no_argument, nullptr, 'l'},
                                {"print-type", no_argument, nullptr, 'T'}, {"type", required_argument, nullptr, 't'},
                                {"exclude-type", required_argument, nullptr, 'x'}, {"total", no_argument, nullptr, 1},
                                {"portability", no_argument, nullptr, 'P'}, {"block-size", required_argument, nullptr, 'B'}, kEnd};
    bool all = false, human = false, si = false, inodes = false, local = false, show_type = false, total = false;
    std::set<std::string> only, excl;
    uint64_t bsize = 1024;
    int c;
    while ((c = getopt_long(argc, argv, "ahHiklmPTt:x:B:", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': all = true; break;
        case 'h': human = true; break;
        case 'H': human = si = true; break;
        case 'i': inodes = true; break;
        case 'k': bsize = 1024; break;
        case 'l': local = true; break;
        case 'm': bsize = 1024 * 1024; break;
        case 'P': break;
        case 'T': show_type = true; break;
        case 't': only.insert(optarg); break;
        case 'x': excl.insert(optarg); break;
        case 'B': if (!parse_size(optarg, bsize) || !bsize) { err(std::string("invalid --block-size argument '") + optarg + "'"); return 1; } break;
        case 1: total = true; break;
        default: return 1;
        }
    }
    struct Row {
        std::string source, type, target;
        uint64_t size = 0, used = 0, avail = 0;
        std::string pct;
    };
    std::vector<Row> rows;
    std::set<std::string> seen_sources;
    auto add = [&](const arch::Mount &m, const std::string &display_target) -> bool {
        struct statvfs sv;
        if (::statvfs(m.target.c_str(), &sv) != 0) return false;
        Row r;
        r.source = m.source;
        r.type = m.fstype;
        r.target = display_target;
        if (inodes) {
            r.size = sv.f_files;
            r.used = sv.f_files - sv.f_ffree;
            r.avail = sv.f_favail;
        } else {
            r.size = static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize;
            r.used = (static_cast<uint64_t>(sv.f_blocks) - sv.f_bfree) * sv.f_frsize;
            r.avail = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
        }
        const uint64_t denom = inodes ? r.used + r.avail : r.used + r.avail;
        r.pct = denom == 0 ? "-" : std::to_string(static_cast<unsigned long long>(std::ceil(100.0 * static_cast<double>(r.used) / static_cast<double>(denom)))) + "%";
        if (!all && r.size == 0) return false;
        rows.push_back(std::move(r));
        return true;
    };
    auto type_ok = [&](const std::string &t) {
        if (!only.empty() && !only.count(t)) return false;
        if (excl.count(t)) return false;
        return true;
    };
    auto is_local = [](const std::string &t) {
        return !(t == "nfs" || t == "nfs4" || t == "cifs" || t == "smb3" || t == "smbfs" || t.rfind("fuse.sshfs", 0) == 0 || t == "9p");
    };

    if (optind < argc) {
        for (int i = optind; i < argc; ++i) {
            struct stat st;
            if (::stat(argv[i], &st) != 0) {
                err_errno(std::string("cannot access '") + argv[i] + "'");
                continue;
            }
            // find the mount containing the file: the longest matching mount target
            const arch::Mount *best = nullptr;
            const std::string real = canonicalize(argv[i], false);
            for (const arch::Mount &m : arch::mounts()) {
                if (m.dev != st.st_dev) continue;
                if (!best || m.target.size() > best->target.size() || (real.rfind(m.target, 0) == 0 && m.target.size() >= (best ? best->target.size() : 0))) {
                    if (real.rfind(m.target, 0) == 0 || m.dev == st.st_dev) best = &m;
                }
            }
            if (!best) {
                err(std::string("cannot read table of mounted file systems"));
                continue;
            }
            add(*best, best->target);
        }
    } else {
        for (const arch::Mount &m : arch::mounts()) {
            if (!all && arch::is_pseudo_fs(m.fstype)) continue;
            if (!type_ok(m.fstype)) continue;
            if (local && !is_local(m.fstype)) continue;
            if (m.source.empty() || (!all && m.source == "none")) {
                if (!all) continue;
            }
            // one line per btrfs / bcachefs device: subvolume mounts share the same free space
            if (!all && (m.fstype == "btrfs" || m.fstype == "bcachefs")) {
                const std::string key = m.fstype + ":" + m.source;
                if (seen_sources.count(key)) continue;
                seen_sources.insert(key);
            } else if (!all && m.source.rfind('/', 0) == 0 && m.fstype != "zfs") {
                const std::string key = m.source + "@" + std::to_string(m.dev) + m.root;
                if (seen_sources.count(key)) continue;
                seen_sources.insert(key);
            }
            // show the shortest mount point for btrfs ("/" instead of "/home" when both exist)
            std::string target = m.target;
            if (m.fstype == "btrfs") {
                for (const arch::Mount &o : arch::mounts()) {
                    if (o.fstype == "btrfs" && o.source == m.source && o.target.size() < target.size()) target = o.target;
                }
            }
            arch::Mount mm = m;
            mm.target = target;
            add(mm, target);
        }
    }

    auto fmt = [&](uint64_t v) {
        if (human) return human_size(static_cast<double>(v), si);
        if (inodes) return std::to_string(v);
        return std::to_string((v + bsize - 1) / bsize);
    };
    std::vector<std::vector<std::string>> table;
    std::vector<std::string> head = {"Filesystem"};
    if (show_type) head.push_back("Type");
    if (inodes) { head.insert(head.end(), {"Inodes", "IUsed", "IFree", "IUse%", "Mounted on"}); }
    else {
        std::string sz = human ? "Size" : (bsize == 1024 ? "1K-blocks" : bsize == 1048576 ? "1M-blocks" : std::to_string(bsize) + "-blocks");
        head.insert(head.end(), {sz, "Used", "Avail", "Use%", "Mounted on"});
    }
    table.push_back(head);
    uint64_t tsize = 0, tused = 0, tavail = 0;
    for (const Row &r : rows) {
        std::vector<std::string> line = {r.source.empty() ? "-" : r.source};
        if (show_type) line.push_back(r.type);
        line.push_back(fmt(r.size));
        line.push_back(fmt(r.used));
        line.push_back(fmt(r.avail));
        line.push_back(r.pct);
        line.push_back(r.target);
        table.push_back(line);
        tsize += r.size; tused += r.used; tavail += r.avail;
    }
    if (total) {
        std::vector<std::string> line = {"total"};
        if (show_type) line.push_back("-");
        line.push_back(fmt(tsize));
        line.push_back(fmt(tused));
        line.push_back(fmt(tavail));
        const uint64_t d = tused + tavail;
        line.push_back(d == 0 ? "-" : std::to_string(static_cast<unsigned long long>(std::ceil(100.0 * static_cast<double>(tused) / static_cast<double>(d)))) + "%");
        line.push_back("-");
        table.push_back(line);
    }
    std::vector<size_t> w(head.size(), 0);
    for (const auto &line : table) {
        for (size_t i = 0; i < line.size(); ++i) w[i] = std::max(w[i], display_width(line[i]));
    }
    for (const auto &line : table) {
        std::string s;
        for (size_t i = 0; i < line.size(); ++i) {
            const bool left = (i == 0 || (show_type && i == 1) || i + 1 == line.size());
            const size_t dw = display_width(line[i]);
            if (i > 0) s += ' ';
            if (left) s += line[i] + (i + 1 == line.size() ? "" : std::string(w[i] - dw, ' '));
            else s += std::string(w[i] - dw, ' ') + line[i];
        }
        out(s + "\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// du
// ---------------------------------------------------------------------------------------------
namespace {

struct DuCtx {
    bool all = false, summarize = false, human = false, si = false, total = false, one_fs = false, deref = false, apparent = false,
         separate_dirs = false, count_links = false;
    int max_depth = INT_MAX;
    uint64_t bsize = 1024;
    uint64_t threshold = 0;
    bool thr_neg = false;
    char eol = '\n';
    std::vector<std::string> excludes;
    std::set<std::pair<dev_t, ino_t>> seen;
    dev_t root_dev = 0;
    int rc = 0;
    uint64_t grand = 0;
};

std::string du_fmt(const DuCtx &c, uint64_t bytes)
{
    if (c.human) return human_size(static_cast<double>(bytes), c.si);
    return std::to_string((bytes + c.bsize - 1) / c.bsize);
}

uint64_t du_walk(DuCtx &c, const std::string &path, int depth, bool top)
{
    struct stat st;
    if ((c.deref || (top && false) ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st)) != 0) {
        err_errno("cannot access '" + path + "'");
        c.rc = 1;
        return 0;
    }
    if (top) c.root_dev = st.st_dev;
    for (const std::string &p : c.excludes) {
        if (::fnmatch(p.c_str(), path.c_str(), 0) == 0 || ::fnmatch(p.c_str(), path.substr(path.find_last_of('/') + 1).c_str(), 0) == 0) return 0;
    }
    if (c.one_fs && st.st_dev != c.root_dev) return 0;
    uint64_t size = c.apparent ? static_cast<uint64_t>(st.st_size) : static_cast<uint64_t>(st.st_blocks) * 512;
    if (!S_ISDIR(st.st_mode)) {
        if (!c.count_links && st.st_nlink > 1) {
            if (!c.seen.insert({st.st_dev, st.st_ino}).second) return 0;
        }
        if ((c.all || top) && depth <= c.max_depth && !c.summarize) {
            if (!c.thr_neg ? size >= c.threshold : size <= c.threshold) out(du_fmt(c, size) + "\t" + path + c.eol);
        }
        return size;
    }
    if (!c.seen.insert({st.st_dev, st.st_ino}).second) return 0;
    uint64_t total = size;
    std::error_code ec;
    for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const uint64_t sub = du_walk(c, it->path().string(), depth + 1, false);
        struct stat cs;
        const bool sub_is_dir = ::lstat(it->path().c_str(), &cs) == 0 && S_ISDIR(cs.st_mode);
        if (!(c.separate_dirs && sub_is_dir)) total += sub;
    }
    if (ec) {
        err(path + ": " + ec.message());
        c.rc = 1;
    }
    if ((!c.summarize || top) && depth <= c.max_depth) {
        if (!c.thr_neg ? total >= c.threshold : total <= c.threshold) out(du_fmt(c, total) + "\t" + path + c.eol);
    }
    return total;
}

} // namespace

int cmd_du(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Summarize device usage of the set of FILEs, recursively for directories.")) {
        return 0;
    }
    static const option lo[] = {{"all", no_argument, nullptr, 'a'}, {"summarize", no_argument, nullptr, 's'}, {"human-readable", no_argument, nullptr, 'h'},
                                {"si", no_argument, nullptr, 1}, {"total", no_argument, nullptr, 'c'}, {"max-depth", required_argument, nullptr, 'd'},
                                {"one-file-system", no_argument, nullptr, 'x'}, {"dereference", no_argument, nullptr, 'L'}, {"apparent-size", no_argument, nullptr, 2},
                                {"bytes", no_argument, nullptr, 'b'}, {"separate-dirs", no_argument, nullptr, 'S'}, {"count-links", no_argument, nullptr, 'l'},
                                {"exclude", required_argument, nullptr, 3}, {"threshold", required_argument, nullptr, 't'}, {"null", no_argument, nullptr, '0'},
                                {"block-size", required_argument, nullptr, 'B'}, kEnd};
    DuCtx ctx;
    int c;
    while ((c = getopt_long(argc, argv, "asd:chkmbSxLlt:0B:", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': ctx.all = true; break;
        case 's': ctx.summarize = true; break;
        case 'h': ctx.human = true; break;
        case 1: ctx.human = true; ctx.si = true; break;
        case 'c': ctx.total = true; break;
        case 'd': ctx.max_depth = std::atoi(optarg); break;
        case 'k': ctx.bsize = 1024; break;
        case 'm': ctx.bsize = 1024 * 1024; break;
        case 'b': ctx.apparent = true; ctx.bsize = 1; break;
        case 'S': ctx.separate_dirs = true; break;
        case 'x': ctx.one_fs = true; break;
        case 'L': ctx.deref = true; break;
        case 'l': ctx.count_links = true; break;
        case 2: ctx.apparent = true; break;
        case 3: ctx.excludes.push_back(optarg); break;
        case 't': { std::string s = optarg; if (!s.empty() && s[0] == '-') { ctx.thr_neg = true; s = s.substr(1); } uint64_t v; if (!parse_size(s, v)) { err(std::string("invalid --threshold argument '") + optarg + "'"); return 1; } ctx.threshold = v; break; }
        case '0': ctx.eol = '\0'; break;
        case 'B': if (!parse_size(optarg, ctx.bsize) || !ctx.bsize) { err(std::string("invalid --block-size argument '") + optarg + "'"); return 1; } break;
        default: return 1;
        }
    }
    std::vector<std::string> paths(argv + optind, argv + argc);
    if (paths.empty()) paths.push_back(".");
    for (const std::string &p : paths) {
        ctx.seen.clear();
        ctx.grand += du_walk(ctx, p, 0, true);
    }
    if (ctx.total) out(du_fmt(ctx, ctx.grand) + "\ttotal" + ctx.eol);
    return ctx.rc;
}

// ---------------------------------------------------------------------------------------------
// stat
// ---------------------------------------------------------------------------------------------
namespace {

std::string file_type_name(mode_t m)
{
    if (S_ISREG(m)) return "regular file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISLNK(m)) return "symbolic link";
    if (S_ISCHR(m)) return "character special file";
    if (S_ISBLK(m)) return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "weird file";
}

std::string stat_time(const timespec &ts)
{
    char ns[16];
    std::snprintf(ns, sizeof ns, ".%09ld", static_cast<long>(ts.tv_nsec));
    return format_time(ts.tv_sec, "%Y-%m-%d %H:%M:%S", false) + ns + format_time(ts.tv_sec, " %z", false);
}

} // namespace

int cmd_stat(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE...", "Display file or file system status.")) {
        return 0;
    }
    static const option lo[] = {{"dereference", no_argument, nullptr, 'L'}, {"file-system", no_argument, nullptr, 'f'},
                                {"format", required_argument, nullptr, 'c'}, {"printf", required_argument, nullptr, 1},
                                {"terse", no_argument, nullptr, 't'}, kEnd};
    bool deref = false, fsmode = false, terse = false;
    std::string format;
    bool printf_mode = false;
    int c;
    while ((c = getopt_long(argc, argv, "Lfc:t", lo, nullptr)) != -1) {
        switch (c) {
        case 'L': deref = true; break;
        case 'f': fsmode = true; break;
        case 'c': format = optarg; break;
        case 1: format = optarg; printf_mode = true; break;
        case 't': terse = true; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string path = argv[i];
        if (fsmode) {
            struct statfs sf;
            struct statvfs sv;
            if (::statfs(path.c_str(), &sf) != 0 || ::statvfs(path.c_str(), &sv) != 0) {
                err_errno("cannot read file system information for '" + path + "'");
                rc = 1;
                continue;
            }
            const std::string tname = arch::fs_type_name(static_cast<uint64_t>(sf.f_type));
            if (format.empty()) {
                char b[1024];
                std::snprintf(b, sizeof b,
                              "  File: \"%s\"\n    ID: %08x%08x Namelen: %ld     Type: %s\nBlock size: %-10ld Fundamental block size: %ld\n"
                              "Blocks: Total: %-10llu Free: %-10llu Available: %llu\nInodes: Total: %-10llu Free: %llu\n",
                              path.c_str(), static_cast<unsigned>(sf.f_fsid.__val[0]), static_cast<unsigned>(sf.f_fsid.__val[1]),
                              static_cast<long>(sf.f_namelen), tname.c_str(), static_cast<long>(sf.f_bsize), static_cast<long>(sf.f_frsize),
                              static_cast<unsigned long long>(sf.f_blocks), static_cast<unsigned long long>(sf.f_bfree),
                              static_cast<unsigned long long>(sf.f_bavail), static_cast<unsigned long long>(sf.f_files),
                              static_cast<unsigned long long>(sf.f_ffree));
                out(b);
            } else {
                std::string o;
                for (size_t k = 0; k < format.size(); ++k) {
                    if (format[k] != '%' || k + 1 >= format.size()) { o += format[k]; continue; }
                    const char f = format[++k];
                    switch (f) {
                    case 'n': o += path; break;
                    case 'T': o += tname; break;
                    case 't': { char b[24]; std::snprintf(b, sizeof b, "%lx", static_cast<long>(sf.f_type)); o += b; break; }
                    case 'b': o += std::to_string(static_cast<unsigned long long>(sf.f_blocks)); break;
                    case 'f': o += std::to_string(static_cast<unsigned long long>(sf.f_bfree)); break;
                    case 'a': o += std::to_string(static_cast<unsigned long long>(sf.f_bavail)); break;
                    case 's': o += std::to_string(static_cast<long>(sf.f_bsize)); break;
                    case 'S': o += std::to_string(static_cast<long>(sf.f_frsize)); break;
                    case 'c': o += std::to_string(static_cast<unsigned long long>(sf.f_files)); break;
                    case 'd': o += std::to_string(static_cast<unsigned long long>(sf.f_ffree)); break;
                    case 'l': o += std::to_string(static_cast<long>(sf.f_namelen)); break;
                    case '%': o += '%'; break;
                    default: o += '%'; o += f;
                    }
                }
                out(printf_mode ? unescape(o) : o + "\n");
            }
            continue;
        }
        struct statx sx;
        const int flags = (deref ? 0 : AT_SYMLINK_NOFOLLOW);
        if (::statx(AT_FDCWD, path.c_str(), flags, STATX_BASIC_STATS | STATX_BTIME, &sx) != 0) {
            err_errno("cannot statx '" + path + "'");
            rc = 1;
            continue;
        }
        struct stat st;
        if ((deref ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st)) != 0) {
            err_errno("cannot stat '" + path + "'");
            rc = 1;
            continue;
        }
        const timespec at = {static_cast<time_t>(sx.stx_atime.tv_sec), static_cast<long>(sx.stx_atime.tv_nsec)};
        const timespec mt = {static_cast<time_t>(sx.stx_mtime.tv_sec), static_cast<long>(sx.stx_mtime.tv_nsec)};
        const timespec ct = {static_cast<time_t>(sx.stx_ctime.tv_sec), static_cast<long>(sx.stx_ctime.tv_nsec)};
        const bool has_birth = (sx.stx_mask & STATX_BTIME) != 0;
        const timespec bt = {static_cast<time_t>(sx.stx_btime.tv_sec), static_cast<long>(sx.stx_btime.tv_nsec)};
        std::string name = path;
        std::string quoted = "'" + path + "'";
        if (S_ISLNK(st.st_mode)) {
            quoted += " -> '" + readlink_str(path) + "'";
        }
        if (terse) {
            char b[512];
            std::snprintf(b, sizeof b, "%s %lld %lld %x %u %u %llx %llu %llu %x %x %lld %lld %lld %lld %lld",
                          path.c_str(), (long long)st.st_size, (long long)st.st_blocks, st.st_mode, st.st_uid, st.st_gid,
                          (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, (unsigned long long)st.st_nlink,
                          major(st.st_rdev), minor(st.st_rdev), (long long)at.tv_sec, (long long)mt.tv_sec, (long long)ct.tv_sec,
                          (long long)(has_birth ? bt.tv_sec : 0), (long long)st.st_blksize);
            out(std::string(b) + "\n");
            continue;
        }
        if (format.empty()) {
            char b[2048];
            std::snprintf(b, sizeof b,
                          "  File: %s\n  Size: %-15lld Blocks: %-10lld IO Block: %-6ld %s\n"
                          "Device: %u,%u\tInode: %-10llu Links: %llu%s\n"
                          "Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
                          quoted.c_str(), (long long)st.st_size, (long long)st.st_blocks, (long)st.st_blksize, file_type_name(st.st_mode).c_str(),
                          major(st.st_dev), minor(st.st_dev), (unsigned long long)st.st_ino, (unsigned long long)st.st_nlink,
                          (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) ? (std::string("     Device type: ") + std::to_string(major(st.st_rdev)) + "," + std::to_string(minor(st.st_rdev))).c_str() : "",
                          st.st_mode & 07777, mode_string(st.st_mode).c_str(), st.st_uid, user_name(st.st_uid).c_str(), st.st_gid, group_name(st.st_gid).c_str());
            out(b);
            out("Access: " + stat_time(at) + "\nModify: " + stat_time(mt) + "\nChange: " + stat_time(ct) + "\n Birth: " +
                (has_birth ? stat_time(bt) : std::string("-")) + "\n");
            continue;
        }
        std::string o;
        for (size_t k = 0; k < format.size(); ++k) {
            if (format[k] != '%' || k + 1 >= format.size()) { o += format[k]; continue; }
            char f = format[++k];
            switch (f) {
            case 'a': { char b[16]; std::snprintf(b, sizeof b, "%o", st.st_mode & 07777); o += b; break; }
            case 'A': o += mode_string(st.st_mode); break;
            case 'b': o += std::to_string((long long)st.st_blocks); break;
            case 'B': o += "512"; break;
            case 'd': o += std::to_string((unsigned long long)st.st_dev); break;
            case 'D': { char b[16]; std::snprintf(b, sizeof b, "%llx", (unsigned long long)st.st_dev); o += b; break; }
            case 'f': { char b[16]; std::snprintf(b, sizeof b, "%x", st.st_mode); o += b; break; }
            case 'F': o += file_type_name(st.st_mode); break;
            case 'g': o += std::to_string(st.st_gid); break;
            case 'G': o += group_name(st.st_gid); break;
            case 'h': o += std::to_string((unsigned long long)st.st_nlink); break;
            case 'i': o += std::to_string((unsigned long long)st.st_ino); break;
            case 'm': o += canonicalize(path, false); break;
            case 'n': o += name; break;
            case 'N': o += quoted; break;
            case 'o': o += std::to_string((long)st.st_blksize); break;
            case 's': o += std::to_string((long long)st.st_size); break;
            case 't': { char b[16]; std::snprintf(b, sizeof b, "%x", major(st.st_rdev)); o += b; break; }
            case 'T': { char b[16]; std::snprintf(b, sizeof b, "%x", minor(st.st_rdev)); o += b; break; }
            case 'u': o += std::to_string(st.st_uid); break;
            case 'U': o += user_name(st.st_uid); break;
            case 'w': o += has_birth ? stat_time(bt) : "-"; break;
            case 'W': o += std::to_string(has_birth ? (long long)bt.tv_sec : 0LL); break;
            case 'x': o += stat_time(at); break;
            case 'X': o += std::to_string((long long)at.tv_sec); break;
            case 'y': o += stat_time(mt); break;
            case 'Y': o += std::to_string((long long)mt.tv_sec); break;
            case 'z': o += stat_time(ct); break;
            case 'Z': o += std::to_string((long long)ct.tv_sec); break;
            case 'C': {
                char buf[256];
                const ssize_t n = ::lgetxattr(path.c_str(), "security.selinux", buf, sizeof buf - 1);
                o += n > 0 ? std::string(buf, static_cast<size_t>(n)) : "?";
                break;
            }
            case '%': o += '%'; break;
            default: o += '%'; o += f;
            }
        }
        out(printf_mode ? unescape(o) : o + "\n");
    }
    return rc;
}

} // namespace cu
