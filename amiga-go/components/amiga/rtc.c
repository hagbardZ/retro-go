/*
 * The battery-backed clock: an OKI MSM6242B at $DC0000 (the A500's trapdoor
 * expansion and the A2000 have one; here every model does), as 1.3's
 * SetClock and later Kickstarts' battclock.resource use it.
 *
 * Sixteen 4-bit registers, register n in the low nibble of the bytes at
 * $DC0000 + 4n + 1 and + 3: seconds, minutes, hours, day, month, year (two
 * BCD digits each, units first), the weekday, control D (HOLD, BUSY, IRQ,
 * 30 s adjust), E, F (REST, STOP, 24/12, TEST).
 *
 * The time is the host's local time plus an offset: setting the clock
 * changes the offset, which the board keeps (amiga_config_t.rtc_changed).
 * The board runs the Amiga in real time and takes its clock; the host
 * build, far faster, counts the frames from its start (1.3's SetClock
 * looks for a clock whose seconds move in 1.5 s of the Amiga's time).
 *
 * Kickstart 1.x's SetClock counts years up from 78 to the clock's two
 * digits, so 2000 and later came out as 1978: for it, those years are 100
 * plus the year, the tens digit past 9 (2026: 12 and 6), which it counts
 * right (the registers are 4 bits). Later Kickstarts get plain digits.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "amiga_internal.h"

enum { S1, S10, MI1, MI10, H1, H10, D1, D10, MO1, MO10, Y1, Y10, W, CD, CE, CF };

#define CF_24H      0x04

static struct {
    int64_t offset;
    int64_t start;                  /* (host builds) the host's time at the start */
    uint8_t cd, ce, cf;
    uint8_t digit[13];              /* the time while HOLD is set (or being written) */
    bool held;
    bool written;                   /* a digit written while held: the time to set */
    bool past_99;                   /* years from 2000 as 100 and more (Kickstart 1.x) */
    void (*changed)(int64_t offset);
} s_rtc;

#define CD_HOLD     0x01

void rtc_init(int64_t offset, void (*changed)(int64_t offset), bool past_99)
{
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.past_99 = past_99;
    s_rtc.offset = offset;
#ifdef ESP_PLATFORM
    s_rtc.start = (int64_t)time(NULL);
#else
    /* the host: AMI_TIME (seconds since 1970) makes runs repeatable (games
     * seeded from the clock: Epsilon 9, Maniac Ball)
     */
    const char *at = getenv("AMI_TIME");
    s_rtc.start = at ? atoll(at) : (int64_t)time(NULL);
#endif
    s_rtc.cf = CF_24H;
    s_rtc.changed = changed;
}

/* The clock's time without the offset */
static int64_t base(void)
{
#ifdef ESP_PLATFORM
    return (int64_t)time(NULL);
#else
    return s_rtc.start + ami.frames / amiga_screen_hz();
#endif
}

static int hour12(int h, bool *pm)
{
    *pm = h >= 12;
    h %= 12;
    return h == 0 ? 12 : h;
}

/* The running time as the chip's digits */
static void digits(uint8_t *d)
{
    time_t t = (time_t)(base() + s_rtc.offset);
    struct tm tm;
    localtime_r(&t, &tm);
    int year = (s_rtc.past_99 && tm.tm_year >= 100 && tm.tm_year < 160) ? tm.tm_year : tm.tm_year % 100;
    bool pm = false;
    int h = (s_rtc.cf & CF_24H) ? tm.tm_hour : hour12(tm.tm_hour, &pm);
    d[S1] = tm.tm_sec % 10;
    d[S10] = tm.tm_sec / 10;
    d[MI1] = tm.tm_min % 10;
    d[MI10] = tm.tm_min / 10;
    d[H1] = h % 10;
    d[H10] = (h / 10) | (pm ? 4 : 0);
    d[D1] = tm.tm_mday % 10;
    d[D10] = tm.tm_mday / 10;
    d[MO1] = (tm.tm_mon + 1) % 10;
    d[MO10] = (tm.tm_mon + 1) / 10;
    d[Y1] = year % 10;
    d[Y10] = year / 10;
    d[W] = tm.tm_wday;
}

/* The digits written become the time: the offset follows */
static void apply(const uint8_t *d)
{
    struct tm tm = { 0 };
    int h = (d[H10] & 3) * 10 + d[H1];
    if (!(s_rtc.cf & CF_24H)) {
        h = (h % 12) + ((d[H10] & 4) ? 12 : 0);
    }
    int year = d[Y10] * 10 + d[Y1];
    tm.tm_sec = d[S10] * 10 + d[S1];
    tm.tm_min = d[MI10] * 10 + d[MI1];
    tm.tm_hour = h;
    tm.tm_mday = d[D10] * 10 + d[D1];
    tm.tm_mon = d[MO10] * 10 + d[MO1] - 1;
    tm.tm_year = year < 78 ? 100 + year : year;     /* 1978-2077 (100+: 2000 on) */
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) {
        return;
    }
    int64_t offset = (int64_t)t - base();
    if (offset != s_rtc.offset) {
        s_rtc.offset = offset;
        if (s_rtc.changed) {
            s_rtc.changed(offset);
        }
    }
}

uint8_t rtc_read(int reg)
{
    switch (reg) {
    case CD:
        return s_rtc.cd;                        /* never BUSY */
    case CE:
        return s_rtc.ce;
    case CF:
        return s_rtc.cf;
    default: {
        uint8_t d[13];
        if (s_rtc.held) {
            memcpy(d, s_rtc.digit, sizeof(d));
        } else {
            digits(d);
        }
        return d[reg];
    }
    }
}

void rtc_write(int reg, uint8_t v)
{
    v &= 0x0F;
    switch (reg) {
    case CD:
        s_rtc.cd = v & 0x0D;
        if ((v & CD_HOLD) && !s_rtc.held) {
            digits(s_rtc.digit);
            s_rtc.held = true;
            s_rtc.written = false;
        } else if (!(v & CD_HOLD) && s_rtc.held) {
            /* a read under HOLD leaves the time alone (it kept counting) */
            s_rtc.held = false;
            if (s_rtc.written) {
                apply(s_rtc.digit);
            }
        }
        break;
    case CE:
        s_rtc.ce = v;
        break;
    case CF:
        s_rtc.cf = v & 0x0E;        /* (REST: a reset strobe) */
        break;
    case W:
        break;                      /* (the weekday follows the date) */
    default:
        if (s_rtc.held) {
            s_rtc.digit[reg] = v;
            s_rtc.written = true;
        } else {
            uint8_t d[13];
            digits(d);
            d[reg] = v;
            apply(d);
        }
        break;
    }
}
