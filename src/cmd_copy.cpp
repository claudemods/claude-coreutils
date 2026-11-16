#include <linux/fs.h>
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
// cp, mv, rm, install, shred, dd: the commands that move data around.
// Arch/CachyOS: btrfs, XFS, bcachefs and ZFS are common root filesystems, so copies use reflinks
// (FICLONE) by default and fall back to copy_file_range(2) / read+write.
#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

namespace cu {

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

enum class Reflink { Auto, Always, Never };

std::string base_name(const std::string &p)
{
    const std::string s = strip_trailing_slashes(p);
    const size_t sl = s.find_last_of('/');
    return sl == std::string::npos || s == "/" ? s : s.substr(sl + 1);
}

bool is_dir(const std::string &p, bool follow = true)
{
    struct stat st;
    return (follow ? ::stat(p.c_str(), &st) : ::lstat(p.c_str(), &st)) == 0 && S_ISDIR(st.st_mode);
}

bool ask(const std::string &question)
{
    std::fprintf(stderr, "%s: %s ", prog.c_str(), question.c_str());
    char buf[16] = {0};
    if (!std::fgets(buf, sizeof buf, stdin)) {
        return false;
    }
    return buf[0] == 'y' || buf[0] == 'Y';
}

// ---------------------------------------------------------------------------------------------
// Data copy
// ---------------------------------------------------------------------------------------------

/// Copies the contents of `in` to `out`. Returns 0 or an errno value.
int copy_data(int in, int outfd, off_t size, Reflink reflink)
{
    if (reflink != Reflink::Never) {
        if (::ioctl(outfd, FICLONE, in) == 0) {
            return 0;
        }
        if (reflink == Reflink::Always) {
            return errno;
        }
    }
    // copy_file_range: in-kernel copy, server-side on NFS, reflink-ish on some filesystems
    off_t remaining = size;
    bool cfr_ok = true;
    while (cfr_ok && remaining > 0) {
        const ssize_t n = ::copy_file_range(in, nullptr, outfd, nullptr, static_cast<size_t>(std::min<off_t>(remaining, 1 << 30)), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            cfr_ok = false; // EXDEV, EINVAL, ENOSYS ... use the generic path
            break;
        }
        if (n == 0) {
            return 0;
        }
        remaining -= n;
    }
    if (cfr_ok && remaining <= 0) {
        return 0;
    }
    std::vector<char> buf(1 << 18);
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (n == 0) {
            return 0;
        }
        if (!write_all(outfd, buf.data(), static_cast<size_t>(n))) {
            return errno;
        }
    }
}

void copy_xattrs(const std::string &src, const std::string &dst, bool nofollow)
{
    ssize_t len = nofollow ? ::llistxattr(src.c_str(), nullptr, 0) : ::listxattr(src.c_str(), nullptr, 0);
    if (len <= 0) {
        return;
    }
    std::string names(static_cast<size_t>(len), '\0');
    len = nofollow ? ::llistxattr(src.c_str(), names.data(), names.size()) : ::listxattr(src.c_str(), names.data(), names.size());
    for (size_t i = 0; i < static_cast<size_t>(std::max<ssize_t>(len, 0));) {
        const std::string name = names.c_str() + i;
        i += name.size() + 1;
        const ssize_t vl = nofollow ? ::lgetxattr(src.c_str(), name.c_str(), nullptr, 0) : ::getxattr(src.c_str(), name.c_str(), nullptr, 0);
        if (vl < 0) continue;
        std::string val(static_cast<size_t>(vl), '\0');
        if ((nofollow ? ::lgetxattr(src.c_str(), name.c_str(), val.data(), val.size()) : ::getxattr(src.c_str(), name.c_str(), val.data(), val.size())) < 0) continue;
        if (nofollow) ::lsetxattr(dst.c_str(), name.c_str(), val.data(), val.size(), 0);
        else ::setxattr(dst.c_str(), name.c_str(), val.data(), val.size(), 0);
    }
}

struct CopyOpts {
    bool recursive = false, force = false, interactive = false, no_clobber = false, verbose = false, update = false;
    bool pres_mode = false, pres_own = false, pres_time = false, pres_xattr = false, pres_links = false;
    bool deref = false;          // -L: follow every symlink
    bool deref_cmdline = false;  // -H
    bool make_hard = false, make_sym = false;
    bool remove_dest = false;    // --remove-destination
    bool one_fs = false;
    Reflink reflink = Reflink::Auto;
    std::string verb_prefix = "";
};

class Copier
{
public:
    explicit Copier(const CopyOpts &o) : o_(o) {}

    int copy(const std::string &src, const std::string &dst, bool top)
    {
        struct stat st;
        const bool follow = o_.deref || (top && o_.deref_cmdline) || (top && !o_.recursive && !o_.pres_links);
        if ((follow ? ::stat(src.c_str(), &st) : ::lstat(src.c_str(), &st)) != 0) {
            err_errno("cannot stat '" + src + "'");
            return 1;
        }
        if (top) {
            top_dev_ = st.st_dev;
        } else if (o_.one_fs && st.st_dev != top_dev_) {
            return 0;
        }

        struct stat dst_st;
        const bool dst_exists = ::lstat(dst.c_str(), &dst_st) == 0;
        if (dst_exists && dst_st.st_dev == st.st_dev && dst_st.st_ino == st.st_ino && !S_ISLNK(st.st_mode)) {
            err("'" + src + "' and '" + dst + "' are the same file");
            return 1;
        }
        if (dst_exists && !S_ISDIR(st.st_mode)) {
            if (o_.no_clobber) return 0;
            if (o_.update) {
                struct stat sd;
                if (::stat(dst.c_str(), &sd) == 0 && sd.st_mtim.tv_sec >= st.st_mtim.tv_sec) return 0;
            }
            if (o_.interactive && !ask("overwrite '" + dst + "'?")) return 0;
        }

        if (S_ISDIR(st.st_mode)) {
            return copy_dir(src, dst, st, dst_exists, dst_st);
        }

        // hard link preservation (cp -a / --preserve=links)
        if (o_.pres_links && st.st_nlink > 1) {
            const auto key = std::make_pair(st.st_dev, st.st_ino);
            auto it = seen_.find(key);
            if (it != seen_.end()) {
                if (dst_exists) ::unlink(dst.c_str());
                if (::link(it->second.c_str(), dst.c_str()) == 0) {
                    verbose(src, dst);
                    return 0;
                }
            } else {
                seen_[key] = dst;
            }
        }

        if (o_.make_hard || o_.make_sym) {
            if (dst_exists) ::unlink(dst.c_str());
            const int r = o_.make_hard ? ::link(src.c_str(), dst.c_str()) : ::symlink(fs::absolute(src).c_str(), dst.c_str());
            if (r != 0) {
                err_errno("cannot create link '" + dst + "'");
                return 1;
            }
            verbose(src, dst);
            return 0;
        }

        if (S_ISLNK(st.st_mode)) {
            const std::string target = readlink_str(src);
            if (dst_exists) ::unlink(dst.c_str());
            if (::symlink(target.c_str(), dst.c_str()) != 0) {
                err_errno("cannot create symbolic link '" + dst + "'");
                return 1;
            }
            if (o_.pres_own) ::lchown(dst.c_str(), st.st_uid, st.st_gid);
            verbose(src, dst);
            return 0;
        }
        if (S_ISFIFO(st.st_mode) && o_.recursive) {
            if (dst_exists) ::unlink(dst.c_str());
            if (::mkfifo(dst.c_str(), st.st_mode & 0777) != 0) {
                err_errno("cannot create fifo '" + dst + "'");
                return 1;
            }
            verbose(src, dst);
            return 0;
        }
        if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISSOCK(st.st_mode)) {
            if (!o_.recursive) {
                // cp of a device without -r copies its contents (e.g. /dev/zero): fall through to data copy
            } else {
                if (dst_exists) ::unlink(dst.c_str());
                if (::mknod(dst.c_str(), st.st_mode, st.st_rdev) != 0) {
                    err_errno("cannot create special file '" + dst + "'");
                    return 1;
                }
                verbose(src, dst);
                return 0;
            }
        }
        return copy_regular(src, dst, st, dst_exists);
    }

private:
    void verbose(const std::string &src, const std::string &dst) const
    {
        if (o_.verbose) out("'" + src + "' -> '" + dst + "'\n");
    }

    int copy_regular(const std::string &src, const std::string &dst, const struct stat &st, bool dst_exists)
    {
        const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (in < 0) {
            err_errno("cannot open '" + src + "' for reading");
            return 1;
        }
        const mode_t create_mode = (st.st_mode & 0777) | S_IWUSR;
        int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, o_.pres_mode ? 0600 : create_mode);
        if (fd < 0 && errno == EEXIST) {
            if (o_.remove_dest) {
                ::unlink(dst.c_str());
            }
            fd = ::open(dst.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOCTTY);
            if (fd < 0 && o_.force) {
                ::unlink(dst.c_str());
                fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, create_mode);
            }
        }
        (void)dst_exists;
        if (fd < 0) {
            err_errno("cannot create regular file '" + dst + "'");
            ::close(in);
            return 1;
        }
        const int e = S_ISREG(st.st_mode) ? copy_data(in, fd, st.st_size, o_.reflink) : copy_data(in, fd, LLONG_MAX, Reflink::Never);
        ::close(in);
        if (e != 0) {
            err_errno("failed to copy '" + src + "' to '" + dst + "'", e);
            ::close(fd);
            return 1;
        }
        finish_attrs(src, dst, st, fd);
        if (::close(fd) != 0) {
            err_errno("failed to close '" + dst + "'");
            return 1;
        }
        verbose(src, dst);
        return 0;
    }

    void finish_attrs(const std::string &src, const std::string &dst, const struct stat &st, int fd)
    {
        if (o_.pres_own) {
            if (::fchown(fd, st.st_uid, st.st_gid) != 0) {
                ::fchown(fd, static_cast<uid_t>(-1), st.st_gid);
            }
        }
        if (o_.pres_mode) {
            ::fchmod(fd, st.st_mode & 07777);
        }
        if (o_.pres_xattr) {
            copy_xattrs(src, dst, false);
        }
        if (o_.pres_time) {
            const timespec ts[2] = {st.st_atim, st.st_mtim};
            ::futimens(fd, ts);
        }
    }

    int copy_dir(const std::string &src, const std::string &dst, const struct stat &st, bool dst_exists, const struct stat &dst_st)
    {
        if (!o_.recursive) {
            err("-r not specified; omitting directory '" + src + "'");
            return 1;
        }
        // a directory must not be copied into itself
        if (dst_exists && S_ISDIR(dst_st.st_mode) == false) {
            err("cannot overwrite non-directory '" + dst + "' with directory '" + src + "'");
            return 1;
        }
        const std::string real_src = canonicalize(src, true);
        const std::string real_dst_parent = canonicalize(fs::path(dst).parent_path().empty() ? "." : fs::path(dst).parent_path().string(), false);
        if (!real_src.empty() && (real_dst_parent + "/").rfind(real_src + "/", 0) == 0) {
            err("cannot copy a directory, '" + src + "', into itself, '" + dst + "'");
            return 1;
        }
        if (!dst_exists) {
            if (::mkdir(dst.c_str(), (st.st_mode & 0777) | S_IRWXU) != 0) {
                err_errno("cannot create directory '" + dst + "'");
                return 1;
            }
            verbose(src, dst);
        }
        int rc = 0;
        std::error_code ec;
        for (fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            rc |= copy(it->path().string(), dst + "/" + it->path().filename().string(), false);
        }
        if (ec) {
            err(src + ": " + ec.message());
            rc = 1;
        }
        if (o_.pres_own) ::chown(dst.c_str(), st.st_uid, st.st_gid);
        if (o_.pres_mode) ::chmod(dst.c_str(), st.st_mode & 07777);
        else if (!dst_exists) ::chmod(dst.c_str(), (st.st_mode & 0777) & ~[] { const mode_t m = ::umask(0); ::umask(m); return m; }());
        if (o_.pres_xattr) copy_xattrs(src, dst, false);
        if (o_.pres_time) {
            const timespec ts[2] = {st.st_atim, st.st_mtim};
            ::utimensat(AT_FDCWD, dst.c_str(), ts, 0);
        }
        return rc;
    }

    CopyOpts o_;
    dev_t top_dev_ = 0;
    std::map<std::pair<dev_t, ino_t>, std::string> seen_;
};

/// Shared cp/mv argument resolution: returns the destination for each source.
bool resolve_dest(std::vector<std::string> &args, const std::string &target_dir, bool no_target_dir,
                  std::vector<std::pair<std::string, std::string>> &pairs)
{
    if (!target_dir.empty()) {
        if (!is_dir(target_dir)) {
            err("target '" + target_dir + "' is not a directory");
            return false;
        }
        for (const std::string &s : args) {
            pairs.emplace_back(s, strip_trailing_slashes(target_dir) + "/" + base_name(s));
        }
        return true;
    }
    if (args.size() < 2) {
        if (args.empty()) {
            usage_error("missing file operand");
        } else {
            usage_error("missing destination file operand after '" + args[0] + "'");
        }
        return false;
    }
    const std::string last = args.back();
    args.pop_back();
    if (args.size() == 1 && (no_target_dir || !is_dir(last))) {
        pairs.emplace_back(args[0], last);
        return true;
    }
    if (no_target_dir) {
        usage_error("extra operand '" + args[1] + "'");
        return false;
    }
    if (!is_dir(last)) {
        err("target '" + last + "' is not a directory");
        return false;
    }
    for (const std::string &s : args) {
        pairs.emplace_back(s, strip_trailing_slashes(last) + "/" + base_name(s));
    }
    return true;
}

void apply_preserve_list(const std::string &list, CopyOpts &o)
{
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t e = list.find(',', pos);
        if (e == std::string::npos) e = list.size();
        const std::string w = list.substr(pos, e - pos);
        if (w == "all") o.pres_mode = o.pres_own = o.pres_time = o.pres_xattr = o.pres_links = true;
        else if (w == "mode") o.pres_mode = true;
        else if (w == "ownership") o.pres_own = true;
        else if (w == "timestamps") o.pres_time = true;
        else if (w == "xattr") o.pres_xattr = true;
        else if (w == "links") o.pres_links = true;
        pos = e + 1;
    }
}

} // namespace

int cmd_cp(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... SOURCE... DEST | -t DIRECTORY SOURCE...",
                            "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY (reflinks on btrfs/XFS/bcachefs/ZFS by default).")) {
        return 0;
    }
    static const option lo[] = {{"archive", no_argument, nullptr, 'a'}, {"recursive", no_argument, nullptr, 'r'},
                                {"force", no_argument, nullptr, 'f'}, {"interactive", no_argument, nullptr, 'i'},
                                {"no-clobber", no_argument, nullptr, 'n'}, {"verbose", no_argument, nullptr, 'v'},
                                {"update", no_argument, nullptr, 'u'}, {"link", no_argument, nullptr, 'l'},
                                {"symbolic-link", no_argument, nullptr, 's'}, {"dereference", no_argument, nullptr, 'L'},
                                {"no-dereference", no_argument, nullptr, 'P'}, {"no-target-directory", no_argument, nullptr, 'T'},
                                {"target-directory", required_argument, nullptr, 't'}, {"preserve", optional_argument, nullptr, 1},
                                {"no-preserve", required_argument, nullptr, 2}, {"reflink", optional_argument, nullptr, 3},
                                {"remove-destination", no_argument, nullptr, 4}, {"one-file-system", no_argument, nullptr, 'x'},
                                {"parents", no_argument, nullptr, 5}, kEnd};
    CopyOpts o;
    std::string tdir;
    bool no_target_dir = false, parents = false;
    int c;
    while ((c = getopt_long(argc, argv, "arRfinvulsLPHTt:pdxZ", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': o.recursive = true; o.pres_mode = o.pres_own = o.pres_time = o.pres_xattr = o.pres_links = true; break;
        case 'r': case 'R': o.recursive = true; break;
        case 'f': o.force = true; break;
        case 'i': o.interactive = true; break;
        case 'n': o.no_clobber = true; break;
        case 'v': o.verbose = true; break;
        case 'u': o.update = true; break;
        case 'l': o.make_hard = true; break;
        case 's': o.make_sym = true; break;
        case 'L': o.deref = true; break;
        case 'H': o.deref_cmdline = true; break;
        case 'P': o.deref = false; break;
        case 'T': no_target_dir = true; break;
        case 't': tdir = optarg; break;
        case 'p': o.pres_mode = o.pres_own = o.pres_time = true; break;
        case 'd': o.pres_links = true; break;
        case 'x': o.one_fs = true; break;
        case 'Z': break;
        case 1: apply_preserve_list(optarg ? optarg : "mode,ownership,timestamps", o); break;
        case 2: {
            const std::string w = optarg;
            if (w == "mode") o.pres_mode = false;
            else if (w == "ownership") o.pres_own = false;
            else if (w == "timestamps") o.pres_time = false;
            else if (w == "xattr") o.pres_xattr = false;
            else if (w == "links") o.pres_links = false;
            break;
        }
        case 3: {
            const std::string w = optarg ? optarg : "always";
            if (w == "always") o.reflink = Reflink::Always;
            else if (w == "never") o.reflink = Reflink::Never;
            else o.reflink = Reflink::Auto;
            break;
        }
        case 4: o.remove_dest = true; break;
        case 5: parents = true; break;
        default: return 1;
        }
    }
    std::vector<std::string> args(argv + optind, argv + argc);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!resolve_dest(args, tdir, no_target_dir, pairs)) {
        return 1;
    }
    Copier copier(o);
    int rc = 0;
    for (auto &[src, dst] : pairs) {
        if (parents) {
            const std::string base = tdir.empty() ? std::string(argv[argc - 1]) : tdir;
            dst = strip_trailing_slashes(base) + "/" + src;
            std::error_code ec;
            fs::create_directories(fs::path(dst).parent_path(), ec);
        }
        rc |= copier.copy(src, dst, true);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
namespace {

int remove_tree(const std::string &path, bool recursive, bool force, bool verbose, bool dirs_ok, int interactive,
                bool one_fs, dev_t top_dev, bool top)
{
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (force && errno == ENOENT) return 0;
        err_errno("cannot remove '" + path + "'");
        return 1;
    }
    if (one_fs && !top && st.st_dev != top_dev) return 0;
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            if (dirs_ok) {
                if (interactive == 2 && !ask("remove directory '" + path + "'?")) return 0;
                if (::rmdir(path.c_str()) != 0) {
                    err_errno("cannot remove '" + path + "'");
                    return 1;
                }
                if (verbose) out("removed directory '" + path + "'\n");
                return 0;
            }
            err("cannot remove '" + path + "': Is a directory");
            return 1;
        }
        if (interactive == 2 && !ask("descend into directory '" + path + "'?")) return 0;
        int rc = 0;
        std::error_code ec;
        std::vector<std::string> children;
        for (fs::directory_iterator it(path, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
            children.push_back(it->path().string());
        }
        if (ec) {
            err(path + ": " + ec.message());
            return 1;
        }
        for (const std::string &ch : children) {
            rc |= remove_tree(ch, true, force, verbose, dirs_ok, interactive, one_fs, top_dev, false);
        }
        if (rc != 0) return rc;
        if (interactive == 2 && !ask("remove directory '" + path + "'?")) return 0;
        if (::rmdir(path.c_str()) != 0) {
            err_errno("cannot remove '" + path + "'");
            return 1;
        }
        if (verbose) out("removed directory '" + path + "'\n");
        return 0;
    }
    if (interactive == 2 && !ask(std::string("remove ") + (S_ISREG(st.st_mode) ? "regular file" : "file") + " '" + path + "'?")) return 0;
    if (::unlink(path.c_str()) != 0) {
        err_errno("cannot remove '" + path + "'");
        return 1;
    }
    if (verbose) out("removed '" + path + "'\n");
    return 0;
}

} // namespace

int cmd_rm(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Remove (unlink) the FILE(s).")) {
        return 0;
    }
    static const option lo[] = {{"force", no_argument, nullptr, 'f'}, {"recursive", no_argument, nullptr, 'r'},
                                {"dir", no_argument, nullptr, 'd'}, {"verbose", no_argument, nullptr, 'v'},
                                {"interactive", optional_argument, nullptr, 3}, {"one-file-system", no_argument, nullptr, 1},
                                {"preserve-root", optional_argument, nullptr, 2}, {"no-preserve-root", no_argument, nullptr, 4}, kEnd};
    bool force = false, recursive = false, dirs = false, verbose = false, one_fs = false, preserve_root = true;
    int interactive = 0; // 0 never, 1 once (-I), 2 always (-i)
    int c;
    while ((c = getopt_long(argc, argv, "frRdvIi", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': force = true; interactive = 0; break;
        case 'r': case 'R': recursive = true; break;
        case 'd': dirs = true; break;
        case 'v': verbose = true; break;
        case 'i': interactive = 2; force = false; break;
        case 'I': interactive = 1; break;
        case 1: one_fs = true; break;
        case 2: preserve_root = true; break;
        case 3: interactive = (!optarg || std::strcmp(optarg, "always") == 0) ? 2 : (std::strcmp(optarg, "once") == 0 ? 1 : 0); break;
        case 4: preserve_root = false; break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        if (!force) {
            usage_error("missing operand");
            return 1;
        }
        return 0;
    }
    if (interactive == 1 && (recursive || argc - optind > 3)) {
        if (!ask("remove " + std::to_string(argc - optind) + " argument(s)" + (recursive ? " recursively?" : "?"))) {
            return 0;
        }
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string p = argv[i];
        const std::string base = base_name(p);
        if (base == "." || base == "..") {
            err("refusing to remove '.' or '..' directory: skipping '" + p + "'");
            rc = 1;
            continue;
        }
        if (preserve_root && recursive && canonicalize(p, false) == "/") {
            err("it is dangerous to operate recursively on '/'");
            err("use --no-preserve-root to override this failsafe");
            return 1;
        }
        struct stat st;
        dev_t dev = 0;
        if (::lstat(p.c_str(), &st) == 0) dev = st.st_dev;
        rc |= remove_tree(p, recursive, force, verbose, dirs, interactive == 2 ? 2 : 0, one_fs, dev, true);
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_mv(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... SOURCE... DEST | -t DIRECTORY SOURCE...", "Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.")) {
        return 0;
    }
    static const option lo[] = {{"force", no_argument, nullptr, 'f'}, {"interactive", no_argument, nullptr, 'i'},
                                {"no-clobber", no_argument, nullptr, 'n'}, {"verbose", no_argument, nullptr, 'v'},
                                {"update", no_argument, nullptr, 'u'}, {"no-target-directory", no_argument, nullptr, 'T'},
                                {"target-directory", required_argument, nullptr, 't'}, kEnd};
    bool interactive = false, no_clobber = false, verbose = false, update = false, ntd = false;
    std::string tdir;
    int c;
    while ((c = getopt_long(argc, argv, "finvuTt:Z", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': interactive = false; no_clobber = false; break;
        case 'i': interactive = true; break;
        case 'n': no_clobber = true; break;
        case 'v': verbose = true; break;
        case 'u': update = true; break;
        case 'T': ntd = true; break;
        case 't': tdir = optarg; break;
        case 'Z': break;
        default: return 1;
        }
    }
    std::vector<std::string> args(argv + optind, argv + argc);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!resolve_dest(args, tdir, ntd, pairs)) {
        return 1;
    }
    int rc = 0;
    for (const auto &[src, dst] : pairs) {
        struct stat ss, ds;
        if (::lstat(src.c_str(), &ss) != 0) {
            err_errno("cannot stat '" + src + "'");
            rc = 1;
            continue;
        }
        const bool dexists = ::lstat(dst.c_str(), &ds) == 0;
        if (dexists && ds.st_dev == ss.st_dev && ds.st_ino == ss.st_ino) {
            err("'" + src + "' and '" + dst + "' are the same file");
            rc = 1;
            continue;
        }
        if (dexists) {
            if (no_clobber) continue;
            if (update && ds.st_mtim.tv_sec >= ss.st_mtim.tv_sec) continue;
            if (interactive && !ask("overwrite '" + dst + "'?")) continue;
            if (S_ISDIR(ss.st_mode) && !S_ISDIR(ds.st_mode)) {
                err("cannot overwrite non-directory '" + dst + "' with directory '" + src + "'");
                rc = 1;
                continue;
            }
            if (!S_ISDIR(ss.st_mode) && S_ISDIR(ds.st_mode)) {
                err("cannot overwrite directory '" + dst + "' with non-directory");
                rc = 1;
                continue;
            }
        }
        if (::rename(src.c_str(), dst.c_str()) == 0) {
            if (verbose) out("renamed '" + src + "' -> '" + dst + "'\n");
            continue;
        }
        if (errno != EXDEV) {
            err_errno("cannot move '" + src + "' to '" + dst + "'");
            rc = 1;
            continue;
        }
        // across file systems: copy everything (ownership, times, xattrs, hard links), then remove the source
        CopyOpts o;
        o.recursive = true;
        o.pres_mode = o.pres_own = o.pres_time = o.pres_xattr = o.pres_links = true;
        o.force = true;
        Copier copier(o);
        if (copier.copy(src, dst, true) != 0) {
            rc = 1;
            continue;
        }
        if (remove_tree(src, true, true, false, true, 0, false, 0, true) != 0) {
            rc = 1;
            continue;
        }
        if (verbose) out("copied '" + src + "' -> '" + dst + "'\n");
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_install(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... SOURCE... DEST | -d DIRECTORY...", "Copy files and set attributes (replaces the target atomically, safe for running binaries).")) {
        return 0;
    }
    static const option lo[] = {{"directory", no_argument, nullptr, 'd'}, {"mode", required_argument, nullptr, 'm'},
                                {"owner", required_argument, nullptr, 'o'}, {"group", required_argument, nullptr, 'g'},
                                {"verbose", no_argument, nullptr, 'v'}, {"preserve-timestamps", no_argument, nullptr, 'p'},
                                {"compare", no_argument, nullptr, 'C'}, {"strip", no_argument, nullptr, 's'},
                                {"strip-program", required_argument, nullptr, 1}, {"target-directory", required_argument, nullptr, 't'},
                                {"no-target-directory", no_argument, nullptr, 'T'}, kEnd};
    bool dirs_mode = false, verbose = false, pres_time = false, compare = false, strip = false, mk_leading = false, ntd = false;
    std::string mode_spec = "0755", owner, group, tdir, strip_prog = "strip";
    int c;
    while ((c = getopt_long(argc, argv, "bcCdDg:m:o:pst:TvZ", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': case 'c': case 'Z': break;
        case 'C': compare = true; break;
        case 'd': dirs_mode = true; break;
        case 'D': mk_leading = true; break;
        case 'g': group = optarg; break;
        case 'm': mode_spec = optarg; break;
        case 'o': owner = optarg; break;
        case 'p': pres_time = true; break;
        case 's': strip = true; break;
        case 't': tdir = optarg; break;
        case 'T': ntd = true; break;
        case 'v': verbose = true; break;
        case 1: strip_prog = optarg; break;
        default: return 1;
        }
    }
    mode_t mode;
    if (!parse_mode(mode_spec, 0, false, mode)) {
        err("invalid mode '" + mode_spec + "'");
        return 1;
    }
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (!owner.empty() && !lookup_user(owner, uid)) {
        err("invalid user '" + owner + "'");
        return 1;
    }
    if (!group.empty() && !lookup_group(group, gid)) {
        err("invalid group '" + group + "'");
        return 1;
    }
    auto finish = [&](const std::string &path) -> bool {
        if (::chmod(path.c_str(), mode) != 0) {
            err_errno("cannot set permissions of '" + path + "'");
            return false;
        }
        if ((uid != static_cast<uid_t>(-1) || gid != static_cast<gid_t>(-1)) && ::chown(path.c_str(), uid, gid) != 0) {
            err_errno("cannot change ownership of '" + path + "'");
            return false;
        }
        return true;
    };
    if (dirs_mode) {
        if (optind >= argc) {
            usage_error("missing file operand");
            return 1;
        }
        int rc = 0;
        for (int i = optind; i < argc; ++i) {
            std::error_code ec;
            fs::create_directories(argv[i], ec);
            if (ec) {
                err(std::string("cannot create directory '") + argv[i] + "': " + ec.message());
                rc = 1;
                continue;
            }
            if (verbose) out(std::string("install: creating directory '") + argv[i] + "'\n");
            if (!finish(argv[i])) rc = 1;
        }
        return rc;
    }
    std::vector<std::string> args(argv + optind, argv + argc);
    if (mk_leading && !args.empty()) {
        std::error_code ec;
        const std::string last = args.back();
        fs::create_directories(tdir.empty() ? (ntd || args.size() == 2 ? fs::path(last).parent_path() : fs::path(last)) : fs::path(tdir), ec);
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!resolve_dest(args, tdir, ntd, pairs)) {
        return 1;
    }
    int rc = 0;
    for (const auto &[src, dst] : pairs) {
        struct stat ss;
        if (::stat(src.c_str(), &ss) != 0) {
            err_errno("cannot stat '" + src + "'");
            rc = 1;
            continue;
        }
        if (S_ISDIR(ss.st_mode)) {
            err("omitting directory '" + src + "'");
            rc = 1;
            continue;
        }
        if (compare && !strip && pres_time == false) {
            struct stat ds;
            std::string a, b;
            if (::stat(dst.c_str(), &ds) == 0 && ds.st_size == ss.st_size && ds.st_mode == ((ds.st_mode & ~07777) | mode) &&
                read_file(src, a) && read_file(dst, b) && a == b) {
                continue; // identical: leave it alone (keeps timestamps stable for pacman hooks / make)
            }
        }
        // write to a temporary name next to the target, then rename over it
        std::string tmp = dst + ".XXXXXX";
        const int fd = ::mkostemp(tmp.data(), O_CLOEXEC);
        if (fd < 0) {
            err_errno("cannot create temporary file for '" + dst + "'");
            rc = 1;
            continue;
        }
        const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
        int e = in < 0 ? errno : copy_data(in, fd, ss.st_size, Reflink::Auto);
        if (in >= 0) ::close(in);
        if (e == 0 && ::close(fd) != 0) e = errno;
        else if (e != 0) ::close(fd);
        if (e != 0) {
            err_errno("cannot copy '" + src + "' to '" + dst + "'", e);
            ::unlink(tmp.c_str());
            rc = 1;
            continue;
        }
        if (strip) {
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::execlp(strip_prog.c_str(), strip_prog.c_str(), tmp.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
            int status = 0;
            ::waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                err("strip process failed");
                ::unlink(tmp.c_str());
                rc = 1;
                continue;
            }
        }
        if (!finish(tmp)) {
            ::unlink(tmp.c_str());
            rc = 1;
            continue;
        }
        if (pres_time) {
            const timespec ts[2] = {ss.st_atim, ss.st_mtim};
            ::utimensat(AT_FDCWD, tmp.c_str(), ts, 0);
        }
        if (::rename(tmp.c_str(), dst.c_str()) != 0) {
            err_errno("cannot install '" + dst + "'");
            ::unlink(tmp.c_str());
            rc = 1;
            continue;
        }
        if (verbose) out("'" + src + "' -> '" + dst + "'\n");
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
int cmd_shred(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... FILE...", "Overwrite the specified FILE(s) repeatedly, to make it harder to recover the data.")) {
        return 0;
    }
    static const option lo[] = {{"force", no_argument, nullptr, 'f'}, {"iterations", required_argument, nullptr, 'n'},
                                {"size", required_argument, nullptr, 's'}, {"remove", optional_argument, nullptr, 'u'},
                                {"verbose", no_argument, nullptr, 'v'}, {"zero", no_argument, nullptr, 'z'},
                                {"exact", no_argument, nullptr, 'x'}, kEnd};
    int passes = 3;
    bool force = false, remove = false, verbose = false, zero = false;
    uint64_t forced_size = 0;
    int c;
    while ((c = getopt_long(argc, argv, "fn:s:uvxz", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': force = true; break;
        case 'n': passes = std::atoi(optarg); break;
        case 's': if (!parse_size(optarg, forced_size)) { err(std::string("invalid file size: '") + optarg + "'"); return 1; } break;
        case 'u': remove = true; break;
        case 'v': verbose = true; break;
        case 'z': zero = true; break;
        case 'x': break;
        default: return 1;
        }
    }
    if (optind >= argc) {
        usage_error("missing file operand");
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; ++i) {
        const std::string p = argv[i];
        if (p == "-") {
            continue;
        }
        if (force) ::chmod(p.c_str(), 0600);
        const int fd = ::open(p.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            err_errno("cannot open '" + p + "' for writing");
            rc = 1;
            continue;
        }
        struct stat st;
        ::fstat(fd, &st);
        // Arch/CachyOS default filesystems are copy-on-write: an overwrite does not touch the old blocks.
        struct statfs sf;
        if (verbose && ::fstatfs(fd, &sf) == 0) {
            const std::string t = arch::fs_type_name(static_cast<uint64_t>(sf.f_type));
            if (t == "btrfs" || t == "zfs" || t == "bcachefs" || t == "f2fs") {
                std::fprintf(stderr, "%s: note: %s is copy-on-write; shredding cannot guarantee the old data is gone\n", prog.c_str(), t.c_str());
            }
        }
        const uint64_t size = forced_size ? forced_size : static_cast<uint64_t>(st.st_size);
        std::vector<char> buf(1 << 16);
        bool failed = false;
        const int total = passes + (zero ? 1 : 0);
        for (int pass = 1; pass <= total && !failed; ++pass) {
            const bool zero_pass = zero && pass == total;
            if (verbose) {
                std::fprintf(stderr, "%s: %s: pass %d/%d (%s)...\n", prog.c_str(), p.c_str(), pass, total, zero_pass ? "000000" : "random");
            }
            if (::lseek(fd, 0, SEEK_SET) < 0) { failed = true; break; }
            uint64_t left = size;
            while (left > 0) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
                if (zero_pass) {
                    std::memset(buf.data(), 0, n);
                } else {
                    size_t got = 0;
                    while (got < n) {
                        const ssize_t r = ::getrandom(buf.data() + got, n - got, 0);
                        if (r < 0 && errno == EINTR) continue;
                        if (r < 0) { std::memset(buf.data() + got, 0xA5, n - got); break; }
                        got += static_cast<size_t>(r);
                    }
                }
                if (!write_all(fd, buf.data(), n)) { failed = true; break; }
                left -= n;
            }
            ::fsync(fd);
        }
        ::close(fd);
        if (failed) {
            err_errno("error writing '" + p + "'");
            rc = 1;
            continue;
        }
        if (remove) {
            // rename to a throwaway name first so the original name does not linger in the directory
            std::string dir = fs::path(p).parent_path().string();
            std::string tmp = (dir.empty() ? "" : dir + "/") + std::string(std::min<size_t>(fs::path(p).filename().string().size(), 12), '0');
            const std::string use = ::rename(p.c_str(), tmp.c_str()) == 0 ? tmp : p;
            if (::unlink(use.c_str()) != 0) {
                err_errno("cannot remove '" + p + "'");
                rc = 1;
            } else if (verbose) {
                std::fprintf(stderr, "%s: %s: removed\n", prog.c_str(), p.c_str());
            }
        }
    }
    return rc;
}

// ---------------------------------------------------------------------------------------------
namespace {

volatile sig_atomic_t g_sigusr1 = 0;
void on_usr1(int) { g_sigusr1 = 1; }

} // namespace

int cmd_dd(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPERAND]...", "Copy a file, converting and formatting according to the operands (if= of= bs= ibs= obs= count= skip= seek= conv= iflag= oflag= status=).")) {
        return 0;
    }
    std::string in_path, out_path, status = "default";
    uint64_t ibs = 512, obs = 512, count = UINT64_MAX, skip = 0, seek = 0;
    bool bs_set = false, notrunc = false, sync_pad = false, noerror = false, fsync_end = false, excl = false, nocreat = false, fdatasync_end = false;
    int iflags = 0, oflags = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) {
            err("unrecognized operand '" + a + "'");
            return 1;
        }
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        uint64_t n = 0;
        auto num = [&](uint64_t &dst) {
            if (!parse_size(v, n)) {
                err("invalid number: '" + v + "'");
                return false;
            }
            dst = n;
            return true;
        };
        if (k == "if") in_path = v;
        else if (k == "of") out_path = v;
        else if (k == "bs") { if (!num(ibs)) return 1; obs = ibs; bs_set = true; }
        else if (k == "ibs") { if (!num(ibs)) return 1; }
        else if (k == "obs") { if (!num(obs)) return 1; }
        else if (k == "count") { if (!num(count)) return 1; }
        else if (k == "skip" || k == "iseek") { if (!num(skip)) return 1; }
        else if (k == "seek" || k == "oseek") { if (!num(seek)) return 1; }
        else if (k == "status") status = v;
        else if (k == "conv") {
            size_t pos = 0;
            while (pos <= v.size()) {
                size_t e = v.find(',', pos);
                if (e == std::string::npos) e = v.size();
                const std::string w = v.substr(pos, e - pos);
                if (w == "notrunc") notrunc = true;
                else if (w == "sync") sync_pad = true;
                else if (w == "noerror") noerror = true;
                else if (w == "fsync") fsync_end = true;
                else if (w == "fdatasync") fdatasync_end = true;
                else if (w == "excl") excl = true;
                else if (w == "nocreat") nocreat = true;
                pos = e + 1;
            }
        } else if (k == "iflag" || k == "oflag") {
            int &fl = (k == "iflag") ? iflags : oflags;
            size_t pos = 0;
            while (pos <= v.size()) {
                size_t e = v.find(',', pos);
                if (e == std::string::npos) e = v.size();
                const std::string w = v.substr(pos, e - pos);
                if (w == "direct") fl |= O_DIRECT;
                else if (w == "sync") fl |= O_SYNC;
                else if (w == "dsync") fl |= O_DSYNC;
                else if (w == "nonblock") fl |= O_NONBLOCK;
                else if (w == "noatime") fl |= O_NOATIME;
                else if (w == "append") fl |= O_APPEND;
                pos = e + 1;
            }
        } else {
            err("unrecognized operand '" + a + "'");
            return 1;
        }
    }
    (void)bs_set;
    int in = 0, outfd = 1;
    if (!in_path.empty()) {
        in = ::open(in_path.c_str(), O_RDONLY | O_CLOEXEC | iflags);
        if (in < 0) {
            err_errno("failed to open '" + in_path + "'");
            return 1;
        }
    }
    if (!out_path.empty()) {
        int fl = O_WRONLY | O_CLOEXEC | oflags;
        if (!nocreat) fl |= O_CREAT;
        if (excl) fl |= O_EXCL;
        if (!notrunc && seek == 0) fl |= O_TRUNC;
        outfd = ::open(out_path.c_str(), fl, 0666);
        if (outfd < 0) {
            err_errno("failed to open '" + out_path + "'");
            return 1;
        }
        if (!notrunc && seek > 0) {
            ::ftruncate(outfd, static_cast<off_t>(seek * obs));
        }
    }
    if (skip) {
        if (::lseek(in, static_cast<off_t>(skip * ibs), SEEK_SET) < 0) {
            // not seekable: read and discard
            std::vector<char> sink(ibs);
            for (uint64_t s = 0; s < skip; ++s) {
                if (::read(in, sink.data(), sink.size()) <= 0) break;
            }
        }
    }
    if (seek && ::lseek(outfd, static_cast<off_t>(seek * obs), SEEK_SET) < 0) {
        err_errno("cannot seek in '" + out_path + "'");
        return 1;
    }
    struct sigaction sa{};
    sa.sa_handler = on_usr1;
    ::sigaction(SIGUSR1, &sa, nullptr);

    std::vector<char> ibuf(ibs), obuf;
    uint64_t full_in = 0, part_in = 0, full_out = 0, part_out = 0, bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    auto report = [&](bool final_report) {
        if (status == "none") return;
        if (!final_report && status != "progress") return;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (final_report && status == "progress") std::fprintf(stderr, "\n");
        if (status != "noxfer" || !final_report) {
            if (final_report) {
                std::fprintf(stderr, "%llu+%llu records in\n%llu+%llu records out\n", (unsigned long long)full_in, (unsigned long long)part_in,
                             (unsigned long long)full_out, (unsigned long long)part_out);
            }
        }
        if (status != "noxfer") {
            std::fprintf(stderr, "%llu bytes (%s, %s) copied, %.3f s, %s/s\n", (unsigned long long)bytes, human_size((double)bytes, true).c_str(),
                         human_size((double)bytes).c_str(), secs, human_size(secs > 0 ? bytes / secs : 0, true).c_str());
        }
    };
    int rc = 0;
    while (full_in + part_in < count) {
        if (g_sigusr1) {
            g_sigusr1 = 0;
            const std::string saved = status;
            status = "default";
            report(true);
            status = saved;
        }
        ssize_t n = ::read(in, ibuf.data(), ibuf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            err_errno("error reading '" + (in_path.empty() ? std::string("standard input") : in_path) + "'");
            if (!noerror) { rc = 1; break; }
            n = 0;
            std::fill(ibuf.begin(), ibuf.end(), 0);
            n = sync_pad ? static_cast<ssize_t>(ibuf.size()) : 0;
            if (n == 0) { ::lseek(in, static_cast<off_t>(ibs), SEEK_CUR); continue; }
        }
        if (n == 0) break;
        if (static_cast<uint64_t>(n) == ibs) ++full_in; else ++part_in;
        if (sync_pad && static_cast<uint64_t>(n) < ibs) {
            std::fill(ibuf.begin() + n, ibuf.end(), 0);
            n = static_cast<ssize_t>(ibs);
        }
        obuf.insert(obuf.end(), ibuf.begin(), ibuf.begin() + n);
        while (obuf.size() >= obs) {
            if (!write_all(outfd, obuf.data(), obs)) {
                err_errno("error writing '" + (out_path.empty() ? std::string("standard output") : out_path) + "'");
                rc = 1;
                break;
            }
            obuf.erase(obuf.begin(), obuf.begin() + obs);
            bytes += obs;
            ++full_out;
        }
        if (rc) break;
        if (status == "progress") report(false);
    }
    if (rc == 0 && !obuf.empty()) {
        if (!write_all(outfd, obuf.data(), obuf.size())) {
            err_errno("error writing output");
            rc = 1;
        } else {
            bytes += obuf.size();
            ++part_out;
        }
    }
    if (fsync_end) ::fsync(outfd);
    if (fdatasync_end) ::fdatasync(outfd);
    if (outfd != 1) ::close(outfd);
    if (in != 0) ::close(in);
    report(true);
    return rc;
}

} // namespace cu
