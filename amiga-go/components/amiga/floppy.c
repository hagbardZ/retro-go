/*
 * The floppy drives and Paula's disk DMA.
 *
 * The drives: DF0: and DF1:, selected, stepped and their motors switched
 * through CIA-B's port B, read back on CIA-A's port A:
 *   CIA-B PRB: 7 /MTR (latched by a drive when it's selected), 6-3 /SEL3-0,
 *              2 /SIDE, 1 DIR (0: inwards), 0 /STEP
 *   CIA-A PRA: 5 /RDY, 4 /TK0, 3 /WPRO, 2 /CHNG (the selected drives')
 * The disk turns at 300 rpm: a track is TRACK_WORDS MFM words (2 us a bit
 * cell), an index pulse on CIA-B's FLAG each turn. A high-density disk
 * (22 sectors a track, 1.76 MB) turns at 150 rpm in an Amiga HD drive:
 * the same bits a second, twice the words a turn; the drive then says
 * $AAAAAAAA in its ID (Kickstart 3.1's trackdisk reads 22 sectors then).
 *
 * The images: ADF (80 cylinders, 2 sides, 11 sectors of 512, or 22), kept in
 * memory, written tracks back to the file at amiga_floppy_flush(). The
 * track under the head is made from the image's sectors in AmigaDOS's own
 * format (sync words, header, checksums, odd/even MFM), so trackdisk and
 * the games' own loaders both read it through the DMA; a track written
 * through the DMA is decoded back into sectors when the write ends.
 *
 * UAE's extended ADFs ("UAE-1ADF": per track AmigaDOS sectors or raw MFM
 * of its own length, as copy-protected disks need) are written too: an
 * AmigaDOS track whose 11 sectors all come back (checksums right) into its
 * sectors, anything else as the turn's raw MFM (an AmigaDOS track then
 * becomes a raw one: the image laid out again, written whole). An RP9
 * package's disks are unpacked once each into a hidden ADF beside it
 * (".<name>.rp9.<n>.adf", written to like any ADF); putting the package in
 * the same drive again takes its next disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "amiga_internal.h"
#include "rp9.h"

#define DRIVES          4
#define CYLINDERS       80
#define HEAD_STOPS      84
#define SECTORS_DD      11
#define SECTORS_HD      22
#define ADF_SIZE        (CYLINDERS * 2 * SECTORS_DD * 512)
#define HD_SIZE         (CYLINDERS * 2 * SECTORS_HD * 512)
#define TRACK_WORDS     6334            /* a turn: 12668 bytes of MFM (HD twice) */
#define MFM_MAX         (2 * TRACK_WORDS)   /* (an HD track; an extended ADF's longest) */
#define EXT_MAX         (2560 * 1024)
#define EXT_TRACKS      168
#define WORD_CYCLES     224             /* 16 bit cells of 2 us, in 68000 clocks */
#define SECTOR_WORDS    544
#define SYNC            0x4489
#define SPINUP_CYCLES   (7093790 * 2 / 5)   /* /RDY 400 ms after the motor starts */
#define ADK_WORDSYNC    0x0400

typedef struct {
    bool present;
    bool motor;
    bool selected;
    int cylinder;
    bool changed;                   /* /CHNG low: a disk was out since the last step */
    int spinup;                     /* clocks until the motor is up to speed */
    int id_bit;                     /* the drive's ID, read through /RDY with the motor off */

    uint8_t *data;                  /* the image (NULL: no disk) */
    uint32_t size;
    int sectors;                    /* a track's: 11, a high-density disk 22 */
    bool ext;                       /* an extended ADF */
    int ext_tracks;
    uint32_t ext_off[EXT_TRACKS];   /* where each track's data is */
    bool ext_moved;                 /* its tracks laid out again: written whole */
    uint16_t *mfm;                  /* the track under the head */
    int mfm_track;                  /* which (cylinder * 2 + side), -1: none */
    int track_words;                /* its length */
    char path[256];                 /* the file (an RP9's: its hidden copy; "": none) */
    char shown[256];                /* what the drive has in it (the .rp9 itself) */
    int rp9_disk, rp9_disks;
    bool fast;                      /* turning 8x as fast (amiga_floppy_insert()) */
    bool protect;
    uint8_t dirty[(EXT_TRACKS + 7) / 8];
    bool any_dirty;

    int pos;                        /* the word under the head */
    int frac;                       /* clocks into it */
    uint32_t reads, writes;
} drive_t;

static drive_t s_drive[DRIVES];
static int s_side;
static int s_at;                    /* the disk's time, in clocks into this frame */

void (*amiga_disk_hook)(uint16_t dsklen, uint16_t sync, uint16_t adkcon, int cylinder, int side, uint32_t pt);

/* Paula's side */
static struct {
    uint32_t pt;
    uint16_t len;                   /* the last DSKLEN written: DMA starts on the second */
    uint16_t sync;
    bool active, writing, synced;
    int left;
    uint16_t shift;                 /* the last 16 bits off the disk */
    int bits;                       /* of the word being put together */
    int drive;                      /* the drive being written */
    bool wordequal;
    bool byte_ready;
} dk;

/* ---- MFM ---- */

/* data's bits at the even positions, the clock bits filled in (prev: the
 * MFM long before, for the first clock bit)
 */
static uint32_t mfm_long(uint32_t data, uint32_t prev)
{
    data &= 0x55555555;
    uint32_t clock = ~((data << 1) | (data >> 1) | (prev << 31)) & 0xAAAAAAAA;
    return data | clock;
}

typedef struct {
    uint16_t *w;
    int n;
    uint32_t last;                  /* the last long written (its bit 0) */
} track_writer_t;

static void put_word(track_writer_t *t, uint16_t v)
{
    t->w[t->n++] = v;
    t->last = (t->last << 16) | v;
}

static void put_long(track_writer_t *t, uint32_t raw)
{
    uint32_t m = mfm_long(raw, t->last);
    put_word(t, (uint16_t)(m >> 16));
    put_word(t, (uint16_t)m);
}

/* odd bits then even bits of n longs */
static uint32_t put_odd_even(track_writer_t *t, const uint32_t *v, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        uint32_t odd = (v[i] >> 1) & 0x55555555;
        sum ^= odd;
        put_long(t, odd);
    }
    for (int i = 0; i < n; i++) {
        uint32_t even = v[i] & 0x55555555;
        sum ^= even;
        put_long(t, even);
    }
    return sum;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A turn of the disk in the drive, in MFM words (high density: twice) */
static int turn_words(const drive_t *d)
{
    return d->sectors == SECTORS_HD ? 2 * TRACK_WORDS : TRACK_WORDS;
}

static void make_track(drive_t *d, int track, const uint8_t *sectors)
{
    track_writer_t t = { .w = d->mfm, .n = 0, .last = 0 };
    int n = d->sectors;
    /* the gap first, then sector 0 (WinUAE's layout): a loader that waits
     * for the index and then starts its DMA still finds sector 0's sync
     * (9 Fingers' missed it once its CIA reads took E clock time) */
    while (t.n < turn_words(d) - n * SECTOR_WORDS) {
        put_word(&t, (t.last & 1) ? 0x2AAA : 0xAAAA);
    }
    for (int s = 0; s < n; s++) {
        put_word(&t, (t.last & 1) ? 0x2AAA : 0xAAAA);
        put_word(&t, 0xAAAA);
        put_word(&t, SYNC);
        put_word(&t, SYNC);
        uint32_t info = 0xFF000000u | (uint32_t)track << 16 | (uint32_t)s << 8 | (uint32_t)(n - s);
        uint32_t label[4] = { 0 };
        uint32_t hsum = put_odd_even(&t, &info, 1);
        hsum ^= put_odd_even(&t, label, 4);
        uint32_t data[128];
        const uint8_t *src = sectors + (size_t)s * 512;
        for (int i = 0; i < 128; i++) {
            data[i] = be32(src + i * 4);
        }
        uint32_t dsum = 0;
        for (int i = 0; i < 128; i++) {
            dsum ^= ((data[i] >> 1) & 0x55555555) ^ (data[i] & 0x55555555);
        }
        put_odd_even(&t, &hsum, 1);
        put_odd_even(&t, &dsum, 1);
        put_odd_even(&t, data, 128);
    }
    while (t.n < turn_words(d)) {
        put_word(&t, (t.last & 1) ? 0x2AAA : 0xAAAA);
    }
    d->track_words = turn_words(d);
}

static void blank_track(drive_t *d)
{
    for (int i = 0; i < turn_words(d); i++) {
        d->mfm[i] = 0xAAAA;
    }
    d->track_words = turn_words(d);
}

/* The track under the head from the image */
static void load_track(drive_t *d, int track)
{
    d->mfm_track = track;
    if (!d->ext) {
        if (track < CYLINDERS * 2) {
            make_track(d, track, d->data + (size_t)track * d->sectors * 512);
        } else {
            blank_track(d);
        }
    } else if (track < d->ext_tracks) {
        const uint8_t *e = d->data + 12 + track * 12;
        int type = (e[2] << 8) | e[3];
        uint32_t bits = ((uint32_t)e[8] << 24) | (e[9] << 16) | (e[10] << 8) | e[11];
        const uint8_t *src = d->data + d->ext_off[track];
        if (type == 0 && bits >= SECTORS_DD * 512 * 8) {
            make_track(d, track, src);
        } else if (type == 1 && bits >= 16 * 1024) {
            int words = (int)(bits / 16);
            words = words > MFM_MAX ? MFM_MAX : words;
            for (int i = 0; i < words; i++) {
                d->mfm[i] = (uint16_t)((src[2 * i] << 8) | src[2 * i + 1]);
            }
            d->track_words = words;
        } else {
            blank_track(d);
        }
    } else {
        blank_track(d);
    }
    if (d->pos >= d->track_words) {
        d->pos %= d->track_words;
    }
}

/* (a written track: a standard one, n words, turn_words()) */
static uint32_t get_long(const uint16_t *w, int i, int n)
{
    return ((uint32_t)w[i % n] << 16) | w[(i + 1) % n];
}

static uint32_t decode(const uint16_t *w, int odd_at, int even_at, int n)
{
    return ((get_long(w, odd_at, n) & 0x55555555) << 1) | (get_long(w, even_at, n) & 0x55555555);
}

/* A written track back into the image's sectors */
static void decode_track(drive_t *d, int track)
{
    const uint16_t *w = d->mfm;
    int n = turn_words(d);
    for (int i = 0; i < n; i++) {
        if (w[i] != SYNC || w[(i + 1) % n] == SYNC) {
            continue;
        }
        int h = i + 1;
        uint32_t info = decode(w, h, h + 2, n);
        int sector = (info >> 8) & 0xFF;
        if ((info >> 24) != 0xFF || (int)((info >> 16) & 0xFF) != track || sector >= d->sectors) {
            continue;
        }
        uint8_t *dst = d->data + ((size_t)track * d->sectors + sector) * 512;
        int data_at = h + 4 + 16 + 4 + 4;
        for (int k = 0; k < 128; k++) {
            uint32_t v = decode(w, data_at + 2 * k, data_at + 256 + 2 * k, n);
            dst[4 * k] = (uint8_t)(v >> 24);
            dst[4 * k + 1] = (uint8_t)(v >> 16);
            dst[4 * k + 2] = (uint8_t)(v >> 8);
            dst[4 * k + 3] = (uint8_t)v;
        }
        d->dirty[track / 8] |= (uint8_t)(1u << (track & 7));
        d->any_dirty = true;
    }
}

/* An AmigaDOS track's 11 sectors from the turn written (n words) into out,
 * their checksums right; false if one isn't there
 */
static bool dos_sectors(const uint16_t *w, int n, int track, uint8_t *out)
{
    unsigned found = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] != SYNC || w[(i + 1) % n] == SYNC) {
            continue;
        }
        int h = i + 1;
        uint32_t info = decode(w, h, h + 2, n);
        int sector = (info >> 8) & 0xFF;
        if ((info >> 24) != 0xFF || (int)((info >> 16) & 0xFF) != track || sector >= SECTORS_DD) {
            continue;
        }
        uint32_t hsum = 0, dsum = 0;
        for (int k = 0; k < 10; k++) {
            hsum ^= get_long(w, h + 2 * k, n);
        }
        int data_at = h + 28;
        for (int k = 0; k < 256; k++) {
            dsum ^= get_long(w, data_at + 2 * k, n);
        }
        if ((hsum & 0x55555555) != decode(w, h + 20, h + 22, n) ||
            (dsum & 0x55555555) != decode(w, h + 24, h + 26, n)) {
            continue;
        }
        for (int k = 0; k < 128; k++) {
            put_be32(out + sector * 512 + 4 * k, decode(w, data_at + 2 * k, data_at + 256 + 2 * k, n));
        }
        found |= 1u << sector;
    }
    return found == (1u << SECTORS_DD) - 1;
}

/* An extended ADF's track entry: type (0 AmigaDOS, 1 raw), bytes, bits */
static uint8_t *ext_entry(drive_t *d, int track)
{
    return d->data + 12 + track * 12;
}

/* An extended ADF's track given room for bytes (the tracks after it moved) */
static bool ext_resize(drive_t *d, int track, uint32_t bytes)
{
    uint8_t *e = ext_entry(d, track);
    uint32_t old = be32(e + 4), end = d->ext_off[track] + old;
    if (d->size - old + bytes > EXT_MAX) {
        return false;
    }
    memmove(d->data + end - old + bytes, d->data + end, d->size - end);
    d->size = d->size - old + bytes;
    for (int t = track + 1; t < d->ext_tracks; t++) {
        d->ext_off[t] = d->ext_off[t] - old + bytes;
    }
    put_be32(e + 4, bytes);
    d->ext_moved = true;
    return true;
}

/* A track written to an extended ADF back into it (see the top) */
static void write_ext_track(drive_t *d, int track)
{
    static uint8_t sectors[SECTORS_DD * 512];
    uint8_t *e = ext_entry(d, track);
    int type = (e[2] << 8) | e[3];
    uint32_t bytes = (uint32_t)d->track_words * 2;
    if (type == 0 && be32(e + 4) >= sizeof(sectors) && dos_sectors(d->mfm, d->track_words, track, sectors)) {
        memcpy(d->data + d->ext_off[track], sectors, sizeof(sectors));
    } else {
        if ((type != 1 || be32(e + 4) < bytes) && !ext_resize(d, track, bytes)) {
            AMI_LOGW("DF%d: track %d doesn't fit in the image", (int)(d - s_drive), track);
            return;
        }
        if (type != 1 || be32(e + 8) / 16 != (uint32_t)d->track_words) {
            e[2] = 0;
            e[3] = 1;
            put_be32(e + 8, bytes * 8);
        }
        uint8_t *dst = d->data + d->ext_off[track];
        for (int i = 0; i < d->track_words; i++) {
            dst[2 * i] = (uint8_t)(d->mfm[i] >> 8);
            dst[2 * i + 1] = (uint8_t)d->mfm[i];
        }
    }
    d->dirty[track / 8] |= (uint8_t)(1u << (track & 7));
    d->any_dirty = true;
}

/* ---- The drives ---- */

void floppy_reset(void)
{
    s_at = ami_now();
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        d->present = i < AMIGA_FLOPPIES;
        d->motor = false;
        d->selected = false;
        d->cylinder = 0;
        d->changed = true;
        d->id_bit = 0;
        d->pos = 0;
        d->frac = 0;
    }
    s_side = 0;
    memset(&dk, 0, sizeof(dk));
}

void floppy_control(uint8_t old, uint8_t prb)
{
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        uint8_t sel = 0x08 << i;
        bool now = !(prb & sel);
        if (now && (old & sel)) {
            /* selected: the motor line is latched; with the motor off,
             * each selection shifts the ID out one bit. /MTR low in the
             * write before counts too (as WinUAE has it): Last Action
             * Hero's loader writes $7d (motor on, DF0 not selected),
             * then $f5 (DF0 selected, /MTR back high)
             */
            bool motor = !(prb & 0x80) || !(old & 0x80);
            if (d->motor && !motor) {
                d->id_bit = 0;
            } else if (!motor) {
                d->id_bit = (d->id_bit + 1) & 31;
            }
            if (motor && !d->motor) {
                d->spinup = SPINUP_CYCLES;
            }
            d->motor = motor;
        }
        d->selected = now;
    }
    s_side = (prb & 0x04) ? 0 : 1;
    if ((old & 0x01) && !(prb & 0x01)) {
        for (int i = 0; i < DRIVES; i++) {
            drive_t *d = &s_drive[i];
            if (!d->selected || !d->present) {
                continue;
            }
            if (prb & 0x02) {
                if (d->cylinder > 0) {
                    d->cylinder--;
                }
            } else if (d->cylinder < HEAD_STOPS - 1) {
                d->cylinder++;
            }
            if (d->data) {
                d->changed = false;
            }
        }
    }
}

uint8_t floppy_status(void)
{
    uint8_t v = 0x3C;
    for (int i = 0; i < DRIVES; i++) {
        const drive_t *d = &s_drive[i];
        if (!d->selected || !d->present) {
            continue;
        }
        /* a 3.5" drive: its ID through /RDY while the motor is off, all
         * ones (double density), $AAAAAAAA with a high-density disk in it;
         * the nth selection after the motor went off gives bit 31 - n (as
         * UAE has it: with bit 31 first, Kickstart 3.1 took the drive for
         * none it knew and never read it); ready once up to speed
         */
        bool id = d->data && d->sectors == SECTORS_HD ? (d->id_bit & 1) == 0 : true;
        if (d->motor ? d->spinup <= 0 : id) {
            v &= ~0x20;
        }
        if (d->cylinder == 0) {
            v &= ~0x10;
        }
        if (!d->data || d->protect) {
            v &= ~0x08;
        }
        if (d->changed) {
            v &= ~0x04;
        }
    }
    return v;
}

/* The drive whose data reaches Paula: the first selected one turning */
static drive_t *reading(void)
{
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (d->selected && d->present && d->motor && d->data) {
            return d;
        }
    }
    return NULL;
}

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_DSK)) == (DMA_EN | DMA_DSK);
}

static void end_dma(void)
{
    if (dk.writing) {
        drive_t *d = &s_drive[dk.drive];
        if (d->data && !d->ext && d->mfm_track >= 0 && d->mfm_track < CYLINDERS * 2 && !d->protect &&
            d->track_words == turn_words(d)) {
            decode_track(d, d->mfm_track);
        } else if (d->data && d->ext && d->mfm_track >= 0 && d->mfm_track < d->ext_tracks && !d->protect) {
            write_ext_track(d, d->mfm_track);
        }
        d->writes++;
    }
    dk.active = false;
    ami_set_intreq(INT_DSKBLK);
}

static void deliver(uint16_t w)
{
    chip_write16(dk.pt, w);
    dk.pt += 2;
    if (--dk.left == 0) {
        s_drive[dk.drive].reads++;
        end_dma();
    }
}

/* A word passes under the head of the drive Paula reads. With WORDSYNC
 * its bits go through Paula's shift register one at a time: DSKSYNC is
 * found at any bit (raw tracks needn't have it on a word boundary), starts
 * the transfer (not itself transferred) and puts the words in step again
 * wherever it comes. Without, the words go as they are.
 */
static void word_passes(drive_t *d)
{
    int track = d->cylinder * 2 + s_side;
    if (d->mfm_track != track) {
        load_track(d, track);
    }
    uint16_t w = d->mfm[d->pos];
    bool dma = dk.active && dma_on();
    dk.byte_ready = true;
    dk.wordequal = false;
    if (dma && dk.writing) {
        d->mfm[d->pos] = chip_read16(dk.pt);
        dk.pt += 2;
        dk.drive = (int)(d - s_drive);
        if (--dk.left == 0) {
            end_dma();
        }
        return;
    }
    if (!(ami.adkcon & ADK_WORDSYNC)) {
        dk.shift = w;
        if (w == dk.sync) {
            dk.wordequal = true;
            ami_set_intreq(INT_DSKSYN);
        }
        if (dma) {
            deliver(w);
        }
        return;
    }
    for (int b = 15; b >= 0; b--) {
        dk.shift = (uint16_t)((dk.shift << 1) | ((w >> b) & 1));
        dk.bits++;
        if (dk.shift == dk.sync) {
            dk.wordequal = true;
            ami_set_intreq(INT_DSKSYN);
            if (dma && !dk.synced) {
                dk.synced = true;
                dk.bits = 0;
                continue;
            }
            dk.bits = 16;
        }
        if (dk.bits >= 16) {
            dk.bits = 0;
            if (dma && dk.synced && dk.active) {
                deliver(dk.shift);
                dma = dk.active;
            }
        }
    }
}

/* Bring DMA up to its start/stop or enable change. A DSKLEN write
 * partway through a line must not capture words that passed
 * earlier in that line (Hyperion could lose a sync and decode a short
 * final sector). The line's end advances whatever no access has reached.
 */
void floppy_sync(int at)
{
    int cycles = at - s_at;
    if (cycles <= 0) {
        return;
    }
    s_at = at;
    drive_t *r = reading();
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (d->motor && d->spinup > 0) {
            d->spinup -= cycles;
        }
        if (!d->motor || !d->data) {
            continue;
        }
        d->frac += d->fast ? cycles * 8 : cycles;
        while (d->frac >= WORD_CYCLES) {
            d->frac -= WORD_CYCLES;
            if (d == r) {
                word_passes(d);
            }
            if (++d->pos >= d->track_words) {
                d->pos = 0;
                if (d->selected) {
                    cia_flag(&cia[1]);      /* the index pulse */
                }
            }
        }
    }
}

void floppy_rebase(int frame_cycles)
{
    s_at -= frame_cycles;
}

/* ---- Paula's disk registers ---- */

void floppy_write(uint32_t reg, uint16_t v, int at)
{
    switch (reg) {
    case 0x020:                     /* DSKPTH, DSKPTL */
        dk.pt = (dk.pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        break;
    case 0x022:
        dk.pt = (dk.pt & 0xFFFF0000) | (v & 0xFFFE);
        break;
    case 0x024:                     /* DSKLEN: written twice with bit 15 starts it */
        floppy_sync(at);
        if (!(v & 0x8000)) {
            dk.active = false;
        } else if (dk.len & 0x8000) {
            if (amiga_disk_hook) {
                drive_t *rd = reading();
                amiga_disk_hook(v, dk.sync, ami.adkcon, rd ? rd->cylinder : -1, s_side, dk.pt);
            }
            dk.active = (v & 0x3FFF) != 0;
            dk.writing = (v & 0x4000) != 0;
            dk.left = v & 0x3FFF;
            dk.synced = false;
            dk.drive = 0;
            drive_t *d = reading();
            if (d) {
                dk.drive = (int)(d - s_drive);
            }
        }
        dk.len = v;
        break;
    case 0x07E:                     /* DSKSYNC */
        floppy_sync(at);            /* the new sync takes effect from here */
        dk.sync = v;
        break;
    default:
        break;
    }
}

uint16_t floppy_dskbytr(void)
{
    uint16_t v = 0;
    drive_t *d = reading();
    if (d && d->mfm_track >= 0) {
        v = dk.shift & 0xFF;
    }
    if (dk.byte_ready) {
        v |= 0x8000;
        dk.byte_ready = false;
    }
    if (dk.active) {
        v |= 0x4000;
    }
    if (dk.active && dk.writing) {
        v |= 0x2000;
    }
    if (dk.wordequal) {
        v |= 0x1000;
    }
    return v;
}

/* ---- The images ---- */

/* The written tracks back to the file (an extended ADF laid out again:
 * all of it; it only grows)
 */
static void flush_drive(drive_t *d)
{
    if (!d->data || !d->any_dirty || d->protect || !d->path[0]) {
        return;
    }
    FILE *f = fopen(d->path, "r+b");
    if (f && d->ext && d->ext_moved) {
        fwrite(d->data, 1, d->size, f);
        d->ext_moved = false;
    } else if (f) {
        for (int t = 0; t < (d->ext ? d->ext_tracks : CYLINDERS * 2); t++) {
            if (!(d->dirty[t / 8] & (1u << (t & 7)))) {
                continue;
            }
            if (d->ext) {
                fseek(f, 12 + t * 12, SEEK_SET);
                fwrite(ext_entry(d, t), 1, 12, f);
                fseek(f, (long)d->ext_off[t], SEEK_SET);
                fwrite(d->data + d->ext_off[t], 1, be32(ext_entry(d, t) + 4), f);
            } else {
                long off = (long)t * d->sectors * 512;
                fseek(f, off, SEEK_SET);
                fwrite(d->data + off, 1, (size_t)d->sectors * 512, f);
            }
        }
    }
    if (f) {
        fclose(f);
    }
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
}

/* The drives' image and track buffers, kept for the next disk (big blocks
 * fragment PSRAM); an extended ADF's bigger one when one comes
 */
static bool buffers(int drive, uint32_t size)
{
    static uint8_t *images[AMIGA_FLOPPIES];
    static uint32_t sizes[AMIGA_FLOPPIES];
    static uint16_t *tracks[AMIGA_FLOPPIES];
    drive_t *d = &s_drive[drive];
    if (!tracks[drive]) {
        tracks[drive] = ami_malloc(MFM_MAX * sizeof(uint16_t));
    }
    if (sizes[drive] < size) {
        free(images[drive]);
        images[drive] = ami_malloc(size > ADF_SIZE ? EXT_MAX : ADF_SIZE);
        sizes[drive] = images[drive] ? (size > ADF_SIZE ? EXT_MAX : ADF_SIZE) : 0;
    }
    d->data = images[drive];
    d->mfm = tracks[drive];
    return d->data && d->mfm;
}

/* An AmigaDOS disk with a filesystem (a bootblock saying DOS, a root block
 * in the middle: 880, high density 1760), which loads through trackdisk:
 * fine turning fast
 */
static bool dos_filesystem(const uint8_t *adf, int sectors)
{
    const uint8_t *root = adf + (size_t)CYLINDERS * sectors * 512;
    return adf[0] == 'D' && adf[1] == 'O' && adf[2] == 'S' && root[0] == 0 && root[1] == 0 && root[2] == 0 &&
           root[3] == 2 && root[508] == 0 && root[509] == 0 && root[510] == 0 && root[511] == 1;
}

/* An extended ADF's track table; false if it isn't one */
static bool ext_tracks(drive_t *d)
{
    if (d->size < 12 || memcmp(d->data, "UAE-1ADF", 8) != 0) {
        return false;
    }
    int n = (d->data[10] << 8) | d->data[11];
    n = n > EXT_TRACKS ? EXT_TRACKS : n;
    uint32_t off = 12 + (uint32_t)n * 12;
    for (int t = 0; t < n; t++) {
        const uint8_t *e = d->data + 12 + t * 12;
        d->ext_off[t] = off;
        off += ((uint32_t)e[4] << 24) | (e[5] << 16) | (e[6] << 8) | e[7];
    }
    if (off > d->size) {
        return false;
    }
    d->ext_tracks = n;
    return true;
}

/* The image now in d->data (d->size bytes) into the drive */
static bool take(int drive, const char *file, bool ro)
{
    drive_t *d = &s_drive[drive];
    d->ext = d->size != ADF_SIZE && d->size != HD_SIZE;
    d->sectors = d->size == HD_SIZE ? SECTORS_HD : SECTORS_DD;
    if (d->ext && !ext_tracks(d)) {
        AMI_LOGW("DF%d: %s: %lu bytes is not an ADF", drive, file, (unsigned long)d->size);
        d->data = NULL;
        return false;
    }
    d->mfm_track = -1;
    d->protect = ro;
    d->ext_moved = false;
    d->changed = true;
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
    snprintf(d->path, sizeof(d->path), "%s", file);
    snprintf(d->shown, sizeof(d->shown), "%s", file);
    d->rp9_disks = 0;
    d->fast = !d->ext && dos_filesystem(d->data, d->sectors);
    return true;
}

static bool load_file(int drive, const char *path)
{
    drive_t *d = &s_drive[drive];
    bool ro = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        ro = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != ADF_SIZE && (size < 12 || size > EXT_MAX)) {
        AMI_LOGW("DF%d: %s: %ld bytes is not an ADF", drive, path, size);
        fclose(f);
        return false;
    }
    /* (an extended ADF: room for its tracks to grow when written) */
    bool ok = buffers(drive, size == ADF_SIZE ? ADF_SIZE : EXT_MAX) && fread(d->data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        d->data = NULL;
        return false;
    }
    d->size = (uint32_t)size;
    return take(drive, path, ro);
}

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* An RP9's disk n through its hidden copy (made the first time) */
static bool load_rp9(int drive, const char *path, int n, const rp9_t *r)
{
    drive_t *d = &s_drive[drive];
    char copy[sizeof(d->path)];
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    int dir = slash ? (int)(slash - path + 1) : 0;
    const char *dot = strrchr(r->disk[n], '.');
    snprintf(copy, sizeof(copy), "%.*s.%s.%d%s", dir, path, base, n + 1, dot ? dot : ".adf");
    struct stat st;
    if (stat(copy, &st) == 0 && st.st_size > 0 && load_file(drive, copy)) {
        return true;
    }
    if (!buffers(drive, EXT_MAX)) {
        return false;
    }
    uint32_t size = 0;
    if (!rp9_member(path, r->disk[n], d->data, EXT_MAX, &size)) {
        AMI_LOGW("DF%d: %s: no %s in it", drive, path, r->disk[n]);
        d->data = NULL;
        return false;
    }
    d->size = size;
    FILE *f = fopen(copy, "wb");
    bool saved = f && fwrite(d->data, 1, size, f) == size;
    if (f) {
        fclose(f);
    }
    if (saved) {
        return take(drive, copy, false);
    }
    if (!take(drive, copy, true)) {     /* (kept in memory only: write protected) */
        return false;
    }
    d->path[0] = 0;
    return true;
}

bool amiga_floppy_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= AMIGA_FLOPPIES) {
        return false;
    }
    drive_t *d = &s_drive[drive];
    if (!has_ext(path, ".rp9")) {
        amiga_floppy_eject(drive);
        if (!load_file(drive, path)) {
            return false;
        }
        AMI_LOGI("DF%d: %s%s", drive, path, d->protect ? " (write protected)" : "");
        return true;
    }
    rp9_t r;
    if (!rp9_read(path, &r)) {
        AMI_LOGW("DF%d: %s: not an RP9 package", drive, path);
        return false;
    }
    if (r.disks == 0) {
        AMI_LOGW("DF%d: %s has no floppy disks (a hard disk image: not yet)", drive, path);
        return false;
    }
    /* the same package again: its next disk */
    int n = d->data && d->rp9_disks && strcmp(d->shown, path) == 0 ? (d->rp9_disk + 1) % r.disks : 0;
    amiga_floppy_eject(drive);
    if (!load_rp9(drive, path, n, &r)) {
        return false;
    }
    snprintf(d->shown, sizeof(d->shown), "%s", path);
    d->rp9_disk = n;
    d->rp9_disks = r.disks;
    d->fast = r.turbo && !d->ext;
    AMI_LOGI("DF%d: %s, disk %d of %d (%s, Kickstart %d.%d wanted)", drive, r.title, n + 1, r.disks, r.system,
             r.rom / 100, r.rom % 100 / 10);
    return true;
}

void amiga_floppy_eject(int drive)
{
    if (drive < 0 || drive >= AMIGA_FLOPPIES) {
        return;
    }
    drive_t *d = &s_drive[drive];
    flush_drive(d);
    d->data = NULL;
    d->mfm_track = -1;
    d->path[0] = 0;
    d->shown[0] = 0;
    d->rp9_disks = 0;
    d->changed = true;
    if (dk.active && dk.drive == drive) {
        dk.active = false;
    }
}

const char *amiga_floppy_path(int drive)
{
    return drive >= 0 && drive < AMIGA_FLOPPIES && s_drive[drive].data ? s_drive[drive].shown : NULL;
}

/* A drive's ADF and its blocks (an ordinary one, double or high density;
 * NULL: none, or an extended ADF)
 */
const uint8_t *floppy_image(int drive, uint32_t *blocks)
{
    const drive_t *d = drive >= 0 && drive < AMIGA_FLOPPIES ? &s_drive[drive] : NULL;
    if (!d || !d->data || d->ext) {
        return NULL;
    }
    *blocks = (uint32_t)CYLINDERS * 2 * d->sectors;
    return d->data;
}

bool amiga_floppy_write_protected(int drive)
{
    return drive >= 0 && drive < AMIGA_FLOPPIES && s_drive[drive].protect;
}

bool amiga_floppy_create(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    static const uint8_t zero[512];
    bool ok = true;
    for (int i = 0; i < ADF_SIZE / 512 && ok; i++) {
        ok = fwrite(zero, 1, sizeof(zero), f) == sizeof(zero);
    }
    return fclose(f) == 0 && ok;
}

bool amiga_floppy_dirty(void)
{
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        if (s_drive[i].any_dirty) {
            return true;
        }
    }
    return false;
}

void amiga_floppy_flush(void)
{
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        flush_drive(&s_drive[i]);
    }
}

void amiga_floppy_status(amiga_floppy_status_t *st)
{
    st->light = -1;
    for (int i = 0; i < AMIGA_FLOPPIES; i++) {
        const drive_t *d = &s_drive[i];
        st->reads[i] = d->reads;
        st->writes[i] = d->writes;
        st->cylinder[i] = (uint8_t)d->cylinder;
        st->disk[i] = (uint8_t)(d->data ? d->rp9_disk + 1 : 0);
        st->disks[i] = (uint8_t)(d->data ? d->rp9_disks : 0);
        if (d->motor && st->light < 0) {
            st->light = i;
        }
    }
}
