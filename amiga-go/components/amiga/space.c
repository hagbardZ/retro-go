/*
 * Free space on the disks, for the web page.
 *
 * An AmigaDOS volume (OFS, FFS and their international and directory-cache
 * kinds) counts it in its bitmap: the root block, in the middle of the
 * volume, lists the bitmap blocks (25 in it, then extension blocks of 127
 * and a link), each a checksum and 127 longs whose bits, lowest first, are
 * the volume's blocks after the reserved ones: set, free. Block numbers are
 * the volume's own. A hard disk's volumes are its RDB's DOS partitions (our
 * made-up RDBs too), added up.
 *
 * Worked out on the emulation task (space_frame(), from the frame loop,
 * once a second; a hard disk a second after its last write, as its file is
 * read), kept for the web page's task.
 */
#include <stdio.h>
#include <string.h>
#include "amiga_internal.h"

#define BLOCK           512
#define DRIVES          (AMIGA_FLOPPIES + AMIGA_HARD_DISKS)

typedef bool (*read_fn)(void *ctx, uint32_t block, uint8_t *buf);

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
    char path[256];                 /* (hard disks) the image it's for */
    uint32_t writes;                /* its write count then */
    bool waiting;                   /* written since: once the writes stop */
} space_t;

static space_t s_space[DRIVES];
static int s_frames;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* A volume's free blocks; false if it isn't an AmigaDOS volume with a valid
 * bitmap (being validated, or not formatted yet)
 */
static bool volume_free(read_fn rd, void *ctx, uint32_t first, uint32_t blocks, uint32_t reserved, uint32_t *free_blocks)
{
    static uint8_t root[BLOCK], ext[BLOCK], bm[BLOCK];
    if (blocks <= reserved + 2) {
        return false;
    }
    uint32_t r = (blocks - 1 + reserved) / 2;
    if (!rd(ctx, first + r, root) || be32(root) != 2 || be32(root + 508) != 1 || be32(root + 312) != 0xFFFFFFFFu) {
        return false;
    }
    const uint8_t *list = root + 316;   /* bm_pages[25], then bm_ext */
    int left = 25;
    uint32_t next_ext = be32(root + 416);
    uint32_t bits = blocks - reserved, done = 0, n_free = 0;
    for (int guard = 0; done < bits && guard < 65536; guard++) {
        if (left == 0) {
            if (next_ext == 0 || next_ext >= blocks || !rd(ctx, first + next_ext, ext)) {
                return false;
            }
            list = ext;
            left = 127;
            next_ext = be32(ext + 508);
        }
        uint32_t k = be32(list);
        list += 4;
        left--;
        if (k == 0 || k >= blocks || !rd(ctx, first + k, bm)) {
            return false;
        }
        for (int w = 1; w < BLOCK / 4 && done < bits; w++) {
            uint32_t v = be32(bm + 4 * w), n = bits - done < 32 ? bits - done : 32;
            if (n < 32) {
                v &= (1u << n) - 1;
            }
            n_free += (uint32_t)__builtin_popcount(v);
            done += n;
        }
    }
    *free_blocks = n_free;
    return done >= bits;
}

typedef struct {
    const uint8_t *adf;
    uint32_t blocks;                /* 1760, high density 3520 */
} adf_t;

static bool adf_read(void *ctx, uint32_t block, uint8_t *buf)
{
    const adf_t *a = ctx;
    if (block >= a->blocks) {
        return false;
    }
    memcpy(buf, a->adf + (size_t)block * BLOCK, BLOCK);
    return true;
}

static bool hdf_read(void *ctx, uint32_t block, uint8_t *buf)
{
    return hdf_io(ctx, block, 1, buf, false);
}

static bool floppy_space(int d, uint64_t *free_bytes, uint64_t *size)
{
    adf_t a = { 0 };
    uint32_t f;
    a.adf = floppy_image(d, &a.blocks);
    if (a.adf == NULL || memcmp(a.adf, "DOS", 3) != 0 || !volume_free(adf_read, &a, 0, a.blocks, 2, &f)) {
        return false;
    }
    *free_bytes = (uint64_t)f * BLOCK;
    *size = (uint64_t)a.blocks * BLOCK;
    return true;
}

/* The RDB (in the first 16 blocks), its PART blocks: the DOS\x partitions
 * of 512-byte blocks (DosEnvec at 128: SizeBlock, Surfaces, BlocksPerTrack,
 * Reserved, LowCyl, HighCyl, DosType at 4, 12, 20, 24, 36, 40, 64)
 */
static bool hd_space(hdf_t *h, uint64_t *free_bytes, uint64_t *size)
{
    static uint8_t b[BLOCK];
    uint32_t r = 0;
    while (r < 16 && hdf_read(h, r, b) && memcmp(b, "RDSK", 4) != 0) {
        r++;
    }
    if (r == 16 || memcmp(b, "RDSK", 4) != 0) {
        return false;
    }
    bool any = false;
    *free_bytes = *size = 0;
    uint32_t part = be32(b + 28);
    for (int n = 0; part != 0xFFFFFFFFu && n < 16; n++) {
        if (!hdf_read(h, part, b) || memcmp(b, "PART", 4) != 0) {
            break;
        }
        part = be32(b + 16);
        const uint8_t *env = b + 128;
        uint32_t surfaces = be32(env + 12), per_track = be32(env + 20), reserved = be32(env + 24);
        uint32_t low = be32(env + 36), high = be32(env + 40), f;
        if (be32(env + 4) != BLOCK / 4 || (be32(env + 64) & 0xFFFFFF00u) != 0x444F5300u || high < low) {
            continue;
        }
        uint32_t cyl = surfaces * per_track, blocks = (high - low + 1) * cyl;
        if (volume_free(hdf_read, h, low * cyl, blocks, reserved, &f)) {
            *free_bytes += (uint64_t)f * BLOCK;
            *size += (uint64_t)blocks * BLOCK;
            any = true;
        }
    }
    return any;
}

static void update(int d, bool known, uint64_t free_bytes, uint64_t size)
{
    s_space[d].known = false;
    s_space[d].free = free_bytes;
    s_space[d].size = size;
    s_space[d].known = known;
}

/* Once a frame (from the frame loop): the drives once a second */
void space_frame(void)
{
    if (++s_frames < 50) {
        return;
    }
    s_frames = 0;
    for (int d = 0; d < AMIGA_FLOPPIES; d++) {
        uint64_t f = 0, size = 0;
        bool known = floppy_space(d, &f, &size);
        update(d, known, f, size);
    }
    amiga_hd_status_t hs;
    amiga_hd_get_status(&hs);
    for (int h = 0; h < AMIGA_HARD_DISKS; h++) {
        space_t *s = &s_space[AMIGA_FLOPPIES + h];
        const char *path = amiga_hd_path(h);
        hdf_t *image = hd_image(h);
        if (path == NULL || image == NULL) {
            s->path[0] = '\0';
            update(AMIGA_FLOPPIES + h, false, 0, 0);
            continue;
        }
        bool other = strcmp(path, s->path) != 0;
        if (!other && hs.writes[h] != s->writes) {
            s->writes = hs.writes[h];
            s->waiting = true;      /* (the next second, if it stays so) */
            continue;
        }
        if (!other && !s->waiting) {
            continue;
        }
        snprintf(s->path, sizeof(s->path), "%s", path);
        s->writes = hs.writes[h];
        s->waiting = false;
        uint64_t f = 0, size = 0;
        bool known = hd_space(image, &f, &size);
        update(AMIGA_FLOPPIES + h, known, f, size);
    }
}

bool amiga_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
