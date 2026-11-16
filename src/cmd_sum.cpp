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
// Checksum utilities: md5sum, sha*sum, b2sum, cksum, sum.
#include "commands.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>

namespace cu {

namespace {

const option kEnd = {nullptr, 0, nullptr, 0};

struct Algo {
    const char *name;     // tag name: "MD5", "SHA256", "BLAKE2b"
    const char *evp;      // OpenSSL name
    size_t bits;          // digest bits (b2sum --length may shrink it)
};

std::string hex(const unsigned char *d, size_t n)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}

bool digest_fd(int fd, const Algo &a, size_t bits, std::string &result)
{
    const EVP_MD *md = EVP_get_digestbyname(a.evp);
    if (!md) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) return false;
    if (std::strcmp(a.name, "BLAKE2b") == 0 && bits != 512) {
        size_t sz = bits / 8;
        OSSL_PARAM params[2] = {OSSL_PARAM_construct_size_t("size", &sz), OSSL_PARAM_construct_end()};
        if (EVP_MD_CTX_set_params(ctx.get(), params) != 1) return false;
    }
    char buf[1 << 16];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) break;
        EVP_DigestUpdate(ctx.get(), buf, static_cast<size_t>(n));
    }
    unsigned char out_buf[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    EVP_DigestFinal_ex(ctx.get(), out_buf, &len);
    result = hex(out_buf, std::min<size_t>(len, bits / 8));
    return true;
}

bool digest_path(const std::string &path, const Algo &a, size_t bits, std::string &result)
{
    if (path == "-") return digest_fd(0, a, bits, result);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); errno = EISDIR; return false; }
    const bool ok = digest_fd(fd, a, bits, result);
    const int e = errno;
    ::close(fd);
    errno = e;
    return ok;
}

std::string escape_name(const std::string &n, bool &escaped)
{
    std::string o;
    escaped = false;
    for (char c : n) {
        if (c == '\\') { o += "\\\\"; escaped = true; }
        else if (c == '\n') { o += "\\n"; escaped = true; }
        else if (c == '\r') { o += "\\r"; escaped = true; }
        else o += c;
    }
    return o;
}

std::string unescape_name(const std::string &n)
{
    std::string o;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '\\' && i + 1 < n.size()) {
            ++i;
            o += n[i] == 'n' ? '\n' : (n[i] == 'r' ? '\r' : n[i]);
        } else o += n[i];
    }
    return o;
}

int digest_main(int argc, char **argv, Algo algo, const char *summary)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", summary)) return 0;
    static const option lo[] = {{"binary", no_argument, nullptr, 'b'}, {"check", no_argument, nullptr, 'c'}, {"tag", no_argument, nullptr, 1},
                                {"text", no_argument, nullptr, 't'}, {"zero", no_argument, nullptr, 'z'}, {"ignore-missing", no_argument, nullptr, 2},
                                {"quiet", no_argument, nullptr, 3}, {"status", no_argument, nullptr, 4}, {"strict", no_argument, nullptr, 5},
                                {"warn", no_argument, nullptr, 'w'}, {"length", required_argument, nullptr, 'l'}, kEnd};
    bool check = false, tag = false, zero = false, ignore_missing = false, quiet = false, status = false, strict = false, warn = false, binary = false;
    size_t bits = algo.bits;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "bctzwl:", lo, nullptr)) != -1) {
        switch (c) {
        case 'b': binary = true; break;
        case 'c': check = true; break;
        case 1: tag = true; break;
        case 't': break;
        case 'z': zero = true; break;
        case 2: ignore_missing = true; break;
        case 3: quiet = true; break;
        case 4: status = true; break;
        case 5: strict = true; break;
        case 'w': warn = true; break;
        case 'l': {
            uint64_t v;
            if (std::strcmp(algo.name, "BLAKE2b") != 0 || !parse_u64(optarg, v) || v == 0 || v > 512 || v % 8) { err(std::string("invalid length: '") + optarg + "'"); return 1; }
            bits = static_cast<size_t>(v);
            break;
        }
        default: return 1;
        }
    }
    std::vector<std::string> files(argv + optind, argv + argc);
    if (files.empty()) files.push_back("-");
    int rc = 0;
    if (!check) {
        for (const std::string &f : files) {
            std::string d;
            if (!digest_path(f, algo, bits, d)) { err_errno(f); rc = 1; continue; }
            bool esc;
            const std::string name = zero ? f : escape_name(f, esc);
            if (tag) {
                std::string label = algo.name;
                if (std::strcmp(algo.name, "BLAKE2b") == 0 && bits != 512) label += "-" + std::to_string(bits);
                out((zero ? "" : (esc ? "\\" : "")) + label + " (" + name + ") = " + d + (zero ? '\0' : '\n'));
            } else {
                out(std::string(zero ? "" : (esc ? "\\" : "")) + d + (binary ? " *" : "  ") + name + (zero ? '\0' : '\n'));
            }
        }
        return rc;
    }
    size_t bad_format = 0, failed = 0, unreadable = 0, ok_count = 0;
    for (const std::string &list : files) {
        std::string data;
        if (!read_file(list, data)) { err_errno(list); rc = 1; continue; }
        size_t lineno = 0, matched = 0;
        for (const std::string &line : split_lines(data, zero ? '\0' : '\n')) {
            ++lineno;
            if (line.empty() || line[0] == '#') continue;
            std::string want, name;
            bool esc = !line.empty() && line[0] == '\\';
            const std::string l = esc ? line.substr(1) : line;
            const size_t paren = l.find(" (");
            const size_t close = l.rfind(") = ");
            if (paren != std::string::npos && close != std::string::npos && l.compare(0, std::strlen(algo.name), algo.name) == 0 && close > paren) {
                name = l.substr(paren + 2, close - paren - 2);
                want = l.substr(close + 4);
            } else {
                const size_t sp = l.find(' ');
                if (sp == std::string::npos || sp + 2 > l.size() || (l[sp + 1] != ' ' && l[sp + 1] != '*')) {
                    ++bad_format;
                    if (warn) err(list + ": " + std::to_string(lineno) + ": improperly formatted checksum line");
                    continue;
                }
                want = l.substr(0, sp);
                name = l.substr(sp + 2);
            }
            bool all_hex = !want.empty();
            for (char h : want) if (!std::isxdigit(static_cast<unsigned char>(h))) all_hex = false;
            if (!all_hex || want.size() % 2) {
                ++bad_format;
                if (warn) err(list + ": " + std::to_string(lineno) + ": improperly formatted checksum line");
                continue;
            }
            if (esc) name = unescape_name(name);
            ++matched;
            std::string got;
            if (!digest_path(name, algo, want.size() * 4, got)) {
                if (ignore_missing && errno == ENOENT) { --matched; continue; }
                ++unreadable;
                if (!status) { err_errno(name); out(name + ": FAILED open or read\n"); }
                continue;
            }
            for (char &h : want) h = static_cast<char>(std::tolower(static_cast<unsigned char>(h)));
            if (got == want) {
                ++ok_count;
                if (!quiet && !status) out(name + ": OK\n");
            } else {
                ++failed;
                if (!status) out(name + ": FAILED\n");
            }
        }
        if (matched == 0 && ok_count == 0 && failed == 0 && unreadable == 0) {
            err(list + ": no properly formatted checksum lines found");
            rc = 1;
        }
    }
    if (!status) {
        if (bad_format) err("WARNING: " + std::to_string(bad_format) + " line" + (bad_format > 1 ? "s are" : " is") + " improperly formatted");
        if (unreadable) err("WARNING: " + std::to_string(unreadable) + " listed file" + (unreadable > 1 ? "s" : "") + " could not be read");
        if (failed) err("WARNING: " + std::to_string(failed) + " computed checksum" + (failed > 1 ? "s" : "") + " did NOT match");
    }
    if (failed || unreadable || (strict && bad_format)) rc = 1;
    return rc;
}

// ---- POSIX cksum (CRC-32 variant) and BSD/SysV sums -------------------------------------------------
uint32_t g_crc_table[256];
bool g_crc_ready = false;

void init_crc()
{
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

struct SumResult { uint64_t value = 0; uint64_t bytes = 0; uint64_t blocks = 0; };

bool crc_of(const std::string &data, SumResult &r)
{
    init_crc();
    uint32_t crc = 0;
    for (unsigned char ch : data) crc = (crc << 8) ^ g_crc_table[((crc >> 24) ^ ch) & 0xFF];
    uint64_t len = data.size();
    while (len) { crc = (crc << 8) ^ g_crc_table[((crc >> 24) ^ (len & 0xFF)) & 0xFF]; len >>= 8; }
    r.value = ~crc & 0xFFFFFFFFu;
    r.bytes = data.size();
    return true;
}

} // namespace

int cmd_md5sum(int argc, char **argv) { return digest_main(argc, argv, {"MD5", "MD5", 128}, "Print or check MD5 (128-bit) checksums."); }
int cmd_sha1sum(int argc, char **argv) { return digest_main(argc, argv, {"SHA1", "SHA1", 160}, "Print or check SHA1 (160-bit) checksums."); }
int cmd_sha224sum(int argc, char **argv) { return digest_main(argc, argv, {"SHA224", "SHA224", 224}, "Print or check SHA224 (224-bit) checksums."); }
int cmd_sha256sum(int argc, char **argv) { return digest_main(argc, argv, {"SHA256", "SHA256", 256}, "Print or check SHA256 (256-bit) checksums."); }
int cmd_sha384sum(int argc, char **argv) { return digest_main(argc, argv, {"SHA384", "SHA384", 384}, "Print or check SHA384 (384-bit) checksums."); }
int cmd_sha512sum(int argc, char **argv) { return digest_main(argc, argv, {"SHA512", "SHA512", 512}, "Print or check SHA512 (512-bit) checksums."); }
int cmd_b2sum(int argc, char **argv) { return digest_main(argc, argv, {"BLAKE2b", "BLAKE2b512", 512}, "Print or check BLAKE2b (512-bit) checksums."); }

int cmd_cksum(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Print or verify checksums. By default use the 32 bit CRC algorithm.")) return 0;
    static const option lo[] = {{"algorithm", required_argument, nullptr, 'a'}, {"untagged", no_argument, nullptr, 1}, {"tag", no_argument, nullptr, 2},
                                {"check", no_argument, nullptr, 'c'}, {"length", required_argument, nullptr, 'l'}, {"zero", no_argument, nullptr, 'z'}, kEnd};
    std::string algo = "crc";
    bool untagged = false;
    int c;
    optind = 1;
    std::vector<std::string> pass;
    while ((c = getopt_long(argc, argv, "a:cl:z", lo, nullptr)) != -1) {
        switch (c) {
        case 'a': algo = optarg; break;
        case 1: untagged = true; break;
        case 2: break;
        case 'c': pass.push_back("-c"); break;
        case 'l': pass.push_back("-l"); pass.push_back(optarg); break;
        case 'z': pass.push_back("-z"); break;
        default: return 1;
        }
    }
    struct Map { const char *name; int (*fn)(int, char **); };
    static const Map maps[] = {{"md5", cmd_md5sum}, {"sha1", cmd_sha1sum}, {"sha224", cmd_sha224sum}, {"sha256", cmd_sha256sum},
                               {"sha384", cmd_sha384sum}, {"sha512", cmd_sha512sum}, {"blake2b", cmd_b2sum}};
    for (const Map &m : maps) {
        if (algo == m.name) {
            std::vector<std::string> av = {prog};
            if (!untagged) av.push_back("--tag");
            av.insert(av.end(), pass.begin(), pass.end());
            for (int i = optind; i < argc; ++i) av.push_back(argv[i]);
            std::vector<char *> ptrs;
            for (auto &s : av) ptrs.push_back(s.data());
            optind = 1;
            return m.fn(static_cast<int>(ptrs.size()), ptrs.data());
        }
    }
    const bool is_crc = algo == "crc", is_sysv = algo == "sysv", is_bsd = algo == "bsd";
    if (!is_crc && !is_sysv && !is_bsd) { err("invalid argument '" + algo + "' for '--algorithm'"); return 1; }
    int rc = 0;
    std::vector<std::string> files(argv + optind, argv + argc);
    if (files.empty()) files.push_back("-");
    for (const std::string &f : files) {
        std::string data;
        if (!read_file(f, data)) { err_errno(f); rc = 1; continue; }
        SumResult r;
        if (is_crc) crc_of(data, r);
        else if (is_sysv) {
            uint64_t s = 0;
            for (unsigned char ch : data) s += ch;
            s = (s & 0xFFFF) + (s >> 16);
            s = (s & 0xFFFF) + (s >> 16);
            r.value = s; r.blocks = (data.size() + 511) / 512;
        } else {
            uint32_t s = 0;
            for (unsigned char ch : data) { s = (s >> 1) + ((s & 1) << 15); s += ch; s &= 0xFFFF; }
            r.value = s; r.blocks = (data.size() + 1023) / 1024;
        }
        const std::string name = f == "-" ? "" : f;
        if (is_crc) out(std::to_string(r.value) + " " + std::to_string(r.bytes) + (name.empty() ? "" : " " + name) + "\n");
        else out(std::to_string(r.value) + " " + std::to_string(r.blocks) + (name.empty() ? "" : " " + name) + "\n");
    }
    return rc;
}

int cmd_sum(int argc, char **argv)
{
    if (handle_help_version(argc, argv, "[OPTION]... [FILE]...", "Print checksum and block counts for each FILE (BSD algorithm by default).")) return 0;
    bool sysv = false;
    int c;
    optind = 1;
    static const option lo[] = {{"sysv", no_argument, nullptr, 's'}, kEnd};
    while ((c = getopt_long(argc, argv, "rs", lo, nullptr)) != -1) {
        if (c == 's') sysv = true;
        else if (c == 'r') sysv = false;
        else return 1;
    }
    std::vector<std::string> files(argv + optind, argv + argc);
    const bool multiple = files.size() > 1;
    if (files.empty()) files.push_back("-");
    int rc = 0;
    for (const std::string &f : files) {
        std::string data;
        if (!read_file(f, data)) { err_errno(f); rc = 1; continue; }
        char b[128];
        if (sysv) {
            uint64_t s = 0;
            for (unsigned char ch : data) s += ch;
            s = (s & 0xFFFF) + (s >> 16);
            s = (s & 0xFFFF) + (s >> 16);
            std::snprintf(b, sizeof b, "%llu %llu", (unsigned long long)s, (unsigned long long)((data.size() + 511) / 512));
        } else {
            uint32_t s = 0;
            for (unsigned char ch : data) { s = (s >> 1) + ((s & 1) << 15); s += ch; s &= 0xFFFF; }
            std::snprintf(b, sizeof b, "%05u %5llu", s, (unsigned long long)((data.size() + 1023) / 1024));
        }
        out(std::string(b) + ((f != "-" && (multiple || sysv)) ? " " + f : (f != "-" ? " " + f : "")) + "\n");
    }
    return rc;
}

} // namespace cu
