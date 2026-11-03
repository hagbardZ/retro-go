/*
 * Hard disk images for the drives: an HDF file read and written in place,
 * or an RP9 game's disk joined with the system disk it boots from.
 *
 * Amiga Forever's games on hard disk are data volumes (AF-Application:
 * no C:, no S:Startup-Sequence of their own): the player boots its system
 * disk (the manifest's <boot type="hdf">135</boot>: workbench-135.hdf),
 * whose startup runs AF-Application:S/user-startup. The ROMs' scsi.device
 * mounts one drive, so the drive shows both as one disk: an RDB of our
 * own (block 0, the PART blocks, the filesystems' FSHD and LSEG blocks of
 * both disks renumbered), then the system disk's partitions (bootable),
 * then the game's (not bootable: mounted, its volume name its own), each
 * starting at a cylinder of its own geometry, the rest zeros. The disk's
 * size a multiple of 16 x 63 blocks (the IDE geometry divides it). The
 * game's disk is unpacked beside the package once (".<name>.rp9.hdf"), the
 * system disk is a file in the same folder (from Amiga Forever's
 * Shared/hdf); both are written to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "amiga_internal.h"
#include "rp9.h"

#define SECTOR          512
#define MAX_PARTS       4
#define MAX_FS          2
#define MAX_LSEG        256             /* blocks of a filesystem (128 KB) */
#define NONE            0xFFFFFFFFu
#define DOS0_TYPE       0x444F5300u     /* "DOS\0" */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool block(FILE *f, uint32_t n, uint8_t *dst)
{
    int fd = fileno(f);
    return lseek(fd, (off_t)n * SECTOR, SEEK_SET) >= 0 && read(fd, dst, SECTOR) == SECTOR;
}

/* A block of the RDB's: its checksum made right */
static void checksum(uint8_t *b)
{
    uint32_t n = be32(b + 4), sum = 0;
    put32(b + 8, 0);
    for (uint32_t i = 0; i < n && i < SECTOR / 4; i++) {
        sum += be32(b + 4 * i);
    }
    put32(b + 8, (uint32_t)-sum);
}

/* ---- A disk's RDB ---- */

typedef struct {
    bool made_up;                   /* a bare partition's: Amiga Forever's defaults */
    uint8_t rdsk[SECTOR];
    int parts;
    uint8_t part[MAX_PARTS][SECTOR];
    int fs;
    uint8_t fshd[MAX_FS][SECTOR];
    uint32_t lsegs[MAX_FS];
    uint8_t *lseg[MAX_FS];          /* lsegs[i] blocks */
} rdb_t;

static void rdb_free(rdb_t *r)
{
    for (int i = 0; i < MAX_FS; i++) {
        free(r->lseg[i]);
        r->lseg[i] = NULL;
    }
}

/* A disk that's one bare partition (a filesystem from block 0): the RDB it
 * would have with Amiga Forever's defaults (2 reserved blocks, 32 a track,
 * one surface), bootable
 */
static bool made_up(FILE *f, rdb_t *r, uint32_t blocks)
{
    uint8_t b0[SECTOR];
    if (!block(f, 0, b0) || memcmp(b0, "DOS", 3) || blocks < 64) {
        return false;
    }
    r->made_up = true;
    memcpy(r->rdsk, "RDSK", 4);
    put32(r->rdsk + 4, 64);
    put32(r->rdsk + 12, 7);
    put32(r->rdsk + 16, SECTOR);
    uint8_t *p = r->part[0];
    memcpy(p, "PART", 4);
    put32(p + 4, 64);
    put32(p + 12, 7);
    put32(p + 16, NONE);
    put32(p + 20, 1);
    static const uint32_t env[17] = {
        16, 128, 0, 1, 1, 32, 2, 0, 0, 0, 0, 30, 0, 0x1FE00, 0x7FFFFFFE, 0, 0,
    };
    for (int i = 0; i < 17; i++) {
        put32(p + 128 + 4 * i, env[i]);
    }
    put32(p + 128 + 40, blocks / 32 - 1);                   /* HighCyl */
    put32(p + 128 + 64, be32(b0));                          /* DosType */
    r->parts = 1;
    return true;
}

static bool rdb_read(FILE *f, rdb_t *r, uint32_t blocks)
{
    memset(r, 0, sizeof(*r));
    bool found = false;
    for (uint32_t b = 0; b < 16 && !found; b++) {
        found = block(f, b, r->rdsk) && !memcmp(r->rdsk, "RDSK", 4);
    }
    if (!found) {
        return made_up(f, r, blocks);
    }
    for (uint32_t b = be32(r->rdsk + 28); b != NONE && r->parts < MAX_PARTS; b = be32(r->part[r->parts - 1] + 16)) {
        if (!block(f, b, r->part[r->parts]) || memcmp(r->part[r->parts], "PART", 4)) {
            break;
        }
        r->parts++;
    }
    for (uint32_t b = be32(r->rdsk + 32); b != NONE && r->fs < MAX_FS; b = be32(r->fshd[r->fs - 1] + 16)) {
        uint8_t *h = r->fshd[r->fs];
        if (!block(f, b, h) || memcmp(h, "FSHD", 4)) {
            break;
        }
        uint8_t *seg = malloc(MAX_LSEG * SECTOR);
        uint32_t n = 0;
        for (uint32_t s = be32(h + 72); seg && s != NONE && n < MAX_LSEG; s = be32(seg + (n - 1) * SECTOR + 16)) {
            if (!block(f, s, seg + n * SECTOR) || memcmp(seg + n * SECTOR, "LSEG", 4)) {
                break;
            }
            n++;
        }
        r->lseg[r->fs] = seg;
        r->lsegs[r->fs] = n;
        r->fs++;
    }
    return r->parts > 0;
}

/* ---- The drive's blocks ---- */

static void add_piece(hdf_t *h, uint32_t first, uint32_t count, int file, uint32_t at)
{
    if (h->pieces < HDF_PIECES) {
        h->piece[h->pieces++] = (hdf_piece_t){ first, count, file, at };
    }
}

static const hdf_piece_t *piece_at(const hdf_t *h, uint32_t lba)
{
    for (int i = 0; i < h->pieces; i++) {
        if (lba - h->piece[i].first < h->piece[i].count) {
            return &h->piece[i];
        }
    }
    return NULL;
}

/* The first block of a piece after lba (or the end) */
static uint32_t next_piece(const hdf_t *h, uint32_t lba)
{
    uint32_t next = h->blocks;
    for (int i = 0; i < h->pieces; i++) {
        if (h->piece[i].first > lba && h->piece[i].first < next) {
            next = h->piece[i].first;
        }
    }
    return next;
}

bool hdf_io(hdf_t *h, uint32_t lba, int count, uint8_t *buf, bool write_it)
{
    if (lba + (uint32_t)count > h->blocks) {
        return false;
    }
    while (count > 0) {
        const hdf_piece_t *p = piece_at(h, lba);
        uint32_t n;
        if (p == NULL) {
            n = next_piece(h, lba) - lba;
            n = n < (uint32_t)count ? n : (uint32_t)count;
            if (!write_it) {
                memset(buf, 0, n * SECTOR);
            }
        } else {
            n = p->first + p->count - lba;
            n = n < (uint32_t)count ? n : (uint32_t)count;
            uint32_t at = p->at + (lba - p->first);
            if (p->file < 0) {
                if (write_it) {
                    memcpy(h->head + at * SECTOR, buf, n * SECTOR);
                } else {
                    memcpy(buf, h->head + at * SECTOR, n * SECTOR);
                }
            } else {
                int fd = fileno(h->f[p->file]);
                ssize_t len = (ssize_t)n * SECTOR;
                if (lseek(fd, (off_t)at * SECTOR, SEEK_SET) < 0 ||
                    (write_it ? write(fd, buf, (size_t)len) : read(fd, buf, (size_t)len)) != len) {
                    return false;
                }
            }
        }
        lba += n;
        count -= (int)n;
        buf += n * SECTOR;
    }
    return true;
}

static FILE *open_rw(const char *path, bool *read_only)
{
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        f = fopen(path, "rb");
        *read_only = true;
    }
    return f;
}

static uint32_t blocks_of(FILE *f)
{
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    return size > 0 ? (uint32_t)(size / SECTOR) : 0;
}

/* The disks' partitions behind an RDB of ours: a game's system disk's
 * (f[0], bootable), then the game's (f[1], not); or one bare partition's
 * (f[0], bootable)
 */
static bool join(hdf_t *h, const rdb_t *sys, const rdb_t *app)
{
    const rdb_t *disk[2] = { sys ? sys : app, sys ? app : NULL };
    int disks = sys ? 2 : 1;
    int parts = disk[0]->parts + (sys ? app->parts : 0);
    uint32_t head = 1 + (uint32_t)parts;
    for (int d = 0; d < disks; d++) {
        for (int i = 0; i < disk[d]->fs; i++) {
            head += 1 + disk[d]->lsegs[i];
        }
    }
    h->head = calloc(head, SECTOR);
    if (h->head == NULL) {
        return false;
    }
    h->head_blocks = head;
    add_piece(h, 0, head, -1, 0);

    /* the partitions, each at a cylinder of its own */
    uint32_t at = head;
    int p = 0;
    for (int d = 0; d < disks; d++) {
        for (int i = 0; i < disk[d]->parts; i++, p++) {
            uint8_t *b = h->head + (1 + p) * SECTOR;
            memcpy(b, disk[d]->part[i], SECTOR);
            uint8_t *env = b + 128;
            uint32_t cyl = be32(env + 12) * be32(env + 20), low = be32(env + 36), high = be32(env + 40);
            if (cyl == 0 || high < low) {
                return false;
            }
            uint32_t start = (at + cyl - 1) / cyl * cyl, count = (high - low + 1) * cyl;
            put32(env + 36, start / cyl);
            put32(env + 40, start / cyl + (high - low));
            add_piece(h, start, count, d, low * cyl);
            at = start + count;
            put32(b + 16, p + 1 < parts ? (uint32_t)(2 + p) : NONE);
            put32(b + 20, d == 0 ? be32(b + 20) | 1 : be32(b + 20) & ~1u);      /* bootable: the system's */
            char name[16];
            snprintf(name, sizeof(name), "DH%d", p);
            b[36] = (uint8_t)strlen(name);
            memset(b + 37, 0, 31);
            memcpy(b + 37, name, strlen(name));
            checksum(b);
        }
    }
    h->blocks = (at + 1007) / 1008 * 1008;

    /* the filesystems: FSHD, then its LSEG blocks */
    uint32_t b = 1 + (uint32_t)parts, first_fs = NONE;
    uint8_t *prev = NULL;
    for (int d = 0; d < disks; d++) {
        for (int i = 0; i < disk[d]->fs; i++) {
            uint8_t *fh = h->head + b * SECTOR;
            memcpy(fh, disk[d]->fshd[i], SECTOR);
            uint32_t n = disk[d]->lsegs[i];
            put32(fh + 72, n ? b + 1 : NONE);
            put32(fh + 16, NONE);
            if (prev == NULL) {
                first_fs = b;
            } else {
                put32(prev + 16, b);
                checksum(prev);
            }
            prev = fh;
            checksum(fh);
            for (uint32_t k = 0; k < n; k++) {
                uint8_t *s = h->head + (b + 1 + k) * SECTOR;
                memcpy(s, disk[d]->lseg[i] + k * SECTOR, SECTOR);
                put32(s + 16, k + 1 < n ? b + 2 + k : NONE);
                checksum(s);
            }
            b += 1 + n;
        }
    }

    /* the RDB itself: one cylinder a block */
    uint8_t *r = h->head;
    memcpy(r, disk[0]->rdsk, SECTOR);
    put32(r + 24, NONE);                    /* no bad blocks */
    put32(r + 28, 1);                       /* the partitions */
    put32(r + 32, first_fs);
    put32(r + 36, NONE);                    /* no drive init code */
    put32(r + 64, h->blocks);
    put32(r + 68, 1);
    put32(r + 72, 1);
    put32(r + 80, h->blocks);
    put32(r + 96, h->blocks);
    put32(r + 100, h->blocks);
    put32(r + 128, 0);
    put32(r + 132, head - 1);
    put32(r + 136, head);
    put32(r + 140, h->blocks - 1);
    put32(r + 144, 1);
    put32(r + 152, head - 1);
    checksum(r);
    return true;
}

/* An RP9's own disk, unpacked the first time */
static bool rp9_disk(const char *path, const rp9_t *r, char *image, size_t len)
{
    const char *slash = strrchr(path, '/');
    int dir = slash ? (int)(slash - path + 1) : 0;
    snprintf(image, len, "%.*s.%s.hdf", dir, path, slash ? slash + 1 : path);
    struct stat st;
    if (stat(image, &st) == 0 && st.st_size > 0) {
        return true;
    }
    AMI_LOGI("%s: unpacking %s", path, r->hd);
    return rp9_extract(path, r->hd, image);
}

bool hdf_open(hdf_t *h, const char *path)
{
    memset(h, 0, sizeof(*h));
    char image[256];
    size_t plen = strlen(path);
    rp9_t r;
    bool rp9 = plen > 4 && strcasecmp(path + plen - 4, ".rp9") == 0;
    if (rp9) {
        if (!rp9_read(path, &r) || !r.harddrive) {
            AMI_LOGW("%s: no hard disk in it", path);
            return false;
        }
        if (!rp9_disk(path, &r, image, sizeof(image))) {
            return false;
        }
    } else {
        snprintf(image, sizeof(image), "%s", path);
    }
    h->f[0] = open_rw(image, &h->read_only);
    if (h->f[0] == NULL || blocks_of(h->f[0]) < 16) {
        hdf_close(h);
        return false;
    }
    uint32_t blocks = blocks_of(h->f[0]);
    h->blocks = blocks;
    rdb_t *ra = malloc(sizeof(rdb_t)), *rs = NULL;
    bool has_rdb = ra && rdb_read(h->f[0], ra, blocks);

    /* an RP9 game's system disk, beside it */
    FILE *sf = NULL;
    bool sys_ro = false;
    if (rp9 && r.boot_hdf) {
        char sys[256];
        const char *slash = strrchr(path, '/');
        int dir = slash ? (int)(slash - path + 1) : 0;
        snprintf(sys, sizeof(sys), "%.*sworkbench-%d.hdf", dir, path, r.boot_hdf);
        sf = open_rw(sys, &sys_ro);
        if (sf == NULL) {
            snprintf(h->detail, sizeof(h->detail), "no workbench-%d.hdf (Amiga Forever's system disk) beside it: "
                     "the game's disk alone", r.boot_hdf);
        }
    }
    bool joined = false;
    if (sf && has_rdb && (rs = malloc(sizeof(rdb_t))) != NULL && rdb_read(sf, rs, blocks_of(sf))) {
        h->f[1] = h->f[0];
        h->f[0] = sf;
        joined = join(h, rs, ra);
        if (joined) {
            h->read_only = h->read_only || sys_ro;
            snprintf(h->detail, sizeof(h->detail), "with its system disk workbench-%d.hdf", r.boot_hdf);
        } else {
            h->f[0] = h->f[1];
            h->f[1] = NULL;
        }
    } else if (has_rdb && ra->made_up) {
        joined = join(h, NULL, ra);
        if (joined) {
            snprintf(h->detail, sizeof(h->detail), "a partition without an RDB: one of ours in front");
        }
    }
    if (!joined) {
        if (sf) {
            snprintf(h->detail, sizeof(h->detail), "workbench-%d.hdf or the game's disk unusable: the game's disk "
                     "alone", r.boot_hdf);
        }
        if (h->f[1] == NULL && sf && h->f[0] != sf) {
            fclose(sf);
        }
        free(h->head);
        h->head = NULL;
        h->head_blocks = 0;
        h->pieces = 0;
        h->blocks = blocks;
        add_piece(h, 0, blocks, 0, 0);
    }
    if (h->detail[0]) {
        AMI_LOGI("%s: %s (%lu blocks)", path, h->detail, (unsigned long)h->blocks);
    }
    if (rs) {
        rdb_free(rs);
    }
    if (ra) {
        rdb_free(ra);
    }
    free(rs);
    free(ra);
    return true;
}

/* ---- Blank disks ---- */

bool amiga_hd_create(const char *path, uint32_t bytes)
{
    uint32_t blocks = bytes / SECTOR, cyls = blocks / 1008;
    if (cyls < 4) {
        return false;
    }
    uint8_t b[2][SECTOR];
    memset(b, 0, sizeof(b));
    uint8_t *r = b[0], *p = b[1];
    memcpy(r, "RDSK", 4);
    put32(r + 4, 64);
    put32(r + 12, 7);                       /* host ID */
    put32(r + 16, SECTOR);
    put32(r + 24, NONE);                    /* bad blocks */
    put32(r + 28, 1);                       /* the partition */
    put32(r + 32, NONE);                    /* no filesystems */
    put32(r + 36, NONE);                    /* no drive init code */
    for (int i = 40; i < 64; i += 4) {
        put32(r + i, NONE);
    }
    put32(r + 64, cyls);
    put32(r + 68, 63);
    put32(r + 72, 16);
    put32(r + 76, 1);
    put32(r + 80, cyls);
    put32(r + 96, cyls);
    put32(r + 100, cyls);
    put32(r + 104, 3);
    put32(r + 128, 0);
    put32(r + 132, 1007);                   /* the first cylinder: the RDB's */
    put32(r + 136, 1);
    put32(r + 140, cyls - 1);
    put32(r + 144, 1008);
    put32(r + 152, 1);
    memcpy(r + 160, "ESP32AMI", 8);
    memcpy(r + 168, "HARD DISK IMAGE ", 16);
    memcpy(r + 184, "1.0 ", 4);
    checksum(r);
    memcpy(p, "PART", 4);
    put32(p + 4, 64);
    put32(p + 12, 7);
    put32(p + 16, NONE);
    put32(p + 20, 1);                       /* bootable */
    p[36] = 3;
    memcpy(p + 37, "DH0", 3);
    static const uint32_t env[17] = {
        16, 128, 0, 16, 1, 63, 2, 0, 0, 1, 0, 30, 0, 0x1FE00, 0x7FFFFFFE, 0, DOS0_TYPE,
    };
    for (int i = 0; i < 17; i++) {
        put32(p + 128 + 4 * i, env[i]);
    }
    put32(p + 128 + 40, cyls - 1);          /* HighCyl */
    checksum(p);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    /* the rest unwritten (the file system extends the file: its contents
     * don't matter until formatted) */
    bool ok = fwrite(b, SECTOR, 2, f) == 2 && fseek(f, (long)cyls * 1008 * SECTOR - 1, SEEK_SET) == 0 &&
              fputc(0, f) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(path);
    }
    return ok;
}

bool amiga_rp9_hard_disk(const char *path)
{
    rp9_t r;
    return rp9_read(path, &r) && r.harddrive;
}

void hdf_close(hdf_t *h)
{
    for (int i = 0; i < 2; i++) {
        if (h->f[i]) {
            fclose(h->f[i]);
        }
    }
    free(h->head);
    memset(h, 0, sizeof(*h));
}
