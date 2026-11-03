/*
 * Gayle (the A600's gate array): its IDE port with two drives, the hard
 * disk images (HDF), read and written in place.
 *
 *   $DA0000-$DA7FFF  the IDE task file: register (address >> 2) & 7, the
 *                    control block (alternate status, device control)
 *                    where address bit 12 is set ($DA101A); the registers
 *                    on the high byte, the data register 16 bits
 *   $DA8000          card status (no PCMCIA card)
 *   $DA9000          interrupt status: bit 7 the IDE drive's, latched as
 *                    it asks; writing 0 there clears it
 *   $DAA000          interrupt enable: bit 7 IDE (to level 2, PORTS)
 *   $DAB000          configuration
 *   $DE1000          Gayle's ID ($D0), a bit (7) each read, MSB first;
 *                    writing starts it again
 *
 * Kickstart 2.05 and 3.1 (the A600's) drive it with their scsi.device and
 * mount the partitions of a disk with an RDB. Gayle is there on the A600,
 * and on the other models while a hard disk is inserted.
 *
 * The drives (the PC's hdc.c's ATA): recalibrate, read and write sectors
 * (and MULTIPLE), verify, seek, diagnostics, initialize device parameters
 * (the CHS geometry from then on), identify, set multiple, set features,
 * flush and the power commands (accepted); CHS or LBA. The geometry an
 * image gets: up to 16 heads and 63 sectors that divide it exactly (every
 * block reachable by CHS), else 16 x 63. Gayle swaps the IDE bus's two
 * bytes: a word read gives a sector's bytes in their order (high, low),
 * the HDF's (a disk reads the same on a PC), and IDENTIFY's words come
 * swapped (scsi.device swaps them back). Each sector takes two lines,
 * then the interrupt.
 *
 * The images (hdf.c): a file, or an RP9 game's disk with its system disk.
 */
#include <stdio.h>
#include <string.h>
#include "amiga_internal.h"

#define SECTOR          512
#define BLOCK_MAX       16
#define LINES_A_SECTOR  2
#define GAYLE_ID        0xD0            /* the A600's; the A1200's $D1 */

enum {
    ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80,
    ER_ABRT = 0x04, ER_IDNF = 0x10, ER_UNC = 0x40,
    CTL_NIEN = 0x02, CTL_SRST = 0x04,
};

typedef struct {
    hdf_t hdf;
    bool present;
    char path[256];                 /* as inserted (an RP9: the package) */
    uint32_t blocks;
    uint16_t cylinders, heads, sectors;         /* its own geometry (IDENTIFY's) */
    uint16_t cur_heads, cur_sectors;            /* INITIALIZE DEVICE PARAMETERS' */
    uint32_t reads, writes;
} drive_t;

static drive_t s_drive[AMIGA_HARD_DISKS];

static struct {
    uint8_t error, features, count, sector, cyl_lo, cyl_hi, sdh, status, control;
    uint8_t command;
    bool intrq;                     /* the drive's interrupt line */
    bool latched;                   /* Gayle's IDE interrupt bit */
    uint8_t int_enable, config;
    int id_bit;
    /* (aligned to the cache line, read and written past stdio: the SD card
     * takes runs of sectors from it)
     */
    uint8_t buf[SECTOR * BLOCK_MAX] __attribute__((aligned(128)));
    int pos, len;                   /* the data transfer: bytes */
    int wait;                       /* lines until the command's next step (0: none) */
    int left;                       /* sectors left */
    int multiple;                   /* SET MULTIPLE's block (0: a sector a DRQ) */
    uint32_t lba;                   /* the next sector */
    int light;                      /* the drive in use (-1: none) */
} g;

/* (the master only: the slave is our hard disk board's, hdboard.c) */
static drive_t *selected(void)
{
    drive_t *d = &s_drive[0];
    return !(g.sdh & 0x10) && d->present ? d : NULL;
}

hdf_t *hd_image(int n)
{
    return n >= 0 && n < AMIGA_HARD_DISKS && s_drive[n].present ? &s_drive[n].hdf : NULL;
}

/* (the A600's; the others' while a hard disk is in and the Kickstart has
 * the driver: else our own board serves the disks, hdboard.c)
 */
bool gayle_present(void)
{
    return ami.model == AMIGA_A600 || ami.model == AMIGA_A1200 || (s_drive[0].present && ami_kick_has_ide());
}

/* Gayle's level 2 interrupt */
bool gayle_irq(void)
{
    return g.latched && (g.int_enable & 0x80);
}

static void set_intrq(bool on)
{
    if (on && !(g.control & CTL_NIEN)) {
        g.intrq = true;
        if (!g.latched) {
            g.latched = true;
            if (gayle_irq()) {
                ami_set_intreq(INT_PORTS);
            }
        }
    } else if (!on) {
        g.intrq = false;
    }
}

void gayle_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.status = ST_DRDY | ST_DSC;
    g.count = 1;
    g.sector = 1;
    g.light = -1;
    for (int i = 0; i < AMIGA_HARD_DISKS; i++) {
        s_drive[i].cur_heads = s_drive[i].heads;
        s_drive[i].cur_sectors = s_drive[i].sectors;
    }
}

/* ---- The images ---- */

static void geometry(drive_t *d)
{
    uint32_t best = 0;
    d->heads = 16;
    d->sectors = 63;
    for (int s = 63; s >= 1; s--) {
        for (int h = 16; h >= 1; h--) {
            uint32_t per = (uint32_t)h * s;
            if (per > best && d->blocks % per == 0 && d->blocks / per <= 65535) {
                best = per;
                d->heads = (uint16_t)h;
                d->sectors = (uint16_t)s;
            }
        }
    }
    uint32_t c = d->blocks / ((uint32_t)d->heads * d->sectors);
    d->cylinders = (uint16_t)(c > 65535 ? 65535 : c);
    d->cur_heads = d->heads;
    d->cur_sectors = d->sectors;
}

bool amiga_hd_insert(int n, const char *path)
{
    if (n < 0 || n >= AMIGA_HARD_DISKS) {
        return false;
    }
    amiga_hd_eject(n);
    drive_t *d = &s_drive[n];
    if (!hdf_open(&d->hdf, path)) {
        return false;
    }
    d->present = true;
    d->blocks = d->hdf.blocks;
    snprintf(d->path, sizeof(d->path), "%s", path);
    geometry(d);
    d->reads = d->writes = 0;
    AMI_LOGI("DH%d: %s, %lu KB, %u/%u/%u%s", n, path, (unsigned long)(d->blocks / 2), d->cylinders, d->heads,
             d->sectors, d->hdf.read_only ? ", read-only" : "");
    return true;
}

void amiga_hd_eject(int n)
{
    if (n >= 0 && n < AMIGA_HARD_DISKS && s_drive[n].present) {
        hdf_close(&s_drive[n].hdf);
        memset(&s_drive[n], 0, sizeof(s_drive[n]));
    }
}

const char *amiga_hd_path(int n)
{
    return n >= 0 && n < AMIGA_HARD_DISKS && s_drive[n].present ? s_drive[n].path : NULL;
}

bool amiga_hd_write_protected(int n)
{
    return n >= 0 && n < AMIGA_HARD_DISKS && s_drive[n].hdf.read_only;
}

const char *amiga_hd_detail(int n)
{
    return n >= 0 && n < AMIGA_HARD_DISKS && s_drive[n].present && s_drive[n].hdf.detail[0] ? s_drive[n].hdf.detail
                                                                                                : NULL;
}

void amiga_hd_get_status(amiga_hd_status_t *st)
{
    st->light = g.light;
    for (int i = 0; i < AMIGA_HARD_DISKS; i++) {
        st->reads[i] = s_drive[i].reads;
        st->writes[i] = s_drive[i].writes;
    }
}

/* A drive's light and counters (our hard disk board's transfers too) */
void hd_activity(int n, uint32_t sectors, bool write_it)
{
    if (n < 0 || n >= AMIGA_HARD_DISKS) {
        return;
    }
    g.light = n;
    if (write_it) {
        s_drive[n].writes += sectors;
    } else {
        s_drive[n].reads += sectors;
    }
}

static bool io(drive_t *d, uint32_t lba, int count, bool write_it)
{
    hd_activity((int)(d - s_drive), (uint32_t)count, write_it);
    return hdf_io(&d->hdf, lba, count, g.buf, write_it);
}

/* ---- Commands ---- */

/* The task file's address: LBA, or CHS through the current geometry */
static bool address(drive_t *d, uint32_t *lba)
{
    if (g.sdh & 0x40) {
        *lba = ((uint32_t)(g.sdh & 15) << 24) | ((uint32_t)g.cyl_hi << 16) | ((uint32_t)g.cyl_lo << 8) | g.sector;
        return true;
    }
    uint32_t c = (uint32_t)g.cyl_hi << 8 | g.cyl_lo, h = g.sdh & 15;
    if (g.sector == 0 || g.sector > d->cur_sectors || h >= d->cur_heads) {
        return false;
    }
    *lba = (c * d->cur_heads + h) * d->cur_sectors + g.sector - 1;
    return true;
}

/* The task file after the sectors so far (the next one, as drives leave it) */
static void advance(uint32_t lba)
{
    drive_t *d = selected();
    if (g.sdh & 0x40) {
        g.sector = (uint8_t)lba;
        g.cyl_lo = (uint8_t)(lba >> 8);
        g.cyl_hi = (uint8_t)(lba >> 16);
        g.sdh = (uint8_t)((g.sdh & 0xF0) | ((lba >> 24) & 15));
    } else if (d) {
        uint32_t s = lba % d->cur_sectors, t = lba / d->cur_sectors;
        g.sector = (uint8_t)(s + 1);
        g.sdh = (uint8_t)((g.sdh & 0xF0) | (t % d->cur_heads));
        g.cyl_lo = (uint8_t)(t / d->cur_heads);
        g.cyl_hi = (uint8_t)((t / d->cur_heads) >> 8);
    }
}

static void finish(uint8_t error)
{
    g.error = error;
    g.status = (uint8_t)(ST_DRDY | ST_DSC | (error ? ST_ERR : 0));
    g.pos = g.len = 0;
    g.wait = 0;
    set_intrq(true);
}

static void put_string(uint16_t *w, int words, const char *s)
{
    for (int i = 0; i < words; i++) {
        char a = *s ? *s++ : ' ';
        char b = *s ? *s++ : ' ';
        w[i] = (uint16_t)(a << 8 | b);
    }
}

static void identify(drive_t *d)
{
    uint16_t w[256];
    memset(w, 0, sizeof(w));
    w[0] = 0x0040;
    w[1] = d->cylinders;
    w[3] = d->heads;
    w[4] = (uint16_t)(SECTOR * d->sectors);
    w[5] = SECTOR;
    w[6] = d->sectors;
    put_string(&w[10], 10, "ESP32AMI");
    w[20] = 3;
    w[21] = BLOCK_MAX;
    put_string(&w[23], 4, "1.0");
    put_string(&w[27], 20, "ESP32AMI HARD DISK IMAGE");
    w[47] = 0x8000 | BLOCK_MAX;
    w[49] = 0x0200;                 /* LBA */
    w[51] = 0x0200;
    w[53] = 1;
    w[54] = (uint16_t)(d->cur_heads && d->cur_sectors ? d->blocks / (d->cur_heads * d->cur_sectors) : 0);
    w[55] = d->cur_heads;
    w[56] = d->cur_sectors;
    uint32_t cur = (uint32_t)w[54] * w[55] * w[56];
    w[57] = (uint16_t)cur;
    w[58] = (uint16_t)(cur >> 16);
    w[59] = g.multiple ? (uint16_t)(0x100 | g.multiple) : 0;
    w[60] = (uint16_t)d->blocks;
    w[61] = (uint16_t)(d->blocks >> 16);
    /* (the drive's words low byte first, as it sends them: Gayle swaps the
     * bus's bytes, and scsi.device swaps these back)
     */
    for (int i = 0; i < 256; i++) {
        g.buf[2 * i] = (uint8_t)w[i];
        g.buf[2 * i + 1] = (uint8_t)(w[i] >> 8);
    }
}

/* The sectors of the next DRQ block: a sector, or SET MULTIPLE's */
static int block(void)
{
    int n = (g.command == 0xC4 || g.command == 0xC5) && g.multiple ? g.multiple : 1;
    return n < g.left ? n : g.left;
}

/* A read's next block into the buffer, ready for the host */
static void read_block(void)
{
    drive_t *d = selected();
    int n = block();
    if (!d || !io(d, g.lba, n, false)) {
        finish(ER_IDNF);
        return;
    }
    g.lba += (uint32_t)n;
    g.left -= n;
    advance(g.lba);
    g.pos = 0;
    g.len = n * SECTOR;
    g.status = ST_DRDY | ST_DSC | ST_DRQ;
    set_intrq(true);
}

static void command(uint8_t cmd)
{
    drive_t *d = selected();
    g.command = cmd;
    if (!d) {
        return;                     /* no drive answers */
    }
    set_intrq(false);
    g.error = 0;
    switch (cmd) {
    case 0x20: case 0x21: case 0xC4:            /* READ SECTORS, READ MULTIPLE */
    case 0x30: case 0x31: case 0xC5: {          /* WRITE SECTORS, WRITE MULTIPLE */
        if ((cmd == 0xC4 || cmd == 0xC5) && !g.multiple) {
            finish(ER_ABRT);
            return;
        }
        uint32_t lba;
        if (!address(d, &lba)) {
            finish(ER_IDNF);
            return;
        }
        g.lba = lba;
        g.left = g.count ? g.count : 256;
        if (cmd == 0x30 || cmd == 0x31 || cmd == 0xC5) {
            if (d->hdf.read_only) {
                finish(ER_ABRT);
                return;
            }
            g.pos = 0;
            g.len = block() * SECTOR;
            g.status = ST_DRDY | ST_DSC | ST_DRQ;     /* the first block's data now, no interrupt */
            return;
        }
        g.status = ST_DRDY | ST_DSC | ST_BSY;
        g.wait = LINES_A_SECTOR * block();
        return;
    }
    case 0x40: case 0x41:                       /* READ VERIFY */
    case 0x70:                                  /* SEEK */
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        g.status = ST_DRDY | ST_DSC | ST_BSY;
        g.wait = LINES_A_SECTOR;
        return;
    case 0x90:                                  /* EXECUTE DEVICE DIAGNOSTIC: passed */
        g.error = 0x01;
        g.status = ST_DRDY | ST_DSC;
        set_intrq(true);
        return;
    case 0x91:                                  /* INITIALIZE DEVICE PARAMETERS */
        d->cur_heads = (uint16_t)((g.sdh & 15) + 1);
        d->cur_sectors = g.count;
        finish(0);
        return;
    case 0xC6:                                  /* SET MULTIPLE MODE */
        if (g.count > BLOCK_MAX || (g.count & (g.count - 1))) {
            finish(ER_ABRT);
        } else {
            g.multiple = g.count;
            finish(0);
        }
        return;
    case 0xEC:                                  /* IDENTIFY DEVICE */
        identify(d);
        g.pos = 0;
        g.len = SECTOR;
        g.status = ST_DRDY | ST_DSC | ST_DRQ;
        set_intrq(true);
        return;
    case 0xEF:                                  /* SET FEATURES */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5: case 0xE6: case 0xE7:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99:
        finish(0);
        return;
    default:
        finish(ER_ABRT);
        return;
    }
}

/* Each line: a command's next step after its time */
void gayle_line(void)
{
    if (g.wait == 0 || --g.wait > 0) {
        return;
    }
    switch (g.command) {
    case 0x20: case 0x21: case 0xC4:
        read_block();
        break;
    case 0x30: case 0x31: case 0xC5:
        /* the block written: the next one's data, or done */
        if (g.left == 0) {
            finish(0);
        } else {
            g.pos = 0;
            g.len = block() * SECTOR;
            g.status = ST_DRDY | ST_DSC | ST_DRQ;
            set_intrq(true);
        }
        break;
    case 0x40: case 0x41: {
        drive_t *d = selected();
        uint32_t lba;
        int n = g.count ? g.count : 256;
        finish(d && address(d, &lba) && lba + (uint32_t)n <= d->blocks ? 0 : ER_IDNF);
        break;
    }
    default:
        finish(0);
        break;
    }
}

/* ---- The data register ---- */

static uint16_t data_read(void)
{
    if (g.pos >= g.len) {
        return 0xFFFF;
    }
    uint16_t v = (uint16_t)(g.buf[g.pos] << 8 | g.buf[g.pos + 1]);
    g.pos += 2;
    if (g.pos >= g.len) {
        g.status &= (uint8_t)~ST_DRQ;
        if ((g.command == 0x20 || g.command == 0x21 || g.command == 0xC4) && g.left > 0) {
            g.status |= ST_BSY;
            g.wait = LINES_A_SECTOR * block();
        }
    }
    return v;
}

static void data_write(uint16_t v)
{
    if (g.pos >= g.len) {
        return;
    }
    g.buf[g.pos] = (uint8_t)(v >> 8);
    g.buf[g.pos + 1] = (uint8_t)v;
    g.pos += 2;
    if (g.pos < g.len) {
        return;
    }
    drive_t *d = selected();
    int n = g.len / SECTOR;
    g.status &= (uint8_t)~ST_DRQ;
    if (!d || !io(d, g.lba, n, true)) {
        finish(ER_IDNF);
        return;
    }
    g.lba += (uint32_t)n;
    g.left -= n;
    advance(g.lba);
    g.status |= ST_BSY;
    g.wait = LINES_A_SECTOR * n;
}

/* ---- The bus ---- */

static uint8_t reg_read(uint32_t a)
{
    int reg = (a >> 2) & 7;
    drive_t *d = selected();
    if (a & 0x1000) {
        return reg == 6 ? (d ? g.status : 0) : 0xFF;     /* alternate status */
    }
    switch (reg) {
    case 1: return g.error;
    case 2: return g.count;
    case 3: return g.sector;
    case 4: return g.cyl_lo;
    case 5: return g.cyl_hi;
    case 6: return (uint8_t)(g.sdh | 0xA0);
    case 7:
        if (!d) {
            return 0;               /* (no drive: nothing pulls the bus up) */
        }
        set_intrq(false);
        return g.status;
    default:
        return 0xFF;
    }
}

static void reg_write(uint32_t a, uint8_t v)
{
    int reg = (a >> 2) & 7;
    if (a & 0x1000) {
        if (reg == 6) {
            if ((v & CTL_SRST) && !(g.control & CTL_SRST)) {
                uint8_t ctl = g.control;
                gayle_reset();
                g.control = ctl;
            }
            g.control = v;
        }
        return;
    }
    switch (reg) {
    case 1: g.features = v; break;
    case 2: g.count = v; break;
    case 3: g.sector = v; break;
    case 4: g.cyl_lo = v; break;
    case 5: g.cyl_hi = v; break;
    case 6: g.sdh = v; break;
    case 7: command(v); break;
    default: break;
    }
}

uint8_t gayle_read8(uint32_t a)
{
    if ((a & 0xFF0000) == 0xDE0000) {
        uint8_t v = (uint8_t)(((GAYLE_ID | (ami.model == AMIGA_A1200)) << g.id_bit) & 0x80);
        g.id_bit = (g.id_bit + 1) & 7;
        return v;
    }
    uint32_t off = a & 0xFFFF;
    if (a & 1) {
        return 0xFF;
    }
    if (off < 0x8000) {
        if (((off >> 2) & 7) == 0 && !(off & 0x1000)) {
            return (uint8_t)(data_read() >> 8);
        }
        return reg_read(off);
    }
    switch (off >> 12) {
    case 0x9: return g.latched ? 0x80 : 0;
    case 0xA: return g.int_enable;
    case 0xB: return g.config;
    default: return 0;              /* card status: no card */
    }
}

uint16_t gayle_read16(uint32_t a)
{
    uint32_t off = a & 0xFFFF;
    if ((a & 0xFF0000) == 0xDA0000 && off < 0x8000 && ((off >> 2) & 7) == 0 && !(off & 0x1000)) {
        return data_read();
    }
    uint8_t v = gayle_read8(a & ~1u);
    return (uint16_t)(v << 8 | v);
}

void gayle_write8(uint32_t a, uint8_t v)
{
    if ((a & 0xFF0000) == 0xDE0000) {
        g.id_bit = 0;
        return;
    }
    uint32_t off = a & 0xFFFF;
    if (a & 1) {
        return;
    }
    if (off < 0x8000) {
        reg_write(off, v);
        return;
    }
    switch (off >> 12) {
    case 0x9:
        if (!(v & 0x80)) {
            g.latched = false;
        }
        break;
    case 0xA:
        g.int_enable = v;
        if (gayle_irq()) {
            ami_set_intreq(INT_PORTS);
        }
        break;
    case 0xB:
        g.config = v;
        break;
    default:
        break;
    }
}

void gayle_write16(uint32_t a, uint16_t v)
{
    uint32_t off = a & 0xFFFF;
    if ((a & 0xFF0000) == 0xDA0000 && off < 0x8000 && ((off >> 2) & 7) == 0 && !(off & 0x1000)) {
        data_write(v);
        return;
    }
    gayle_write8(a & ~1u, (uint8_t)(v >> 8));
}
