/*
 * Paula's sound: four channels of 8-bit samples, each byte held for the
 * channel's period in colour clocks, at its volume (0-64).
 *
 * DMA: when a channel's DMA starts, its location and length are copied
 * and the interrupt raised (the program can set up the next buffer); at
 * the buffer's end they're copied again and the interrupt raised again.
 * Without DMA, a word written to AUDxDAT plays and raises the interrupt.
 * When a channel's DMA goes off, the word playing finishes, then the
 * interrupt (replay routines stop a channel and wait for it).
 *
 * Attached (ADKCON bits 0-7): a channel's words go to the next channel's
 * volume and/or period instead of playing, and it's silent (channel 3's
 * bits only silence it). Each word stands in for one of a played word's two
 * bytes: a volume word where the low byte would end (and the first at the
 * DMA's start), a period word where the high byte would; with both, one a
 * period, alternating volume and period.
 *
 * Mixed channels 0 and 3 left, 1 and 2 right, averaged over each sample's
 * time at AMIGA_AUDIO_HZ, through the A500's fixed low-pass filter (and the
 * second one the power LED switches on), into a ring the speaker's task
 * takes from. Run at each line's end.
 */
#include <string.h>
#include "amiga_internal.h"

#define RING            8192            /* frames, ~190 ms */
#define MIN_PERIOD      113             /* what Paula's DMA keeps up with */
#define ATT_VOL         0x01            /* ADKCON >> c: channel c attached to c + 1's volume */
#define ATT_PER         0x10            /* ... and/or its period */

typedef struct {
    uint32_t lc, pt;
    uint16_t len, per, vol;
    uint32_t left;                      /* words of the buffer to go */
    uint16_t dat;                       /* the word playing */
    int byte;                           /* 0 its high byte, 1 the low */
    int32_t cnt;                        /* 1/256 colour clocks of the byte left */
    bool dma;                           /* playing from memory */
    bool drain;                         /* its DMA went off: the word finishing */
    bool playing;
    int out;                            /* the sample x the volume */
} chan_t;

static struct {
    chan_t ch[4];
    int32_t period;                     /* 1/256 colour clocks a sample */
    int32_t sample_left;
    int32_t acc_l, acc_r;               /* (2 x 8192 x a period: fits, and no 64-bit division) */
    int32_t lp1_l, lp1_r, lp2_l, lp2_r, lp3_l, lp3_r;   /* the filters (x 256) */
    bool enabled;
} au;

static int16_t *s_ring;
static volatile uint32_t s_write, s_read;

static bool dma_on(int c)
{
    return (ami.dmacon & DMA_EN) && (ami.dmacon & (1u << c));
}

void audio_reset(void)
{
    if (!s_ring) {
        s_ring = ami_malloc(RING * 2 * sizeof(int16_t));
    }
    bool enabled = au.enabled;
    memset(&au, 0, sizeof(au));
    au.enabled = enabled;
    int cck_hz = ami.ntsc ? 3579545 : 3546895;
    au.period = (int32_t)(((int64_t)cck_hz * 256) / AMIGA_AUDIO_HZ);
    au.sample_left = au.period;
}

static inline int attached(int c)
{
    return (ami.adkcon >> c) & (ATT_VOL | ATT_PER);
}

static void set_out(chan_t *ch)
{
    int8_t sample = (int8_t)(ch->byte ? (ch->dat & 0xFF) : (ch->dat >> 8));
    ch->out = attached((int)(ch - au.ch)) ? 0 : sample * ch->vol;
}

static void set_vol(chan_t *ch, uint16_t v)
{
    ch->vol = (v & 0x40) ? 64 : (v & 0x3F);
    if (ch->playing) {
        set_out(ch);
    }
}

/* An attached channel's word to the next one's period or volume */
static void modulate(int c, bool period)
{
    if (c == 3) {
        return;
    }
    if (period) {
        au.ch[c + 1].per = au.ch[c].dat;
    } else {
        set_vol(&au.ch[c + 1], au.ch[c].dat);
    }
}

static uint16_t period_of(const chan_t *ch)
{
    return ch->per < MIN_PERIOD ? MIN_PERIOD : ch->per;
}

/* The next word from memory: a new buffer when this one ran out */
static void fetch(int c)
{
    chan_t *ch = &au.ch[c];
    if (ch->left == 0) {
        ch->pt = ch->lc;
        ch->left = ch->len ? ch->len : 0x10000;
        ami_set_intreq((uint16_t)(INT_AUD0 << c));
    }
    ch->dat = chip_read16(ch->pt);
    ch->pt += 2;
    ch->left--;
}

static void start_dma(int c)
{
    chan_t *ch = &au.ch[c];
    ch->dma = true;
    ch->drain = false;
    ch->playing = true;
    ch->left = 0;
    fetch(c);
    ch->byte = 0;
    ch->cnt = period_of(ch) * 256;
    if (attached(c) & ATT_VOL) {
        modulate(c, false);
    }
    set_out(ch);
}

/* A byte played out (attached: a word's half used) */
static void next_byte(int c)
{
    chan_t *ch = &au.ch[c];
    int att = attached(c);
    if (ch->byte == 0) {
        ch->byte = 1;
        if (att & ATT_PER) {
            if ((att & ATT_VOL) && ch->dma) {
                fetch(c);
            }
            modulate(c, true);
        }
    } else if (ch->dma) {
        fetch(c);
        ch->byte = 0;
        if (att & ATT_VOL) {
            modulate(c, false);
        }
    } else {
        /* a written word played: its interrupt, then the last sample holds;
         * the last word of a DMA turned off: its interrupt, then silence */
        ch->playing = false;
        if (ch->drain) {
            ch->drain = false;
            ch->out = 0;
        }
        ami_set_intreq((uint16_t)(INT_AUD0 << c));
        return;
    }
    ch->cnt = period_of(ch) * 256;
    set_out(ch);
}

void audio_dma_changed(void)
{
    for (int c = 0; c < 4; c++) {
        chan_t *ch = &au.ch[c];
        bool on = dma_on(c);
        if (on && !ch->dma) {
            start_dma(c);
        } else if (!on && ch->dma) {
            ch->dma = false;
            ch->drain = true;
        }
    }
}

void audio_write(uint32_t reg, uint16_t v)
{
    int c = (int)((reg - 0x0A0) >> 4);
    chan_t *ch = &au.ch[c];
    switch (reg & 0xF) {
    case 0x0:
        ch->lc = (ch->lc & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        break;
    case 0x2:
        ch->lc = (ch->lc & 0xFFFF0000) | (v & 0xFFFE);
        break;
    case 0x4:
        ch->len = v;
        break;
    case 0x6:
        ch->per = v;
        break;
    case 0x8:
        set_vol(ch, v);
        break;
    case 0xA:
        if (!ch->dma) {
            ch->dat = v;
            ch->drain = false;
            ch->byte = 0;
            ch->cnt = period_of(ch) * 256;
            ch->playing = true;
            if (attached(c) & ATT_VOL) {
                modulate(c, false);
            }
            set_out(ch);
        }
        break;
    default:
        break;
    }
}

/* One pole: y += (x - y) * a / 256 */
static inline int32_t pole(int32_t *y, int32_t x, int32_t a)
{
    *y += (int32_t)(((int64_t)(x * 256 - *y) * a) >> 8);
    return *y >> 8;
}

static void emit(void)
{
    int32_t l = au.acc_l / au.period;
    int32_t r = au.acc_r / au.period;
    au.acc_l = au.acc_r = 0;
    /* the A500's fixed filter (~4.4 kHz), then the LED's (~3.3 kHz, two poles) */
    l = pole(&au.lp1_l, l, 119);
    r = pole(&au.lp1_r, r, 119);
    uint8_t pra = (cia[0].pra & cia[0].ddra) | (uint8_t)~cia[0].ddra;
    if (!(pra & 0x02)) {
        l = pole(&au.lp2_l, l, 96);
        r = pole(&au.lp2_r, r, 96);
        l = pole(&au.lp3_l, l, 96);
        r = pole(&au.lp3_r, r, 96);
    }
    if (!au.enabled || !s_ring || s_write - s_read >= RING) {
        return;
    }
    l *= 2;
    r *= 2;
    uint32_t i = (s_write % RING) * 2;
    s_ring[i] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
    s_ring[i + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    __atomic_store_n(&s_write, s_write + 1, __ATOMIC_RELEASE);
}

AMI_HOT void audio_line(void)
{
    int32_t remaining = ami_line_cck(ami.line) * 256;
    while (remaining > 0) {
        int32_t step = remaining < au.sample_left ? remaining : au.sample_left;
        for (int c = 0; c < 4; c++) {
            if (au.ch[c].playing && au.ch[c].cnt < step) {
                step = au.ch[c].cnt;
            }
        }
        au.acc_l += (au.ch[0].out + au.ch[3].out) * step;
        au.acc_r += (au.ch[1].out + au.ch[2].out) * step;
        remaining -= step;
        au.sample_left -= step;
        for (int c = 0; c < 4; c++) {
            chan_t *ch = &au.ch[c];
            if (ch->playing && (ch->cnt -= step) <= 0) {
                next_byte(c);
            }
        }
        if (au.sample_left <= 0) {
            emit();
            au.sample_left += au.period;
        }
    }
}

/* ---- The speaker's side ---- */

void amiga_audio_enable(bool on)
{
    au.enabled = on;
    if (!on) {
        s_read = s_write;
    }
}

int amiga_audio_pending(void)
{
    return (int)(__atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read);
}

int amiga_audio_render(int16_t *dst, int frames)
{
    uint32_t avail = __atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read;
    int n = (int)avail < frames ? (int)avail : frames;
    for (int i = 0; i < n; i++) {
        uint32_t k = ((s_read + (uint32_t)i) % RING) * 2;
        dst[2 * i] = s_ring[k];
        dst[2 * i + 1] = s_ring[k + 1];
    }
    s_read += (uint32_t)n;
    return n;
}
