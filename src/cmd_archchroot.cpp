// arch-chroot: chroot with /proc /sys /dev /run /tmp and resolv.conf set up, like Arch's arch-install-scripts.
// The mounts live in a private mount namespace (unshare), so they vanish when the last process in the
// chroot exits: nothing to unmount and nothing left behind on the host if the shell is killed.
#include <getopt.h>
#include <pwd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "commands.hpp"
#include "common.hpp"

namespace cu {
namespace {

bool is_dir(const std::string &p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdir_p(const std::string &p)
{
    if (is_dir(p)) return true;
    const auto slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !mkdir_p(p.substr(0, slash))) return false;
    return ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

// mount `type` on root+target, creating the mount point first
bool do_mount(const std::string &root, const char *src, const char *target, const char *type,
              unsigned long flags, const char *data)
{
    const std::string dest = root + target;
    if (!mkdir_p(dest)) { err_errno("cannot create " + dest); return false; }
    if (::mount(src, dest.c_str(), type, flags, data) != 0) {
        err_errno(std::string("cannot mount ") + target);
        return false;
    }
    return true;
}

// where the chroot's /etc/resolv.conf really lives (it is often a symlink into /run)
std::string resolv_target(const std::string &root)
{
    const std::string link = root + "/etc/resolv.conf";
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
    if (n < 0) return "/etc/resolv.conf";
    buf[n] = '\0';
    std::string t = buf;
    if (t[0] != '/') t = "/etc/" + t;
    // collapse "/etc/../run/x" without touching the filesystem
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < t.size()) {
        const size_t j = t.find('/', i);
        const std::string seg = t.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string r;
    for (const std::string &p : parts) r += "/" + p;
    return r.empty() ? "/etc/resolv.conf" : r;
}

void bind_resolv_conf(const std::string &root)
{
    char host[PATH_MAX];
    if (!::realpath("/etc/resolv.conf", host)) return;   // host has none: leave the chroot's alone
    const std::string target = root + resolv_target(root);
    const auto slash = target.find_last_of('/');
    if (slash == std::string::npos || !mkdir_p(target.substr(0, slash))) return;
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0) {
        const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0) return;
        ::close(fd);
    }
    if (::mount(host, target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        err_errno("warning: cannot bind /etc/resolv.conf");
    }
}

} // namespace

int cmd_arch_chroot(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-u USER[:GROUP]] CHROOT-DIR [COMMAND [ARG]...]",
                            "Enter an Arch Linux chroot with /proc, /sys, /dev, /run, /tmp and resolv.conf set up.\n"
                            "The mounts are private to the chroot and disappear when it exits.\n"
                            "Runs /bin/bash when no COMMAND is given.")) return 0;
    std::string userspec;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "+u:")) != -1) {
        if (c == 'u') userspec = optarg;
        else { usage_error("invalid option"); return 1; }
    }
    if (optind >= argc) { usage_error("missing operand: chroot directory"); return 1; }
    if (::geteuid() != 0) { err("must be run as root"); return 1; }

    char resolved[PATH_MAX];
    if (!::realpath(argv[optind], resolved) || !is_dir(resolved)) {
        err(std::string("cannot use '") + argv[optind] + "' as a chroot: not a directory");
        return 1;
    }
    const std::string root = resolved;
    ++optind;

    if (::unshare(CLONE_NEWNS) != 0) { err_errno("cannot create a private mount namespace"); return 1; }
    // stop our mounts reaching the host
    if (::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) { err_errno("cannot make / private"); return 1; }
    // pacman and friends want the chroot to be a mount point
    if (::mount(root.c_str(), root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) { err_errno("cannot bind " + root); return 1; }

    const unsigned long NSD = MS_NOSUID | MS_NODEV, NSDE = MS_NOSUID | MS_NODEV | MS_NOEXEC;
    if (!do_mount(root, "proc", "/proc", "proc", NSDE, nullptr)) return 1;
    if (!do_mount(root, "sys", "/sys", "sysfs", NSDE | MS_RDONLY, nullptr)) return 1;
    if (!do_mount(root, "udev", "/dev", "devtmpfs", MS_NOSUID, "mode=0755")) return 1;
    if (!do_mount(root, "devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5,newinstance,ptmxmode=0666")) return 1;
    if (!do_mount(root, "shm", "/dev/shm", "tmpfs", NSD, "mode=1777")) return 1;
    if (!do_mount(root, "run", "/run", "tmpfs", NSD, "mode=0755")) return 1;
    if (!do_mount(root, "tmp", "/tmp", "tmpfs", NSD, "mode=1777,strictatime")) return 1;
    bind_resolv_conf(root);   // after /run is mounted: the symlink target is usually under /run

    // finish with the regular chroot command (user switch, exec)
    std::vector<std::string> store = {"chroot"};
    if (!userspec.empty()) { store.push_back("-u"); store.push_back(userspec); }
    store.push_back(root);
    if (optind < argc) { for (int i = optind; i < argc; ++i) store.push_back(argv[i]); }
    else store.push_back("/bin/bash");
    std::vector<char *> av;
    for (std::string &s : store) av.push_back(s.data());
    av.push_back(nullptr);
    return cmd_chroot(static_cast<int>(store.size()), av.data());
}

namespace {

const char *const kSaved = "/etc/claude-coreutils/archroot";

bool looks_like_root(const std::string &d) { return is_dir(d) && ::access((d + "/etc/os-release").c_str(), F_OK) == 0; }

std::string expand_tilde(std::string p)
{
    if (p.empty() || p[0] != '~') return p;
    const char *sudo_user = std::getenv("SUDO_USER");
    const passwd *pw = sudo_user ? ::getpwnam(sudo_user) : ::getpwuid(::geteuid());
    return pw ? std::string(pw->pw_dir) + p.substr(1) : p;
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

bool ask(const char *prompt, std::string &answer)
{
    std::fputs(prompt, stderr);
    std::fflush(stderr);
    char buf[PATH_MAX];
    if (!std::fgets(buf, sizeof buf, stdin)) return false;
    answer = trim(buf);
    return true;
}

} // namespace

int cmd_archroot(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-r DIR] [COMMAND [ARG]...]",
                            "Enter your Arch chroot. Looks in -r DIR, $ARCH_ROOT, the saved location, then ./arch-root;\n"
                            "if none is found it asks where the chroot is and offers to remember it.")) return 0;
    std::string dir;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "+r:")) != -1) {
        if (c == 'r') dir = optarg;
        else { usage_error("invalid option"); return 1; }
    }
    if (::geteuid() != 0) { err("must be run as root"); return 1; }

    std::vector<std::string> cand;
    if (!dir.empty()) cand.push_back(expand_tilde(dir));
    if (const char *e = std::getenv("ARCH_ROOT"); e && *e) cand.push_back(expand_tilde(e));
    std::string saved;
    if (read_file(kSaved, saved)) { saved = trim(saved); if (!saved.empty()) cand.push_back(saved); }
    cand.push_back("./arch-root");

    std::string found;
    for (const std::string &d : cand) if (looks_like_root(d)) { found = d; break; }

    if (found.empty()) {
        if (!dir.empty()) { err("'" + dir + "' is not an Arch root (no etc/os-release)"); return 1; }
        if (!::isatty(0)) { err("Arch root not found (use -r DIR or set ARCH_ROOT)"); return 1; }
        std::fprintf(stderr, "archroot: no Arch root found.\n");
        for (int tries = 0; tries < 3 && found.empty(); ++tries) {
            std::string ans;
            if (!ask("Where is your Arch root folder? (empty to cancel): ", ans) || ans.empty()) return 1;
            ans = expand_tilde(ans);
            if (looks_like_root(ans)) found = ans;
            else std::fprintf(stderr, "archroot: '%s' is not an Arch root (no etc/os-release).\n", ans.c_str());
        }
        if (found.empty()) return 1;
        std::string yn;
        if (ask("Remember this location? [Y/n]: ", yn) && (yn.empty() || yn[0] == 'y' || yn[0] == 'Y')) {
            char abs[PATH_MAX];
            const std::string keep = ::realpath(found.c_str(), abs) ? abs : found;
            if (mkdir_p("/etc/claude-coreutils")) {
                const int fd = ::open(kSaved, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { write_all(fd, keep + "\n"); ::close(fd); }
                else err_errno(std::string("cannot save to ") + kSaved);
            }
        }
    }

    std::vector<std::string> store = {"arch-chroot", found};
    for (int i = optind; i < argc; ++i) store.push_back(argv[i]);
    std::vector<char *> av;
    for (std::string &s : store) av.push_back(s.data());
    av.push_back(nullptr);
    return cmd_arch_chroot(static_cast<int>(store.size()), av.data());
}

} // namespace cu
