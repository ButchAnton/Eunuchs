/* time: calendar conversion and formatting. Local time is UTC. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char *tzname[2] = { "UTC", "UTC" };
long timezone;
int daylight;

void tzset(void)
{
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return gmtime_r(t, out);
}

struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100) || y % 400 == 0;
}

static long days_from_civil(long y, int m, int d)   /* m 1..12 */
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

time_t timegm(struct tm *tm)
{
    /* normalize months into years first, then let days carry */
    long year = tm->tm_year + 1900L, mon = tm->tm_mon;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }
    long days = days_from_civil(year, (int)mon + 1, 1) + tm->tm_mday - 1;
    time_t t = (time_t)days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    gmtime_r(&t, tm);                   /* normalize the fields, fill wday/yday */
    return t;
}

time_t mktime(struct tm *tm)
{
    return timegm(tm);
}

double difftime(time_t a, time_t b)
{
    return (double)(a - b);
}

clock_t clock(void)
{
    /* no per-process CPU time yet: time since boot (CLOCKS_PER_SEC is 10^6) */
    return (clock_t)uptime_ms() * 1000;
}

static const char *const wdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                     "Friday", "Saturday" };
static const char *const months[] = { "January", "February", "March", "April", "May", "June",
                                      "July", "August", "September", "October", "November",
                                      "December" };

char *asctime_r(const struct tm *tm, char *buf)
{
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", wdays[tm->tm_wday % 7],
             months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *asctime(const struct tm *tm)
{
    static char buf[32];
    return asctime_r(tm, buf);
}

char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;
    return asctime_r(gmtime_r(t, &tm), buf);
}

char *ctime(const time_t *t)
{
    struct tm tm;
    return asctime(gmtime_r(t, &tm));
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    size_t n = 0;
    char buf[64];
#define PUT(str)                                    \
    do {                                            \
        const char *_p = (str);                     \
        while (*_p) {                               \
            if (n + 1 >= max)                       \
                return 0;                           \
            s[n++] = *_p++;                         \
        }                                           \
    } while (0)
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            char one[2] = { *f, 0 };
            PUT(one);
            continue;
        }
        f++;
        if (*f == 'E' || *f == 'O')
            f++;
        int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        switch (*f) {
        case 'a': snprintf(buf, sizeof(buf), "%.3s", wdays[tm->tm_wday % 7]); break;
        case 'A': snprintf(buf, sizeof(buf), "%s", wdays[tm->tm_wday % 7]); break;
        case 'b': case 'h': snprintf(buf, sizeof(buf), "%.3s", months[tm->tm_mon % 12]); break;
        case 'B': snprintf(buf, sizeof(buf), "%s", months[tm->tm_mon % 12]); break;
        case 'c': snprintf(buf, sizeof(buf), "%.3s %.3s %2d %02d:%02d:%02d %d", wdays[tm->tm_wday % 7],
                           months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min,
                           tm->tm_sec, tm->tm_year + 1900); break;
        case 'C': snprintf(buf, sizeof(buf), "%02d", (tm->tm_year + 1900) / 100); break;
        case 'd': snprintf(buf, sizeof(buf), "%02d", tm->tm_mday); break;
        case 'D': snprintf(buf, sizeof(buf), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                           tm->tm_year % 100); break;
        case 'e': snprintf(buf, sizeof(buf), "%2d", tm->tm_mday); break;
        case 'F': snprintf(buf, sizeof(buf), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                           tm->tm_mday); break;
        case 'H': snprintf(buf, sizeof(buf), "%02d", tm->tm_hour); break;
        case 'I': snprintf(buf, sizeof(buf), "%02d", h12); break;
        case 'j': snprintf(buf, sizeof(buf), "%03d", tm->tm_yday + 1); break;
        case 'k': snprintf(buf, sizeof(buf), "%2d", tm->tm_hour); break;
        case 'l': snprintf(buf, sizeof(buf), "%2d", h12); break;
        case 'm': snprintf(buf, sizeof(buf), "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(buf, sizeof(buf), "%02d", tm->tm_min); break;
        case 'n': snprintf(buf, sizeof(buf), "\n"); break;
        case 'p': snprintf(buf, sizeof(buf), "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'P': snprintf(buf, sizeof(buf), "%s", tm->tm_hour < 12 ? "am" : "pm"); break;
        case 'r': snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", h12, tm->tm_min, tm->tm_sec,
                           tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'R': snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 's': {
            struct tm copy = *tm;
            snprintf(buf, sizeof(buf), "%lld", (long long)timegm(&copy));
            break;
        }
        case 'S': snprintf(buf, sizeof(buf), "%02d", tm->tm_sec); break;
        case 't': snprintf(buf, sizeof(buf), "\t"); break;
        case 'T': snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'u': snprintf(buf, sizeof(buf), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
        case 'w': snprintf(buf, sizeof(buf), "%d", tm->tm_wday); break;
        case 'U': snprintf(buf, sizeof(buf), "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
        case 'W': snprintf(buf, sizeof(buf), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
        case 'x': snprintf(buf, sizeof(buf), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                           tm->tm_year % 100); break;
        case 'X': snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'y': snprintf(buf, sizeof(buf), "%02d", tm->tm_year % 100); break;
        case 'Y': snprintf(buf, sizeof(buf), "%d", tm->tm_year + 1900); break;
        case 'z': snprintf(buf, sizeof(buf), "+0000"); break;
        case 'Z': snprintf(buf, sizeof(buf), "UTC"); break;
        case '%': snprintf(buf, sizeof(buf), "%%"); break;
        case 0: f--; buf[0] = 0; break;
        default: snprintf(buf, sizeof(buf), "%%%c", *f); break;
        }
        PUT(buf);
    }
#undef PUT
    if (max)
        s[n] = 0;
    (void)is_leap;
    return n;
}
