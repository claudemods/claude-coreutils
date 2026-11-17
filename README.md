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

## Install it as the coreutils of an Arch system in a folder

This builds an Arch system in a folder, compiles claude-coreutils inside it with its own `arch-chroot`, swaps GNU coreutils for it, and drops you into the result. Only `pacstrap` (from `arch-install-scripts`) is needed from the host, to create the base system. Run these from the project folder, after building once as shown in "Build and install" (`cmake --build build`):

```bash
# 1. a normal Arch system (it still has GNU coreutils, which the compiler needs)
sudo pacstrap -K arch-root base base-devel cmake openssl

# 2. copy the sources in and compile them INSIDE the chroot, using the arch-chroot of the binary you built on the host
sudo mkdir -p arch-root/build/src
sudo cp -r CMakeLists.txt src completions arch-root/build/src/
sudo ./build/claude-coreutils arch-chroot arch-root cmake -S /build/src -B /build/out -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCU_MARCH=x86-64-v3
sudo ./build/claude-coreutils arch-chroot arch-root cmake --build /build/out -j
sudo ./build/claude-coreutils arch-chroot arch-root cmake --install /build/out

# 3. put a symlink per command in /usr/local/bin first
sudo ./build/claude-coreutils arch-chroot arch-root claude-coreutils --install /usr/local/bin --force

# 4. keep pacman from reinstalling GNU coreutils, then remove it
sudo sed -i '/^\[options\]/a IgnorePkg = coreutils' arch-root/etc/pacman.conf
sudo ./build/claude-coreutils arch-chroot arch-root pacman -Rdd --noconfirm coreutils

# 5. replace the GNU files in /usr/bin with links to claude-coreutils
sudo ./build/claude-coreutils arch-chroot arch-root claude-coreutils --install /usr/bin --force
sudo ./build/claude-coreutils arch-chroot arch-root claude-coreutils --uninstall /usr/local/bin

# 6. enter the chroot
sudo ./build/claude-coreutils archroot -r arch-root
```

What each step does:

1. `pacstrap` makes an ordinary Arch system. GNU coreutils stays for now, because the compiler and build tools need it.
2. The sources are compiled inside the chroot with cmake (prefix `/usr`), producing a single multi-call binary at `/usr/bin/claude-coreutils`. `arch-chroot` is our own command: it sets up `/proc`, `/sys`, `/dev`, `/run`, `/tmp` and `resolv.conf`, then chroots.
3. `claude-coreutils --install DIR` makes one symlink per command (`ls`, `cp`, `mv`, ...) pointing at that binary, which acts as whichever command it is called as. They go into `/usr/local/bin` first because it comes before `/usr/bin` in `PATH`, so package scripts keep working while GNU coreutils is removed.
4. `pacman -Rdd coreutils` removes the GNU package (`-dd` because other packages depend on it). `IgnorePkg` stops pacman from putting it back on upgrades.
5. The links are created again in `/usr/bin`, replacing the GNU files, and the temporary ones are removed.
6. `archroot` enters the chroot. After this first time, `sudo claude-coreutils archroot` finds it again (or asks where it is).

The host's own coreutils is never touched. `stdbuf` does not work inside the result, because it needs the GNU package's `libstdbuf.so`. These steps have not been run as root yet.

## Commands

Run `claude-coreutils --list` for the full list with descriptions. It covers every GNU coreutils command except the SELinux-only extras, which are implemented through xattrs (`chcon`) or report that SELinux is missing (`runcon`).
