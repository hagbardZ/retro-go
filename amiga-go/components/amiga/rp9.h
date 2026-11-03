/*
 * Amiga Forever's RP9 packages (rp9.c)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RP9_DISKS       8
#define RP9_NAME        96

typedef struct {
    char title[64];
    char system[16];                /* "a-500", "a-1200", ... */
    int rom;                        /* systemrom: 120 = Kickstart 1.2, 310 = 3.1 */
    uint32_t chip_ram, fast_ram;    /* bytes it asks for (fast: Zorro III's too; 0: the system's) */
    int disks;                      /* floppies, in their order */
    char disk[RP9_DISKS][RP9_NAME];
    bool harddrive;                 /* a hard disk image too: */
    char hd[RP9_NAME];              /* its member's name */
    int boot_hdf;                   /* <boot type="hdf">135</boot>: Amiga Forever's
                                       system disk it boots from (0: none) */
    bool turbo;                     /* fine with fast floppy disks */
} rp9_t;

bool rp9_read(const char *path, rp9_t *rp9);
bool rp9_member(const char *path, const char *name, uint8_t *dst, uint32_t max, uint32_t *size);
/* A member into a file of its own, unpacked as it goes (hard disk images) */
bool rp9_extract(const char *path, const char *name, const char *out_path);
