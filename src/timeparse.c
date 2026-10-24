/*
 * timeparse.c - turns snapshot names into points in time.
 *
 * Accepted forms (all usable as .snapshots/<name>/ or --at <name>):
 *   now
 *   <duration>-ago        e.g. 30s-ago, 2min-ago, 1h30min-ago, 1d-ago
 *   @<epoch-seconds>      e.g. @1759674000
 *   @YYYY-MM-DD[THH:MM[:SS]]
 *   @HH:MM[:SS]           (today, local time)
 *   <tag>                 a name created with `chronofs tag`
 *
 * A time given to the second (or minute, or day) means "the end of that
 * second", so @19:53:12 includes everything written during 19:53:12.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chronofs.h"

static int unit_ns(const char *u, size_t len, int64_t *out)
{
    static const struct { const char *name; int64_t ns; } units[] = {
        { "s", NS_PER_SEC }, { "sec", NS_PER_SEC }, { "secs", NS_PER_SEC },
        { "second", NS_PER_SEC }, { "seconds", NS_PER_SEC },
        { "m", 60 * NS_PER_SEC }, { "min", 60 * NS_PER_SEC }, { "mins", 60 * NS_PER_SEC },
        { "minute", 60 * NS_PER_SEC }, { "minutes", 60 * NS_PER_SEC },
        { "h", 3600 * NS_PER_SEC }, { "hr", 3600 * NS_PER_SEC }, { "hrs", 3600 * NS_PER_SEC },
        { "hour", 3600 * NS_PER_SEC }, { "hours", 3600 * NS_PER_SEC },
        { "d", 86400 * NS_PER_SEC }, { "day", 86400 * NS_PER_SEC }, { "days", 86400 * NS_PER_SEC },
        { "w", 604800 * NS_PER_SEC }, { "week", 604800 * NS_PER_SEC },
        { "weeks", 604800 * NS_PER_SEC },
    };
    size_t i;
    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (strlen(units[i].name) == len && strncmp(units[i].name, u, len) == 0) {
            *out = units[i].ns;
            return 0;
        }
    }
    return -1;
}

/* "90s", "2min", "1h30min", or a bare number of seconds. */
int parse_duration(const char *s, int64_t *out_ns)
{
    int64_t total = 0;
    const char *p = s;

    if (!*p)
        return -1;
    while (*p) {
        int64_t num = 0, mul;
        const char *u;
        if (!isdigit((unsigned char)*p))
            return -1;
        while (isdigit((unsigned char)*p)) {
            num = num * 10 + (*p - '0');
            if (num > 1000000000LL)
                return -1;
            p++;
        }
        u = p;
        while (isalpha((unsigned char)*p))
            p++;
        if (p == u) {
            if (*p)
                return -1;
            mul = NS_PER_SEC;           /* bare number = seconds */
        } else if (unit_ns(u, (size_t)(p - u), &mul) < 0) {
            return -1;
        }
        if (num > (INT64_MAX - total) / mul)
            return -1;
        total += num * mul;
    }
    *out_ns = total;
    return 0;
}

/* Parses the part after '@'. */
static int parse_absolute(const char *s, int64_t *out)
{
    struct tm tm;
    int Y, M, D, h = 23, m = 59, sec = 59, used = 0;
    int64_t extra = NS_PER_SEC - 1;
    time_t t;
    size_t len = strlen(s);

    if (len > 0 && strspn(s, "0123456789") == len) {
        char *end;
        long long seconds;
        errno = 0;
        seconds = strtoll(s, &end, 10);
        if (errno || *end || seconds > (INT64_MAX - NS_PER_SEC + 1) / NS_PER_SEC)
            return -1;
        *out = seconds * NS_PER_SEC + NS_PER_SEC - 1;
        return 0;
    }

    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &used) == 3 && used == 10) {
        const char *rest = s + 10;
        if (*rest == 'T' || *rest == ' ' || *rest == '_') {
            int n2 = 0;
            rest++;
            if (sscanf(rest, "%2d:%2d:%2d%n", &h, &m, &sec, &n2) == 3 && rest[n2] == '\0') {
                /* to the second */
            } else if (n2 = 0, sscanf(rest, "%2d:%2d%n", &h, &m, &n2) == 2 && rest[n2] == '\0') {
                sec = 59;
            } else {
                return -1;
            }
        } else if (*rest != '\0') {
            return -1;
        }
    } else {
        time_t nowt = time(NULL);
        struct tm today;
        int n2 = 0;
        localtime_r(&nowt, &today);
        Y = today.tm_year + 1900;
        M = today.tm_mon + 1;
        D = today.tm_mday;
        if (sscanf(s, "%2d:%2d:%2d%n", &h, &m, &sec, &n2) == 3 && s[n2] == '\0') {
            /* to the second */
        } else if (n2 = 0, sscanf(s, "%2d:%2d%n", &h, &m, &n2) == 2 && s[n2] == '\0') {
            sec = 59;
        } else {
            return -1;
        }
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 ||
        sec < 0 || sec > 60)
        return -1;

    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    if (t == (time_t)-1)
        return -1;
    if (tm.tm_year != Y - 1900 || tm.tm_mon != M - 1 || tm.tm_mday != D ||
        tm.tm_hour != h || tm.tm_min != m || tm.tm_sec != sec ||
        t < 0 || (uint64_t)t > (uint64_t)(INT64_MAX - extra) / NS_PER_SEC)
        return -1;
    *out = (int64_t)t * NS_PER_SEC + extra;
    return 0;
}

int parse_when(const char *spec, int64_t now, int64_t *out)
{
    size_t len = strlen(spec);

    if (strcmp(spec, "now") == 0) {
        *out = now;
        return 0;
    }
    if (len > 4 && strcmp(spec + len - 4, "-ago") == 0) {
        char buf[64];
        int64_t d;
        if (len - 4 >= sizeof(buf))
            return -1;
        memcpy(buf, spec, len - 4);
        buf[len - 4] = '\0';
        if (parse_duration(buf, &d) < 0)
            return -1;
        *out = now - d;
        return 0;
    }
    if (spec[0] == '@')
        return parse_absolute(spec + 1, out);
    return -1;
}

int tag_lookup(const struct store *st, const char *name, int64_t *out)
{
    char path[PATH_MAX], line[512], tname[256];
    long long ts;
    int found = 0;
    FILE *f;

    store_path(st, path, sizeof(path), "tags");
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s %lld", tname, &ts) == 2 && strcmp(tname, name) == 0) {
            *out = ts;                  /* later lines override earlier ones */
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int resolve_when(const struct store *st, const char *spec, int64_t *out)
{
    if (parse_when(spec, now_ns(), out) == 0)
        return 0;
    return tag_lookup(st, spec, out);
}

void fmt_time(int64_t ts_ns, char *buf, size_t n)
{
    time_t t = (time_t)(ts_ns / NS_PER_SEC);
    struct tm tm;
    size_t l;
    localtime_r(&t, &tm);
    l = strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(buf + l, n - l, ".%03d", (int)((ts_ns % NS_PER_SEC) / 1000000));
}

/* Name used for per-version entries in .snapshots/ (e.g. "@2026-10-05T19:53:12"). */
void fmt_time_spec(int64_t ts_ns, char *buf, size_t n)
{
    time_t t = (time_t)(ts_ns / NS_PER_SEC);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "@%Y-%m-%dT%H:%M:%S", &tm);
}

void fmt_size(uint64_t bytes, char *buf, size_t n)
{
    if (bytes < 1024)
        snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, n, "%.1f KiB", bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024)
        snprintf(buf, n, "%.1f MiB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, n, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
}
