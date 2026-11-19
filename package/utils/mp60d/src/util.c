#include "mp60d.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>
#include <unistd.h>

void log_msg(const char *level, const char *fmt, ...)
{
    va_list args;
    char message[512];
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    syslog(LOG_INFO, "%s: %s", level, message);
    fprintf(stderr, "mp60d %s: %s\n", level, message);
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
            return 0;
    }
    return !*a && !*b;
}

int parse_on_off(const char *text, int allow_toggle)
{
    while (isspace((unsigned char)*text))
        text++;
    if (str_ieq(text, "on") || !strcmp(text, "1"))
        return 1;
    if (str_ieq(text, "off") || !strcmp(text, "0"))
        return 0;
    if (allow_toggle && str_ieq(text, "toggle"))
        return 2;
    return -1;
}

static const char *skip_space(const char *p)
{
    while (isspace((unsigned char)*p))
        p++;
    return p;
}

static int parse_string(const char **cursor, char *output, size_t capacity)
{
    const char *p = *cursor;
    size_t used = 0;
    if (*p++ != '"')
        return -1;
    while (*p && *p != '"') {
        unsigned value;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': case '\\': case '/': value = (unsigned char)*p; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'u':
                if (!isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2]) ||
                    !isxdigit((unsigned char)p[3]) || !isxdigit((unsigned char)p[4]))
                    return -1;
                value = '?';
                p += 4;
                break;
            default: return -1;
            }
            p++;
        } else {
            if ((unsigned char)*p < 32)
                return -1;
            value = (unsigned char)*p++;
        }
        if (output && used + 1 < capacity)
            output[used++] = (char)value;
        else if (output)
            return -1;
    }
    if (*p != '"')
        return -1;
    if (output)
        output[used] = 0;
    *cursor = p + 1;
    return 0;
}

static const char *skip_value(const char *p)
{
    int depth = 0, string = 0, escape = 0;
    p = skip_space(p);
    if (*p == '"')
        return parse_string(&p, NULL, 0) ? NULL : p;
    while (*p) {
        char c = *p;
        if (string) {
            if (escape)
                escape = 0;
            else if (c == '\\')
                escape = 1;
            else if (c == '"')
                string = 0;
        } else if (c == '"') {
            string = 1;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (!depth)
                break;
            depth--;
        } else if (c == ',' && !depth) {
            break;
        }
        p++;
    }
    return string || depth ? NULL : p;
}

int json_field(const char *json, const char *wanted, char *type, char *value, size_t value_size)
{
    const char *p = skip_space(json);
    if (*p++ != '{')
        return -1;
    p = skip_space(p);
    if (*p == '}')
        return 0;
    for (;;) {
        char key[64];
        const char *end;
        int found;
        p = skip_space(p);
        if (parse_string(&p, key, sizeof(key)))
            return -1;
        p = skip_space(p);
        if (*p++ != ':')
            return -1;
        p = skip_space(p);
        found = !strcmp(key, wanted);
        if (*p == '"') {
            char temporary[512];
            if (parse_string(&p, found ? value : temporary, found ? value_size : sizeof(temporary)))
                return -1;
            if (found) {
                *type = 's';
                return 1;
            }
        } else {
            const char *start = p;
            size_t length;
            end = skip_value(p);
            if (!end || end == start)
                return -1;
            length = (size_t)(end - start);
            while (length && isspace((unsigned char)start[length - 1]))
                length--;
            if (found) {
                if (length + 1 > value_size)
                    return -1;
                memcpy(value, start, length);
                value[length] = 0;
                *type = length == 4 && !memcmp(start, "true", 4) ? 'b' :
                         length == 5 && !memcmp(start, "false", 5) ? 'b' :
                         length == 4 && !memcmp(start, "null", 4) ? 'z' : 'n';
                return 1;
            }
            p = end;
        }
        p = skip_space(p);
        if (*p == '}')
            return 0;
        if (*p++ != ',')
            return -1;
    }
}

int json_append(char *buf, size_t size, const char *text)
{
    size_t used = strlen(buf);
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        char escape[7];
        const char *part = NULL;
        switch (c) {
        case '"': part = "\\\""; break;
        case '\\': part = "\\\\"; break;
        case '\n': part = "\\n"; break;
        case '\r': part = "\\r"; break;
        case '\t': part = "\\t"; break;
        default:
            if (c < 32) {
                snprintf(escape, sizeof(escape), "\\u%04x", c);
                part = escape;
            }
        }
        if (part) {
            size_t n = strlen(part);
            if (used + n + 1 > size)
                return -1;
            memcpy(buf + used, part, n);
            used += n;
            buf[used] = 0;
        } else {
            if (used + 2 > size)
                return -1;
            buf[used++] = (char)c;
            buf[used] = 0;
        }
    }
    return 0;
}

long long monotonic_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void random_hex(char *output, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[64];
    size_t i, got = 0;
    int fd;
    if (bytes > sizeof(random))
        bytes = sizeof(random);
    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (got < bytes) {
            ssize_t n = read(fd, random + got, bytes - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        close(fd);
    }
    if (got < bytes) {
        srand((unsigned)(time(NULL) ^ getpid()));
        while (got < bytes)
            random[got++] = (unsigned char)(rand() & 0xff);
    }
    for (i = 0; i < bytes; i++) {
        output[i * 2] = hex[random[i] >> 4];
        output[i * 2 + 1] = hex[random[i] & 15];
    }
    output[bytes * 2] = 0;
}

int base64_decode(const char *input, unsigned char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned accumulator = 0;
    int bits = 0, used = 0;
    while (*input && *input != '=') {
        const char *found = strchr(alphabet, *input++);
        if (!found)
            return -1;
        accumulator = (accumulator << 6) | (unsigned)(found - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if ((size_t)used >= capacity)
                return -1;
            output[used++] = (unsigned char)((accumulator >> bits) & 0xff);
        }
    }
    return used;
}

int constant_time_equal(const char *a, const char *b)
{
    size_t i, la = strlen(a), lb = strlen(b);
    unsigned char difference = (unsigned char)(la ^ lb);
    size_t length = la > lb ? la : lb;
    for (i = 0; i < length; i++)
        difference |= (i < la ? (unsigned char)a[i] : 0) ^ (i < lb ? (unsigned char)b[i] : 0);
    return difference == 0;
}

/* Compact SHA-256 implementation for salted UI-password verification. */
typedef struct { uint32_t h[8]; uint64_t bytes; unsigned char block[64]; size_t used; } Sha256;
static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(Sha256 *s, const unsigned char *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64], a,b,c,d,e,f,g,h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotate(w[i-15],7) ^ rotate(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotate(w[i-2],17) ^ rotate(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha256_init(Sha256 *s)
{
    static const uint32_t initial[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memset(s, 0, sizeof(*s));
    memcpy(s->h, initial, sizeof(initial));
}
static void sha256_update(Sha256 *s, const unsigned char *data, size_t length)
{
    s->bytes += length;
    while (length) {
        size_t take = 64 - s->used;
        if (take > length) take = length;
        memcpy(s->block + s->used, data, take);
        s->used += take; data += take; length -= take;
        if (s->used == 64) { sha256_block(s, s->block); s->used = 0; }
    }
}
static void sha256_final(Sha256 *s, unsigned char output[32])
{
    uint64_t bits = s->bytes * 8;
    unsigned char padding[72] = {0x80};
    size_t padding_length = s->used < 56 ? 56 - s->used : 120 - s->used;
    int i;
    sha256_update(s, padding, padding_length);
    for (i = 0; i < 8; i++)
        padding[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(s, padding, 8);
    for (i = 0; i < 8; i++) {
        output[i*4] = (unsigned char)(s->h[i] >> 24);
        output[i*4+1] = (unsigned char)(s->h[i] >> 16);
        output[i*4+2] = (unsigned char)(s->h[i] >> 8);
        output[i*4+3] = (unsigned char)s->h[i];
    }
}
void sha256_hex(const void *data, size_t length, char output[65])
{
    static const char hex[] = "0123456789abcdef";
    Sha256 state;
    unsigned char digest[32];
    int i;
    sha256_init(&state);
    sha256_update(&state, (const unsigned char *)data, length);
    sha256_final(&state, digest);
    for (i = 0; i < 32; i++) {
        output[i*2] = hex[digest[i] >> 4];
        output[i*2+1] = hex[digest[i] & 15];
    }
    output[64] = 0;
}
