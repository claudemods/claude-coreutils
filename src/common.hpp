// claude-coreutils: shared helpers used by every command.
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef CU_VERSION
#define CU_VERSION "2.0.0"
#endif

namespace cu {

/// Name of the command being run (used in diagnostics: "ls: cannot access ...").
extern std::string prog;

void err(const std::string &msg);                     ///< "prog: msg"
void err_errno(const std::string &what, int e = -1);  ///< "prog: what: strerror(errno)"
void usage_error(const std::string &msg);             ///< "prog: msg" + "Try 'prog --help'"

/// Handles a lone --help / --version argument. Returns true when it printed something.
bool handle_help_version(int argc, char **argv, const char *usage, const char *summary);

// --- I/O ---------------------------------------------------------------------------------------
bool read_fd(int fd, std::string &out);
bool read_file(const std::string &path, std::string &out); ///< "-" reads stdin
bool write_all(int fd, const char *data, size_t n);
inline bool write_all(int fd, const std::string &s) { return write_all(fd, s.data(), s.size()); }
void out(const std::string &s);                            ///< to stdout (buffered)
bool flush_out();                                          ///< false + diagnostic on write error

/// Calls `fn(line, had_newline)` for every line of `data`.
std::vector<std::string> split_lines(const std::string &data, char sep = '\n');

// --- strings / numbers -------------------------------------------------------------------------
std::string strip_trailing_slashes(std::string s);
bool parse_u64(const std::string &s, uint64_t &out, int base = 10);
bool parse_i64(const std::string &s, int64_t &out);
bool parse_double(const std::string &s, double &out);
/// "10K", "2M", "1GiB", "3kB" ... -> bytes
bool parse_size(const std::string &s, uint64_t &out);
std::string human_size(double bytes, bool si = false);
std::string join(const std::vector<std::string> &v, const std::string &sep);
/// Backslash escapes as used by echo -e / printf (\n \t \xHH \0NNN \uHHHH ...); stops at \c.
std::string unescape(const std::string &s, bool *stop = nullptr);
std::string quote_shell(const std::string &s);
std::string format_time(time_t t, const std::string &fmt, bool utc = false);
/// Date strings as accepted by `date -d` / `touch -d`: @EPOCH, now, today, yesterday, tomorrow,
/// "2 hours ago", "+3 days", YYYY-MM-DD, "YYYY-MM-DD HH:MM[:SS]", ISO 8601 with Z or +HH:MM, HH:MM.
bool parse_date(const std::string &s, time_t now, time_t &out);

// --- users / groups / modes --------------------------------------------------------------------
bool lookup_user(const std::string &name, uid_t &uid);
bool lookup_group(const std::string &name, gid_t &gid);
std::string user_name(uid_t uid);   ///< name or the number
std::string group_name(gid_t gid);
/// chmod style mode ("755", "u+rwx,go-w", "a=rx", ...). `cur` is the current mode.
bool parse_mode(const std::string &spec, mode_t cur, bool is_dir, mode_t &out);
std::string mode_string(mode_t m);  ///< "-rwxr-xr-x"

/// Built-in default for LS_COLORS (same database as `dircolors -p`, plus Arch package files).
extern const char *const kDefaultLsColors;

// --- misc --------------------------------------------------------------------------------------
std::string readlink_str(const std::string &path);
std::string canonicalize(const std::string &path, bool must_exist);

} // namespace cu
