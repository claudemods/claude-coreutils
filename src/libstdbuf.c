/* libstdbuf.so: LD_PRELOAD shim used by `stdbuf`. Same contract as GNU coreutils' libstdbuf:
 *   _STDBUF_I / _STDBUF_O / _STDBUF_E hold the mode for stdin / stdout / stderr:
 *     "L"   line buffered
 *     "0"   unbuffered
 *     SIZE  fully buffered with that many bytes (suffixes K, M, G, T, P, E, Z, Y, with optional B / iB)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_size(const char *s, size_t *out)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s) return -1;
    unsigned long long mult = 1;
    int base = 1024;
    if (*end) {
        const char *units = "KMGTPEZY";
        const char *p = strchr(units, *end);
        if (!p || !*end) return -1;
        size_t n = (size_t)(p - units) + 1;
        const char *rest = end + 1;
        if (strcmp(rest, "B") == 0) base = 1000;
        else if (*rest && strcmp(rest, "iB") != 0) return -1;
        for (size_t i = 0; i < n && i < 6; ++i) mult *= (unsigned long long)base;   /* up to E; beyond overflows anyway */
    }
    *out = (size_t)(v * mult);
    return 0;
}

static void apply(FILE *f, const char *name, const char *stream)
{
    const char *m = getenv(name);
    if (!m || !*m) return;
    (void)stream;
    if (strcmp(m, "L") == 0 || strcmp(m, "l") == 0) {
        setvbuf(f, NULL, _IOLBF, BUFSIZ);
    } else if (strcmp(m, "0") == 0) {
        setvbuf(f, NULL, _IONBF, 0);
    } else {
        size_t n;
        if (parse_size(m, &n) == 0 && n > 0) setvbuf(f, NULL, _IOFBF, n);
        else if (parse_size(m, &n) == 0) setvbuf(f, NULL, _IONBF, 0);
        else fprintf(stderr, "libstdbuf: invalid buffering mode '%s' in %s\n", m, name);
    }
}

__attribute__((constructor)) static void stdbuf_init(void)
{
    apply(stdin, "_STDBUF_I", "stdin");
    apply(stdout, "_STDBUF_O", "stdout");
    apply(stderr, "_STDBUF_E", "stderr");
}
