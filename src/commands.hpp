// The command table. X(name, function-suffix, one-line summary).
#pragma once

#define CU_COMMANDS(X) \
    X("[", lbracket, "evaluate a conditional expression") \
    X("claude-chroot", claude_chroot, "enter an Arch chroot with /proc /sys /dev /run /tmp and resolv.conf mounted (private mounts)") \
    X("archroot", archroot, "enter your Arch chroot; asks where it is if it cannot be found") \
    X("arch", arch, "print machine architecture (and the CachyOS x86-64 level with --level)") \
    X("b2sum", b2sum, "compute and check BLAKE2b message digests") \
    X("base32", base32, "base32 encode/decode data") \
    X("base64", base64, "base64 encode/decode data") \
    X("basename", basename, "strip directory and suffix from file names") \
    X("basenc", basenc, "encode/decode data (base64, base32, base16, z85, ...)") \
    X("cat", cat, "concatenate files and print on the standard output") \
    X("chcon", chcon, "change file security context") \
    X("chgrp", chgrp, "change group ownership") \
    X("chmod", chmod, "change file mode bits") \
    X("chown", chown, "change file owner and group") \
    X("chroot", chroot, "run command or interactive shell with special root directory") \
    X("cksum", cksum, "compute and verify CRC / digest checksums") \
    X("comm", comm, "compare two sorted files line by line") \
    X("cp", cp, "copy files and directories (reflink aware on btrfs/xfs/bcachefs/zfs)") \
    X("csplit", csplit, "split a file into context-determined pieces") \
    X("cut", cut, "remove sections from each line of files") \
    X("date", date, "print or set the system date and time") \
    X("dd", dd, "convert and copy a file") \
    X("df", df, "report file system space usage (btrfs/zfs/bcachefs aware)") \
    X("dir", dir, "list directory contents (ls -C -b)") \
    X("dircolors", dircolors, "color setup for ls") \
    X("dirname", dirname, "strip last component from file name") \
    X("du", du, "estimate file space usage") \
    X("echo", echo, "display a line of text") \
    X("env", env, "run a program in a modified environment") \
    X("expand", expand, "convert tabs to spaces") \
    X("expr", expr, "evaluate expressions") \
    X("factor", factor, "factor numbers") \
    X("false", false, "do nothing, unsuccessfully") \
    X("fetch", fetch, "print a fastfetch-style system summary (OS, CPU, GPU, memory, disk)") \
    X("fmt", fmt, "simple optimal text formatter") \
    X("fold", fold, "wrap each input line to fit in specified width") \
    X("groups", groups, "print the groups a user is in") \
    X("head", head, "output the first part of files") \
    X("hostid", hostid, "print the numeric identifier for the current host") \
    X("hostname", hostname, "show or set the system host name") \
    X("id", id, "print real and effective user and group IDs") \
    X("install", install, "copy files and set attributes") \
    X("join", join, "join lines of two files on a common field") \
    X("kill", kill, "send a signal to processes") \
    X("link", link, "call the link function to create a link to a file") \
    X("ln", ln, "make links between files") \
    X("logname", logname, "print user's login name") \
    X("ls", ls, "list directory contents") \
    X("md5sum", md5sum, "compute and check MD5 message digests") \
    X("mkdir", mkdir, "make directories") \
    X("mkfifo", mkfifo, "make FIFOs (named pipes)") \
    X("mknod", mknod, "make block or character special files") \
    X("mktemp", mktemp, "create a temporary file or directory") \
    X("mv", mv, "move (rename) files") \
    X("nice", nice, "run a program with modified scheduling priority") \
    X("nl", nl, "number lines of files") \
    X("nohup", nohup, "run a command immune to hangups") \
    X("nproc", nproc, "print the number of processing units (cgroup aware)") \
    X("numfmt", numfmt, "convert numbers from/to human-readable strings") \
    X("od", od, "dump files in octal and other formats") \
    X("paste", paste, "merge lines of files") \
    X("pathchk", pathchk, "check whether file names are valid or portable") \
    X("pinky", pinky, "lightweight finger") \
    X("pr", pr, "convert text files for printing") \
    X("printenv", printenv, "print all or part of environment") \
    X("printf", printf, "format and print data") \
    X("ptx", ptx, "produce a permuted index of file contents") \
    X("pwd", pwd, "print name of current/working directory") \
    X("readlink", readlink, "print resolved symbolic links or canonical file names") \
    X("realpath", realpath, "print the resolved path") \
    X("rm", rm, "remove files or directories") \
    X("rmdir", rmdir, "remove empty directories") \
    X("runcon", runcon, "run command with specified security context") \
    X("seq", seq, "print a sequence of numbers") \
    X("sha1sum", sha1sum, "compute and check SHA1 message digests") \
    X("sha224sum", sha224sum, "compute and check SHA224 message digests") \
    X("sha256sum", sha256sum, "compute and check SHA256 message digests") \
    X("sha384sum", sha384sum, "compute and check SHA384 message digests") \
    X("sha512sum", sha512sum, "compute and check SHA512 message digests") \
    X("shred", shred, "overwrite a file to hide its contents, and optionally delete it") \
    X("shuf", shuf, "generate random permutations") \
    X("sleep", sleep, "delay for a specified amount of time") \
    X("sort", sort, "sort lines of text files") \
    X("split", split, "split a file into pieces") \
    X("stat", stat, "display file or file system status") \
    X("stdbuf", stdbuf, "run COMMAND, with modified buffering operations for its standard streams") \
    X("stty", stty, "change and print terminal line settings") \
    X("sum", sum, "checksum and count the blocks in a file") \
    X("sync", sync, "synchronize cached writes to persistent storage") \
    X("tac", tac, "concatenate and print files in reverse") \
    X("tail", tail, "output the last part of files") \
    X("tee", tee, "read from standard input and write to standard output and files") \
    X("test", test, "check file types and compare values") \
    X("timeout", timeout, "run a command with a time limit") \
    X("touch", touch, "change file timestamps") \
    X("tr", tr, "translate or delete characters") \
    X("true", true, "do nothing, successfully") \
    X("truncate", truncate, "shrink or extend the size of a file to the specified size") \
    X("tsort", tsort, "perform topological sort") \
    X("tty", tty, "print the file name of the terminal connected to standard input") \
    X("uname", uname, "print system information (shows Arch/CachyOS with -o)") \
    X("unexpand", unexpand, "convert spaces to tabs") \
    X("uniq", uniq, "report or omit repeated lines") \
    X("unlink", unlink, "call the unlink function to remove the specified file") \
    X("uptime", uptime, "tell how long the system has been running") \
    X("users", users, "print the user names of users currently logged in (utmp or systemd-logind)") \
    X("vdir", vdir, "list directory contents (ls -l -b)") \
    X("wc", wc, "print newline, word, and byte counts for each file") \
    X("who", who, "show who is logged on (utmp or systemd-logind)") \
    X("whoami", whoami, "print effective user name") \
    X("yes", yes, "output a string repeatedly until killed")

namespace cu {
#define CU_DECLARE(name, ident, summary) int cmd_##ident(int argc, char **argv);
CU_COMMANDS(CU_DECLARE)
#undef CU_DECLARE
} // namespace cu
