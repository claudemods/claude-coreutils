# claude-coreutils

A single multi-call binary (like busybox) that implements all 108 GNU coreutils commands plus three extras (`fetch`, `arch-chroot`, `archroot`), built for **Arch Linux and CachyOS**.

> Status: it compiles and links on CachyOS (GCC 16). It has had little runtime testing, so expect bugs. `arch-chroot` and `archroot` have not been run as root yet.

## Build and install

```bash
sudo pacman -S --needed base-devel cmake openssl
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCU_MARCH=x86-64-v3
cmake --build build -j
sudo cmake --install build
```

Use `-DCU_NATIVE=ON` for a build tuned to your own CPU. Use `-DCU_MARCH=x86-64-v4` for AVX-512 machines.

Package it with `cd packaging && makepkg -si`.

## Use

```bash
claude-coreutils ls -lah            # run a command by name
claude-coreutils --list             # list all commands
claude-coreutils --install ~/bin    # symlink every command (use --force to replace)
claude-coreutils --uninstall ~/bin
claude-coreutils --completions fish # fish (CachyOS default), bash or zsh
claude-coreutils --arch-info        # distro, x86-64 level, btrfs layout, logind
```

When a symlink is named `ls`, `cp`, `sha256sum` and so on, the binary behaves as that command.

## Arch / CachyOS specifics

| Area | Behaviour |
|---|---|
| `cp`, `mv` | Reflink first (`FICLONE`, instant on btrfs, xfs, bcachefs, zfs), then `copy_file_range`, then read/write. |
| `shred` | Warns on copy-on-write filesystems (btrfs, zfs, bcachefs, f2fs) where overwriting in place does not work. |
| `df` | Hides pseudo filesystems. Counts btrfs/bcachefs subvolume mounts once. |
| `who`, `users`, `pinky`, `logname` | Arch has no utmp by default, so they fall back to systemd-logind sessions. |
| `arch --level` | Prints the CPU level (`x86-64-v1` to `v4`) that CachyOS repos target. |
| `uname --distro` | Prints the PRETTY_NAME from os-release. `uname -o` stays `GNU/Linux` for script compatibility. |
| `nproc` | Honours CPU affinity and the cgroup v2 `cpu.max` quota (containers). |
| `ls --color`, `dircolors` | Colours for `*.pkg.tar.zst`, `*.pkg.tar.xz` and PKGBUILD files. `dircolors` emits fish syntax when `$SHELL` is fish. |
| `chroot` | Works as the base for `arch-chroot`-style scripts. Runs `$SHELL` when no command is given. |
| `arch-chroot` | Sets up `/proc`, `/sys`, `/dev`, `/run`, `/tmp` and `resolv.conf`, then chroots. Mounts are private to the chroot and vanish when it exits. |
| `stdbuf` | Finds `/usr/lib/coreutils/libstdbuf.so` from the Arch `coreutils` package. |

## Extra commands

| Command | What it does |
|---|---|
| `fetch` | fastfetch-style summary: OS, kernel, uptime, pacman packages, shell, desktop, terminal, CPU and x86-64 level, GPU, memory, swap, disk. `--no-logo`, `--no-color`. |
| `arch-chroot [-u USER[:GROUP]] DIR [COMMAND...]` | Enter an Arch chroot (root only). Runs `/bin/bash` when no command is given. |
| `archroot [-r DIR] [COMMAND...]` | Enters your Arch chroot. Looks in `-r DIR`, `$ARCH_ROOT`, the saved location (`/etc/claude-coreutils/archroot`), then `./arch-root`. If none is found it asks where the folder is and offers to remember it. |

## Build an Arch system in a folder

`setup-arch-root.sh` installs Arch into a folder with `pacstrap`, compiles claude-coreutils inside it, removes GNU coreutils and installs this one in its place:

```bash
sudo ./setup-arch-root.sh [ROOT_DIR] [MARCH]   # defaults: ./arch-root  x86-64-v3
sudo claude-coreutils archroot                 # then enter it
```

It needs `arch-install-scripts` on the host. The host's own coreutils is never touched. `stdbuf` does not work inside the result, because it needs the GNU package's `libstdbuf.so`.

## Commands

Run `claude-coreutils --list` for the full list with descriptions. It covers every GNU coreutils command except the SELinux-only extras, which are implemented through xattrs (`chcon`) or report that SELinux is missing (`runcon`).
