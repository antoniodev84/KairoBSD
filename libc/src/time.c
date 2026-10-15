#include <libkario.h>
#include <time.h>
struct tm *gmtime(const time_t *t)
{
    static struct tm result;
    long days = *t / 86400, seconds = *t % 86400;
    if (seconds < 0) {
        seconds += 86400;
        days--;
    }
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = z - era * 146097;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long year = yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2;
    if (year < -2147481748L || year > 2147483647L) {
        errno = EOVERFLOW;
        return NULL;
    }
    static const unsigned before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int weekday = (days + 4) % 7;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    result = (struct tm){seconds % 60,
                         seconds / 60 % 60,
                         seconds / 3600,
                         day,
                         month - 1,
                         year - 1900,
                         weekday < 0 ? weekday + 7 : weekday,
                         before[month - 1] + day - 1 + (leap && month > 2),
                         0};
    return &result;
}
struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}
char *ctime_r(const time_t *t, char *out)
{
    struct tm *tm = gmtime(t);
    if (!tm)
        return NULL;
    static const char weekdays[] = "SunMonTueWedThuFriSat";
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    snprintf(out, 26, "%.3s %.3s %2d %02d:%02d:%02d %04d\n", weekdays + tm->tm_wday * 3,
             months + tm->tm_mon * 3, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return out;
}

time_t time(time_t *out)
{
    struct ksysinfo info;
    if (sysinfo(&info) < 0)
        return -1;
    time_t value = info.realtime_sec;
    if (out)
        *out = value;
    return value;
}
