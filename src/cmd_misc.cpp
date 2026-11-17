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
// dircolors, chcon, runcon, chroot, stdbuf, stty.
#include "commands.hpp"
#include "common.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <map>
#include <sstream>

namespace cu {

namespace {
const option kEnd = {nullptr, 0, nullptr, 0};
}

// ================================================================================================
// dircolors
// ================================================================================================
namespace {

const std::pair<const char *, const char *> kDcKeys[] = {
    {"NORMAL", "no"}, {"NORM", "no"}, {"FILE", "fi"}, {"RESET", "rs"}, {"DIR", "di"}, {"LNK", "ln"}, {"LINK", "ln"}, {"SYMLINK", "ln"},
    {"MULTIHARDLINK", "mh"}, {"FIFO", "pi"}, {"PIPE", "pi"}, {"SOCK", "so"}, {"DOOR", "do"}, {"BLK", "bd"}, {"BLOCK", "bd"}, {"CHR", "cd"},
    {"CHAR", "cd"}, {"ORPHAN", "or"}, {"MISSING", "mi"}, {"SETUID", "su"}, {"SETGID", "sg"}, {"CAPABILITY", "ca"},
    {"STICKY_OTHER_WRITABLE", "tw"}, {"OTHER_WRITABLE", "ow"}, {"STICKY", "st"}, {"EXEC", "ex"}, {"LEFTCODE", "lc"}, {"RIGHTCODE", "rc"},
    {"ENDCODE", "ec"}, {"CLEAR_TO_EOL", "cl"}};

std::string dc_long_name(const std::string &code)
{
    for (const auto &k : kDcKeys) if (code == k.second) return k.first;
    return "";
}

} // namespace

int cmd_dircolors(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]", "Output commands to set the LS_COLORS environment variable (-b sh, -c csh, --fish, -p print database).")) return 0;
    enum { SH, CSH, FISH } style = SH;
    bool print_db = false, print_ls = false;
    // shell autodetect from $SHELL (CachyOS ships fish as the default shell)
    if (const char *sh = std::getenv("SHELL")) {
        const std::string s = sh;
        if (s.size() >= 4 && s.compare(s.size() - 4, 4, "fish") == 0) style = FISH;
        else if (s.find("csh") != std::string::npos) style = CSH;
    }
    static const option lo[] = {{"sh", no_argument, nullptr, 'b'}, {"bourne-shell", no_argument, nullptr, 'b'}, {"csh", no_argument, nullptr, 'c'},
                                {"c-shell", no_argument, nullptr, 'c'}, {"fish", no_argument, nullptr, 'f'}, {"print-database", no_argument, nullptr, 'p'},
                                {"print-ls-colors", no_argument, nullptr, 'l'}, kEnd};
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "bcpf", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': style = SH; break;
        case 'c': style = CSH; break;
        case 'f': style = FISH; break;
        case 'p': print_db = true; break;
        case 'l': print_ls = true; break;
        default: return 1;
        }
    }
    std::string spec = kDefaultLsColors;
    std::string db_text;
    if (optind < argc) {
        std::string data;
        if (!read_file(argv[optind], data)) { err_errno(argv[optind]); return 1; }
        std::map<std::string, std::string> kv;
        std::vector<std::string> order;
        bool term_ok = true;
        const char *term = std::getenv("TERM");
        for (std::string line : split_lines(data)) {
            const size_t h = line.find('#');
            if (h != std::string::npos) line.erase(h);
            std::istringstream ss(line);
            std::string key, val;
            if (!(ss >> key >> val)) continue;
            if (key == "TERM") { if (term && val == term) term_ok = true; continue; }
            if (key == "COLOR" || key == "OPTIONS" || key == "EIGHTBIT") continue;
            std::string code;
            if (key[0] == '.' || key[0] == '*') code = key[0] == '.' ? "*" + key : key;
            else for (const auto &k : kDcKeys) if (key == k.first) code = k.second;
            if (code.empty()) { err("unrecognized keyword " + key); continue; }
            if (!kv.count(code)) order.push_back(code);
            kv[code] = val;
        }
        (void)term_ok;
        spec.clear();
        for (const auto &k : order) spec += k + "=" + kv[k] + ":";
    } else {
        // the built-in database
    }
    if (print_db) {
        out("# Configuration file for dircolors, a utility to help you set the LS_COLORS environment variable in ls.\n"
            "# Generated from the claude-coreutils built-in database (includes Arch *.pkg.tar.zst).\n\nTERM xterm-256color\nTERM alacritty\nTERM kitty\nTERM foot\nTERM konsole\nTERM *256color*\n\n");
        std::istringstream ss(spec);
        std::string item;
        while (std::getline(ss, item, ':')) {
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            if (k[0] == '*') out(k.substr(1) + " " + v + "\n");
            else { const std::string n = dc_long_name(k); out((n.empty() ? k : n) + " " + v + "\n"); }
        }
        return 0;
    }
    if (print_ls) { out(spec + "\n"); return 0; }
    switch (style) {
    case SH: out("LS_COLORS='" + spec + "';\nexport LS_COLORS\n"); break;
    case CSH: out("setenv LS_COLORS '" + spec + "'\n"); break;
    case FISH: out("set -gx LS_COLORS '" + spec + "'\n"); break;
    }
    return 0;
}

// ================================================================================================
// chcon / runcon (SELinux is optional on Arch: the security.selinux xattr is used directly)
// ================================================================================================
namespace {

bool selinux_enabled()
{
    struct stat st;
    return stat("/sys/fs/selinux", &st) == 0 && access("/sys/fs/selinux/enforce", F_OK) == 0;
}

} // namespace

int cmd_chcon(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... CONTEXT FILE...", "Change the SELinux security context of each FILE to CONTEXT (security.selinux xattr).")) return 0;
    bool recursive = false, deref = true, verbose = false;
    std::string reference;
    int c;
    optind = 1;
    static const option lo[] = {{"recursive", no_argument, nullptr, 'R'}, {"no-dereference", no_argument, nullptr, 'h'}, {"verbose", no_argument, nullptr, 'v'},
                                {"reference", required_argument, nullptr, 1}, kEnd};
    while ((c = getopt_long(argc, argv, "RhvHLPuroltn", lo, nullptr)) != -1) {
        switch (c) {
        case 'R': recursive = true; break;
        case 'h': deref = false; break;
        case 'v': verbose = true; break;
        case 1: reference = optarg; break;
        case 'H': case 'L': case 'P': case 'u': case 'r': case 'o': case 'l': case 't': case 'n': break;
        default: return 1;
        }
    }
    std::string ctx;
    if (!reference.empty()) {
        char buf[512];
        const ssize_t n = getxattr(reference.c_str(), "security.selinux", buf, sizeof buf);
        if (n < 0) { err_errno("failed to get security context of '" + reference + "'"); return 1; }
        ctx.assign(buf, static_cast<size_t>(n));
        if (!ctx.empty() && ctx.back() == '\0') ctx.pop_back();
    } else {
        if (optind >= argc) { usage_error("missing operand"); return 1; }
        ctx = argv[optind++];
    }
    if (optind >= argc) { usage_error("missing operand after '" + ctx + "'"); return 1; }
    int rc = 0;
    std::function<void(const std::string &)> apply = [&](const std::string &path) {
        const int r = deref ? setxattr(path.c_str(), "security.selinux", ctx.c_str(), ctx.size() + 1, 0)
                            : lsetxattr(path.c_str(), "security.selinux", ctx.c_str(), ctx.size() + 1, 0);
        if (r != 0) {
            if (errno == ENOTSUP || errno == EOPNOTSUPP) err("failed to change context of '" + path + "' to '" + ctx + "': Operation not supported (SELinux is not enabled on this system)");
            else err_errno("failed to change context of '" + path + "' to '" + ctx + "'");
            rc = 1;
            return;
        }
        if (verbose) out("context of '" + path + "' changed to '" + ctx + "'\n");
        struct stat st;
        if (recursive && lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *d = opendir(path.c_str());
            if (!d) return;
            while (const dirent *e = readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
                apply(path + (path.back() == '/' ? "" : "/") + e->d_name);
            }
            closedir(d);
        }
    };
    for (int i = optind; i < argc; ++i) apply(argv[i]);
    return rc;
}

int cmd_runcon(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "CONTEXT COMMAND [args]", "Run COMMAND with completely-specified CONTEXT.")) return 0;
    if (!selinux_enabled()) { err("runcon may be used only on a SELinux kernel"); return 125; }
    if (argc < 3) { usage_error("missing operand"); return 125; }
    const std::string ctx = argv[1];
    const int fd = open("/proc/self/attr/exec", O_WRONLY);
    if (fd < 0 || write(fd, ctx.c_str(), ctx.size()) < 0) { err_errno("failed to set new context '" + ctx + "'"); return 125; }
    close(fd);
    flush_out();
    execvp(argv[2], argv + 2);
    err_errno("failed to run command '" + std::string(argv[2]) + "'");
    return errno == ENOENT ? 127 : 126;
}

// ================================================================================================
// chroot
// ================================================================================================
int cmd_chroot(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] NEWROOT [COMMAND [ARG]...]", "Run COMMAND with root directory set to NEWROOT (what arch-chroot builds on).")) return 0;
    static const option lo[] = {{"groups", required_argument, nullptr, 'g'}, {"userspec", required_argument, nullptr, 'u'}, {"skip-chdir", no_argument, nullptr, 's'}, kEnd};
    std::string groups, userspec;
    bool skip_chdir = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+", lo, nullptr)) != -1) {
        switch (c) {
        case 'g': groups = optarg; break;
        case 'u': userspec = optarg; break;
        case 's': skip_chdir = true; break;
        default: return 125;
        }
    }
    if (optind >= argc) { usage_error("missing operand"); return 125; }
    const std::string root = argv[optind++];
    if (skip_chdir && root != "/") { err("option --skip-chdir only permitted if NEWROOT is old '/'"); return 125; }
    if (chroot(root.c_str()) != 0) { err_errno("cannot change root directory to '" + root + "'"); return 125; }
    if (!skip_chdir && chdir("/") != 0) { err_errno("cannot chdir to root directory"); return 125; }
    // resolve user/group after entering the new root (like coreutils does)
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (!userspec.empty()) {
        std::string u = userspec, g;
        const size_t col = userspec.find(':');
        if (col != std::string::npos) { u = userspec.substr(0, col); g = userspec.substr(col + 1); }
        if (!u.empty() && !lookup_user(u, uid)) { err("invalid user: '" + u + "'"); return 125; }
        if (!g.empty() && !lookup_group(g, gid)) { err("invalid group: '" + g + "'"); return 125; }
        if (g.empty() && uid != static_cast<uid_t>(-1)) { if (const passwd *pw = getpwuid(uid)) gid = pw->pw_gid; }
    }
    if (!groups.empty()) {
        std::vector<gid_t> gl;
        std::istringstream ss(groups);
        std::string g;
        while (std::getline(ss, g, ',')) { gid_t x; if (!lookup_group(g, x)) { err("invalid group: '" + g + "'"); return 125; } gl.push_back(x); }
        if (setgroups(gl.size(), gl.data()) != 0) { err_errno("failed to set supplemental groups"); return 125; }
    }
    if (gid != static_cast<gid_t>(-1) && setgid(gid) != 0) { err_errno("failed to set group-ID"); return 125; }
    if (uid != static_cast<uid_t>(-1) && setuid(uid) != 0) { err_errno("failed to set user-ID"); return 125; }
    flush_out();
    if (optind < argc) {
        execvp(argv[optind], argv + optind);
        err_errno("failed to run command '" + std::string(argv[optind]) + "'");
        return errno == ENOENT ? 127 : 126;
    }
    const char *sh = std::getenv("SHELL");
    if (!sh || !*sh) sh = "/bin/sh";
    execlp(sh, sh, "-i", static_cast<char *>(nullptr));
    err_errno("failed to run command '" + std::string(sh) + "'");
    return errno == ENOENT ? 127 : 126;
}

// ================================================================================================
// stdbuf
// ================================================================================================
int cmd_stdbuf(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "OPTION... COMMAND", "Run COMMAND with modified buffering operations for its standard streams (-i/-o/-e MODE: L, 0 or SIZE).")) return 0;
    std::string in_m, out_m, err_m;
    int i = 1;
    auto mode = [&](const std::string &m, std::string &dst) {
        if (m != "L" && m != "0") { uint64_t v; if (!parse_size(m, v)) { err("invalid mode '" + m + "'"); return false; } }
        dst = m;
        return true;
    };
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        std::string val;
        char which = 0;
        if (a.size() >= 2 && a[0] == '-' && (a[1] == 'i' || a[1] == 'o' || a[1] == 'e')) {
            which = a[1];
            if (a.size() > 2) val = a.substr(2); else if (i + 1 < argc) val = argv[++i]; else { usage_error("option requires an argument -- '" + std::string(1, which) + "'"); return 125; }
        } else if (a.rfind("--input=", 0) == 0) { which = 'i'; val = a.substr(8); }
        else if (a.rfind("--output=", 0) == 0) { which = 'o'; val = a.substr(9); }
        else if (a.rfind("--error=", 0) == 0) { which = 'e'; val = a.substr(8); }
        else if (a == "--") { ++i; break; }
        else break;
        if (!mode(val, which == 'i' ? in_m : which == 'o' ? out_m : err_m)) return 125;
    }
    if (i >= argc) { usage_error("missing operand"); return 125; }
    if (in_m.empty() && out_m.empty() && err_m.empty()) { usage_error("you must specify a buffering mode option"); return 125; }
    if (!in_m.empty()) setenv("_STDBUF_I", in_m.c_str(), 1);
    if (!out_m.empty()) setenv("_STDBUF_O", out_m.c_str(), 1);
    if (!err_m.empty()) setenv("_STDBUF_E", err_m.c_str(), 1);
    // Arch's coreutils package ships the preload shim here
    std::vector<std::string> candidates;
    {   // our own shim first: next to this binary (build dir) or in ../lib/claude-coreutils (installed)
        char me[4096];
        const ssize_t n = readlink("/proc/self/exe", me, sizeof me - 1);
        if (n > 0) {
            me[n] = '\0';
            std::string d = me;
            d = d.substr(0, d.find_last_of('/'));
            candidates.push_back(d + "/libstdbuf.so");
            candidates.push_back(d + "/../lib/claude-coreutils/libstdbuf.so");
        }
    }
    for (const char *p : {"/usr/lib/claude-coreutils/libstdbuf.so", "/usr/lib/coreutils/libstdbuf.so",
                          "/usr/libexec/coreutils/libstdbuf.so", "/usr/lib64/coreutils/libstdbuf.so"}) candidates.push_back(p);
    std::string lib;
    for (const std::string &p : candidates) if (access(p.c_str(), R_OK) == 0) { lib = p; break; }
    if (lib.empty()) err("warning: libstdbuf.so not found (build the project: it is built next to claude-coreutils); buffering is unchanged");
    else {
        std::string pre = lib;
        if (const char *old = std::getenv("LD_PRELOAD")) if (*old) pre = std::string(old) + ":" + lib;
        setenv("LD_PRELOAD", pre.c_str(), 1);
    }
    flush_out();
    execvp(argv[i], argv + i);
    err_errno("failed to run command '" + std::string(argv[i]) + "'");
    return errno == ENOENT ? 127 : 126;
}

// ================================================================================================
// stty
// ================================================================================================
namespace {

struct Flag { const char *name; tcflag_t termios::*field; tcflag_t mask; };

const Flag kFlags[] = {
    {"ignbrk", &termios::c_iflag, IGNBRK}, {"brkint", &termios::c_iflag, BRKINT}, {"ignpar", &termios::c_iflag, IGNPAR}, {"parmrk", &termios::c_iflag, PARMRK},
    {"inpck", &termios::c_iflag, INPCK}, {"istrip", &termios::c_iflag, ISTRIP}, {"inlcr", &termios::c_iflag, INLCR}, {"igncr", &termios::c_iflag, IGNCR},
    {"icrnl", &termios::c_iflag, ICRNL}, {"ixon", &termios::c_iflag, IXON}, {"ixoff", &termios::c_iflag, IXOFF}, {"ixany", &termios::c_iflag, IXANY},
    {"imaxbel", &termios::c_iflag, IMAXBEL}, {"iutf8", &termios::c_iflag, IUTF8},
    {"opost", &termios::c_oflag, OPOST}, {"onlcr", &termios::c_oflag, ONLCR}, {"ocrnl", &termios::c_oflag, OCRNL}, {"onocr", &termios::c_oflag, ONOCR},
    {"onlret", &termios::c_oflag, ONLRET},
    {"parenb", &termios::c_cflag, PARENB}, {"parodd", &termios::c_cflag, PARODD}, {"cstopb", &termios::c_cflag, CSTOPB}, {"hupcl", &termios::c_cflag, HUPCL},
    {"clocal", &termios::c_cflag, CLOCAL}, {"cread", &termios::c_cflag, CREAD}, {"crtscts", &termios::c_cflag, CRTSCTS},
    {"isig", &termios::c_lflag, ISIG}, {"icanon", &termios::c_lflag, ICANON}, {"echo", &termios::c_lflag, ECHO}, {"echoe", &termios::c_lflag, ECHOE},
    {"echok", &termios::c_lflag, ECHOK}, {"echonl", &termios::c_lflag, ECHONL}, {"noflsh", &termios::c_lflag, NOFLSH}, {"tostop", &termios::c_lflag, TOSTOP},
    {"echoctl", &termios::c_lflag, ECHOCTL}, {"echoke", &termios::c_lflag, ECHOKE}, {"iexten", &termios::c_lflag, IEXTEN}};

struct Special { const char *name; int idx; };
const Special kSpecials[] = {{"intr", VINTR}, {"quit", VQUIT}, {"erase", VERASE}, {"kill", VKILL}, {"eof", VEOF}, {"eol", VEOL}, {"start", VSTART},
                             {"stop", VSTOP}, {"susp", VSUSP}, {"rprnt", VREPRINT}, {"werase", VWERASE}, {"lnext", VLNEXT}, {"discard", VDISCARD}};

struct Speed { speed_t code; unsigned baud; };
const Speed kSpeeds[] = {{B0, 0}, {B50, 50}, {B75, 75}, {B110, 110}, {B134, 134}, {B150, 150}, {B200, 200}, {B300, 300}, {B600, 600}, {B1200, 1200},
                         {B1800, 1800}, {B2400, 2400}, {B4800, 4800}, {B9600, 9600}, {B19200, 19200}, {B38400, 38400}, {B57600, 57600}, {B115200, 115200},
                         {B230400, 230400}, {B460800, 460800}, {B921600, 921600}, {B1000000, 1000000}, {B2000000, 2000000}, {B4000000, 4000000}};

unsigned baud_of(speed_t s) { for (const auto &x : kSpeeds) if (x.code == s) return x.baud; return 0; }

std::string ctl_str(cc_t c)
{
    if (c == _POSIX_VDISABLE) return "<undef>";
    if (c < 32) return std::string("^") + static_cast<char>(c + 64);
    if (c == 127) return "^?";
    return std::string(1, static_cast<char>(c));
}

cc_t parse_ctl(const std::string &s)
{
    if (s == "^-" || s == "undef") return _POSIX_VDISABLE;
    if (s.size() == 2 && s[0] == '^') return s[1] == '?' ? 127 : static_cast<cc_t>(std::toupper(static_cast<unsigned char>(s[1])) - 64);
    if (s.size() == 1) return static_cast<cc_t>(s[0]);
    return static_cast<cc_t>(std::atoi(s.c_str()));
}

void set_sane(termios &t)
{
    t.c_iflag |= BRKINT | ICRNL | IMAXBEL | IXON | IUTF8;
    t.c_iflag &= ~(IGNBRK | INLCR | IGNCR | IXOFF | IUCLC | IXANY | ISTRIP);
    t.c_oflag |= OPOST | ONLCR;
    t.c_oflag &= ~(OLCUC | OCRNL | ONOCR | ONLRET);
    t.c_cflag |= CREAD;
    t.c_lflag |= ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    t.c_lflag &= ~(ECHONL | NOFLSH | XCASE | TOSTOP | ECHOPRT);
    t.c_cc[VINTR] = 3; t.c_cc[VQUIT] = 28; t.c_cc[VERASE] = 127; t.c_cc[VKILL] = 21; t.c_cc[VEOF] = 4; t.c_cc[VSTART] = 17;
    t.c_cc[VSTOP] = 19; t.c_cc[VSUSP] = 26; t.c_cc[VREPRINT] = 18; t.c_cc[VWERASE] = 23; t.c_cc[VLNEXT] = 22; t.c_cc[VDISCARD] = 15;
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
}

} // namespace

int cmd_stty(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-F DEVICE | --file=DEVICE] [SETTING]...", "Print or change terminal characteristics.")) return 0;
    std::string device;
    bool all = false, save = false;
    std::vector<std::string> settings;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-F" || a == "--file") && i + 1 < argc) device = argv[++i];
        else if (a.rfind("--file=", 0) == 0) device = a.substr(7);
        else if (a == "-a" || a == "--all") all = true;
        else if (a == "-g" || a == "--save") save = true;
        else settings.push_back(a);
    }
    int fd = 0;
    if (!device.empty()) {
        fd = open(device.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) { err_errno(device); return 1; }
    }
    termios t;
    if (tcgetattr(fd, &t) != 0) { err_errno(device.empty() ? "standard input" : device); return 1; }
    if (save) {
        char b[512];
        int n = std::snprintf(b, sizeof b, "%x:%x:%x:%x", t.c_iflag, t.c_oflag, t.c_cflag, t.c_lflag);
        for (int k = 0; k < NCCS; ++k) n += std::snprintf(b + n, sizeof b - static_cast<size_t>(n), ":%x", t.c_cc[k]);
        out(std::string(b) + "\n");
        return 0;
    }
    if (settings.empty() || all) {
        winsize ws{};
        ioctl(fd, TIOCGWINSZ, &ws);
        std::string o = "speed " + std::to_string(baud_of(cfgetospeed(&t))) + " baud; rows " + std::to_string(ws.ws_row) + "; columns " + std::to_string(ws.ws_col) + ";\n";
        std::string line;
        for (const auto &s : kSpecials) line += std::string(s.name) + " = " + ctl_str(t.c_cc[s.idx]) + "; ";
        line += "min = " + std::to_string(t.c_cc[VMIN]) + "; time = " + std::to_string(t.c_cc[VTIME]) + ";\n";
        o += line;
        const tcflag_t termios::*cur = nullptr;
        (void)cur;
        for (const char *group : {"iflag", "oflag", "cflag", "lflag"}) {
            std::string l;
            for (const Flag &f : kFlags) {
                const bool is_group = (!std::strcmp(group, "iflag") && f.field == &termios::c_iflag) || (!std::strcmp(group, "oflag") && f.field == &termios::c_oflag) ||
                                      (!std::strcmp(group, "cflag") && f.field == &termios::c_cflag) || (!std::strcmp(group, "lflag") && f.field == &termios::c_lflag);
                if (!is_group) continue;
                const bool on = (t.*(f.field) & f.mask) != 0;
                if (!all && !on) continue;
                l += std::string(on ? "" : "-") + f.name + " ";
            }
            if (!l.empty()) { l.pop_back(); o += l + "\n"; }
        }
        out(o);
        return 0;
    }
    for (size_t i = 0; i < settings.size(); ++i) {
        const std::string &s = settings[i];
        bool done = false;
        if (s == "size") {
            winsize ws{};
            ioctl(fd, TIOCGWINSZ, &ws);
            out(std::to_string(ws.ws_row) + " " + std::to_string(ws.ws_col) + "\n");
            continue;
        }
        if (s == "sane") { set_sane(t); continue; }
        if (s == "raw" || s == "-cooked") { cfmakeraw(&t); continue; }
        if (s == "cooked" || s == "-raw") { set_sane(t); continue; }
        if (s == "cbreak") { t.c_lflag &= ~ICANON; continue; }
        if (s == "-cbreak") { t.c_lflag |= ICANON; continue; }
        if (s == "rows" || s == "cols" || s == "columns") {
            if (i + 1 >= settings.size()) { err("missing argument to '" + s + "'"); return 1; }
            winsize ws{};
            ioctl(fd, TIOCGWINSZ, &ws);
            const unsigned short v = static_cast<unsigned short>(std::atoi(settings[++i].c_str()));
            if (s == "rows") ws.ws_row = v; else ws.ws_col = v;
            if (ioctl(fd, TIOCSWINSZ, &ws) != 0) { err_errno("cannot set window size"); return 1; }
            continue;
        }
        if (s == "min" || s == "time") {
            if (i + 1 >= settings.size()) { err("missing argument to '" + s + "'"); return 1; }
            t.c_cc[s == "min" ? VMIN : VTIME] = static_cast<cc_t>(std::atoi(settings[++i].c_str()));
            continue;
        }
        if (s == "ispeed" || s == "ospeed") {
            if (i + 1 >= settings.size()) { err("missing argument to '" + s + "'"); return 1; }
            const unsigned b = static_cast<unsigned>(std::atoi(settings[++i].c_str()));
            bool ok = false;
            for (const auto &sp : kSpeeds) if (sp.baud == b) { if (s == "ispeed") cfsetispeed(&t, sp.code); else cfsetospeed(&t, sp.code); ok = true; }
            if (!ok) { err("invalid speed '" + settings[i] + "'"); return 1; }
            continue;
        }
        for (const auto &sp : kSpecials) {
            if (s == sp.name) {
                if (i + 1 >= settings.size()) { err("missing argument to '" + s + "'"); return 1; }
                t.c_cc[sp.idx] = parse_ctl(settings[++i]);
                done = true;
                break;
            }
        }
        if (done) continue;
        bool is_num = !s.empty() && std::isdigit(static_cast<unsigned char>(s[0]));
        if (is_num) {
            const unsigned b = static_cast<unsigned>(std::atoi(s.c_str()));
            bool ok = false;
            for (const auto &sp : kSpeeds) if (sp.baud == b) { cfsetispeed(&t, sp.code); cfsetospeed(&t, sp.code); ok = true; }
            if (!ok) { err("invalid integer argument '" + s + "'"); return 1; }
            continue;
        }
        if (s.size() == 3 && s.rfind("cs", 0) == 0 && s[2] >= '5' && s[2] <= '8') {
            t.c_cflag &= ~CSIZE;
            t.c_cflag |= s[2] == '5' ? CS5 : s[2] == '6' ? CS6 : s[2] == '7' ? CS7 : CS8;
            continue;
        }
        const bool neg = s[0] == '-';
        const std::string name = neg ? s.substr(1) : s;
        for (const Flag &f : kFlags) {
            if (name == f.name) {
                if (neg) t.*(f.field) &= ~f.mask; else t.*(f.field) |= f.mask;
                done = true;
                break;
            }
        }
        if (name == "evenp" || name == "parity") { if (neg) { t.c_cflag &= ~(PARENB | CSIZE); t.c_cflag |= CS8; } else { t.c_cflag |= PARENB | CS7; t.c_cflag &= ~PARODD; } done = true; }
        if (name == "oddp") { if (neg) { t.c_cflag &= ~(PARENB | CSIZE); t.c_cflag |= CS8; } else { t.c_cflag |= PARENB | PARODD | CS7; } done = true; }
        if (name == "lcase" || name == "LCASE") done = true;
        if (!done) { err("invalid argument '" + s + "'"); return 1; }
    }
    if (tcsetattr(fd, TCSADRAIN, &t) != 0) { err_errno(device.empty() ? "standard input" : device); return 1; }
    return 0;
}

} // namespace cu
