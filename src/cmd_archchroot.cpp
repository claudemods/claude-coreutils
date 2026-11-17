// claude-chroot: chroot with /proc /sys /dev /run /tmp and resolv.conf set up, like Arch's arch-install-scripts.
// The mounts live in a private mount namespace (unshare), so they vanish when the last process in the
// chroot exits: nothing to unmount and nothing left behind on the host if the shell is killed.
#include <getopt.h>
#include <pwd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

int cmd_claude_chroot(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-u USER[:GROUP]] [CHROOT-DIR [COMMAND [ARG]...]]",
                            "Enter an Arch Linux chroot with /proc, /sys, /dev, /run, /tmp and resolv.conf set up.\n"
                            "The mounts are private to the chroot and disappear when it exits.\n"
                            "Runs /bin/bash when no COMMAND is given. Without CHROOT-DIR it behaves like archroot:\n"
                            "it looks for your Arch root and asks where it is if it cannot find it.")) return 0;
    std::string userspec;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "+u:")) != -1) {
        if (c == 'u') userspec = optarg;
        else { usage_error("invalid option"); return 1; }
    }
    if (optind >= argc) {
        // no folder given: find it (or ask for it) the way archroot does
        char name[] = "archroot";
        char *av[] = {name, nullptr};
        return cmd_archroot(1, av);
    }
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

// run COMMAND inside the chroot (own mount namespace each time) and wait for it
int run_in(const std::string &root, const std::vector<std::string> &cmd)
{
    flush_out();
    const pid_t pid = ::fork();
    if (pid < 0) { err_errno("fork"); return -1; }
    if (pid == 0) {
        std::vector<std::string> store = {"claude-chroot", root};
        store.insert(store.end(), cmd.begin(), cmd.end());
        std::vector<char *> av;
        for (std::string &x : store) av.push_back(x.data());
        av.push_back(nullptr);
        ::_exit(cmd_claude_chroot(static_cast<int>(store.size()), av.data()));
    }
    int st = 0;
    if (::waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void say(const std::string &m) { std::fprintf(stderr, "==> %s\n", m.c_str()); }

// is a package installed? (looks for NAME-<digit>... in the chroot's pacman database)
bool has_pkg(const std::string &root, const std::string &name)
{
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(root + "/var/lib/pacman/local", ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > name.size() + 1 && n.compare(0, name.size(), name) == 0 && n[name.size()] == '-' &&
            std::isdigit(static_cast<unsigned char>(n[name.size() + 1])))
            return true;
    }
    return false;
}

// create a new Arch system in `dir` with pacstrap (from arch-install-scripts)
bool create_root(const std::string &dir)
{
    say("Creating a new Arch system in " + dir + " (pacstrap -K base openssl)");
    if (!mkdir_p(dir)) { err_errno("cannot create " + dir); return false; }   // pacstrap needs it to exist
    flush_out();
    const pid_t pid = ::fork();
    if (pid < 0) { err_errno("fork"); return false; }
    if (pid == 0) {
        ::execlp("pacstrap", "pacstrap", "-K", dir.c_str(), "base", "openssl", static_cast<char *>(nullptr));
        std::fprintf(stderr, "archroot: cannot run pacstrap (install arch-install-scripts): %s\n", std::strerror(errno));
        ::_exit(127);
    }
    int st = 0;
    if (::waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) { err("pacstrap failed"); return false; }
    return looks_like_root(dir);
}

// Install THIS binary as the coreutils of the Arch system in `root`.
//  1 copy the binary to /usr/bin/claude-coreutils and check that it runs inside the chroot
//  2 stage one symlink per command in /usr/local/bin (first in PATH: keeps the system working)
//  3 pin coreutils in pacman.conf (IgnorePkg) and remove the GNU package (pacman -Rdd)
//  4 link every command in /usr/bin over the GNU files, drop the staging links, verify
bool install_coreutils(const std::string &root)
{
    namespace fs = std::filesystem;
    const std::string self_bin = "/usr/bin/claude-coreutils";
    char me[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", me, sizeof me - 1);
    if (n < 0) { err("cannot find my own path"); return false; }
    me[n] = '\0';
    if (!fs::exists(root + "/usr/bin/pacman")) { err(root + " has no pacman: create it with pacstrap first"); return false; }

    say("Copying claude-coreutils to " + root + self_bin);
    std::error_code ec;
    const std::string tmp = root + "/usr/bin/.claude-coreutils.new";
    fs::copy_file(me, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { err("cannot copy the binary: " + ec.message()); return false; }
    ::chmod(tmp.c_str(), 0755);
    fs::rename(tmp, root + self_bin, ec);
    if (ec) { err("cannot install the binary: " + ec.message()); return false; }

    say("Checking that it runs inside the chroot");
    if (run_in(root, {self_bin, "--version"}) != 0) {
        err("the binary does not run in the chroot (missing libraries such as openssl or gcc-libs?); nothing was removed");
        return false;
    }

    say("Staging a symlink per command in /usr/local/bin (first in PATH, keeps the system working during removal)");
    if (run_in(root, {self_bin, "--install", "/usr/local/bin", "--force"}) != 0) { err("could not create the staging links"); return false; }

    if (has_pkg(root, "coreutils")) {
        say("Pinning coreutils in pacman.conf so upgrades do not bring it back");
        std::string conf;
        const std::string cf = root + "/etc/pacman.conf";
        if (read_file(cf, conf) && conf.find("IgnorePkg = coreutils") == std::string::npos) {
            const size_t o = conf.find("[options]");
            if (o != std::string::npos) {
                conf.insert(conf.find('\n', o) + 1, "IgnorePkg = coreutils\n");
                const int fd = ::open(cf.c_str(), O_WRONLY | O_TRUNC);
                if (fd >= 0) { write_all(fd, conf); ::close(fd); }
            }
        }
        say("Removing the GNU coreutils package (pacman -Rdd: other packages depend on it)");
        if (run_in(root, {"pacman", "-Rdd", "--noconfirm", "coreutils"}) != 0) { err("pacman could not remove coreutils"); return false; }
    } else {
        say("GNU coreutils is not installed in the chroot, skipping removal");
    }

    say("Replacing the GNU files in /usr/bin with symlinks to claude-coreutils");
    if (run_in(root, {self_bin, "--install", "/usr/bin", "--force"}) != 0) { err("could not link the commands into /usr/bin"); return false; }
    run_in(root, {self_bin, "--uninstall", "/usr/local/bin"});

    say("Verifying");
    bool ok = !has_pkg(root, "coreutils");
    if (!ok) err("coreutils is still installed");
    for (const char *cmd : {"ls", "cp", "mv", "rm", "cat", "mkdir", "env", "sha256sum"}) {
        const std::string l = root + "/usr/bin/" + cmd;
        if (!fs::is_symlink(l, ec) || fs::read_symlink(l, ec) != self_bin) { err(std::string("/usr/bin/") + cmd + " is not a link to claude-coreutils"); ok = false; }
    }
    return ok;
}

} // namespace

int cmd_archroot(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-i] [-r DIR] [COMMAND [ARG]...]",
                            "Enter your Arch chroot. Looks in -r DIR, $ARCH_ROOT, the saved location, then ./arch-root;\n"
                            "if none is found it asks where the chroot is and offers to remember it.\n"
                            "-i, --install  first install this binary as the chroot's coreutils (replaces the GNU package),\n"
                            "               then enter the chroot.")) return 0;
    std::string dir;
    bool do_install = false;
    int c;
    optind = 1;
    static const option lo[] = {{"install", no_argument, nullptr, 'i'}, {"root", required_argument, nullptr, 'r'}, {nullptr, 0, nullptr, 0}};
    while ((c = getopt_long(argc, argv, "+ir:", lo, nullptr)) != -1) {
        if (c == 'r') dir = optarg;
        else if (c == 'i') do_install = true;
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

    char cwdbuf[PATH_MAX];
    const std::string cwd = ::getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : ".";
    bool created = false;

    if (found.empty()) {
        if (!::isatty(0)) { err("Arch root not found (use -r DIR or set ARCH_ROOT)"); return 1; }
        // offer to make the folder: the -r DIR that does not exist yet, or ./arch-root in the current directory
        std::string target = cwd + "/arch-root";
        if (!dir.empty()) target = dir[0] == '/' ? dir : cwd + "/" + dir;
        std::fprintf(stderr, "archroot: no Arch root found.\n");
        std::string yn;
        if (!ask(("Create a new Arch system in " + target + "? [Y/n]: ").c_str(), yn)) return 1;
        if (yn.empty() || yn[0] == 'y' || yn[0] == 'Y') {
            if (!create_root(target)) return 1;
            found = target;
            created = true;
        }
        for (int tries = 0; found.empty() && tries < 3; ++tries) {
            std::string ans;
            if (!ask("Where is your Arch root folder? (empty to cancel): ", ans) || ans.empty()) return 1;
            ans = expand_tilde(ans);
            if (looks_like_root(ans)) found = ans;
            else std::fprintf(stderr, "archroot: '%s' is not an Arch root (no etc/os-release).\n", ans.c_str());
        }
        if (found.empty()) return 1;
        std::string rem;
        if (ask("Remember this location? [Y/n]: ", rem) && (rem.empty() || rem[0] == 'y' || rem[0] == 'Y')) {
            char abs[PATH_MAX];
            const std::string keep = ::realpath(found.c_str(), abs) ? abs : found;
            if (mkdir_p("/etc/claude-coreutils")) {
                const int fd = ::open(kSaved, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { write_all(fd, keep + "\n"); ::close(fd); }
                else err_errno(std::string("cannot save to ") + kSaved);
            }
        }
    }
    if (created && !do_install) say("Tip: run with -i to install claude-coreutils as this system's coreutils");

    if (do_install && !install_coreutils(found)) return 1;
    if (do_install) say("Done: the chroot now runs claude-coreutils instead of GNU coreutils (stdbuf will not work: it needs the GNU libstdbuf.so)");

    std::vector<std::string> store = {"claude-chroot", found};
    for (int i = optind; i < argc; ++i) store.push_back(argv[i]);
    std::vector<char *> av;
    for (std::string &s : store) av.push_back(s.data());
    av.push_back(nullptr);
    return cmd_claude_chroot(static_cast<int>(store.size()), av.data());
}

} // namespace cu
