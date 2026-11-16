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
#include "arch.hpp"

#include "common.hpp"

#include <dirent.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace cu::arch {

namespace {

std::string trim(const std::string &s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string unquote(std::string v)
{
    v = trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

} // namespace

const OsRelease &os_release()
{
    static const OsRelease rel = [] {
        OsRelease r;
        std::ifstream f("/etc/os-release");
        if (!f) {
            f.open("/usr/lib/os-release");
        }
        std::string line;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#') {
                continue;
            }
            const std::string k = line.substr(0, eq);
            const std::string v = unquote(line.substr(eq + 1));
            if (k == "ID") r.id = v;
            else if (k == "ID_LIKE") r.id_like = v;
            else if (k == "NAME") r.name = v;
            else if (k == "PRETTY_NAME") r.pretty_name = v;
            else if (k == "VERSION_ID") r.version_id = v;
            else if (k == "BUILD_ID") r.build_id = v;
        }
        return r;
    }();
    return rel;
}

bool is_arch_family()
{
    const OsRelease &r = os_release();
    if (r.id == "arch" || r.id == "cachyos") {
        return true;
    }
    std::istringstream like(r.id_like);
    std::string w;
    while (like >> w) {
        if (w == "arch") {
            return true;
        }
    }
    return false;
}

bool is_cachyos()
{
    return os_release().id == "cachyos";
}

std::string distro_name()
{
    const OsRelease &r = os_release();
    if (!r.pretty_name.empty()) {
        return r.pretty_name;
    }
    return r.name.empty() ? "GNU/Linux" : r.name;
}

std::string x86_level()
{
#if defined(__x86_64__) || defined(__i386__)
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    std::set<std::string> flags;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) == 0) {
            const size_t colon = line.find(':');
            std::istringstream ss(line.substr(colon + 1));
            std::string w;
            while (ss >> w) {
                flags.insert(w);
            }
            break;
        }
    }
    auto has = [&](std::initializer_list<const char *> l) {
        for (const char *x : l) {
            if (!flags.count(x)) {
                return false;
            }
        }
        return true;
    };
    int level = 1;
    if (has({"cx16", "lahf_lm", "popcnt", "sse4_1", "sse4_2", "ssse3"})) {
        level = 2;
        if (has({"avx", "avx2", "bmi1", "bmi2", "f16c", "fma", "abm", "movbe", "xsave"})) {
            level = 3;
            if (has({"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"})) {
                level = 4;
            }
        }
    }
    return "x86-64-v" + std::to_string(level);
#else
    return {};
#endif
}

std::string fs_type_name(uint64_t t)
{
    struct E {
        uint64_t magic;
        const char *name;
    };
    static const E table[] = {
        {0xEF53, "ext4"},          {0x9123683E, "btrfs"},     {0x58465342, "xfs"},
        {0x2FC12FC1, "zfs"},       {0xF2F52010, "f2fs"},      {0xCA451A4E, "bcachefs"},
        {0x01021994, "tmpfs"},     {0x794C7630, "overlay"},   {0x6969, "nfs"},
        {0x7366746E, "ntfs3"},     {0x65735546, "fuseblk"},   {0x4D44, "vfat"},
        {0x2011BAB0, "exfat"},     {0x73717368, "squashfs"},  {0xE0F5E1E2, "erofs"},
        {0xFF534D42, "cifs"},      {0x9FA0, "proc"},          {0x62656572, "sysfs"},
        {0x63677270, "cgroup2"},   {0x27E0EB, "cgroup"},      {0xDE5E81E4, "efivarfs"},
        {0x1CD1, "devpts"},        {0x858458F6, "ramfs"},     {0x9660, "iso9660"},
        {0x15013346, "udf"},       {0x6165676C, "pstore"},    {0x74726163, "tracefs"},
        {0xCAFE4A11, "bpf"},       {0x73636673, "securityfs"},{0x1373, "devfs"},
        {0x19800202, "mqueue"},    {0x958458F6, "hugetlbfs"}, {0x65735543, "fuse"},
        {0x4244, "hfs"},           {0x482B, "hfsplus"},       {0x3153464A, "jfs"},
        {0x52654973, "reiserfs"},  {0x517B, "smb"},           {0xF15F, "ecryptfs"},
    };
    for (const E &e : table) {
        if (e.magic == t) {
            return e.name;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "UNKNOWN (0x%llx)", static_cast<unsigned long long>(t));
    return buf;
}

static std::string unescape_mount(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() + 0 && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            r += static_cast<char>(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}

const std::vector<Mount> &mounts()
{
    static const std::vector<Mount> list = [] {
        std::vector<Mount> v;
        std::ifstream f("/proc/self/mountinfo");
        std::string line;
        while (std::getline(f, line)) {
            // 36 35 98:0 /root /mnt opts optional - fstype source superopts
            std::istringstream ss(line);
            std::string id, parent, majmin, root, target, opts, tok;
            ss >> id >> parent >> majmin >> root >> target >> opts;
            while (ss >> tok && tok != "-") {
            }
            Mount m;
            std::string superopts;
            ss >> m.fstype >> m.source >> superopts;
            m.root = unescape_mount(root);
            m.target = unescape_mount(target);
            m.source = unescape_mount(m.source);
            m.options = opts + "," + superopts;
            struct stat st;
            if (::stat(m.target.c_str(), &st) == 0) {
                m.dev = st.st_dev;
            }
            v.push_back(std::move(m));
        }
        if (v.empty()) {
            std::ifstream mf("/proc/mounts");
            while (std::getline(mf, line)) {
                std::istringstream ss(line);
                Mount m;
                ss >> m.source >> m.target >> m.fstype >> m.options;
                m.target = unescape_mount(m.target);
                m.root = "/";
                struct stat st;
                if (::stat(m.target.c_str(), &st) == 0) {
                    m.dev = st.st_dev;
                }
                v.push_back(std::move(m));
            }
        }
        return v;
    }();
    return list;
}

bool is_pseudo_fs(const std::string &t)
{
    static const std::set<std::string> pseudo = {
        "proc", "sysfs", "devpts", "devtmpfs", "cgroup", "cgroup2", "securityfs", "debugfs", "tracefs",
        "configfs", "fusectl", "mqueue", "pstore", "bpf", "autofs", "binfmt_misc", "hugetlbfs",
        "efivarfs", "rpc_pipefs", "nsfs", "selinuxfs", "overlay-ignored", "fuse.gvfsd-fuse", "fuse.portal"};
    return pseudo.count(t) > 0;
}

// ---------------------------------------------------------------------------------------------
// systemd-logind
// ---------------------------------------------------------------------------------------------

bool logind_running()
{
    struct stat st;
    return ::stat("/run/systemd/sessions", &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<Session> logind_sessions()
{
    std::vector<Session> out;
    DIR *d = ::opendir("/run/systemd/sessions");
    if (!d) {
        return out;
    }
    while (const dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || name.find('.') != std::string::npos) {
            continue; // skip "*.ref" and hidden files
        }
        std::ifstream f("/run/systemd/sessions/" + name);
        Session s;
        s.id = name;
        std::string line;
        bool has_class_user = false;
        std::string seat;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if (k == "UID") s.uid = static_cast<uid_t>(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "USER") s.user = v;
            else if (k == "TTY") s.tty = v;
            else if (k == "DISPLAY") s.display = v;
            else if (k == "REMOTE_HOST") s.remote_host = v;
            else if (k == "TYPE") s.type = v;
            else if (k == "STATE") s.state = v;
            else if (k == "SEAT") seat = v;
            else if (k == "CLASS") has_class_user = (v == "user");
            else if (k == "REALTIME") s.since = static_cast<int64_t>(std::strtoull(v.c_str(), nullptr, 10) / 1000000ULL);
        }
        if (!has_class_user || s.user.empty()) {
            continue; // greeter / background sessions are not logins
        }
        if (s.tty.empty()) {
            s.tty = seat.empty() ? (s.display.empty() ? "?" : s.display) : seat;
        }
        out.push_back(std::move(s));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const Session &a, const Session &b) { return a.since < b.since; });
    return out;
}

// ---------------------------------------------------------------------------------------------
// CPUs
// ---------------------------------------------------------------------------------------------

int usable_cpus()
{
    int n = 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        n = CPU_COUNT(&set);
    }
    if (n <= 0) {
        n = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    }
    if (n <= 0) {
        n = 1;
    }
    // cgroup v2 quota: "max 100000" or "200000 100000" (quota period)
    std::ifstream cg("/sys/fs/cgroup/cpu.max");
    std::string quota;
    double period = 0;
    if (cg >> quota >> period && quota != "max" && period > 0) {
        const double q = std::strtod(quota.c_str(), nullptr);
        if (q > 0) {
            n = std::min(n, std::max(1, static_cast<int>(std::ceil(q / period))));
        }
    }
    return n;
}

} // namespace cu::arch
