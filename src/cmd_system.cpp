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
// System / process / shell-script utilities: arch, date, echo, env, expr, factor, true/false, id, groups, hostid,
// hostname, kill, logname, nice, nohup, nproc, pinky, printenv, seq, sleep, test/[, timeout, tty, uname, uptime,
// users, who, whoami, yes.
#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <regex>
#include <set>

extern char **environ;

namespace cu {

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

} // namespace

// ================================================================================================
// true / false / yes / echo / sleep / whoami / logname / tty / hostid
// ================================================================================================
int cmd_true(int argc, char **argv)
{
    if (argc == 2) handle_help_version(argc, argv, "", "Exit with a status code indicating success.");
    return 0;
}

int cmd_false(int argc, char **argv)
{
    if (argc == 2 && handle_help_version(argc, argv, "", "Exit with a status code indicating failure.")) return 1;
    return 1;
}

int cmd_yes(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[STRING]...", "Repeatedly output a line with all specified STRING(s), or 'y'.")) return 0;
    std::string line;
    if (argc <= 1) line = "y";
    else for (int i = 1; i < argc; ++i) { if (i > 1) line += ' '; line += argv[i]; }
    line += '\n';
    std::string buf;
    while (buf.size() < 65536) buf += line;
    ::signal(SIGPIPE, SIG_DFL);
    for (;;) {
        if (!write_all(1, buf)) { err_errno("standard output"); return 1; }
    }
}

int cmd_echo(int argc, char **argv)
{
    bool nl = true, esc = false;
    int i = 1;
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "--version") == 0)) {
        handle_help_version(argc, argv, "[SHORT-OPTION]... [STRING]...", "Echo the STRING(s) to standard output.");
        return 0;
    }
    for (; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        bool ok = true;
        for (const char *p = a + 1; *p; ++p) if (!std::strchr("neE", *p)) ok = false;
        if (!ok) break;
        for (const char *p = a + 1; *p; ++p) { if (*p == 'n') nl = false; else if (*p == 'e') esc = true; else esc = false; }
    }
    std::string o;
    for (; i < argc; ++i) {
        if (!o.empty() || (i > 1 && !o.empty())) {}
        std::string part = argv[i];
        bool stop = false;
        if (esc) part = unescape(part, &stop);
        o += part;
        if (stop) { nl = false; out(o); return 0; }
        if (i + 1 < argc) o += ' ';
    }
    if (nl) o += '\n';
    out(o);
    return 0;
}

int cmd_sleep(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "NUMBER[SUFFIX]...", "Pause for NUMBER seconds. SUFFIX may be 's' (default), 'm', 'h' or 'd'.")) return 0;
    if (argc < 2) { usage_error("missing operand"); return 1; }
    double total = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        double mult = 1;
        if (!a.empty() && std::strchr("smhd", a.back())) {
            mult = a.back() == 'm' ? 60 : a.back() == 'h' ? 3600 : a.back() == 'd' ? 86400 : 1;
            a.pop_back();
        }
        double v;
        if (!parse_double(a, v) || v < 0) { err(std::string("invalid time interval '") + argv[i] + "'"); return 1; }
        total += v * mult;
    }
    timespec ts;
    ts.tv_sec = static_cast<time_t>(total);
    ts.tv_nsec = static_cast<long>((total - std::floor(total)) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    return 0;
}

int cmd_whoami(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print the user name associated with the current effective user ID.")) return 0;
    out(user_name(geteuid()) + "\n");
    return 0;
}

int cmd_logname(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]", "Print the name of the current user.")) return 0;
    char buf[256];
    if (getlogin_r(buf, sizeof buf) == 0 && buf[0]) { out(std::string(buf) + "\n"); return 0; }
    // Arch has no utmp: use the logind session of this uid, then $LOGNAME
    for (const auto &s : arch::logind_sessions()) {
        if (s.uid == getuid()) { out(s.user + "\n"); return 0; }
    }
    if (const char *l = std::getenv("LOGNAME")) { out(std::string(l) + "\n"); return 0; }
    err("no login name");
    return 1;
}

int cmd_tty(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print the file name of the terminal connected to standard input.")) return 0;
    bool silent = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "-s") || !std::strcmp(argv[i], "--silent") || !std::strcmp(argv[i], "--quiet")) silent = true;
    const char *n = ttyname(0);
    if (!n) { if (!silent) out("not a tty\n"); return isatty(0) ? 3 : 1; }
    if (!silent) out(std::string(n) + "\n");
    return 0;
}

int cmd_hostid(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "", "Print the numeric identifier (in hexadecimal) for the current host.")) return 0;
    char b[32];
    std::snprintf(b, sizeof b, "%08lx\n", static_cast<unsigned long>(gethostid()) & 0xFFFFFFFFul);
    out(b);
    return 0;
}

int cmd_hostname(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[NAME]", "Print or set the system host name (-s short, -f / -A fully qualified, -i / -I addresses).")) return 0;
    bool shortn = false;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "sfdiIAy")) != -1) {
        if (c == 's') shortn = true;
        else if (c == '?') return 1;
    }
    char host[256] = {0};
    if (optind < argc) {
        if (sethostname(argv[optind], std::strlen(argv[optind])) != 0) { err_errno("cannot set name to '" + std::string(argv[optind]) + "'"); return 1; }
        return 0;
    }
    gethostname(host, sizeof host - 1);
    std::string h = host;
    if (shortn) h = h.substr(0, h.find('.'));
    out(h + "\n");
    return 0;
}

// ================================================================================================
// arch / uname / uptime / nproc
// ================================================================================================
int cmd_arch(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print machine architecture. --level also prints the x86-64 microarchitecture level (v1-v4) used by CachyOS repos.")) return 0;
    utsname u;
    uname(&u);
    bool level = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--level") || !std::strcmp(argv[i], "-l")) level = true;
    if (level) {
        const std::string l = arch::x86_level();
        out(l.empty() ? std::string(u.machine) + "\n" : l + "\n");
        return 0;
    }
    out(std::string(u.machine) + "\n");
    return 0;
}

int cmd_uname(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print certain system information. With no OPTION, same as -s. --distro prints the Arch/CachyOS release name.")) return 0;
    static const option lo[] = {{"all", no_argument, nullptr, 'a'}, {"kernel-name", no_argument, nullptr, 's'}, {"nodename", no_argument, nullptr, 'n'},
                                {"kernel-release", no_argument, nullptr, 'r'}, {"kernel-version", no_argument, nullptr, 'v'}, {"machine", no_argument, nullptr, 'm'},
                                {"processor", no_argument, nullptr, 'p'}, {"hardware-platform", no_argument, nullptr, 'i'}, {"operating-system", no_argument, nullptr, 'o'},
                                {"distro", no_argument, nullptr, 'd'}, kEnd};
    bool s = false, n = false, r = false, v = false, m = false, p = false, i = false, o = false, d = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "asnrvmpio", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': s = n = r = v = m = o = true; break;
        case 's': s = true; break; case 'n': n = true; break; case 'r': r = true; break; case 'v': v = true; break;
        case 'm': m = true; break; case 'p': p = true; break; case 'i': i = true; break; case 'o': o = true; break;
        case 'd': d = true; break;
        default: return 1;
        }
    }
    if (!(s || n || r || v || m || p || i || o || d)) s = true;
    utsname u;
    uname(&u);
    std::vector<std::string> parts;
    if (s) parts.push_back(u.sysname);
    if (n) parts.push_back(u.nodename);
    if (r) parts.push_back(u.release);
    if (v) parts.push_back(u.version);
    if (m) parts.push_back(u.machine);
    if (p) parts.push_back(u.machine);
    if (i) parts.push_back(u.machine);
    if (o) parts.push_back("GNU/Linux");
    if (d) parts.push_back(arch::distro_name());
    out(join(parts, " ") + "\n");
    return 0;
}

int cmd_nproc(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print the number of processing units available to the current process (honours affinity and cgroup v2 cpu.max).")) return 0;
    static const option lo[] = {{"all", no_argument, nullptr, 'a'}, {"ignore", required_argument, nullptr, 'i'}, kEnd};
    bool all = false;
    long ignore = 0;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", lo, nullptr)) != -1) {
        if (c == 'a') all = true;
        else if (c == 'i') ignore = std::atol(optarg);
        else return 1;
    }
    long n = all ? sysconf(_SC_NPROCESSORS_CONF) : arch::usable_cpus();
    if (!all) {
        if (const char *e = std::getenv("OMP_NUM_THREADS")) { const long v = std::atol(e); if (v > 0 && v < n) n = v; }
    }
    n -= ignore;
    if (n < 1) n = 1;
    out(std::to_string(n) + "\n");
    return 0;
}

int cmd_uptime(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]...", "Print the current time, system uptime, logged in users and load average (-p pretty, -s since).")) return 0;
    bool pretty = false, since = false;
    for (int i = 1; i < argc; ++i) { if (!std::strcmp(argv[i], "-p") || !std::strcmp(argv[i], "--pretty")) pretty = true; if (!std::strcmp(argv[i], "-s") || !std::strcmp(argv[i], "--since")) since = true; }
    struct sysinfo si;
    sysinfo(&si);
    const long up = si.uptime;
    const time_t now = std::time(nullptr);
    if (since) { out(format_time(now - up, "%Y-%m-%d %H:%M:%S") + "\n"); return 0; }
    const long d = up / 86400, h = (up % 86400) / 3600, m = (up % 3600) / 60;
    if (pretty) {
        std::string s = "up";
        if (d) s += " " + std::to_string(d) + (d > 1 ? " days," : " day,");
        if (h) s += " " + std::to_string(h) + (h > 1 ? " hours," : " hour,");
        s += " " + std::to_string(m) + (m == 1 ? " minute" : " minutes");
        if (!s.empty() && s.back() == ',') s.pop_back();
        out(s + "\n");
        return 0;
    }
    size_t users = arch::logind_sessions().size();
    if (!users) { setutent(); while (getutent()) {} endutent(); }
    char b[256];
    std::string uptime_s;
    if (d) uptime_s = std::to_string(d) + (d > 1 ? " days, " : " day, ");
    char hm[32];
    if (h) std::snprintf(hm, sizeof hm, "%ld:%02ld", h, m); else std::snprintf(hm, sizeof hm, "%ld min", m);
    uptime_s += hm;
    std::snprintf(b, sizeof b, " %s up %s, %zu user%s, load average: %.2f, %.2f, %.2f\n", format_time(now, "%H:%M:%S").c_str(), uptime_s.c_str(), users,
                  users == 1 ? "" : "s", si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.loads[2] / 65536.0);
    out(b);
    return 0;
}

// ================================================================================================
// id / groups
// ================================================================================================
namespace {

std::vector<gid_t> groups_of(const std::string &user, gid_t primary)
{
    int n = 64;
    std::vector<gid_t> g(static_cast<size_t>(n));
    while (getgrouplist(user.c_str(), primary, g.data(), &n) < 0) g.resize(static_cast<size_t>(n) * 2), n = static_cast<int>(g.size());
    g.resize(static_cast<size_t>(n));
    return g;
}

} // namespace

int cmd_id(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [USER]...", "Print user and group information for each specified USER, or (when USER omitted) for the current process.")) return 0;
    bool u = false, g = false, G = false, name = false, real = false, zero = false, ctx = false;
    int c;
    optind = 1;
    static const option lo[] = {{"user", no_argument, nullptr, 'u'}, {"group", no_argument, nullptr, 'g'}, {"groups", no_argument, nullptr, 'G'},
                                {"name", no_argument, nullptr, 'n'}, {"real", no_argument, nullptr, 'r'}, {"zero", no_argument, nullptr, 'z'},
                                {"context", no_argument, nullptr, 'Z'}, kEnd};
    while ((c = getopt_long(argc, argv, "ugGnrzZ", lo, nullptr)) != -1) {
        switch (c) {
        case 'u': u = true; break; case 'g': g = true; break; case 'G': G = true; break; case 'n': name = true; break;
        case 'r': real = true; break; case 'z': zero = true; break; case 'Z': ctx = true; break;
        default: return 1;
        }
    }
    std::vector<std::string> users(argv + optind, argv + argc);
    if (users.empty()) users.push_back("");
    int rc = 0;
    const char sep = zero ? '\0' : '\n';
    for (const std::string &who : users) {
        uid_t ruid, euid;
        gid_t rgid, egid;
        std::string uname_s;
        std::vector<gid_t> grps;
        if (who.empty()) {
            ruid = getuid(); euid = geteuid(); rgid = getgid(); egid = getegid();
            uname_s = user_name(euid);
            int n = getgroups(0, nullptr);
            grps.resize(static_cast<size_t>(n > 0 ? n : 0));
            if (n > 0) getgroups(n, grps.data());
            if (std::find(grps.begin(), grps.end(), egid) == grps.end()) grps.insert(grps.begin(), egid);
        } else {
            uid_t id;
            if (!lookup_user(who, id)) { err("'" + who + "': no such user"); rc = 1; continue; }
            ruid = euid = id;
            const passwd *pw = getpwuid(id);
            rgid = egid = pw ? pw->pw_gid : id;
            uname_s = pw ? pw->pw_name : who;
            grps = groups_of(uname_s, rgid);
        }
        auto uid_s = [&](uid_t x) { return name ? user_name(x) : std::to_string(x); };
        auto gid_s = [&](gid_t x) { return name ? group_name(x) : std::to_string(x); };
        if (ctx) { out(std::string("?") + sep); continue; }
        if (u) { out(uid_s(real ? ruid : euid) + sep); continue; }
        if (g) { out(gid_s(real ? rgid : egid) + sep); continue; }
        if (G) {
            std::vector<std::string> v;
            for (gid_t x : grps) v.push_back(gid_s(x));
            out(join(v, zero ? std::string(1, '\0') : " ") + sep);
            continue;
        }
        std::string o = "uid=" + std::to_string(ruid) + "(" + user_name(ruid) + ") gid=" + std::to_string(rgid) + "(" + group_name(rgid) + ")";
        if (euid != ruid) o += " euid=" + std::to_string(euid) + "(" + user_name(euid) + ")";
        if (egid != rgid) o += " egid=" + std::to_string(egid) + "(" + group_name(egid) + ")";
        o += " groups=";
        for (size_t i = 0; i < grps.size(); ++i) { if (i) o += ","; o += std::to_string(grps[i]) + "(" + group_name(grps[i]) + ")"; }
        out(o + sep);
    }
    return rc;
}

int cmd_groups(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [USERNAME]...", "Print group memberships for each USERNAME or, if no USERNAME is specified, for the current process.")) return 0;
    std::vector<std::string> users(argv + 1, argv + argc);
    int rc = 0;
    if (users.empty()) {
        int n = getgroups(0, nullptr);
        std::vector<gid_t> g(static_cast<size_t>(n > 0 ? n : 0));
        if (n > 0) getgroups(n, g.data());
        if (std::find(g.begin(), g.end(), getegid()) == g.end()) g.insert(g.begin(), getegid());
        std::vector<std::string> names;
        for (gid_t x : g) names.push_back(group_name(x));
        out(join(names, " ") + "\n");
        return 0;
    }
    for (const std::string &u : users) {
        uid_t id;
        if (!lookup_user(u, id)) { err("'" + u + "': no such user"); rc = 1; continue; }
        const passwd *pw = getpwuid(id);
        std::vector<std::string> names;
        for (gid_t x : groups_of(pw ? pw->pw_name : u, pw ? pw->pw_gid : id)) names.push_back(group_name(x));
        out(u + " : " + join(names, " ") + "\n");
    }
    return rc;
}

// ================================================================================================
// env / printenv / nice / nohup / timeout / kill
// ================================================================================================
int cmd_printenv(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [VARIABLE]...", "Print the values of the specified environment VARIABLE(s). If no VARIABLE is specified, print name and value pairs for them all.")) return 0;
    char eol = '\n';
    int i = 1;
    if (i < argc && (!std::strcmp(argv[i], "-0") || !std::strcmp(argv[i], "--null"))) { eol = '\0'; ++i; }
    if (i >= argc) {
        for (char **e = environ; e && *e; ++e) out(std::string(*e) + eol);
        return 0;
    }
    int rc = 0;
    for (; i < argc; ++i) {
        const char *v = std::getenv(argv[i]);
        if (v) out(std::string(v) + eol); else rc = 1;
    }
    return rc;
}

int cmd_env(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]", "Set each NAME to VALUE in the environment and run COMMAND.")) return 0;
    bool ignore = false;
    char eol = '\n';
    std::vector<std::string> unset;
    std::string chdir_to;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-i" || a == "-" || a == "--ignore-environment") ignore = true;
        else if (a == "-0" || a == "--null") eol = '\0';
        else if (a == "-u" && i + 1 < argc) unset.push_back(argv[++i]);
        else if (a.rfind("--unset=", 0) == 0) unset.push_back(a.substr(8));
        else if (a == "-C" && i + 1 < argc) chdir_to = argv[++i];
        else if (a.rfind("--chdir=", 0) == 0) chdir_to = a.substr(8);
        else if (a == "--") { ++i; break; }
        else if (a[0] == '-' && a.size() > 1) { usage_error("unrecognized option '" + a + "'"); return 125; }
        else break;
    }
    if (ignore) clearenv();
    for (const auto &u : unset) unsetenv(u.c_str());
    for (; i < argc; ++i) {
        const char *eq = std::strchr(argv[i], '=');
        if (!eq) break;
        setenv(std::string(argv[i], static_cast<size_t>(eq - argv[i])).c_str(), eq + 1, 1);
    }
    if (i >= argc) {
        for (char **e = environ; e && *e; ++e) out(std::string(*e) + eol);
        return 0;
    }
    if (!chdir_to.empty() && chdir(chdir_to.c_str()) != 0) { err_errno("cannot change directory to '" + chdir_to + "'"); return 125; }
    flush_out();
    execvp(argv[i], argv + i);
    err_errno("'" + std::string(argv[i]) + "'");
    return errno == ENOENT ? 127 : 126;
}

int cmd_nice(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] [COMMAND [ARG]...]", "Run COMMAND with an adjusted niceness. With no COMMAND, print the current niceness.")) return 0;
    int adj = 10, i = 1;
    if (i < argc) {
        if (!std::strcmp(argv[i], "-n") && i + 1 < argc) { adj = std::atoi(argv[i + 1]); i += 2; }
        else if (!std::strncmp(argv[i], "--adjustment=", 13)) { adj = std::atoi(argv[i] + 13); ++i; }
        else if (argv[i][0] == '-' && (std::isdigit(static_cast<unsigned char>(argv[i][1])) || argv[i][1] == '-')) {
            if (argv[i][1] == '-' && argv[i][2]) { adj = -std::atoi(argv[i] + 2); ++i; } // GNU: --5 means -5
            else { adj = std::atoi(argv[i] + 1); ++i; }
        }
    }
    if (i >= argc) {
        errno = 0;
        const int n = getpriority(PRIO_PROCESS, 0);
        out(std::to_string(n) + "\n");
        return 0;
    }
    errno = 0;
    const int cur = getpriority(PRIO_PROCESS, 0);
    if (setpriority(PRIO_PROCESS, 0, cur + adj) != 0) err_errno("cannot set niceness");
    flush_out();
    execvp(argv[i], argv + i);
    err_errno("'" + std::string(argv[i]) + "'");
    return errno == ENOENT ? 127 : 126;
}

int cmd_nohup(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "COMMAND [ARG]...", "Run COMMAND, ignoring hangup signals. Output goes to nohup.out when stdout is a terminal.")) return 0;
    if (argc < 2) { usage_error("missing operand"); return 125; }
    const bool in_tty = isatty(0), out_tty = isatty(1), err_tty = isatty(2);
    if (in_tty) { const int fd = open("/dev/null", O_RDONLY); if (fd >= 0) { dup2(fd, 0); close(fd); } }
    if (out_tty) {
        std::string path = "nohup.out";
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0) {
            if (const char *h = std::getenv("HOME")) { path = std::string(h) + "/nohup.out"; fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600); }
        }
        if (fd < 0) { err_errno("failed to open 'nohup.out'"); return 125; }
        std::fprintf(stderr, "%s: ignoring input and appending output to '%s'\n", prog.c_str(), path.c_str());
        dup2(fd, 1);
        close(fd);
    }
    if (err_tty) dup2(1, 2);
    signal(SIGHUP, SIG_IGN);
    flush_out();
    execvp(argv[1], argv + 1);
    err_errno("failed to run command '" + std::string(argv[1]) + "'");
    return errno == ENOENT ? 127 : 126;
}

namespace {

struct SigName { const char *name; int num; };
const SigName kSignals[] = {{"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL}, {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"BUS", SIGBUS},
                            {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE},
                            {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"STKFLT", SIGSTKFLT}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
                            {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}, {"URG", SIGURG}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ},
                            {"VTALRM", SIGVTALRM}, {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"IO", SIGIO}, {"PWR", SIGPWR}, {"SYS", SIGSYS}};

int signal_number(std::string s)
{
    if (s.empty()) return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s.rfind("SIG", 0) == 0) s = s.substr(3);
    for (const SigName &n : kSignals) if (s == n.name) return n.num;
    return -1;
}

} // namespace

int cmd_kill(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[-s SIGNAL | -SIGNAL] PID...", "Send the specified signal (default TERM) to the given processes; -l lists signal names.")) return 0;
    int sig = SIGTERM, i = 1;
    if (i < argc && (!std::strcmp(argv[i], "-l") || !std::strcmp(argv[i], "-L") || !std::strcmp(argv[i], "--list") || !std::strcmp(argv[i], "--table"))) {
        if (i + 1 < argc) {
            for (int k = i + 1; k < argc; ++k) {
                const int n = std::atoi(argv[k]) & 0x7F;
                bool found = false;
                for (const SigName &s : kSignals) if (s.num == n) { out(std::string(s.name) + "\n"); found = true; }
                if (!found) { const int sn = signal_number(argv[k]); if (sn > 0) out(std::to_string(sn) + "\n"); }
            }
            return 0;
        }
        for (const SigName &s : kSignals) out(std::to_string(s.num) + ") SIG" + s.name + "\n");
        return 0;
    }
    if (i < argc && (!std::strcmp(argv[i], "-s") || !std::strcmp(argv[i], "--signal"))) {
        if (i + 1 >= argc) { usage_error("option requires an argument -- 's'"); return 1; }
        sig = signal_number(argv[i + 1]);
        if (sig < 0) { err(std::string("'") + argv[i + 1] + "': invalid signal"); return 1; }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && std::strcmp(argv[i], "--")) {
        sig = signal_number(argv[i] + 1);
        if (sig < 0) { err(std::string("'") + (argv[i] + 1) + "': invalid signal"); return 1; }
        ++i;
    }
    if (i < argc && !std::strcmp(argv[i], "--")) ++i;
    if (i >= argc) { usage_error("missing operand"); return 1; }
    int rc = 0;
    for (; i < argc; ++i) {
        char *end = nullptr;
        const long pid = std::strtol(argv[i], &end, 10);
        if (*end || end == argv[i]) { err(std::string("failed to parse argument: '") + argv[i] + "'"); rc = 1; continue; }
        if (::kill(static_cast<pid_t>(pid), sig) != 0) { err_errno(std::string("sending signal to ") + argv[i]); rc = 1; }
    }
    return rc;
}

namespace {

volatile sig_atomic_t g_timeout_sig = 0;
volatile sig_atomic_t g_child_pid_for_sig = 0;

void forward_signal(int s)
{
    g_timeout_sig = s;
    if (g_child_pid_for_sig > 0) ::kill(static_cast<pid_t>(g_child_pid_for_sig), s);
}

double parse_duration(const std::string &in, bool &ok)
{
    std::string a = in;
    double mult = 1;
    ok = true;
    if (!a.empty() && std::strchr("smhd", a.back())) {
        mult = a.back() == 'm' ? 60 : a.back() == 'h' ? 3600 : a.back() == 'd' ? 86400 : 1;
        a.pop_back();
    }
    double v;
    if (!parse_double(a, v) || v < 0) { ok = false; return 0; }
    return v * mult;
}

} // namespace

int cmd_timeout(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION] DURATION COMMAND [ARG]...", "Start COMMAND, and kill it if still running after DURATION.")) return 0;
    static const option lo[] = {{"foreground", no_argument, nullptr, 'f'}, {"kill-after", required_argument, nullptr, 'k'}, {"preserve-status", no_argument, nullptr, 'p'},
                                {"signal", required_argument, nullptr, 's'}, {"verbose", no_argument, nullptr, 'v'}, kEnd};
    bool foreground = false, preserve = false, verbose = false;
    int sig = SIGTERM;
    double kill_after = 0;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+fk:ps:v", lo, nullptr)) != -1) {
        switch (c) {
        case 'f': foreground = true; break;
        case 'p': preserve = true; break;
        case 'v': verbose = true; break;
        case 's': sig = signal_number(optarg); if (sig < 0) { err(std::string("'") + optarg + "': invalid signal"); return 125; } break;
        case 'k': { bool ok; kill_after = parse_duration(optarg, ok); if (!ok) { err(std::string("invalid time interval '") + optarg + "'"); return 125; } break; }
        default: return 125;
        }
    }
    if (argc - optind < 2) { usage_error(argc - optind < 1 ? "missing operand" : "missing operand after '" + std::string(argv[optind]) + "'"); return 125; }
    bool ok;
    const double dur = parse_duration(argv[optind], ok);
    if (!ok) { err(std::string("invalid time interval '") + argv[optind] + "'"); return 125; }
    char **cmd = argv + optind + 1;
    if (!foreground) setpgid(0, 0);
    flush_out();
    const pid_t pid = fork();
    if (pid < 0) { err_errno("fork system call failed"); return 125; }
    if (pid == 0) {
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        execvp(cmd[0], cmd);
        const int e = errno;
        std::fprintf(stderr, "%s: failed to run command '%s': %s\n", prog.c_str(), cmd[0], std::strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    g_child_pid_for_sig = pid;
    for (int s : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) signal(s, forward_signal);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    auto now = [] { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; };
    const double start = now();
    bool timed_out = false, killed = false;
    int status = 0;
    for (;;) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) { status = 0; break; }
        const double el = now() - start;
        if (!timed_out && dur > 0 && el >= dur) {
            timed_out = true;
            if (verbose) std::fprintf(stderr, "%s: sending signal %d to command '%s'\n", prog.c_str(), sig, cmd[0]);
            ::kill(foreground ? pid : -pid, sig);
            if (!foreground && sig != SIGKILL && sig != SIGCONT) ::kill(-pid, SIGCONT);
        }
        if (timed_out && kill_after > 0 && !killed && el >= dur + kill_after) {
            killed = true;
            ::kill(foreground ? pid : -pid, SIGKILL);
        }
        timespec ts{0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    if (timed_out && !preserve) return 124;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        const int s = WTERMSIG(status);
        signal(s, SIG_DFL);
        if (killed) return 137;
        return 128 + s;
    }
    return 125;
}

// ================================================================================================
// seq
// ================================================================================================
int cmd_seq(int argc, char **argv)
{
    if (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "--version"))) {
        handle_help_version(argc, argv, "[OPTION]... LAST | FIRST LAST | FIRST INCREMENT LAST", "Print numbers from FIRST to LAST, in steps of INCREMENT.");
        return 0;
    }
    std::string fmt, sep = "\n";
    bool equal = false;
    std::vector<std::string> nums;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-f" && i + 1 < argc) fmt = argv[++i];
        else if (a.rfind("--format=", 0) == 0) fmt = a.substr(9);
        else if (a == "-s" && i + 1 < argc) sep = argv[++i];
        else if (a.rfind("--separator=", 0) == 0) sep = a.substr(12);
        else if (a == "-w" || a == "--equal-width") equal = true;
        else if (a == "--") { for (++i; i < argc; ++i) nums.push_back(argv[i]); }
        else nums.push_back(a);
    }
    if (nums.empty()) { usage_error("missing operand"); return 1; }
    if (nums.size() > 3) { usage_error("extra operand '" + nums[3] + "'"); return 1; }
    double first = 1, inc = 1, last;
    auto pd = [&](const std::string &s, double &v) { if (!parse_double(s, v) || std::isnan(v)) { err("invalid floating point argument: '" + s + "'"); return false; } return true; };
    if (nums.size() == 1) { if (!pd(nums[0], last)) return 1; }
    else if (nums.size() == 2) { if (!pd(nums[0], first) || !pd(nums[1], last)) return 1; }
    else { if (!pd(nums[0], first) || !pd(nums[1], inc) || !pd(nums[2], last)) return 1; }
    if (inc == 0) { err("invalid Zero increment value: '" + nums[1] + "'"); return 1; }
    // decimals needed to print exact values
    auto decimals = [](const std::string &s) { const size_t p = s.find('.'); return p == std::string::npos || s.find_first_of("eE") != std::string::npos ? 0 : static_cast<int>(s.size() - p - 1); };
    int prec = 0;
    for (const auto &n : nums) prec = std::max(prec, decimals(n));
    int width = 0;
    if (equal) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*f", prec, first); width = std::max<int>(width, static_cast<int>(std::strlen(b)));
        std::snprintf(b, sizeof b, "%.*f", prec, last); width = std::max<int>(width, static_cast<int>(std::strlen(b)));
    }
    std::string o;
    bool first_item = true;
    const long double eps = std::fabs(inc) * 1e-9L;
    long n = 0;
    for (;; ++n) {
        const long double v = static_cast<long double>(first) + static_cast<long double>(inc) * n;
        if (inc > 0 ? v > last + eps : v < last - eps) break;
        char b[512];
        if (!fmt.empty()) std::snprintf(b, sizeof b, fmt.c_str(), static_cast<double>(v));
        else if (equal) std::snprintf(b, sizeof b, "%0*.*f", width, prec, static_cast<double>(v));
        else std::snprintf(b, sizeof b, "%.*f", prec, static_cast<double>(v));
        if (!first_item) o += sep;
        o += b;
        first_item = false;
        if (o.size() > 1 << 16) { out(o); o.clear(); }
    }
    if (!first_item) o += '\n';
    out(o);
    return 0;
}

// ================================================================================================
// factor
// ================================================================================================
namespace {

using u64 = unsigned long long;
using u128 = unsigned __int128;

u64 mulmod(u64 a, u64 b, u64 m) { return static_cast<u64>(static_cast<u128>(a) * b % m); }
u64 powmod(u64 b, u64 e, u64 m)
{
    u64 r = 1;
    b %= m;
    while (e) { if (e & 1) r = mulmod(r, b, m); b = mulmod(b, b, m); e >>= 1; }
    return r;
}

bool is_prime(u64 n)
{
    if (n < 2) return false;
    for (u64 p : {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL, 19ULL, 23ULL, 29ULL, 31ULL, 37ULL}) {
        if (n % p == 0) return n == p;
    }
    u64 d = n - 1;
    int s = 0;
    while (!(d & 1)) { d >>= 1; ++s; }
    for (u64 a : {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL, 19ULL, 23ULL, 29ULL, 31ULL, 37ULL}) {
        u64 x = powmod(a, d, n);
        if (x == 1 || x == n - 1) continue;
        bool comp = true;
        for (int r = 1; r < s; ++r) { x = mulmod(x, x, n); if (x == n - 1) { comp = false; break; } }
        if (comp) return false;
    }
    return true;
}

u64 gcd64(u64 a, u64 b) { while (b) { const u64 t = a % b; a = b; b = t; } return a; }

u64 rho(u64 n)
{
    if (n % 2 == 0) return 2;
    for (u64 c = 1;; ++c) {
        u64 x = 2, y = 2, d = 1;
        auto f = [&](u64 v) { return static_cast<u64>((static_cast<u128>(v) * v + c) % n); };
        while (d == 1) {
            x = f(x);
            y = f(f(y));
            d = gcd64(x > y ? x - y : y - x, n);
        }
        if (d != n) return d;
    }
}

void factor_rec(u64 n, std::vector<u64> &out_f)
{
    if (n == 1) return;
    if (is_prime(n)) { out_f.push_back(n); return; }
    const u64 d = rho(n);
    factor_rec(d, out_f);
    factor_rec(n / d, out_f);
}

} // namespace

int cmd_factor(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[NUMBER]...", "Print the prime factors of each specified integer NUMBER (read from stdin if none given).")) return 0;
    std::vector<std::string> in(argv + 1, argv + argc);
    if (in.empty()) {
        std::string data;
        read_file("-", data);
        std::string tok;
        for (char ch : data + " ") { if (std::isspace(static_cast<unsigned char>(ch))) { if (!tok.empty()) in.push_back(tok); tok.clear(); } else tok += ch; }
    }
    int rc = 0;
    for (const std::string &s : in) {
        uint64_t v;
        if (s.empty() || s[0] == '-' || !parse_u64(s, v)) { err("'" + s + "' is not a valid positive integer"); rc = 1; continue; }
        std::vector<u64> f;
        u64 n = v;
        for (u64 p = 2; p < 1000 && p * p <= n; p += (p == 2 ? 1 : 2)) while (n % p == 0) { f.push_back(p); n /= p; }
        factor_rec(n, f);
        std::sort(f.begin(), f.end());
        std::string o = std::to_string(v) + ":";
        for (u64 p : f) o += " " + std::to_string(p);
        out(o + "\n");
    }
    return rc;
}

// ================================================================================================
// expr
// ================================================================================================
namespace {

struct ExprVal {
    bool is_num = false;
    long long n = 0;
    std::string s;
    std::string str() const { return is_num ? std::to_string(n) : s; }
    bool as_num(long long &out_n) const
    {
        if (is_num) { out_n = n; return true; }
        if (s.empty()) return false;
        char *end;
        errno = 0;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (*end || errno) return false;
        out_n = v;
        return true;
    }
    bool truthy() const
    {
        if (is_num) return n != 0;
        if (s.empty()) return false;
        long long v;
        if (as_num(v)) return v != 0;
        return true;
    }
    static ExprVal num(long long v) { ExprVal e; e.is_num = true; e.n = v; return e; }
    static ExprVal text(std::string v) { ExprVal e; e.s = std::move(v); return e; }
};

struct ExprError { std::string msg; int code; };

class ExprParser {
public:
    explicit ExprParser(std::vector<std::string> t) : t_(std::move(t)) {}
    ExprVal run()
    {
        if (t_.empty()) throw ExprError{"missing operand", 2};
        ExprVal v = parse_or();
        if (i_ < t_.size()) throw ExprError{"syntax error: unexpected argument '" + t_[i_] + "'", 2};
        return v;
    }

private:
    std::vector<std::string> t_;
    size_t i_ = 0;
    bool at(const char *s) const { return i_ < t_.size() && t_[i_] == s; }

    ExprVal parse_or()
    {
        ExprVal l = parse_and();
        while (at("|")) {
            ++i_;
            ExprVal r = parse_and();
            l = l.truthy() ? l : (r.truthy() ? r : ExprVal::num(0));
        }
        return l;
    }
    ExprVal parse_and()
    {
        ExprVal l = parse_cmp();
        while (at("&")) {
            ++i_;
            ExprVal r = parse_cmp();
            l = (l.truthy() && r.truthy()) ? l : ExprVal::num(0);
        }
        return l;
    }
    ExprVal parse_cmp()
    {
        ExprVal l = parse_add();
        while (i_ < t_.size() && (at("=") || at("==") || at("!=") || at("<") || at("<=") || at(">") || at(">="))) {
            const std::string op = t_[i_++];
            ExprVal r = parse_add();
            long long a, b;
            int cmp;
            if (l.as_num(a) && r.as_num(b)) cmp = a < b ? -1 : (a > b ? 1 : 0);
            else { const int c = std::strcoll(l.str().c_str(), r.str().c_str()); cmp = c < 0 ? -1 : (c > 0 ? 1 : 0); }
            bool res = op == "=" || op == "==" ? cmp == 0 : op == "!=" ? cmp != 0 : op == "<" ? cmp < 0 : op == "<=" ? cmp <= 0 : op == ">" ? cmp > 0 : cmp >= 0;
            l = ExprVal::num(res ? 1 : 0);
        }
        return l;
    }
    ExprVal parse_add()
    {
        ExprVal l = parse_mul();
        while (at("+") || at("-")) {
            const bool plus = t_[i_++] == "+";
            ExprVal r = parse_mul();
            long long a, b;
            if (!l.as_num(a) || !r.as_num(b)) throw ExprError{"non-integer argument", 2};
            l = ExprVal::num(plus ? a + b : a - b);
        }
        return l;
    }
    ExprVal parse_mul()
    {
        ExprVal l = parse_match();
        while (at("*") || at("/") || at("%")) {
            const char op = t_[i_++][0];
            ExprVal r = parse_match();
            long long a, b;
            if (!l.as_num(a) || !r.as_num(b)) throw ExprError{"non-integer argument", 2};
            if (op != '*' && b == 0) throw ExprError{"division by zero", 2};
            l = ExprVal::num(op == '*' ? a * b : op == '/' ? a / b : a % b);
        }
        return l;
    }
    ExprVal parse_match()
    {
        ExprVal l = parse_primary();
        while (at(":")) {
            ++i_;
            ExprVal r = parse_primary();
            std::regex rx;
            try { rx = std::regex("^" + r.str(), std::regex::basic); } catch (const std::regex_error &) { throw ExprError{"Invalid regular expression", 2}; }
            std::smatch m;
            const std::string subject = l.str();
            if (std::regex_search(subject, m, rx)) {
                if (m.size() > 1) l = ExprVal::text(m[1].str());
                else l = ExprVal::num(static_cast<long long>(m.length(0)));
            } else {
                l = r.str().find("\\(") != std::string::npos ? ExprVal::text("") : ExprVal::num(0);
            }
        }
        return l;
    }
    ExprVal parse_primary()
    {
        if (i_ >= t_.size()) throw ExprError{"syntax error: missing argument", 2};
        const std::string tok = t_[i_];
        if (tok == "(") {
            ++i_;
            ExprVal v = parse_or();
            if (!at(")")) throw ExprError{"syntax error: expecting ')' after '" + (i_ < t_.size() ? t_[i_] : std::string("")) + "'", 2};
            ++i_;
            return v;
        }
        if (tok == "length" && i_ + 1 < t_.size()) { ++i_; return ExprVal::num(static_cast<long long>(parse_primary().str().size())); }
        if (tok == "substr" && i_ + 3 < t_.size()) {
            ++i_;
            const std::string s = parse_primary().str();
            long long pos, len;
            if (!parse_primary().as_num(pos) || !parse_primary().as_num(len)) throw ExprError{"non-integer argument", 2};
            if (pos < 1 || len < 1 || static_cast<size_t>(pos) > s.size()) return ExprVal::text("");
            return ExprVal::text(s.substr(static_cast<size_t>(pos - 1), static_cast<size_t>(len)));
        }
        if (tok == "index" && i_ + 2 < t_.size()) {
            ++i_;
            const std::string s = parse_primary().str(), chars = parse_primary().str();
            const size_t p = s.find_first_of(chars);
            return ExprVal::num(p == std::string::npos ? 0 : static_cast<long long>(p + 1));
        }
        if (tok == "match" && i_ + 2 < t_.size()) {
            ++i_;
            std::vector<std::string> sub = {t_[i_], ":", t_[i_ + 1]};
            i_ += 2;
            return ExprParser(sub).run();
        }
        if (tok == "+" && i_ + 1 < t_.size()) { i_ += 2; return ExprVal::text(t_[i_ - 1]); }
        ++i_;
        return ExprVal::text(tok);
    }
};

} // namespace

int cmd_expr(int argc, char **argv)
{
    if (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "--version"))) {
        handle_help_version(argc, argv, "EXPRESSION", "Print the value of EXPRESSION to standard output.");
        return 0;
    }
    try {
        ExprVal v = ExprParser(std::vector<std::string>(argv + 1, argv + argc)).run();
        out(v.str() + "\n");
        return v.truthy() ? 0 : 1;
    } catch (const ExprError &e) {
        err(e.msg);
        return e.code;
    }
}

// ================================================================================================
// test / [
// ================================================================================================
namespace {

class TestParser {
public:
    TestParser(std::vector<std::string> a) : a_(std::move(a)) {}
    bool run()
    {
        if (a_.empty()) return false;
        const bool r = parse_or();
        if (i_ < a_.size()) throw std::string("extra argument '" + a_[i_] + "'");
        return r;
    }

private:
    std::vector<std::string> a_;
    size_t i_ = 0;

    static bool is_binary_op(const std::string &s)
    {
        static const std::set<std::string> ops = {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef"};
        return ops.count(s) > 0;
    }
    bool parse_or()
    {
        bool l = parse_and();
        while (i_ < a_.size() && a_[i_] == "-o") { ++i_; const bool r = parse_and(); l = l || r; }
        return l;
    }
    bool parse_and()
    {
        bool l = parse_not();
        while (i_ < a_.size() && a_[i_] == "-a") { ++i_; const bool r = parse_not(); l = l && r; }
        return l;
    }
    bool parse_not()
    {
        if (i_ < a_.size() && a_[i_] == "!" && i_ + 1 < a_.size()) { ++i_; return !parse_not(); }
        return parse_primary();
    }
    static long long to_int(const std::string &s)
    {
        std::string t = s;
        size_t b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
        if (b == std::string::npos) throw std::string("'" + s + "': integer expression expected");
        t = t.substr(b, e - b + 1);
        char *end;
        errno = 0;
        const long long v = std::strtoll(t.c_str(), &end, 10);
        if (*end || errno || t.empty()) throw std::string("'" + s + "': integer expression expected");
        return v;
    }
    bool parse_primary()
    {
        if (i_ >= a_.size()) throw std::string("missing argument");
        const std::string tok = a_[i_];
        if (tok == "(" ) {
            // "( x )" with a single operand is just that operand
            ++i_;
            const bool v = parse_or();
            if (i_ >= a_.size() || a_[i_] != ")") throw std::string("missing ')'");
            ++i_;
            return v;
        }
        // binary operator form takes priority if arg[i+1] is a binary op and there are enough args
        if (i_ + 2 < a_.size() + 0 && is_binary_op(a_[i_ + 1])) {
            const std::string l = a_[i_], op = a_[i_ + 1], r = a_[i_ + 2];
            i_ += 3;
            return binary(l, op, r);
        }
        if (i_ + 2 == a_.size() + 0 && false) {}
        if (tok.size() == 2 && tok[0] == '-' && std::strchr("bcdefghkLNOpGrSstuwxzn", tok[1]) && i_ + 1 < a_.size()) {
            const std::string operand = a_[i_ + 1];
            // "-n" or "-z" alone followed by a binary op is a plain string test
            if (!(i_ + 2 < a_.size() && is_binary_op(a_[i_ + 1]))) {
                i_ += 2;
                return unary(tok[1], operand);
            }
        }
        ++i_;
        return !tok.empty();
    }
    static bool unary(char op, const std::string &f)
    {
        struct stat st;
        switch (op) {
        case 'n': return !f.empty();
        case 'z': return f.empty();
        case 't': return isatty(static_cast<int>(to_int(f)));
        case 'L': case 'h': return lstat(f.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        case 'e': return stat(f.c_str(), &st) == 0;
        case 'f': return stat(f.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        case 'd': return stat(f.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        case 'b': return stat(f.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
        case 'c': return stat(f.c_str(), &st) == 0 && S_ISCHR(st.st_mode);
        case 'p': return stat(f.c_str(), &st) == 0 && S_ISFIFO(st.st_mode);
        case 'S': return stat(f.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        case 's': return stat(f.c_str(), &st) == 0 && st.st_size > 0;
        case 'g': return stat(f.c_str(), &st) == 0 && (st.st_mode & S_ISGID);
        case 'u': return stat(f.c_str(), &st) == 0 && (st.st_mode & S_ISUID);
        case 'k': return stat(f.c_str(), &st) == 0 && (st.st_mode & S_ISVTX);
        case 'O': return stat(f.c_str(), &st) == 0 && st.st_uid == geteuid();
        case 'G': return stat(f.c_str(), &st) == 0 && st.st_gid == getegid();
        case 'N': return stat(f.c_str(), &st) == 0 && (st.st_mtim.tv_sec > st.st_atim.tv_sec || (st.st_mtim.tv_sec == st.st_atim.tv_sec && st.st_mtim.tv_nsec > st.st_atim.tv_nsec));
        case 'r': return access(f.c_str(), R_OK) == 0;
        case 'w': return access(f.c_str(), W_OK) == 0;
        case 'x': return access(f.c_str(), X_OK) == 0;
        default: return false;
        }
    }
    static bool binary(const std::string &l, const std::string &op, const std::string &r)
    {
        if (op == "=" || op == "==") return l == r;
        if (op == "!=") return l != r;
        if (op == "<") return std::strcoll(l.c_str(), r.c_str()) < 0;
        if (op == ">") return std::strcoll(l.c_str(), r.c_str()) > 0;
        if (op == "-nt" || op == "-ot" || op == "-ef") {
            struct stat a, b;
            const bool ha = stat(l.c_str(), &a) == 0, hb = stat(r.c_str(), &b) == 0;
            if (op == "-ef") return ha && hb && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
            auto newer = [](const struct stat &x, const struct stat &y) { return x.st_mtim.tv_sec > y.st_mtim.tv_sec || (x.st_mtim.tv_sec == y.st_mtim.tv_sec && x.st_mtim.tv_nsec > y.st_mtim.tv_nsec); };
            if (op == "-nt") return ha && (!hb || newer(a, b));
            return hb && (!ha || newer(b, a));
        }
        const long long a = to_int(l), b = to_int(r);
        if (op == "-eq") return a == b;
        if (op == "-ne") return a != b;
        if (op == "-lt") return a < b;
        if (op == "-le") return a <= b;
        if (op == "-gt") return a > b;
        return a >= b;
    }
};

int test_main(std::vector<std::string> args)
{
    try {
        return TestParser(std::move(args)).run() ? 0 : 1;
    } catch (const std::string &m) {
        err(m);
        return 2;
    }
}

} // namespace

int cmd_test(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--help")) { handle_help_version(argc, argv, "EXPRESSION", "Exit with the status determined by EXPRESSION."); return 0; }
    if (argc == 2 && !std::strcmp(argv[1], "--version")) { handle_help_version(argc, argv, "EXPRESSION", "Exit with the status determined by EXPRESSION."); return 0; }
    return test_main(std::vector<std::string>(argv + 1, argv + argc));
}

int cmd_lbracket(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[argc - 1], "]") != 0) { err("missing ']'"); return 2; }
    return test_main(std::vector<std::string>(argv + 1, argv + argc - 1));
}

// ================================================================================================
// date
// ================================================================================================
int cmd_date(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [+FORMAT]", "Display the current time in the given FORMAT, or set the system date.")) return 0;
    static const option lo[] = {{"date", required_argument, nullptr, 'd'}, {"utc", no_argument, nullptr, 'u'}, {"universal", no_argument, nullptr, 'u'},
                                {"reference", required_argument, nullptr, 'r'}, {"set", required_argument, nullptr, 's'}, {"rfc-email", no_argument, nullptr, 'R'},
                                {"iso-8601", optional_argument, nullptr, 'I'}, {"rfc-3339", required_argument, nullptr, 3}, {"file", required_argument, nullptr, 'f'}, kEnd};
    bool utc = false;
    std::string date_s, ref, set_s, fmt, iso;
    bool rfc_email = false;
    std::string rfc3339;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "d:ur:s:RI::f:", lo, nullptr)) != -1) {
        switch (c) {
        case 'd': date_s = optarg; break;
        case 'u': utc = true; break;
        case 'r': ref = optarg; break;
        case 's': set_s = optarg; break;
        case 'R': rfc_email = true; break;
        case 'I': iso = optarg ? optarg : "date"; break;
        case 3: rfc3339 = optarg; break;
        case 'f': date_s = optarg; break;
        default: return 1;
        }
    }
    for (int i = optind; i < argc; ++i) {
        if (argv[i][0] == '+') fmt = argv[i] + 1;
        else if (set_s.empty() && date_s.empty() && std::isdigit(static_cast<unsigned char>(argv[i][0]))) set_s = argv[i];
        else { usage_error(std::string("extra operand '") + argv[i] + "'"); return 1; }
    }
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    if (!ref.empty()) {
        struct stat st;
        if (stat(ref.c_str(), &st) != 0) { err_errno(ref); return 1; }
        t = st.st_mtim.tv_sec;
        ts.tv_nsec = st.st_mtim.tv_nsec;
    }
    if (!date_s.empty()) {
        if (!parse_date(date_s, t, t)) { err("invalid date '" + date_s + "'"); return 1; }
        ts.tv_nsec = 0;
    }
    if (!set_s.empty()) {
        time_t nt;
        if (!parse_date(set_s, t, nt)) { err("invalid date '" + set_s + "'"); return 1; }
        timespec set{nt, 0};
        if (clock_settime(CLOCK_REALTIME, &set) != 0) { err_errno("cannot set date"); return 1; }
        t = nt;
    }
    if (rfc_email) fmt = "%a, %d %b %Y %H:%M:%S %z";
    else if (!iso.empty()) {
        if (iso == "date") fmt = "%Y-%m-%d";
        else if (iso == "hours") fmt = "%Y-%m-%dT%H%:z";
        else if (iso == "minutes") fmt = "%Y-%m-%dT%H:%M%:z";
        else if (iso == "seconds") fmt = "%Y-%m-%dT%H:%M:%S%:z";
        else fmt = "%Y-%m-%dT%H:%M:%S,%N%:z";
    } else if (!rfc3339.empty()) {
        fmt = rfc3339 == "date" ? "%Y-%m-%d" : (rfc3339 == "seconds" ? "%Y-%m-%d %H:%M:%S%:z" : "%Y-%m-%d %H:%M:%S.%N%:z");
    } else if (fmt.empty()) fmt = "%a %b %e %H:%M:%S %Z %Y";
    // glibc strftime lacks %N and %:z: expand them here first.
    std::string f2;
    char nb[16];
    std::snprintf(nb, sizeof nb, "%09ld", ts.tv_nsec);
    tm tmv;
    if (utc) gmtime_r(&t, &tmv); else localtime_r(&t, &tmv);
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == 'N') { f2 += nb; ++i; continue; }
            if (fmt[i + 1] == ':' && i + 2 < fmt.size() && fmt[i + 2] == 'z') {
                char z[16];
                std::strftime(z, sizeof z, "%z", &tmv);
                std::string zs = z;
                if (zs.size() == 5) zs.insert(3, ":");
                f2 += zs;
                i += 2;
                continue;
            }
            f2 += fmt[i];
            f2 += fmt[i + 1];
            ++i;
        } else f2 += fmt[i];
    }
    out(format_time(t, f2, utc) + "\n");
    return 0;
}

// ================================================================================================
// who / users / pinky  (utmp on classic systems, systemd-logind on Arch/CachyOS)
// ================================================================================================
namespace {

struct Login {
    std::string user, line, host, comment;
    int64_t when = 0;
    pid_t pid = 0;
};

std::vector<Login> current_logins()
{
    std::vector<Login> v;
    setutent();
    while (const utmp *u = getutent()) {
        if (u->ut_type != USER_PROCESS || !u->ut_user[0]) continue;
        Login l;
        l.user = std::string(u->ut_user, strnlen(u->ut_user, sizeof u->ut_user));
        l.line = std::string(u->ut_line, strnlen(u->ut_line, sizeof u->ut_line));
        l.host = std::string(u->ut_host, strnlen(u->ut_host, sizeof u->ut_host));
        l.when = u->ut_tv.tv_sec;
        l.pid = u->ut_pid;
        v.push_back(l);
    }
    endutent();
    if (!v.empty()) return v;
    for (const auto &s : arch::logind_sessions()) {
        if (s.state == "closing") continue;
        Login l;
        l.user = s.user;
        l.line = s.tty.empty() ? "seat0" : s.tty;
        l.host = s.remote_host;
        l.comment = s.display;
        l.when = s.since;
        v.push_back(l);
    }
    return v;
}

} // namespace

int cmd_users(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]", "Output who is currently logged in according to utmp or systemd-logind.")) return 0;
    std::vector<std::string> names;
    for (const Login &l : current_logins()) names.push_back(l.user);
    std::sort(names.begin(), names.end());
    if (!names.empty()) out(join(names, " ") + "\n");
    return 0;
}

int cmd_who(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [ FILE | ARG1 ARG2 ]", "Print information about users who are currently logged in (utmp, or systemd-logind on Arch/CachyOS).")) return 0;
    bool heading = false, count = false, boot = false, mine = false;
    int c;
    optind = 1;
    static const option lo[] = {{"heading", no_argument, nullptr, 'H'}, {"count", no_argument, nullptr, 'q'}, {"boot", no_argument, nullptr, 'b'}, {"all", no_argument, nullptr, 'a'}, kEnd};
    while ((c = getopt_long(argc, argv, "HqbaummslTwd", lo, nullptr)) != -1) {
        switch (c) {
        case 'H': heading = true; break;
        case 'q': count = true; break;
        case 'b': boot = true; break;
        case 'm': mine = true; break;
        case 'a': case 'u': case 's': case 'l': case 'T': case 'w': case 'd': break;
        default: return 1;
        }
    }
    if (optind + 2 == argc) mine = true;
    if (boot) {
        struct sysinfo si;
        sysinfo(&si);
        out("         system boot  " + format_time(std::time(nullptr) - si.uptime, "%Y-%m-%d %H:%M") + "\n");
        return 0;
    }
    std::vector<Login> logins = current_logins();
    if (mine) {
        const char *tn = ttyname(0);
        const std::string line = tn ? std::string(tn).substr(0, 5) == "/dev/" ? tn + 5 : tn : "";
        logins.erase(std::remove_if(logins.begin(), logins.end(), [&](const Login &l) { return l.line != line; }), logins.end());
    }
    if (count) {
        std::vector<std::string> names;
        for (const auto &l : logins) names.push_back(l.user);
        out(join(names, " ") + "\n# users=" + std::to_string(names.size()) + "\n");
        return 0;
    }
    if (heading) out("NAME     LINE         TIME             COMMENT\n");
    for (const Login &l : logins) {
        char b[512];
        std::string tail = l.host.empty() ? l.comment : "(" + l.host + ")";
        if (!l.host.empty() && l.host[0] == '(') tail = l.host;
        std::snprintf(b, sizeof b, "%-8s %-12s %s %s", l.user.c_str(), l.line.c_str(), format_time(static_cast<time_t>(l.when), "%Y-%m-%d %H:%M").c_str(), tail.empty() ? "" : ("(" + (l.host.empty() ? l.comment : l.host) + ")").c_str());
        std::string line = b;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!l.comment.empty() && l.host.empty()) { line = line.substr(0, line.rfind('(') == std::string::npos ? line.size() : line.rfind('(')); while (!line.empty() && line.back() == ' ') line.pop_back(); line += " (" + l.comment + ")"; }
        out(line + "\n");
    }
    return 0;
}

int cmd_pinky(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [USER]...", "Print information about users who are currently logged in (lightweight finger).")) return 0;
    std::set<std::string> filter;
    for (int i = 1; i < argc; ++i) if (argv[i][0] != '-') filter.insert(argv[i]);
    bool shorter = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "-f") || !std::strcmp(argv[i], "-w") || !std::strcmp(argv[i], "-i")) {}
    (void)shorter;
    out("Login    Name                 TTY      Idle   When             Where\n");
    for (const Login &l : current_logins()) {
        if (!filter.empty() && !filter.count(l.user)) continue;
        const passwd *pw = getpwnam(l.user.c_str());
        std::string gecos = pw && pw->pw_gecos ? pw->pw_gecos : "";
        gecos = gecos.substr(0, gecos.find(','));
        char b[512];
        std::snprintf(b, sizeof b, "%-8s %-20s %-8s        %s %s", l.user.c_str(), gecos.c_str(), l.line.c_str(), format_time(static_cast<time_t>(l.when), "%Y-%m-%d %H:%M").c_str(),
                      (l.host.empty() ? l.comment : l.host).c_str());
        out(std::string(b) + "\n");
    }
    return 0;
}

} // namespace cu
