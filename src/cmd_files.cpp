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
// File-system utilities: names, permissions, directories, links, timestamps, paths.
#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace cu {

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

/// Applies `fn(path, is_cmdline)` to `path` and, with `recursive`, to everything below it (symlinks are not followed).
template <class F>
int walk(const std::string &path, bool recursive, F &&fn)
{
    int rc = fn(path);
    if (!recursive) {
        return rc;
    }
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return rc;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            err(it->path().string() + ": " + ec.message());
            rc = 1;
            ec.clear();
            continue;
        }
        rc |= fn(it->path().string());
    }
    return rc;
}

} // namespace

// ---------------------------------------------------------------------------------------------
int cmd_basename(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "NAME [SUFFIX] | OPTION... NAME...", "Print NAME with any leading directory components removed.")) {
        return 0;
    }
    static const option lo[] = {{"multiple", no_argument, nullptr, 'a'}, {"suffix", required_argument, nullptr, 's'},
                                {"zero", no_argument, nullptr, 'z'}, kEnd};
    bool multiple = false, zero = false;
    std::string suffix;
    int c;
    while ((c = getopt_long(argc, argv, "+as:z", lo, nullptr)) != -1) {
        if (c == 'a') multiple = true;
        else if (c == 's') suffix = optarg, multiple = true;
        else if (c == 'z') zero = true;
        else return 1;
    }
    std::vector<std::string> names(argv + optind, argv + argc);
    if (names.empty()) {
        usage_error("missing operand");
        return 1;
    }
    if (!multiple) {
        if (names.size() > 2) {
            usage_error("extra operand '" + names[2] + "'");
            return 1;
        }
        if (names.size() == 2) {
            suffix = names[1];
            names.resize(1);
        }
    }
    for (std::string n : names) {
        n = strip_trailing_slashes(n);
        std::string b = (n == "/") ? "/" : n.substr(n.find_last_of('/') == std::string::npos ? 0 : n.find_last_of('/') + 1);
        if (!suffix.empty() && b.size() > suffix.size() && b.compare(b.size() - suffix.size(), suffix.size(), suffix) == 0) {
            b.resize(b.size() - suffix.size());
        }
        out(b + (zero ? '\0' : '\n'));
    }
    return 0;
}

int cmd_dirname(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] NAME...", "Output each NAME with its last non-slash component removed.")) {
        return 0;
    }
    bool zero = false;
    int c;
    static const option lo[] = {{"zero", no_argument, nullptr, 'z'}, kEnd};
    while ((c = getopt_long(argc, argv, "z", lo, nullptr)) != -1) {
        if (c == 'z') zero = true;
        else return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    for (int i = optind; i < argc; ++i) {
        std::string n = strip_trailing_slashes(argv[i]);
        std::string d;
        const size_t slash = n.find_last_of('/');
        if (slash == std::string::npos) {
            d = ".";
        } else {
            d = strip_trailing_slashes(n.substr(0, slash));
            if (d.empty()) {
                d = "/";
            }
        }
        out(d + (zero ? '\0' : '\n'));
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
int cmd_chmod(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... MODE[,MODE]... FILE... | --reference=RFILE FILE...", "Change the mode of each FILE to MODE.")) {
        return 0;
    }
    // a mode such as "-w" or "-x" looks like an option: pull it out before getopt sees it
    std::string mode_arg;
    std::vector<char *> args{argv[0]};
    bool dashdash = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (!dashdash && a == "--") {
            dashdash = true;
        }
        if (!dashdash && mode_arg.empty() && a.size() > 1 && a[0] == '-' &&
            a.find_first_not_of("rwxXstugoa+=,-", 1) == std::string::npos && a.find_first_of("-+=", 1) == std::string::npos) {
            mode_arg = a;
            continue;
        }
        args.push_back(argv[i]);
    }
    int ac = static_cast<int>(args.size());
    char **av = args.data();

    static const option lo[] = {{"recursive", no_argument, nullptr, 'R'}, {"verbose", no_argument, nullptr, 'v'},
                                {"changes", no_argument, nullptr, 'c'}, {"silent", no_argument, nullptr, 'f'},
                                {"quiet", no_argument, nullptr, 'f'}, {"reference", required_argument, nullptr, 1},
                                {"preserve-root", no_argument, nullptr, 2}, {"no-preserve-root", no_argument, nullptr, 3}, kEnd};
    bool recursive = false, verbose = false, changes = false, quiet = false, preserve_root = false;
    std::string reference;
    int c;
    optind = 1;
    while ((c = getopt_long(ac, av, "Rvcf", lo, nullptr)) != -1) {
        switch (c) {
        case 'R': recursive = true; break;
        case 'v': verbose = true; break;
        case 'c': changes = true; break;
        case 'f': quiet = true; break;
        case 1: reference = optarg; break;
        case 2: preserve_root = true; break;
        case 3: preserve_root = false; break;
        default: return 1;
        }
    }
    std::string spec = mode_arg;
    mode_t ref_mode = 0;
    if (!reference.empty()) {
        struct stat st;
        if (::stat(reference.c_str(), &st) != 0) {
            err_errno("failed to get attributes of '" + reference + "'");
            return 1;
        }
        ref_mode = st.st_mode & 07777;
    } else if (spec.empty()) {
        if (optind >= ac) {
            usage_error("missing operand");
            return 1;
        }
        spec = av[optind++];
    }
    if (optind >= ac) {
        usage_error("missing operand after '" + (reference.empty() ? spec : reference) + "'");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < ac; ++i) {
        if (preserve_root && std::string(av[i]) == "/" && recursive) {
            err("it is dangerous to operate recursively on '/'");
            return 1;
        }
        rc |= walk(av[i], recursive, [&](const std::string &p) -> int {
            struct stat st;
            if (::stat(p.c_str(), &st) != 0) {
                if (!quiet) err_errno("cannot access '" + p + "'");
                return 1;
            }
            mode_t nm;
            if (!reference.empty()) {
                nm = ref_mode;
            } else if (!parse_mode(spec, st.st_mode, S_ISDIR(st.st_mode), nm)) {
                err("invalid mode: '" + spec + "'");
                return 1;
            }
            if (S_ISDIR(st.st_mode) && !reference.empty() == false && (st.st_mode & (S_ISUID | S_ISGID))) {
                // directories keep their set-id bits unless named explicitly
                if (std::isdigit(static_cast<unsigned char>(spec[0])) && spec.size() < 5) {
                    nm |= st.st_mode & (S_ISUID | S_ISGID);
                }
            }
            if (::chmod(p.c_str(), nm) != 0) {
                if (!quiet) err_errno("changing permissions of '" + p + "'");
                return 1;
            }
            const mode_t old = st.st_mode & 07777;
            char buf[160];
            if (old != nm && (verbose || changes)) {
                std::snprintf(buf, sizeof buf, "mode of '%s' changed from %04o (%s) to %04o (%s)", p.c_str(), old,
                              mode_string(st.st_mode).c_str() + 1, nm, mode_string((st.st_mode & ~07777) | nm).c_str() + 1);
                out(std::string(buf) + "\n");
            } else if (verbose && old == nm) {
                std::snprintf(buf, sizeof buf, "mode of '%s' retained as %04o (%s)", p.c_str(), nm, mode_string(st.st_mode).c_str() + 1);
                out(std::string(buf) + "\n");
            }
            return 0;
        });
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct Owner {
    bool has_uid = false, has_gid = false;
    uid_t uid = 0;
    gid_t gid = 0;
};

bool parse_owner(const std::string &spec, Owner &o, bool group_only)
{
    if (group_only) {
        o.has_gid = lookup_group(spec, o.gid);
        return o.has_gid;
    }
    size_t sep = spec.find(':');
    if (sep == std::string::npos) {
        sep = spec.find('.');
        if (sep != std::string::npos && !spec.empty()) {
            uid_t tmp;
            if (lookup_user(spec, tmp)) { // a user name containing a dot
                sep = std::string::npos;
            }
        }
    }
    const std::string u = sep == std::string::npos ? spec : spec.substr(0, sep);
    if (!u.empty()) {
        if (!lookup_user(u, o.uid)) {
            return false;
        }
        o.has_uid = true;
    }
    if (sep != std::string::npos) {
        const std::string g = spec.substr(sep + 1);
        if (!g.empty()) {
            if (!lookup_group(g, o.gid)) {
                return false;
            }
            o.has_gid = true;
        } else if (o.has_uid) { // "user:" = the user's login group
            if (const passwd *pw = getpwuid(o.uid)) {
                o.gid = pw->pw_gid;
                o.has_gid = true;
            }
        }
    }
    return o.has_uid || o.has_gid;
}

int chown_main(int argc, char **argv, bool group_only)
{
    static const option lo[] = {{"recursive", no_argument, nullptr, 'R'}, {"verbose", no_argument, nullptr, 'v'},
                                {"changes", no_argument, nullptr, 'c'}, {"silent", no_argument, nullptr, 'f'},
                                {"quiet", no_argument, nullptr, 'f'}, {"no-dereference", no_argument, nullptr, 'h'},
                                {"dereference", no_argument, nullptr, 'd'}, {"reference", required_argument, nullptr, 1},
                                {"from", required_argument, nullptr, 2}, kEnd};
    bool recursive = false, verbose = false, changes = false, quiet = false, no_deref = false;
    std::string reference, from;
    int c;
    while ((c = getopt_long(argc, argv, "RvcfhHLP", lo, nullptr)) != -1) {
        switch (c) {
        case 'R': recursive = true; break;
        case 'v': verbose = true; break;
        case 'c': changes = true; break;
        case 'f': quiet = true; break;
        case 'h': no_deref = true; break;
        case 'd': case 'H': case 'L': case 'P': break;
        case 1: reference = optarg; break;
        case 2: from = optarg; break;
        default: return 1;
        }
    }
    Owner target;
    if (!reference.empty()) {
        struct stat st;
        if (::stat(reference.c_str(), &st) != 0) {
            err_errno("failed to get attributes of '" + reference + "'");
            return 1;
        }
        target = {!group_only, true, st.st_uid, st.st_gid};
    } else {
        if (optind >= argc) {
            usage_error("missing operand");
            return 1;
        }
        const std::string spec = argv[optind++];
        if (!parse_owner(spec, target, group_only)) {
            err(std::string("invalid ") + (group_only ? "group" : "user") + ": '" + spec + "'");
            return 1;
        }
    }
    Owner from_owner;
    if (!from.empty() && !parse_owner(from, from_owner, false)) {
        err("invalid user: '" + from + "'");
        return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        rc |= walk(argv[i], recursive, [&](const std::string &p) -> int {
            struct stat st;
            const int sr = no_deref ? ::lstat(p.c_str(), &st) : ::stat(p.c_str(), &st);
            if (sr != 0) {
                if (!quiet) err_errno("cannot access '" + p + "'");
                return 1;
            }
            if ((from_owner.has_uid && st.st_uid != from_owner.uid) || (from_owner.has_gid && st.st_gid != from_owner.gid)) {
                return 0;
            }
            const uid_t nu = target.has_uid ? target.uid : static_cast<uid_t>(-1);
            const gid_t ng = target.has_gid ? target.gid : static_cast<gid_t>(-1);
            const int r = no_deref ? ::lchown(p.c_str(), nu, ng) : ::chown(p.c_str(), nu, ng);
            if (r != 0) {
                if (!quiet) err_errno(std::string("changing ownership of '") + p + "'");
                return 1;
            }
            const bool changed = (target.has_uid && st.st_uid != target.uid) || (target.has_gid && st.st_gid != target.gid);
            if (verbose || (changes && changed)) {
                out(std::string(changed ? "changed ownership of '" : "ownership of '") + p + (changed ? "' from " : "' retained as ") +
                    (changed ? user_name(st.st_uid) + ":" + group_name(st.st_gid) + " to " : "") +
                    user_name(target.has_uid ? target.uid : st.st_uid) + ":" + group_name(target.has_gid ? target.gid : st.st_gid) + "\n");
            }
            return 0;
        });
    }
    return rc;
}

} // namespace

int cmd_chown(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [OWNER][:[GROUP]] FILE...", "Change the owner and/or group of each FILE.")) {
        return 0;
    }
    return chown_main(argc, argv, false);
}

int cmd_chgrp(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... GROUP FILE...", "Change the group of each FILE to GROUP.")) {
        return 0;
    }
    return chown_main(argc, argv, true);
}

// ---------------------------------------------------------------------------------------------
int cmd_mkdir(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... DIRECTORY...", "Create the DIRECTORY(ies), if they do not already exist.")) {
        return 0;
    }
    static const option lo[] = {{"mode", required_argument, nullptr, 'm'}, {"parents", no_argument, nullptr, 'p'},
                                {"verbose", no_argument, nullptr, 'v'}, kEnd};
    bool parents = false, verbose = false;
    std::string mode_spec;
    int c;
    while ((c = getopt_long(argc, argv, "m:pvZ", lo, nullptr)) != -1) {
        if (c == 'm') mode_spec = optarg;
        else if (c == 'p') parents = true;
        else if (c == 'v') verbose = true;
        else if (c == 'Z') {}
        else return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    mode_t mode = 0777 & ~[] { const mode_t m = ::umask(0); ::umask(m); return m; }();
    bool explicit_mode = false;
    if (!mode_spec.empty()) {
        if (!parse_mode(mode_spec, 0777, true, mode)) {
            err("invalid mode '" + mode_spec + "'");
            return 1;
        }
        explicit_mode = true;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        std::string path = strip_trailing_slashes(argv[i]);
        if (parents) {
            std::string cur;
            size_t pos = 0;
            while (pos <= path.size()) {
                size_t slash = path.find('/', pos);
                if (slash == std::string::npos) slash = path.size();
                cur = path.substr(0, slash);
                if (!cur.empty() && cur != ".") {
                    struct stat st;
                    if (::stat(cur.c_str(), &st) != 0) {
                        if (::mkdir(cur.c_str(), slash == path.size() && explicit_mode ? mode : 0777) != 0) {
                            err_errno("cannot create directory '" + cur + "'");
                            rc = 1;
                            break;
                        }
                        if (verbose) out("mkdir: created directory '" + cur + "'\n");
                    } else if (!S_ISDIR(st.st_mode)) {
                        err("cannot create directory '" + cur + "': Not a directory");
                        rc = 1;
                        break;
                    }
                }
                pos = slash + 1;
            }
        } else {
            if (::mkdir(path.c_str(), explicit_mode ? 0700 : mode) != 0) {
                err_errno("cannot create directory '" + path + "'");
                rc = 1;
                continue;
            }
            if (explicit_mode) {
                ::chmod(path.c_str(), mode);
            }
            if (verbose) out("mkdir: created directory '" + path + "'\n");
        }
    }
    return rc;
}

int cmd_rmdir(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... DIRECTORY...", "Remove the DIRECTORY(ies), if they are empty.")) {
        return 0;
    }
    static const option lo[] = {{"parents", no_argument, nullptr, 'p'}, {"verbose", no_argument, nullptr, 'v'},
                                {"ignore-fail-on-non-empty", no_argument, nullptr, 1}, kEnd};
    bool parents = false, verbose = false, ignore = false;
    int c;
    while ((c = getopt_long(argc, argv, "pv", lo, nullptr)) != -1) {
        if (c == 'p') parents = true;
        else if (c == 'v') verbose = true;
        else if (c == 1) ignore = true;
        else return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        std::string p = strip_trailing_slashes(argv[i]);
        for (;;) {
            if (verbose) out("rmdir: removing directory, '" + p + "'\n");
            if (::rmdir(p.c_str()) != 0) {
                if (!(ignore && (errno == ENOTEMPTY || errno == EEXIST))) {
                    err_errno("failed to remove '" + p + "'");
                    rc = 1;
                }
                break;
            }
            if (!parents) break;
            const size_t slash = p.find_last_of('/');
            if (slash == std::string::npos || slash == 0) break;
            p = strip_trailing_slashes(p.substr(0, slash));
        }
    }
    return rc;
}

int cmd_mkfifo(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... NAME...", "Create named pipes (FIFOs) with the given NAMEs.")) {
        return 0;
    }
    static const option lo[] = {{"mode", required_argument, nullptr, 'm'}, kEnd};
    std::string mode_spec;
    int c;
    while ((c = getopt_long(argc, argv, "m:Z", lo, nullptr)) != -1) {
        if (c == 'm') mode_spec = optarg;
        else if (c == 'Z') {}
        else return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    mode_t mode = 0666;
    if (!mode_spec.empty() && !parse_mode(mode_spec, 0666, false, mode)) {
        err("invalid mode '" + mode_spec + "'");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        if (::mkfifo(argv[i], mode_spec.empty() ? 0666 : 0600) != 0) {
            err_errno(std::string("cannot create fifo '") + argv[i] + "'");
            rc = 1;
        } else if (!mode_spec.empty()) {
            ::chmod(argv[i], mode);
        }
    }
    return rc;
}

int cmd_mknod(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... NAME TYPE [MAJOR MINOR]", "Create the special file NAME of the given TYPE (b, c, u, p).")) {
        return 0;
    }
    static const option lo[] = {{"mode", required_argument, nullptr, 'm'}, kEnd};
    std::string mode_spec;
    int c;
    while ((c = getopt_long(argc, argv, "m:Z", lo, nullptr)) != -1) {
        if (c == 'm') mode_spec = optarg;
        else if (c == 'Z') {}
        else return 1;
    }
    if (argc - optind < 2) {
        usage_error("missing operand");
        return 1;
    }
    const std::string name = argv[optind], type = argv[optind + 1];
    mode_t mode = 0666;
    if (!mode_spec.empty() && !parse_mode(mode_spec, 0666, false, mode)) {
        err("invalid mode '" + mode_spec + "'");
        return 1;
    }
    dev_t dev = 0;
    mode_t kind;
    if (type == "p") {
        kind = S_IFIFO;
        if (argc - optind != 2) {
            usage_error("extra operand '" + std::string(argv[optind + 2]) + "'");
            return 1;
        }
    } else if (type == "b" || type == "c" || type == "u") {
        kind = type == "b" ? S_IFBLK : S_IFCHR;
        uint64_t maj, min;
        if (argc - optind != 4 || !parse_u64(argv[optind + 2], maj, 0) || !parse_u64(argv[optind + 3], min, 0)) {
            usage_error("special files require major and minor device numbers");
            return 1;
        }
        dev = makedev(static_cast<unsigned>(maj), static_cast<unsigned>(min));
    } else {
        err("invalid device type '" + type + "'");
        return 1;
    }
    if (::mknod(name.c_str(), kind | (mode_spec.empty() ? 0666 : 0600), dev) != 0) {
        err_errno("cannot create '" + name + "'");
        return 1;
    }
    if (!mode_spec.empty()) {
        ::chmod(name.c_str(), mode);
    }
    return 0;
}

int cmd_mktemp(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [TEMPLATE]", "Create a temporary file or directory, safely, and print its name.")) {
        return 0;
    }
    static const option lo[] = {{"directory", no_argument, nullptr, 'd'}, {"dry-run", no_argument, nullptr, 'u'},
                                {"quiet", no_argument, nullptr, 'q'}, {"suffix", required_argument, nullptr, 1},
                                {"tmpdir", optional_argument, nullptr, 'p'}, kEnd};
    bool dir = false, dry = false, quiet = false, use_tmpdir = false;
    std::string suffix, tmpdir;
    int c;
    while ((c = getopt_long(argc, argv, "dqtup:", lo, nullptr)) != -1) {
        switch (c) {
        case 'd': dir = true; break;
        case 'u': dry = true; break;
        case 'q': quiet = true; break;
        case 't': use_tmpdir = true; break;
        case 'p': use_tmpdir = true; if (optarg) tmpdir = optarg; break;
        case 1: suffix = optarg; break;
        default: return 1;
        }
    }
    std::string tmpl = optind < argc ? argv[optind] : "tmp.XXXXXXXXXX";
    if (optind == argc) {
        use_tmpdir = true;
    }
    if (use_tmpdir) {
        if (tmpdir.empty()) {
            const char *t = std::getenv("TMPDIR");
            tmpdir = (t && *t) ? t : "/tmp";
        }
        tmpl = tmpdir + "/" + tmpl;
    }
    const size_t x = tmpl.rfind("XXX");
    if (x == std::string::npos) {
        if (!quiet) err("too few X's in template '" + tmpl + "'");
        return 1;
    }
    std::string full = tmpl + suffix;
    const size_t tail = full.size() - tmpl.size() + (tmpl.size() - (x + 3));
    (void)tail;
    // number of trailing X's directly before the suffix
    size_t xs = 0;
    for (size_t i = tmpl.size(); i > 0 && tmpl[i - 1] == 'X'; --i) ++xs;
    if (xs < 3) {
        if (!quiet) err("too few X's in template '" + tmpl + "'");
        return 1;
    }
    if (dry) {
        static const char alnum[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        FILE *rnd = std::fopen("/dev/urandom", "rb");
        for (size_t i = 0; i < xs; ++i) {
            unsigned char b = 0;
            if (!rnd || std::fread(&b, 1, 1, rnd) != 1) b = static_cast<unsigned char>(std::rand());
            full[tmpl.size() - xs + i] = alnum[b % (sizeof(alnum) - 1)];
        }
        if (rnd) std::fclose(rnd);
        out(full + "\n");
        return 0;
    }
    std::string buf = full;
    if (dir) {
        if (!suffix.empty()) {
            if (!quiet) err("cannot combine --directory with --suffix");
            return 1;
        }
        if (!::mkdtemp(buf.data())) {
            if (!quiet) err_errno("failed to create directory via template '" + tmpl + "'");
            return 1;
        }
    } else {
        const int fd = ::mkostemps(buf.data(), static_cast<int>(suffix.size()), O_CLOEXEC);
        if (fd < 0) {
            if (!quiet) err_errno("failed to create file via template '" + tmpl + "'");
            return 1;
        }
        ::close(fd);
    }
    out(buf + "\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
int cmd_ln(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... TARGET LINK_NAME | TARGET... DIRECTORY", "Create links between files (hard links by default).")) {
        return 0;
    }
    static const option lo[] = {{"symbolic", no_argument, nullptr, 's'}, {"force", no_argument, nullptr, 'f'},
                                {"no-dereference", no_argument, nullptr, 'n'}, {"verbose", no_argument, nullptr, 'v'},
                                {"relative", no_argument, nullptr, 'r'}, {"no-target-directory", no_argument, nullptr, 'T'},
                                {"target-directory", required_argument, nullptr, 't'}, {"interactive", no_argument, nullptr, 'i'},
                                {"logical", no_argument, nullptr, 'L'}, {"physical", no_argument, nullptr, 'P'}, kEnd};
    bool symbolic = false, force = false, no_deref = false, verbose = false, relative = false, notd = false, inter = false;
    std::string tdir;
    int c;
    while ((c = getopt_long(argc, argv, "sfnvrTt:iLP", lo, nullptr)) != -1) {
        switch (c) {
        case 's': symbolic = true; break;
        case 'f': force = true; break;
        case 'n': no_deref = true; break;
        case 'v': verbose = true; break;
        case 'r': relative = true; break;
        case 'T': notd = true; break;
        case 't': tdir = optarg; break;
        case 'i': inter = true; break;
        case 'L': case 'P': break;
        default: return 1;
        }
    }
    std::vector<std::string> a(argv + optind, argv + argc);
    if (a.empty()) {
        usage_error("missing file operand");
        return 1;
    }
    std::vector<std::string> sources;
    std::string dest_dir;
    auto is_dir = [&](const std::string &p) {
        struct stat st;
        return (no_deref ? ::lstat(p.c_str(), &st) : ::stat(p.c_str(), &st)) == 0 && S_ISDIR(st.st_mode);
    };
    if (!tdir.empty()) {
        sources = a;
        dest_dir = tdir;
    } else if (a.size() == 1) {
        sources = a;
        dest_dir = ".";
    } else if (!notd && is_dir(a.back())) {
        dest_dir = a.back();
        a.pop_back();
        sources = a;
    } else if (a.size() == 2) {
        sources = {a[0]};
    } else {
        usage_error("target '" + a.back() + "' is not a directory");
        return 1;
    }
    int rc = 0;
    for (const std::string &src : sources) {
        std::string dst;
        if (!dest_dir.empty()) {
            const std::string base = strip_trailing_slashes(src);
            const size_t sl = base.find_last_of('/');
            dst = strip_trailing_slashes(dest_dir) + "/" + (sl == std::string::npos ? base : base.substr(sl + 1));
        } else {
            dst = a.back();
        }
        std::string target = src;
        if (relative && symbolic) {
            std::error_code ec;
            const fs::path abs_src = fs::absolute(canonicalize(src, false));
            const fs::path abs_dstdir = fs::absolute(canonicalize(fs::path(dst).parent_path().empty() ? "." : fs::path(dst).parent_path().string(), false));
            target = abs_src.lexically_relative(abs_dstdir).string();
        }
        if (inter && ::access(dst.c_str(), F_OK) == 0) {
            std::fprintf(stderr, "%s: replace '%s'? ", prog.c_str(), dst.c_str());
            char ans[8] = {0};
            if (!std::fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) continue;
            force = true;
        }
        if (force) {
            struct stat st;
            if (::lstat(dst.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
                ::unlink(dst.c_str());
            }
        }
        const int r = symbolic ? ::symlink(target.c_str(), dst.c_str()) : ::link(src.c_str(), dst.c_str());
        if (r != 0) {
            err_errno("failed to create " + std::string(symbolic ? "symbolic" : "hard") + " link '" + dst + "' => '" + src + "'");
            rc = 1;
        } else if (verbose) {
            out("'" + dst + "' " + (symbolic ? "->" : "=>") + " '" + src + "'\n");
        }
    }
    return rc;
}

int cmd_link(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "FILE1 FILE2", "Call the link function to create a link named FILE2 to an existing FILE1.")) {
        return 0;
    }
    if (argc != 3) {
        usage_error(argc < 3 ? "missing operand" : "extra operand '" + std::string(argv[3]) + "'");
        return 1;
    }
    if (::link(argv[1], argv[2]) != 0) {
        err_errno(std::string("cannot create link '") + argv[2] + "' to '" + argv[1] + "'");
        return 1;
    }
    return 0;
}

int cmd_unlink(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "FILE", "Call the unlink function to remove the specified FILE.")) {
        return 0;
    }
    if (argc != 2) {
        usage_error(argc < 2 ? "missing operand" : "extra operand '" + std::string(argv[2]) + "'");
        return 1;
    }
    if (::unlink(argv[1]) != 0) {
        err_errno(std::string("cannot unlink '") + argv[1] + "'");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
int cmd_touch(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE...", "Update the access and modification times of each FILE to the current time.")) {
        return 0;
    }
    static const option lo[] = {{"no-create", no_argument, nullptr, 'c'}, {"date", required_argument, nullptr, 'd'},
                                {"reference", required_argument, nullptr, 'r'}, {"no-dereference", no_argument, nullptr, 'h'}, kEnd};
    bool no_create = false, only_a = false, only_m = false, no_deref = false;
    std::string date_s, stamp, ref;
    int c;
    while ((c = getopt_long(argc, argv, "acd:fhmr:t:", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': only_a = true; break;
        case 'm': only_m = true; break;
        case 'c': no_create = true; break;
        case 'd': date_s = optarg; break;
        case 'f': break;
        case 'h': no_deref = true; break;
        case 'r': ref = optarg; break;
        case 't': stamp = optarg; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        usage_error("missing file operand");
        return 1;
    }
    timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
    if (!ref.empty()) {
        struct stat st;
        if (::stat(ref.c_str(), &st) != 0) {
            err_errno("failed to get attributes of '" + ref + "'");
            return 1;
        }
        ts[0] = st.st_atim;
        ts[1] = st.st_mtim;
    }
    if (!date_s.empty() || !stamp.empty()) {
        time_t t;
        if (!stamp.empty()) {
            // [[CC]YY]MMDDhhmm[.ss]
            std::string s = stamp;
            std::string secs = "00";
            const size_t dot = s.find('.');
            if (dot != std::string::npos) {
                secs = s.substr(dot + 1);
                s = s.substr(0, dot);
            }
            std::tm tmv{};
            tmv.tm_isdst = -1;
            auto num = [&](size_t pos, size_t n) { return std::atoi(s.substr(pos, n).c_str()); };
            if (s.size() == 12) { tmv.tm_year = num(0, 4) - 1900; tmv.tm_mon = num(4, 2) - 1; tmv.tm_mday = num(6, 2); tmv.tm_hour = num(8, 2); tmv.tm_min = num(10, 2); }
            else if (s.size() == 10) { const int yy = num(0, 2); tmv.tm_year = (yy < 69 ? 2000 + yy : 1900 + yy) - 1900; tmv.tm_mon = num(2, 2) - 1; tmv.tm_mday = num(4, 2); tmv.tm_hour = num(6, 2); tmv.tm_min = num(8, 2); }
            else if (s.size() == 8) { const time_t now = std::time(nullptr); localtime_r(&now, &tmv); tmv.tm_mon = num(0, 2) - 1; tmv.tm_mday = num(2, 2); tmv.tm_hour = num(4, 2); tmv.tm_min = num(6, 2); }
            else { err("invalid date format '" + stamp + "'"); return 1; }
            tmv.tm_sec = std::atoi(secs.c_str());
            t = mktime(&tmv);
        } else if (!parse_date(date_s, std::time(nullptr), t)) {
            err("invalid date format '" + date_s + "'");
            return 1;
        }
        ts[0].tv_sec = ts[1].tv_sec = t;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
    }
    if (only_a && !only_m) ts[1].tv_nsec = UTIME_OMIT;
    if (only_m && !only_a) ts[0].tv_nsec = UTIME_OMIT;

    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string p = argv[i];
        struct stat st;
        const bool exists = (no_deref ? ::lstat(p.c_str(), &st) : ::stat(p.c_str(), &st)) == 0;
        if (!exists) {
            if (no_create) continue;
            const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_NOCTTY | O_CLOEXEC, 0666);
            if (fd < 0) {
                err_errno("cannot touch '" + p + "'");
                rc = 1;
                continue;
            }
            ::close(fd);
        }
        if (::utimensat(AT_FDCWD, p.c_str(), ts, no_deref ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
            err_errno("setting times of '" + p + "'");
            rc = 1;
        }
    }
    return rc;
}

int cmd_truncate(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "OPTION... FILE...", "Shrink or extend the size of each FILE to the specified size.")) {
        return 0;
    }
    static const option lo[] = {{"no-create", no_argument, nullptr, 'c'}, {"size", required_argument, nullptr, 's'},
                                {"reference", required_argument, nullptr, 'r'}, kEnd};
    bool no_create = false;
    std::string size_s, ref;
    int c;
    while ((c = getopt_long(argc, argv, "cos:r:", lo, nullptr)) != -1) {
        if (c == 'c') no_create = true;
        else if (c == 'o') {}
        else if (c == 's') size_s = optarg;
        else if (c == 'r') ref = optarg;
        else return 1;
    }
    if (size_s.empty() && ref.empty()) {
        usage_error("you must specify either '--size' or '--reference'");
        return 1;
    }
    if (optind >= argc) {
        usage_error("missing file operand");
        return 1;
    }
    char op = 0;
    int64_t amount = 0;
    if (!size_s.empty()) {
        std::string s = size_s;
        if (std::strchr("+-<>/%", s[0])) {
            op = s[0];
            s = s.substr(1);
        }
        uint64_t v;
        if (!parse_size(s, v)) {
            err("Invalid number: '" + size_s + "'");
            return 1;
        }
        amount = static_cast<int64_t>(v);
    }
    int64_t ref_size = -1;
    if (!ref.empty()) {
        struct stat st;
        if (::stat(ref.c_str(), &st) != 0) {
            err_errno("cannot stat '" + ref + "'");
            return 1;
        }
        ref_size = st.st_size;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const int fd = ::open(argv[i], O_WRONLY | (no_create ? 0 : O_CREAT) | O_CLOEXEC | O_NOCTTY, 0666);
        if (fd < 0) {
            if (no_create && errno == ENOENT) continue;
            err_errno(std::string("cannot open '") + argv[i] + "' for writing");
            rc = 1;
            continue;
        }
        struct stat st;
        ::fstat(fd, &st);
        int64_t cur = st.st_size, target = ref_size >= 0 ? ref_size : amount;
        if (ref_size >= 0 && !size_s.empty()) amount = ref_size;
        switch (op) {
        case '+': target = cur + amount; break;
        case '-': target = cur - amount; break;
        case '<': target = std::min(cur, amount); break;
        case '>': target = std::max(cur, amount); break;
        case '/': target = amount ? (cur / amount) * amount : cur; break;
        case '%': target = amount ? ((cur + amount - 1) / amount) * amount : cur; break;
        default: break;
        }
        if (target < 0) target = 0;
        if (::ftruncate(fd, target) != 0) {
            err_errno(std::string("failed to truncate '") + argv[i] + "'");
            rc = 1;
        }
        ::close(fd);
    }
    return rc;
}

int cmd_sync(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] [FILE]...", "Synchronize cached writes to persistent storage.")) {
        return 0;
    }
    static const option lo[] = {{"data", no_argument, nullptr, 'd'}, {"file-system", no_argument, nullptr, 'f'}, kEnd};
    bool data = false, fsys = false;
    int c;
    while ((c = getopt_long(argc, argv, "df", lo, nullptr)) != -1) {
        if (c == 'd') data = true;
        else if (c == 'f') fsys = true;
        else return 1;
    }
    if (optind >= argc) {
        if (data || fsys) {
            usage_error("-d/-f need at least one FILE");
            return 1;
        }
        ::sync();
        return 0;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const int fd = ::open(argv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            err_errno(std::string("error opening '") + argv[i] + "'");
            rc = 1;
            continue;
        }
        const int r = fsys ? ::syncfs(fd) : (data ? ::fdatasync(fd) : ::fsync(fd));
        if (r != 0) {
            err_errno(std::string("error syncing '") + argv[i] + "'");
            rc = 1;
        }
        ::close(fd);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_readlink(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE...", "Print value of a symbolic link or canonical file name.")) {
        return 0;
    }
    static const option lo[] = {{"canonicalize", no_argument, nullptr, 'f'}, {"canonicalize-existing", no_argument, nullptr, 'e'},
                                {"canonicalize-missing", no_argument, nullptr, 'm'}, {"no-newline", no_argument, nullptr, 'n'},
                                {"quiet", no_argument, nullptr, 'q'}, {"silent", no_argument, nullptr, 's'},
                                {"verbose", no_argument, nullptr, 'v'}, {"zero", no_argument, nullptr, 'z'}, kEnd};
    char mode = 0;
    bool nonl = false, verbose = false, zero = false;
    int c;
    while ((c = getopt_long(argc, argv, "femnqsvz", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': case 'e': case 'm': mode = static_cast<char>(c); break;
        case 'n': nonl = true; break;
        case 'q': case 's': verbose = false; break;
        case 'v': verbose = true; break;
        case 'z': zero = true; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    if (argc - optind > 1) {
        nonl = false;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        std::string r;
        if (mode) {
            r = canonicalize(argv[i], mode == 'e');
        } else {
            r = readlink_str(argv[i]);
        }
        if (r.empty()) {
            if (verbose) err_errno(argv[i]);
            rc = 1;
            continue;
        }
        out(r + (nonl ? "" : (zero ? std::string(1, '\0') : "\n")));
    }
    return rc;
}

int cmd_realpath(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE...", "Print the resolved absolute file name.")) {
        return 0;
    }
    static const option lo[] = {{"canonicalize-existing", no_argument, nullptr, 'e'}, {"canonicalize-missing", no_argument, nullptr, 'm'},
                                {"no-symlinks", no_argument, nullptr, 's'}, {"strip", no_argument, nullptr, 's'},
                                {"quiet", no_argument, nullptr, 'q'}, {"zero", no_argument, nullptr, 'z'},
                                {"logical", no_argument, nullptr, 'L'}, {"physical", no_argument, nullptr, 'P'},
                                {"relative-to", required_argument, nullptr, 1}, {"relative-base", required_argument, nullptr, 2}, kEnd};
    bool must_exist = false, missing = false, nosym = false, quiet = false, zero = false;
    std::string rel_to, rel_base;
    int c;
    while ((c = getopt_long(argc, argv, "emsqzLP", lo, nullptr)) != -1) {
        switch (c) {
        case 'e': must_exist = true; break;
        case 'm': missing = true; break;
        case 's': nosym = true; break;
        case 'q': quiet = true; break;
        case 'z': zero = true; break;
        case 'L': case 'P': break;
        case 1: rel_to = optarg; break;
        case 2: rel_base = optarg; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        std::string r;
        if (nosym) {
            r = fs::absolute(argv[i]).lexically_normal().string();
            if (r.size() > 1 && r.back() == '/') r.pop_back();
        } else {
            r = canonicalize(argv[i], must_exist);
            if (r.empty() || (!missing && !must_exist && ::access(r.c_str(), F_OK) != 0 && !fs::exists(fs::path(r).parent_path()))) {
                if (!quiet) err_errno(argv[i], ENOENT);
                rc = 1;
                continue;
            }
        }
        if (!rel_to.empty() || !rel_base.empty()) {
            const std::string base = canonicalize(rel_to.empty() ? rel_base : rel_to, false);
            const std::string rb = rel_base.empty() ? "" : canonicalize(rel_base, false);
            if (rb.empty() || r.rfind(rb, 0) == 0) {
                r = fs::path(r).lexically_relative(base).string();
                if (r.empty()) r = ".";
            }
        }
        out(r + (zero ? std::string(1, '\0') : "\n"));
    }
    return rc;
}

int cmd_pathchk(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... NAME...", "Diagnose invalid or unportable file names.")) {
        return 0;
    }
    static const option lo[] = {{"portability", no_argument, nullptr, 2}, kEnd};
    bool posix = false, extra = false;
    int c;
    while ((c = getopt_long(argc, argv, "pP", lo, nullptr)) != -1) {
        if (c == 'p' || c == 2) posix = true;
        else if (c == 'P') extra = true;
        else return 1;
    }
    if (optind >= argc) {
        usage_error("missing operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string p = argv[i];
        const size_t path_max = posix ? 256 : PATH_MAX;
        const size_t name_max = posix ? 14 : NAME_MAX;
        if (p.empty() && (posix || extra)) {
            err("empty file name");
            rc = 1;
            continue;
        }
        if (p.size() >= path_max) {
            err("limit " + std::to_string(path_max) + " exceeded by length " + std::to_string(p.size()) + " of file name '" + p + "'");
            rc = 1;
            continue;
        }
        bool bad = false;
        size_t pos = 0;
        while (pos <= p.size() && !bad) {
            size_t e = p.find('/', pos);
            if (e == std::string::npos) e = p.size();
            const std::string comp = p.substr(pos, e - pos);
            if (comp.size() > name_max) {
                err("limit " + std::to_string(name_max) + " exceeded by length " + std::to_string(comp.size()) + " of file name component '" + comp + "'");
                bad = true;
            } else if ((posix || extra) && !comp.empty() && comp[0] == '-' ) {
                err("leading '-' in a component of file name '" + p + "'");
                bad = true;
            } else if (posix) {
                for (char ch : comp) {
                    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-')) {
                        err(std::string("nonportable character '") + ch + "' in file name '" + p + "'");
                        bad = true;
                        break;
                    }
                }
            }
            pos = e + 1;
        }
        rc |= bad;
    }
    return rc;
}

int cmd_pwd(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-LP]", "Print the full filename of the current working directory.")) {
        return 0;
    }
    bool physical = false;
    int c;
    while ((c = getopt(argc, argv, "LP")) != -1) {
        if (c == 'P') physical = true;
        else if (c == 'L') physical = false;
        else return 1;
    }
    if (!physical) {
        const char *pwd = std::getenv("PWD");
        if (pwd && pwd[0] == '/') {
            struct stat a, b;
            if (::stat(pwd, &a) == 0 && ::stat(".", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                out(std::string(pwd) + "\n");
                return 0;
            }
        }
    }
    char buf[PATH_MAX];
    if (!::getcwd(buf, sizeof buf)) {
        err_errno("cannot get current directory");
        return 1;
    }
    out(std::string(buf) + "\n");
    return 0;
}

} // namespace cu
