// Arch Linux / CachyOS specifics: distro detection, CPU ISA level, filesystem names, mount table
// and systemd-logind sessions (Arch has no utmp by default, so `who`/`users` fall back to logind).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cu::arch {

struct OsRelease {
    std::string id;         ///< "arch", "cachyos", "endeavouros", "manjaro" ...
    std::string id_like;    ///< "arch"
    std::string name;
    std::string pretty_name;
    std::string version_id;
    std::string build_id;
};

const OsRelease &os_release();
/// Arch itself or a derivative (ID or ID_LIKE contains "arch"): CachyOS, EndeavourOS, Manjaro, Garuda...
bool is_arch_family();
bool is_cachyos();
std::string distro_name();  ///< PRETTY_NAME or "GNU/Linux"

/// "x86-64-v1".."x86-64-v4" from /proc/cpuinfo (the levels CachyOS packages are built for), "" elsewhere.
std::string x86_level();

/// Filesystem name for a statfs f_type value ("btrfs", "zfs", "bcachefs", "ext4", ...).
std::string fs_type_name(uint64_t f_type);

struct Mount {
    std::string source;
    std::string target;
    std::string fstype;
    std::string root;        ///< root of the mount inside the filesystem (btrfs subvolume path)
    std::string options;
    uint64_t dev = 0;        ///< st_dev of the mount point (0 if it could not be read)
};
/// /proc/self/mountinfo (falls back to /proc/mounts)
const std::vector<Mount> &mounts();
/// Pseudo filesystems that `df` hides by default (proc, sysfs, cgroup2, devpts, ...)
bool is_pseudo_fs(const std::string &fstype);

struct Session {
    std::string user;
    uid_t uid = 0;
    std::string tty;        ///< "tty2", "pts/0" or "seat0" for graphical sessions
    std::string display;    ///< ":0" for graphical sessions
    std::string remote_host;
    std::string type;       ///< tty, x11, wayland, ...
    std::string state;      ///< active / online / closing
    int64_t since = 0;      ///< seconds since the epoch
    std::string id;
};
/// Sessions from systemd-logind (/run/systemd/sessions); empty when logind is not running.
std::vector<Session> logind_sessions();
bool logind_running();

/// CPUs this process may really use: sched affinity capped by the cgroup v2 cpu.max quota (containers).
int usable_cpus();

} // namespace cu::arch
