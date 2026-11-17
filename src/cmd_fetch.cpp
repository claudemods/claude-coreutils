// fetch: a small neofetch/fastfetch style system summary (Arch / CachyOS aware).
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

namespace cu {
namespace {

std::string slurp(const std::string &path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string basename_of(const std::string &p)
{
    const auto i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string getenv_s(const char *n)
{
    const char *v = std::getenv(n);
    return v ? v : "";
}

std::string fmt_uptime(long up)
{
    const long d = up / 86400, h = (up % 86400) / 3600, m = (up % 3600) / 60;
    std::string s;
    if (d) s += std::to_string(d) + (d == 1 ? " day, " : " days, ");
    if (h) s += std::to_string(h) + (h == 1 ? " hour, " : " hours, ");
    s += std::to_string(m) + (m == 1 ? " min" : " mins");
    return s;
}

std::string gib(double bytes)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    return b;
}

std::string pct(double used, double total)
{
    char b[16];
    std::snprintf(b, sizeof b, " (%d%%)", total > 0 ? static_cast<int>(used * 100 / total + 0.5) : 0);
    return b;
}

long count_dirs(const std::string &path)
{
    long n = 0;
    if (DIR *d = opendir(path.c_str())) {
        while (const dirent *e = readdir(d)) {
            if (e->d_name[0] != '.') ++n;
        }
        closedir(d);
    }
    return n;
}

std::string cpu_model()
{
    std::ifstream f("/proc/cpuinfo");
    std::string line, model;
    long threads = 0;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0 && model.empty()) {
            const auto c = line.find(':');
            if (c != std::string::npos && c + 2 <= line.size()) model = line.substr(c + 2);
        }
        if (line.rfind("processor", 0) == 0) ++threads;
    }
    for (const char *junk : {"(R)", "(TM)", "(tm)", " 6-Core Processor", " 8-Core Processor", " 12-Core Processor",
                             " 16-Core Processor", " CPU"}) {
        for (size_t p; (p = model.find(junk)) != std::string::npos;) model.erase(p, std::strlen(junk));
    }
    for (size_t p; (p = model.find("  ")) != std::string::npos;) model.erase(p, 1);
    std::string out = model.empty() ? "unknown" : model;
    if (threads) out += " (" + std::to_string(threads) + ")";
    const std::string khz = slurp("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!khz.empty()) {
        char b[24];
        std::snprintf(b, sizeof b, " @ %.2f GHz", std::atof(khz.c_str()) / 1e6);
        out += b;
    }
    return out;
}

// "10de" + "1f08" -> "NVIDIA GeForce RTX 2060 Rev. A" using the system pci.ids
std::string pci_name(const std::string &vendor, const std::string &device)
{
    std::string vname, dname;
    for (const char *path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"}) {
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        bool in_vendor = false;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] != '\t') {
                if (in_vendor) break;
                if (line.compare(0, 4, vendor) == 0 && line.size() > 6) {
                    in_vendor = true;
                    vname = line.substr(6);
                }
            } else if (in_vendor && line.size() > 7 && line[1] != '\t' && line.compare(1, 4, device) == 0) {
                dname = line.substr(7);
                break;
            }
        }
        if (in_vendor) break;
    }
    if (vname.find("NVIDIA") != std::string::npos) vname = "NVIDIA";
    else if (vname.find("AMD") != std::string::npos || vname.find("ATI") != std::string::npos) vname = "AMD";
    else if (vname.find("Intel") != std::string::npos) vname = "Intel";
    const auto a = dname.find('['), b = dname.rfind(']');
    if (a != std::string::npos && b != std::string::npos && b > a) dname = dname.substr(a + 1, b - a - 1);
    if (dname.empty()) return vname.empty() ? "" : vname + " (" + device + ")";
    return vname.empty() ? dname : vname + " " + dname;
}

std::vector<std::string> gpus()
{
    std::vector<std::string> g;
    if (DIR *d = opendir("/sys/class/drm")) {
        while (const dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("card", 0) != 0 || n.find('-') != std::string::npos) continue;
            const std::string v = slurp("/sys/class/drm/" + n + "/device/vendor");
            const std::string dv = slurp("/sys/class/drm/" + n + "/device/device");
            if (v.size() < 6 || dv.size() < 6) continue;
            const std::string name = pci_name(v.substr(2), dv.substr(2));
            if (!name.empty()) g.push_back(name);
        }
        closedir(d);
    }
    return g;
}

// walk up the process tree past shells and multiplexers to find the terminal emulator
std::string terminal()
{
    pid_t pid = getppid();
    static const char *skip[] = {"bash", "zsh", "fish", "sh", "dash", "tmux: server", "tmux", "screen", "sudo", "su"};
    for (int i = 0; i < 6 && pid > 1; ++i) {
        const std::string comm = slurp("/proc/" + std::to_string(pid) + "/comm");
        bool skipped = false;
        for (const char *s : skip) if (comm == s) skipped = true;
        if (!skipped && !comm.empty()) return comm;
        const std::string stat = slurp("/proc/" + std::to_string(pid) + "/stat");
        const auto rp = stat.rfind(')');
        int ppid = 0;
        if (rp == std::string::npos || std::sscanf(stat.c_str() + rp + 2, "%*c %d", &ppid) != 1) break;
        pid = ppid;
    }
    const std::string t = getenv_s("TERM_PROGRAM");
    return t.empty() ? getenv_s("TERM") : t;
}

struct Line {
    std::string key, value;
};

} // namespace

int cmd_fetch(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[--no-logo] [--no-color]",
                            "Print a system summary: OS, kernel, uptime, packages, shell, desktop, CPU, GPU, memory, disk.")) return 0;
    bool logo = true, color = ::isatty(1) != 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-logo")) logo = false;
        else if (!std::strcmp(argv[i], "--no-color")) color = false;
        else if (!std::strcmp(argv[i], "--color")) color = true;
        else { usage_error(std::string("unrecognized option '") + argv[i] + "'"); return 1; }
    }
    const char *C1 = color ? "\033[1;36m" : "";
    const char *C2 = color ? "\033[0;36m" : "";
    const char *RS = color ? "\033[0m" : "";

    utsname un{};
    ::uname(&un);
    char host[256] = "";
    ::gethostname(host, sizeof host - 1);
    struct sysinfo si{};
    ::sysinfo(&si);

    std::vector<Line> L;
    const std::string title = user_name(::geteuid()) + "@" + host;
    L.push_back({"", title});
    L.push_back({"", std::string(title.size(), '-')});
    L.push_back({"OS", arch::distro_name() + " " + un.machine});
    L.push_back({"Kernel", std::string(un.sysname) + " " + un.release});
    L.push_back({"Uptime", fmt_uptime(si.uptime)});
    if (const long pk = count_dirs("/var/lib/pacman/local"); pk > 0) L.push_back({"Packages", std::to_string(pk) + " (pacman)"});
    L.push_back({"Shell", basename_of(getenv_s("SHELL"))});
    {
        const std::string de = getenv_s("XDG_CURRENT_DESKTOP"), st = getenv_s("XDG_SESSION_TYPE");
        if (!de.empty()) L.push_back({"Desktop", de + (st.empty() ? "" : " (" + st + ")")});
    }
    L.push_back({"Terminal", terminal()});
    L.push_back({"CPU", cpu_model()});
    const std::string lvl = arch::x86_level();
    if (!lvl.empty()) L.push_back({"CPU level", lvl});
    for (const std::string &g : gpus()) L.push_back({"GPU", g});
    {
        const double unit = si.mem_unit ? si.mem_unit : 1;
        double total = si.totalram * unit, avail = (si.freeram + si.bufferram) * unit;
        std::ifstream f("/proc/meminfo");   // MemAvailable counts reclaimable cache; sysinfo() does not
        std::string line;
        while (std::getline(f, line)) {
            unsigned long long kb;
            if (std::sscanf(line.c_str(), "MemTotal: %llu kB", &kb) == 1) total = kb * 1024.0;
            if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) avail = kb * 1024.0;
        }
        L.push_back({"Memory", gib(total - avail) + " / " + gib(total) + pct(total - avail, total)});
        const double st = si.totalswap * unit, su = (si.totalswap - si.freeswap) * unit;
        if (st > 0) L.push_back({"Swap", gib(su) + " / " + gib(st) + pct(su, st)});
    }
    {
        struct statvfs sv{};
        if (::statvfs("/", &sv) == 0) {
            const double total = static_cast<double>(sv.f_blocks) * sv.f_frsize;
            const double used = total - static_cast<double>(sv.f_bfree) * sv.f_frsize;
            std::string fs;
            for (const auto &m : arch::mounts()) if (m.target == "/") fs = m.fstype;
            L.push_back({"Disk (/)", gib(used) + " / " + gib(total) + pct(used, total) + (fs.empty() ? "" : " - " + fs)});
        }
    }
    if (const std::string loc = getenv_s("LANG"); !loc.empty()) L.push_back({"Locale", loc});

    static const char *LOGO[] = {
        "       /\\       ",
        "      /  \\      ",
        "     /    \\     ",
        "    /  ,,  \\    ",
        "   /  |  |  \\   ",
        "  /_-''  ''-_\\  ",
    };
    const size_t nlogo = sizeof LOGO / sizeof *LOGO;
    const size_t rows = L.size() > nlogo ? L.size() : nlogo;
    for (size_t i = 0; i < rows; ++i) {
        std::string row;
        if (logo) {
            char pad[40];
            std::snprintf(pad, sizeof pad, "%-18s", i < nlogo ? LOGO[i] : "");
            row += std::string(C2) + pad + RS + "  ";
        }
        if (i < L.size()) {
            const Line &ln = L[i];
            if (ln.key.empty()) row += std::string(i == 0 ? C1 : "") + ln.value + RS;
            else row += std::string(C1) + ln.key + RS + ": " + ln.value;
        }
        out(row + "\n");
    }
    if (color) {
        out(std::string("\n") + (logo ? std::string(20, ' ') : ""));
        for (int c = 40; c <= 47; ++c) out("\033[" + std::to_string(c) + "m   ");
        out(std::string(RS) + "\n");
    }
    return 0;
}

} // namespace cu
