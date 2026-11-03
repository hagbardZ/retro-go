/*
 * Our own hard disk board, for the Kickstarts without an IDE driver (1.3,
 * 2.04; not 1.2: see hdboard_wanted()): an autoconfig board (Zorro II, 256 KB, in the I/O space)
 * with a boot ROM whose device driver is native code here. On the other
 * Kickstarts Gayle's IDE (gayle.c) serves the same drives instead: a disk
 * is never mounted by both.
 *
 * The ROM (hdrom.h, assembled by tools/hdrom.py) is a little 68000 code:
 * ILLEGAL ($4AFC) and a function word call the routines below, honoured
 * only while the PC is in the ROM. The rest of the board is RAM for what
 * they build: the device (its jump table to the ROM's stubs), the
 * partitions' parameter packets, names and BootNodes, and the filesystems
 * loaded from the disks' RDBs.
 *
 *   DiagArea   copied to RAM by expansion.library; its DiagPoint jumps to
 *              FN_DIAG, which fills in the Resident tag in the copy
 *   init       the Resident's (RTF_COLDSTART, after expansion): FN_INIT
 *              builds the device, AddDevice; then for each partition FN_NEXT
 *              gives its parameter packet, MakeDosNode, FN_BOOTNODE patches
 *              the DeviceNode (a filesystem from the RDB where the Kickstart
 *              has none for it: 1.x has only the old one) and fills in its
 *              BootNode, Enqueue into expansion's MountList (bootable
 *              partitions at their priority, the others at -128)
 *   BootPoint  strap's, booting from one of ours: dos.library's init
 *   device     Open, Close, BeginIO (each command done at once, a ReplyMsg
 *              unless IOF_QUICK), Expunge, AbortIO (nothing pending)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "m68k.h"
#include "amiga_internal.h"
#include "hdrom.h"

#define BOARD_SIZE      0x40000
#define ROM_END         0x1000          /* native calls honoured below this */
#define SECTOR          512

/* The RAM (offsets in the board) */
#define VEC             0x4000          /* the device's jump table (6 x 6 bytes, below DEV) */
#define DEV             0x4040          /* the device: struct Library */
#define UNITS           0x4080          /* a longword a unit: io_Unit points at it */
#define PACKETS         0x4100          /* MakeDosNode's parameter packets */
#define PACKET_SIZE     0x60
#define MAX_PARTS       8
#define BOOTNODES       0x4400
#define BOOTNODE_SIZE   0x20
#define NAMES           0x4600          /* the partitions' names, 16 bytes each */
#define FS_START        0x5000          /* loaded filesystems, upwards */
#define MAX_FS          4

enum { FN_DIAG = 1, FN_INIT, FN_NEXT, FN_BOOTNODE, FN_OPEN, FN_CLOSE, FN_BEGINIO };

/* exec and trackdisk */
#define NT_DEVICE       3
#define NT_BOOTNODE     16
#define IOF_QUICK       1
#define IOERR_OPENFAIL  (-1)
#define IOERR_NOCMD     (-3)
#define IOERR_BADLENGTH (-4)
#define IOERR_BADADDRESS (-5)
#define TDERR_WRITEPROT 28
#define DOS0            0x444F5300u

static uint8_t *s_board;            /* the board's memory, words in host order */
static uint32_t s_base;             /* where autoconfig put it (0: not yet) */
static uint32_t s_configdev;
static uint32_t s_top;              /* the next free byte for filesystems */
static int s_parts, s_next;

typedef struct {
    int drive;
    uint8_t part[SECTOR];           /* its PART block */
} part_t;

static part_t s_part[MAX_PARTS];
static char s_taken[MAX_PARTS][16];  /* names scsi.device gives DH0:'s partitions */
static int s_taken_count;

typedef struct {
    int drive;
    uint32_t dostype, seglist;      /* (a BPTR) */
    uint8_t fshd[SECTOR];
} fs_t;

static fs_t s_fs[MAX_FS];
static int s_fs_count;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- The board's memory and the 68000's ---- */

static void bw16(uint32_t off, uint16_t v)
{
    *(uint16_t *)(s_board + off) = v;
}

static void bw32(uint32_t off, uint32_t v)
{
    bw16(off, (uint16_t)(v >> 16));
    bw16(off + 2, (uint16_t)v);
}

static void bw8(uint32_t off, uint8_t v)
{
    s_board[off ^ 1] = v;
}

static uint32_t rd32(uint32_t a)
{
    return m68k_read_memory_32(a & 0xFFFFFF);
}

static void wr32(uint32_t a, uint32_t v)
{
    m68k_write_memory_32(a & 0xFFFFFF, v);
}

static uint32_t reg(m68k_register_t r)
{
    return m68k_get_reg(NULL, r);
}

/* ---- Presence ---- */

/* A Kickstart whose scsi.device drives Gayle (2.05 and 3.1) */
bool ami_kick_has_ide(void)
{
    return ami.kick_major > 37 || (ami.kick_major == 37 && ami.kick_minor >= 300);
}

/* The drives the board serves: both, or with Gayle's driver in the
 * Kickstart DH1: alone (its scsi.device only ever uses the master)
 */
static bool serves(int d)
{
    return hd_image(d) && (d > 0 || !ami_kick_has_ide());
}

/* (not with Kickstart 1.2: its expansion.library copies a word-wide
 * DiagArea with exec's CopyMem and then takes AllocMem from exec's base as
 * if it were its own, at $24: a jump into chip RAM, an address error, the
 * alert, a reset, again. 1.2 can't boot from a board anyway.)
 */
bool hdboard_wanted(void)
{
    return ami.kick_major >= 34 && (serves(0) || serves(1));
}

uint8_t *hdboard_memory(uint32_t *base, uint32_t *size)
{
    *base = s_base;
    *size = BOARD_SIZE;
    return s_board;
}

void hdboard_reset(void)
{
    s_base = 0;
    if (s_board == NULL) {
        s_board = ami_malloc(BOARD_SIZE);
    }
    if (s_board) {
        memset(s_board, 0, BOARD_SIZE);
        for (uint32_t i = 0; i < sizeof(hdrom); i += 2) {
            bw16(i, (uint16_t)(hdrom[i] << 8 | hdrom[i + 1]));
        }
    }
}

bool hdboard_configured(uint32_t base)
{
    if (s_board == NULL) {
        return false;
    }
    s_base = base;
    mem_update_map();
    return true;
}

/* ---- The disks' RDBs ---- */

static bool block(int drive, uint32_t n, uint8_t *dst)
{
    hdf_t *h = hd_image(drive);
    return h && hdf_io(h, n, 1, dst, false);
}

static bool find_rdb(int drive, uint8_t *rdsk)
{
    for (uint32_t b = 0; b < 16; b++) {
        if (block(drive, b, rdsk) && !memcmp(rdsk, "RDSK", 4)) {
            return true;
        }
    }
    return false;
}

static void read_partitions(int drive)
{
    uint8_t rdsk[SECTOR];
    if (!find_rdb(drive, rdsk)) {
        return;
    }
    for (uint32_t b = be32(rdsk + 28); b != 0xFFFFFFFFu && s_parts < MAX_PARTS; b = be32(s_part[s_parts - 1].part + 16)) {
        part_t *p = &s_part[s_parts];
        if (!block(drive, b, p->part) || memcmp(p->part, "PART", 4)) {
            break;
        }
        p->drive = drive;
        s_parts++;
    }
    for (uint32_t b = be32(rdsk + 32); b != 0xFFFFFFFFu && s_fs_count < MAX_FS; b = be32(s_fs[s_fs_count - 1].fshd + 16)) {
        fs_t *f = &s_fs[s_fs_count];
        if (!block(drive, b, f->fshd) || memcmp(f->fshd, "FSHD", 4)) {
            break;
        }
        f->drive = drive;
        f->dostype = be32(f->fshd + 32);
        f->seglist = 0;
        s_fs_count++;
    }
}

/* ---- Loading a filesystem: the hunks of its LSEG blocks into the board ---- */

static uint32_t alloc(uint32_t bytes)
{
    uint32_t at = (s_top + 7) & ~7u;
    if (at + bytes > BOARD_SIZE) {
        return 0;
    }
    s_top = at + bytes;
    return at;
}

static uint32_t load_hunks(const uint8_t *file, uint32_t len)
{
    uint32_t n = len / 4, i = 0;
    #define NEXT() (i < n ? be32(file + 4 * i++) : 0)
    if (NEXT() != 0x3F3) {
        return 0;
    }
    while (i < n) {                             /* resident library names */
        uint32_t names = NEXT();
        if (names == 0) {
            break;
        }
        i += names;
    }
    NEXT();                                     /* table size */
    uint32_t first = NEXT(), last = NEXT();
    if (last < first || last - first >= 16) {
        return 0;
    }
    uint32_t count = last - first + 1, data[16], seg[16];
    for (uint32_t h = 0; h < count; h++) {
        uint32_t bytes = (NEXT() & 0x3FFFFFFF) * 4;
        uint32_t at = alloc(bytes + 8);
        if (at == 0) {
            return 0;
        }
        bw32(at, bytes + 8);
        seg[h] = at + 4;
        data[h] = at + 8;
    }
    for (uint32_t h = 0; h < count; h++) {
        bw32(seg[h], h + 1 < count ? (s_base + seg[h + 1]) >> 2 : 0);
    }
    uint32_t h = 0;
    while (i < n && h < count) {
        uint32_t type = NEXT() & 0x3FFFFFFF;
        switch (type) {
        case 0x3E9: case 0x3EA: {               /* HUNK_CODE, HUNK_DATA */
            uint32_t longs = NEXT();
            for (uint32_t k = 0; k < longs; k++) {
                bw32(data[h] + 4 * k, NEXT());
            }
            break;
        }
        case 0x3EB:                             /* HUNK_BSS (zeroed already) */
            NEXT();
            break;
        case 0x3EC:                             /* HUNK_RELOC32 */
            for (;;) {
                uint32_t offsets = NEXT();
                if (offsets == 0) {
                    break;
                }
                uint32_t target = NEXT();
                for (uint32_t k = 0; k < offsets; k++) {
                    uint32_t off = data[h] + NEXT();
                    uint32_t v = (uint32_t)*(uint16_t *)(s_board + off) << 16 | *(uint16_t *)(s_board + off + 2);
                    bw32(off, v + s_base + data[target < count ? target : 0]);
                }
            }
            break;
        case 0x3FC: {                           /* HUNK_RELOC32SHORT: 16-bit numbers */
            uint32_t pos = i * 4;
            for (;;) {
                uint32_t offsets = (uint32_t)file[pos] << 8 | file[pos + 1];
                pos += 2;
                if (offsets == 0) {
                    break;
                }
                uint32_t target = (uint32_t)file[pos] << 8 | file[pos + 1];
                pos += 2;
                for (uint32_t k = 0; k < offsets; k++) {
                    uint32_t off = data[h] + ((uint32_t)file[pos] << 8 | file[pos + 1]);
                    pos += 2;
                    uint32_t v = (uint32_t)*(uint16_t *)(s_board + off) << 16 | *(uint16_t *)(s_board + off + 2);
                    bw32(off, v + s_base + data[target < count ? target : 0]);
                }
            }
            i = (pos + 3) / 4;
            break;
        }
        case 0x3F0:                             /* HUNK_SYMBOL */
            for (;;) {
                uint32_t longs = NEXT();
                if (longs == 0) {
                    break;
                }
                i += longs + 1;
            }
            break;
        case 0x3F1: {                           /* HUNK_DEBUG */
            uint32_t longs = NEXT();
            i += longs;
            break;
        }
        case 0x3F2:                             /* HUNK_END */
            h++;
            break;
        default:
            AMI_LOGW("hard disk board: hunk type $%lx in a filesystem", (unsigned long)type);
            return 0;
        }
    }
    #undef NEXT
    return (s_base + seg[0]) >> 2;
}

/* The filesystem for a partition: loaded the first time (BPTR; 0: none) */
static fs_t *filesystem(int drive, uint32_t dostype)
{
    fs_t *best = NULL;
    for (int k = 0; k < s_fs_count; k++) {
        fs_t *f = &s_fs[k];
        if (f->dostype == dostype && (!best || f->drive == drive)) {
            best = f;
        }
    }
    if (best == NULL || best->seglist) {
        return best;
    }
    uint8_t *file = malloc(256 * 492);
    uint8_t blk[SECTOR];
    uint32_t len = 0;
    for (uint32_t b = be32(best->fshd + 72); file && b != 0xFFFFFFFFu && len < 256 * 492; b = be32(blk + 16)) {
        if (!block(best->drive, b, blk) || memcmp(blk, "LSEG", 4)) {
            break;
        }
        memcpy(file + len, blk + 20, 492);
        len += 492;
    }
    best->seglist = file ? load_hunks(file, len) : 0;
    free(file);
    AMI_LOGI("hard disk board: filesystem %08lx %u.%u %s", (unsigned long)dostype, (unsigned)(be32(best->fshd + 36) >> 16),
             (unsigned)(be32(best->fshd + 36) & 0xFFFF), best->seglist ? "loaded" : "unusable");
    return best->seglist ? best : NULL;
}

/* Does this Kickstart need the RDB's filesystem for a DosType? */
static bool needs_filesystem(uint32_t dostype)
{
    if (ami.kick_major < 36) {
        return dostype != DOS0;             /* 1.x: only the old one in ROM */
    }
    return dostype > DOS0 + 3;              /* 2.04: FFS too, not the newer modes */
}

/* ---- The native routines ---- */

static void diag(void)
{
    uint32_t copy = reg(M68K_REG_A2), rt = copy + HDROM_ROMTAG;
    s_configdev = reg(M68K_REG_A3);
    wr32(rt + 2, rt);
    wr32(rt + 6, copy + HDROM_DIAG_SIZE);
    wr32(rt + 14, s_base + HDROM_DEVNAME);
    wr32(rt + 18, s_base + HDROM_IDSTRING);
    wr32(rt + 22, s_base + HDROM_INIT);
}

/* The names of DH0:'s partitions when Gayle's driver mounts them */
static void taken_names(void)
{
    s_taken_count = 0;
    uint8_t rdsk[SECTOR], part[SECTOR];
    if (serves(0) || !hd_image(0) || !find_rdb(0, rdsk)) {
        return;
    }
    for (uint32_t b = be32(rdsk + 28); b != 0xFFFFFFFFu && s_taken_count < MAX_PARTS; b = be32(part + 16)) {
        if (!block(0, b, part) || memcmp(part, "PART", 4)) {
            break;
        }
        int len = part[36] < 15 ? part[36] : 15;
        memcpy(s_taken[s_taken_count], part + 37, (size_t)len);
        s_taken[s_taken_count++][len] = 0;
    }
}

static uint32_t init(void)
{
    s_parts = s_next = s_fs_count = 0;
    s_top = FS_START;
    taken_names();
    for (int d = 0; d < AMIGA_HARD_DISKS; d++) {
        if (serves(d)) {
            read_partitions(d);
        }
    }
    if (s_parts == 0) {
        AMI_LOGW("hard disk board: no partitions (no RDB?)");
        return 0;
    }
    static const uint32_t fn[6] = {
        HDROM_OPEN, HDROM_CLOSE, HDROM_EXPUNGE, HDROM_EXPUNGE, HDROM_BEGINIO, HDROM_ABORTIO,
    };
    for (int k = 0; k < 6; k++) {
        uint32_t at = DEV - 6 * (k + 1);
        bw16(at, 0x4EF9);                       /* jmp abs.l */
        bw32(at + 2, s_base + fn[k]);
    }
    for (uint32_t o = DEV; o < DEV + 0x40; o += 2) {
        bw16(o, 0);
    }
    bw8(DEV + 8, NT_DEVICE);
    bw32(DEV + 10, s_base + HDROM_DEVNAME);     /* ln_Name */
    bw16(DEV + 16, 36);                         /* lib_NegSize */
    bw16(DEV + 18, 0x40);                       /* lib_PosSize */
    bw16(DEV + 20, 1);                          /* lib_Version */
    bw32(DEV + 24, s_base + HDROM_IDSTRING);
    m68k_set_reg(M68K_REG_A1, s_base + DEV);
    AMI_LOGI("hard disk board: %d partition(s)", s_parts);
    return 1;
}

/* A name a partition before `before` (or DH0:'s, through Gayle) has */
static bool name_used(const char *name, int before)
{
    for (int k = 0; k < s_taken_count; k++) {
        if (!strcasecmp(s_taken[k], name)) {
            return true;
        }
    }
    for (int k = 0; k < before; k++) {
        char prev[16];
        uint32_t nm = NAMES + (uint32_t)k * 16;
        for (int i = 0; i < 16; i++) {
            prev[i] = (char)s_board[(nm + (uint32_t)i) ^ 1];
        }
        prev[15] = 0;
        if (!strcasecmp(prev, name)) {
            return true;
        }
    }
    return false;
}

static uint32_t next(void)
{
    if (s_next >= s_parts) {
        return 0;
    }
    const part_t *p = &s_part[s_next];
    uint32_t pk = PACKETS + (uint32_t)s_next * PACKET_SIZE, nm = NAMES + (uint32_t)s_next * 16;
    /* its name: the RDB's, else the first DHn nobody has */
    char name[16];
    int len = p->part[36] < 15 ? p->part[36] : 15;
    memcpy(name, p->part + 37, (size_t)len);
    name[len] = 0;
    for (int n = 0; n < 100 && name_used(name, s_next); n++) {
        snprintf(name, sizeof(name), "DH%d", n);
    }
    for (int k = 0; k < 16; k++) {
        bw8(nm + (uint32_t)k, (uint8_t)name[k]);
    }
    bw32(pk, s_base + nm);                      /* dos name */
    bw32(pk + 4, s_base + HDROM_DEVNAME);       /* exec device */
    bw32(pk + 8, (uint32_t)p->drive);           /* unit */
    bw32(pk + 12, be32(p->part + 32));          /* flags */
    uint32_t longs = be32(p->part + 128) + 1;
    longs = longs > 20 ? 20 : longs;
    for (uint32_t k = 0; k < longs; k++) {
        bw32(pk + 16 + 4 * k, be32(p->part + 128 + 4 * k));
    }
    m68k_set_reg(M68K_REG_A0, s_base + pk);
    return 1;
}

static uint32_t bootnode(void)
{
    uint32_t dn = reg(M68K_REG_D0);
    const part_t *p = &s_part[s_next];
    uint32_t bn = BOOTNODES + (uint32_t)s_next * BOOTNODE_SIZE;
    s_next++;
    if (dn == 0 || (be32(p->part + 20) & 2)) {  /* MakeDosNode failed, or "don't mount" */
        return 0;
    }
    uint32_t dostype = be32(p->part + 128 + 64);
    if (needs_filesystem(dostype)) {
        fs_t *f = filesystem(p->drive, dostype);
        if (f) {
            uint32_t patch = be32(f->fshd + 40);
            if (patch & 0x10) {
                wr32(dn + 20, be32(f->fshd + 60));      /* stack */
            }
            if (patch & 0x20) {
                wr32(dn + 24, be32(f->fshd + 64));      /* priority */
            }
            wr32(dn + 32, f->seglist);
            wr32(dn + 36, (patch & 0x100) ? be32(f->fshd + 76) : 0xFFFFFFFFu);
            if (rd32(dn + 20) < 4096) {
                wr32(dn + 20, 4096);
            }
        } else {
            AMI_LOGW("hard disk board: no filesystem for %08lx", (unsigned long)dostype);
        }
    }
    bool bootable = be32(p->part + 20) & 1;
    int pri = (int32_t)be32(p->part + 128 + 60);
    if (p->drive > 0 && hd_image(0)) {
        pri--;                                  /* DH0: boots first */
    }
    pri = pri > 127 ? 127 : pri < -127 ? -127 : pri;
    for (uint32_t o = bn; o < bn + BOOTNODE_SIZE; o += 2) {
        bw16(o, 0);
    }
    bw8(bn + 8, NT_BOOTNODE);
    bw8(bn + 9, (uint8_t)(bootable ? pri : -128));
    bw32(bn + 10, s_configdev);                 /* ln_Name: our ConfigDev */
    bw32(bn + 16, dn);                          /* bn_DeviceNode */
    m68k_set_reg(M68K_REG_A1, s_base + bn);
    return 1;
}

static void open_unit(void)
{
    uint32_t io = reg(M68K_REG_A1), unit = reg(M68K_REG_D0);
    if (unit >= AMIGA_HARD_DISKS || !hd_image((int)unit)) {
        m68k_write_memory_8(io + 31, (uint8_t)IOERR_OPENFAIL);
        return;
    }
    wr32(io + 24, s_base + UNITS + 4 * unit);   /* io_Unit */
    m68k_write_memory_8(io + 31, 0);
    uint16_t *cnt = (uint16_t *)(s_board + DEV + 32);
    (*cnt)++;
}

static void close_unit(void)
{
    uint32_t io = reg(M68K_REG_A1);
    wr32(io + 24, 0xFFFFFFFFu);
    wr32(io + 20, 0xFFFFFFFFu);
    uint16_t *cnt = (uint16_t *)(s_board + DEV + 32);
    if (*cnt) {
        (*cnt)--;
    }
}

/* Reads and writes between the image and the 68000's memory */
static int transfer(hdf_t *h, uint64_t offset, uint32_t len, uint32_t data, bool write_it, uint32_t *done)
{
    static uint8_t buf[SECTOR * 16];
    *done = 0;
    if ((offset | len) & (SECTOR - 1)) {
        return IOERR_BADLENGTH;
    }
    uint64_t lba = offset / SECTOR;
    if (lba + len / SECTOR > h->blocks) {
        return IOERR_BADADDRESS;
    }
    if (write_it && h->read_only) {
        return TDERR_WRITEPROT;
    }
    while (*done < len) {
        uint32_t n = (len - *done) / SECTOR;
        n = n > 16 ? 16 : n;
        if (write_it) {
            mem_copy_from(buf, data + *done, n * SECTOR);
            if (!hdf_io(h, (uint32_t)lba, (int)n, buf, true)) {
                return IOERR_BADADDRESS;
            }
        } else {
            if (!hdf_io(h, (uint32_t)lba, (int)n, buf, false)) {
                return IOERR_BADADDRESS;
            }
            mem_copy_to(data + *done, buf, n * SECTOR);
        }
        lba += n;
        *done += n * SECTOR;
    }
    return 0;
}

static uint32_t begin_io(void)
{
    uint32_t io = reg(M68K_REG_A1);
    uint16_t cmd = (uint16_t)(rd32(io + 28) >> 16);
    uint8_t flags = (uint8_t)m68k_read_memory_8(io + 30);
    uint32_t unit = (rd32(io + 24) - s_base - UNITS) / 4;
    hdf_t *h = unit < AMIGA_HARD_DISKS ? hd_image((int)unit) : NULL;
    uint32_t len = rd32(io + 36), data = rd32(io + 40), offset = rd32(io + 44);
    int error = 0;
    uint32_t actual = 0;
    if (h == NULL) {
        error = IOERR_OPENFAIL;
    } else {
        switch (cmd) {
        case 2:                                 /* CMD_READ */
        case 24:                                /* TD_READ64 */
            error = transfer(h, (cmd == 24 ? (uint64_t)rd32(io + 32) << 32 : 0) | offset, len, data, false, &actual);
            hd_activity((int)unit, actual / SECTOR, false);
            break;
        case 3: case 11:                        /* CMD_WRITE, TD_FORMAT */
        case 25: case 27:                       /* TD_WRITE64, TD_FORMAT64 */
            error = transfer(h, (cmd >= 25 ? (uint64_t)rd32(io + 32) << 32 : 0) | offset, len, data, true, &actual);
            hd_activity((int)unit, actual / SECTOR, true);
            break;
        case 4: case 5: case 9: case 10:        /* CMD_UPDATE, CMD_CLEAR, TD_MOTOR, TD_SEEK */
        case 12: case 13: case 14: case 21:     /* TD_REMOVE, TD_CHANGENUM, TD_CHANGESTATE, TD_REMCHANGEINT */
            break;
        case 20:                                /* TD_ADDCHANGEINT: held (a fixed disk never changes) */
            m68k_write_memory_8(io + 30, flags & ~IOF_QUICK);
            return 0;
        case 15:                                /* TD_PROTSTATUS */
            actual = h->read_only ? 1 : 0;
            break;
        case 18:                                /* TD_GETDRIVETYPE: 3.5" */
            actual = 1;
            break;
        case 19:                                /* TD_GETNUMTRACKS */
            actual = h->blocks / 63;
            break;
        case 22: {                              /* TD_GETGEOMETRY */
            uint32_t g[8] = { SECTOR, h->blocks, h->blocks / 1008, 1008, 16, 63, 1, 0 };
            for (int k = 0; k < 8 && 4u * k + 4 <= len; k++) {
                wr32(data + 4 * k, g[k]);
            }
            actual = len < 32 ? len : 32;
            break;
        }
        default:
            error = IOERR_NOCMD;
            break;
        }
    }
    m68k_write_memory_8(io + 31, (uint8_t)error);
    wr32(io + 32, actual);
    return (flags & IOF_QUICK) ? 0 : 1;
}

bool hdboard_native(void)
{
    uint32_t pc = reg(M68K_REG_PC) & 0xFFFFFF, at = pc - 2;
    if (s_base == 0 || at - s_base >= ROM_END) {
        return false;
    }
    uint16_t fn = (uint16_t)m68k_read_memory_16(pc);
    m68k_set_reg(M68K_REG_PC, pc + 2);
    uint32_t d0 = 0;
    switch (fn) {
    case FN_DIAG: diag(); break;
    case FN_INIT: d0 = init(); break;
    case FN_NEXT: d0 = next(); break;
    case FN_BOOTNODE: d0 = bootnode(); break;
    case FN_OPEN: open_unit(); break;
    case FN_CLOSE: close_unit(); break;
    case FN_BEGINIO: d0 = begin_io(); break;
    default: break;
    }
    if (fn != FN_OPEN && fn != FN_CLOSE && fn != FN_DIAG) {
        m68k_set_reg(M68K_REG_D0, d0);
    }
    return true;
}
