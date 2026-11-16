// claude-coreutils: a coreutils-compatible multi-call program for Arch Linux and CachyOS.
//
//   claude-coreutils <command> [args...]      run one command
//   ls -l  (via a symlink named "ls")         multi-call, like busybox / GNU `coreutils`
//   claude-coreutils --list | --install DIR | --completions SHELL | --arch-info | --help | --version
#include "arch.hpp"
#include "commands.hpp"
#include "common.hpp"

#include <getopt.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
    const char *name;
    int (*fn)(int, char **);
    const char *summary;
};

const std::vector<Entry> &table()
{
    static const std::vector<Entry> t = {
#define CU_ENTRY(name, ident, summary) {name, cu::cmd_##ident, summary},
        CU_COMMANDS(CU_ENTRY)
#undef CU_ENTRY
    };
    return t;
}

const Entry *find(const std::string &name)
{
    for (const Entry &e : table()) {
        if (name == e.name) {
            return &e;
        }
    }
    return nullptr;
}

int run(const Entry &e, int argc, char **argv)
{
    cu::prog = e.name;
    optind = 1;
    const int rc = e.fn(argc, argv);
    return cu::flush_out() ? rc : (rc ? rc : 1);
}

void usage()
{
    std::printf(
        "Usage: claude-coreutils COMMAND [ARGS]...\n"
        "   or: COMMAND [ARGS]...        (when invoked through a symlink named COMMAND)\n\n"
        "A coreutils-compatible toolbox for Arch Linux and CachyOS.\n\n"
        "Options:\n"
        "  --list                 list all %zu commands\n"
        "  --install DIR          create a symlink for every command in DIR (use --force to replace files)\n"
        "  --uninstall DIR        remove the symlinks created by --install\n"
        "  --completions SHELL    print completions for fish (CachyOS default), bash or zsh\n"
        "  --arch-info            show distro, CPU level, filesystem and logind information\n"
        "  --help, --version\n",
        table().size());
}

int list_commands()
{
    for (const Entry &e : table()) {
        std::printf("%-10s %s\n", e.name, e.summary);
    }
    return 0;
}

int completions(const std::string &shell)
{
    std::string names;
    for (const Entry &e : table()) {
        names += e.name;
        names += ' ';
    }
    if (shell == "fish") {
        std::printf("# fish completions for claude-coreutils (install to /usr/share/fish/vendor_completions.d/)\n");
        std::printf("complete -c claude-coreutils -f -n '__fish_use_subcommand' -a '%s'\n", names.c_str());
        for (const Entry &e : table()) {
            std::printf("complete -c claude-coreutils -f -n '__fish_seen_subcommand_from %s' -l help -l version\n", e.name);
        }
    } else if (shell == "bash") {
        std::printf("_claude_coreutils() {\n"
                    "    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                    "    if [ \"$COMP_CWORD\" -eq 1 ]; then\n"
                    "        COMPREPLY=($(compgen -W \"%s --list --install --uninstall --completions --arch-info --help --version\" -- \"$cur\"))\n"
                    "    else\n"
                    "        COMPREPLY=($(compgen -f -- \"$cur\"))\n"
                    "    fi\n"
                    "}\ncomplete -F _claude_coreutils claude-coreutils\n",
                    names.c_str());
    } else if (shell == "zsh") {
        std::printf("#compdef claude-coreutils\n_arguments '1:command:(%s)' '*:file:_files'\n", names.c_str());
    } else {
        std::fprintf(stderr, "claude-coreutils: unknown shell '%s' (use fish, bash or zsh)\n", shell.c_str());
        return 2;
    }
    return 0;
}

int install_links(const std::string &dir, bool remove, bool force)
{
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof self - 1);
    if (n < 0) {
        std::fprintf(stderr, "claude-coreutils: cannot find my own path\n");
        return 1;
    }
    self[n] = '\0';
    int rc = 0;
    std::error_code ec;
    if (!remove) {
        fs::create_directories(dir, ec);
    }
    for (const Entry &e : table()) {
        const fs::path link = fs::path(dir) / e.name;
        if (remove) {
            if (fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == fs::path(self)) {
                fs::remove(link, ec);
            }
            continue;
        }
        if (fs::exists(fs::symlink_status(link, ec))) {
            if (!force) {
                std::fprintf(stderr, "claude-coreutils: %s exists (use --force to replace it)\n", link.c_str());
                rc = 1;
                continue;
            }
            fs::remove(link, ec);
        }
        fs::create_symlink(self, link, ec);
        if (ec) {
            std::fprintf(stderr, "claude-coreutils: cannot create %s: %s\n", link.c_str(), ec.message().c_str());
            rc = 1;
        }
    }
    return rc;
}

int arch_info()
{
    using namespace cu::arch;
    std::printf("Distribution : %s\n", distro_name().c_str());
    std::printf("Arch family  : %s%s\n", is_arch_family() ? "yes" : "no", is_cachyos() ? " (CachyOS)" : "");
    const std::string level = x86_level();
    if (!level.empty()) {
        std::printf("CPU level    : %s\n", level.c_str());
    }
    std::printf("Usable CPUs  : %d\n", usable_cpus());
    for (const char *p : {"/", "/home", "/boot"}) {
        struct stat st;
        if (::stat(p, &st) != 0) {
            continue;
        }
        for (const Mount &m : mounts()) {
            if (m.target == p) {
                std::printf("%-13s: %s on %s%s\n", p, m.fstype.c_str(), m.source.c_str(),
                            m.root != "/" ? (" (subvolume " + m.root + ")").c_str() : "");
            }
        }
    }
    std::printf("Init         : %s\n", logind_running() ? "systemd-logind" : "no logind");
    std::printf("pacman       : %s\n", ::access("/usr/bin/pacman", X_OK) == 0 ? "installed" : "not installed");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::setlocale(LC_ALL, "");
    std::string self = fs::path(argv[0]).filename().string();

    // multi-call: invoked as "ls", "cp", ...
    if (self != "claude-coreutils" && self != "coreutils") {
        if (const Entry *e = find(self)) {
            return run(*e, argc, argv);
        }
    }

    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string a1 = argv[1];
    if (a1 == "--help" || a1 == "-h") {
        usage();
        return 0;
    }
    if (a1 == "--version") {
        std::printf("claude-coreutils %s\n", CU_VERSION);
        return 0;
    }
    if (a1 == "--list") {
        return list_commands();
    }
    if (a1 == "--arch-info") {
        return arch_info();
    }
    if (a1 == "--completions" && argc >= 3) {
        return completions(argv[2]);
    }
    if ((a1 == "--install" || a1 == "--uninstall") && argc >= 3) {
        const bool force = argc >= 4 && std::strcmp(argv[3], "--force") == 0;
        return install_links(argv[2], a1 == "--uninstall", force);
    }

    if (const Entry *e = find(a1)) {
        return run(*e, argc - 1, argv + 1);
    }
    std::fprintf(stderr, "claude-coreutils: unknown command '%s'\nTry 'claude-coreutils --list'.\n", a1.c_str());
    return 127;
}
