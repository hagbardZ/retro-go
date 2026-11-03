/*
 * Shared state of the Amiga. Not part of the public API.
 *
 * Time is counted in the 68000's clocks since the frame began (two to a
 * colour clock, 454 to a line); at each frame's end everything that keeps
 * a time is moved back by the frame's length.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#define ami_malloc(n)       heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define AMI_HOT             IRAM_ATTR
#define AMI_LOGI(...)       ESP_LOGI("amiga", __VA_ARGS__)
#define AMI_LOGW(...)       ESP_LOGW("amiga", __VA_ARGS__)
#else
#include <stdio.h>
#include <stdlib.h>
#define ami_malloc(n)       malloc(n)
#define AMI_HOT
#define AMI_LOGI(fmt, ...)  printf("I amiga: " fmt "\n", ##__VA_ARGS__)
#define AMI_LOGW(fmt, ...)  printf("W amiga: " fmt "\n", ##__VA_ARGS__)
#endif

#define CCK_LINE            227         /* colour clocks a line (NTSC: every other one 228) */
#define CYCLES_LINE         (CCK_LINE * 2)
#define NEVER               0x7FFFFFFF
#define VBLANK_END_PAL      26          /* first line bitplane DMA may use */
#define VBLANK_END_NTSC     21

/* INTENA / INTREQ bits */
#define INT_TBE             0x0001
#define INT_DSKBLK          0x0002
#define INT_SOFT            0x0004
#define INT_PORTS           0x0008
#define INT_COPER           0x0010
#define INT_VERTB           0x0020
#define INT_BLIT            0x0040
#define INT_AUD0            0x0080
#define INT_RBF             0x0800
#define INT_DSKSYN          0x1000
#define INT_EXTER           0x2000
#define INT_INTEN           0x4000

/* DMACON bits */
#define DMA_AUD             0x000F
#define DMA_DSK             0x0010
#define DMA_SPR             0x0020
#define DMA_BLT             0x0040
#define DMA_COP             0x0080
#define DMA_BPL             0x0100
#define DMA_EN              0x0200
#define DMA_BLTPRI          0x0400
#define DMA_BZERO           0x2000
#define DMA_BBUSY           0x4000

/* The clocks the 68000 loses for each of its (even) chip bus slots that
 * DMA takes: a slot is 4 clocks and the 68000 is on the bus about 3/4 of
 * the time (SysInfo and xSysInfo against a real A600 on their 3-plane
 * hires screens)
 */
#define SLOT_WAIT       3

typedef struct {
    uint8_t pra, prb, ddra, ddrb;
    uint8_t cra, crb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint32_t tod, tod_latch, alarm;
    bool tod_latched;               /* TODHI read: frozen for reading until TODLO */
    bool tod_halted;                /* TODHI written: stopped until TODLO */
    uint8_t sdr;
    uint8_t icr, icr_mask;
    bool irq;
    int last;                       /* the time it's up to date with */
    int next;                       /* its next interrupting underflow, or NEVER */
    int index;                      /* 0 CIA-A, 1 CIA-B */
} cia_t;

typedef struct {
    amiga_model_t model;
    bool ecs, ntsc;
    bool aga;                       /* the A1200's Alice and Lisa */
    int cpu_shift;                  /* the CPU's clocks a time unit: 1 << shift (the A1200's 68EC020: 2) */
    int cpu_rem;                    /* (cpu_shift) its clocks not yet a whole unit */

    /* Memory, each 16-bit word in host order (byte n at index n ^ 1); the
     * 68000's view through 64 KB banks (NULL: the slow path)
     */
    uint8_t *chip;
    uint32_t chip_size, chip_mask;  /* what Agnus addresses */
    uint8_t *slow;
    uint32_t slow_size;
    uint8_t *fast;
    uint32_t fast_size;
    uint32_t fast_base;             /* where autoconfig put it (0: not yet) */
    int kick_major, kick_minor;     /* the Kickstart's version */
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t *ext;                   /* the extension ROM at $E00000 (512 KB), or NULL */
    bool ovl;                       /* the ROM over chip RAM (reads) after reset */

    /* Time */
    int cycle;                      /* when the running slice began (or now, between) */
    bool cpu_running;
    int line, lines;                /* this frame's line, its lines */
    int line_start;                 /* the time the line began */
    bool lof;                       /* long frame */
    bool long_lines;                /* this frame NTSC's: 227 and 228 colour clocks by turns */
    bool lol0;                      /* (long_lines) its line 0 a long one */
    int e_phase;                    /* the E clock's phase at the frame's start (0-9) */
    uint32_t frames;
    bool halted;
    uint32_t insns;

    /* Paula's interrupts, Agnus's DMA */
    uint16_t intena, intreq;
    int ipl;
    uint16_t dmacon, adkcon;
    uint16_t potgo;
    uint16_t bus;                   /* the last word on the chip bus (CPU accesses) */
    int stall;                      /* clocks the 68000 owes the chips' DMA (even slots taken) */
    uint16_t beamcon0;
    void (*serial)(int ch);
} amiga_t;

#define EXT_ROM_SIZE    (512 * 1024)    /* the extension ROM's, at $E00000 */

extern amiga_t ami;
extern cia_t cia[2];

static inline int ami_now(void);

#define BEAMCON0_PAL    0x0020

/* The beam's standard now: ECS's Agnus follows BEAMCON0's PAL bit (the
 * chip's own, ami.ntsc, stays in VPOSR's ID and the clocks)
 */
static inline bool ami_ntsc_beam(void)
{
    return ami.ecs ? !(ami.beamcon0 & BEAMCON0_PAL) : ami.ntsc;
}

/* NTSC's lines average 227.5 colour clocks: every other one has 228 (a
 * long line, VPOSR's LOL), the turns going on from frame to frame (a
 * frame of 263 lines starts the next with the other kind); PAL's all have
 * 227. Decided for a frame at its start (ami.long_lines, ami.lol0).
 */
static inline bool ami_long_line(int line)
{
    return ami.long_lines && ((line + ami.lol0) & 1);
}

/* When line n begins, in 68000 clocks from the frame's start */
static inline int ami_line_start(int line)
{
    return line * CYCLES_LINE + (ami.long_lines ? ((line + ami.lol0) >> 1) * 2 : 0);
}

static inline int ami_line_cck(int line)
{
    return CCK_LINE + ami_long_line(line);
}

/* The beam at colour clock cck of the frame (on past its end as if the
 * frame went on)
 */
static inline void ami_beam_at(int cck, int *line, int *h)
{
    int t = cck * 2, l = t / CYCLES_LINE;
    while (l > 0 && ami_line_start(l) > t) {
        l--;
    }
    *line = l;
    *h = (t - ami_line_start(l)) / 2;
}

/* amiga.c */
void ami_update_irq(void);
void ami_set_intreq(uint16_t bits);
void ami_cpu_wake(void);            /* an earlier event was scheduled: end the CPU's slice */

/* mem.c */
void mem_update_map(void);
void mem_cacr(uint32_t cacr);
bool mem_poll_loop(uint32_t pc);
/* The 68000 on the chip bus: a line's even slots the DMA takes (`at` in
 * clocks into the frame), cleared as a line begins
 */
void bus_line_start(void);
void bus_take(int at);
bool bus_waited_over(int t);        /* the 68000's last access waited for DMA from before t to t or later */
/* Bytes between the host and the 68000's memory (words there in host order) */
void mem_copy_to(uint32_t a, const uint8_t *src, uint32_t len);
void mem_copy_from(uint8_t *dst, uint32_t a, uint32_t len);

/* hdboard.c: our own hard disk board (Kickstarts without an IDE driver) */
bool ami_kick_has_ide(void);        /* 2.05 and 3.1: their scsi.device drives Gayle */
bool hdboard_wanted(void);
void hdboard_reset(void);
bool hdboard_configured(uint32_t base);
uint8_t *hdboard_memory(uint32_t *base, uint32_t *size);
bool hdboard_native(void);

/* hdf.c: hard disk images (a file, or an RP9 game's disk with its system disk) */
#define HDF_PIECES      4

typedef struct {
    uint32_t first, count;          /* the drive's blocks */
    int file;                       /* f[file], or -1: the made-up blocks */
    uint32_t at;                    /* the first one's block there */
} hdf_piece_t;

typedef struct {
    FILE *f[2];
    bool read_only;
    uint32_t blocks;
    hdf_piece_t piece[HDF_PIECES];  /* blocks in none: zeros */
    int pieces;
    uint8_t *head;                  /* the made-up blocks (an RDB) */
    uint32_t head_blocks;
    char detail[96];                /* how it was put together ("": a file) */
} hdf_t;

bool hdf_open(hdf_t *h, const char *path);
void hdf_close(hdf_t *h);
bool hdf_io(hdf_t *h, uint32_t lba, int count, uint8_t *buf, bool write);

/* gayle.c: the A600's IDE port and the hard disks */
hdf_t *hd_image(int n);             /* a drive's image (NULL: empty) */
void hd_activity(int n, uint32_t sectors, bool write);  /* its light and counters */
bool gayle_present(void);
bool gayle_irq(void);               /* its level 2 interrupt */
void gayle_reset(void);
void gayle_line(void);
uint8_t gayle_read8(uint32_t a);
uint16_t gayle_read16(uint32_t a);
void gayle_write8(uint32_t a, uint8_t v);
void gayle_write16(uint32_t a, uint16_t v);

/* autoconf.c: the expansion boards at $E80000 */
void autoconf_reset(void);
uint8_t autoconf_read8(uint32_t a);
uint16_t autoconf_read16(uint32_t a);
void autoconf_write8(uint32_t a, uint8_t v);
void autoconf_write16(uint32_t a, uint16_t v);
/* Agnus's DMA in chip RAM (its address lines: chip_mask), a word in host
 * order. Inline: every DMA word (planes, sprites, copper, blitter, sound,
 * disk) comes this way.
 */
static inline uint16_t chip_read16(uint32_t a)
{
    return *(const uint16_t *)(ami.chip + (a & ami.chip_mask & ~1u));
}

static inline void chip_write16(uint32_t a, uint16_t v)
{
    *(uint16_t *)(ami.chip + (a & ami.chip_mask & ~1u)) = v;
}

/* The custom chip registers (custom.c: $DFF000-$DFF1FE, register offsets) */
uint16_t custom_read(uint32_t reg);
void custom_write(uint32_t reg, uint16_t value, int at, bool copper);
void custom_reset(void);

/* copper.c */
typedef enum { COP_STOPPED, COP_FETCH, COP_WAIT } copper_state_t;
typedef struct {
    uint32_t lc[2];
    uint32_t pc;
    int at;                         /* its next step, or NEVER */
    copper_state_t state;
    uint16_t ir1, ir2;
    bool danger;
} copper_t;
extern copper_t cop;
void copper_reset(void);
void copper_vblank(void);
void copper_write(uint32_t reg, uint16_t value, int at, bool copper);
void copper_dma_changed(int at);
bool copper_enabled(void);
int copper_next(void);
void copper_run(int now, int limit);

/* blitter.c */
typedef struct {
    uint16_t con0, con1, afwm, alwm;
    uint32_t pt[4];                 /* A, B, C, D */
    int16_t mod[4];
    uint16_t dat[3];                /* A, B, C */
    uint32_t height, width;
    bool busy, pending, zero;
    int end;                        /* when it's done (beyond this line: not yet known) */
    int left;                       /* the bus slots it still needs after this line's */
    int steal;                      /* the 68000's clocks it still takes (spread over its lines) */
    int steal_line;                 /* of a line */
} blitter_t;
extern blitter_t blit;
void blitter_reset(void);
void blitter_write(uint32_t reg, uint16_t value, int at);
void blitter_dma_changed(int at);
void blitter_line_start(void);      /* a line's free slots to a blit under way */
void blitter_finish(void);
int blitter_steal_line(void);       /* the clocks the blitter takes from the 68000 this line */
bool blitter_busy(int at);

/* display.c: Agnus's bitplane DMA and Denise */
void display_init(void *picture);
void display_reset(void);
void display_write(uint32_t reg, uint16_t value, int at);
void display_line_start(void);
int display_fetch_at(void);
int display_dma_slots(void);        /* the bus slots this line's bitplanes and sprites take */
void display_fetch(void);
void display_line_end(void);
bool display_frame_end(void);       /* true: the frame was interlaced */
void display_next_field(bool lace);
int display_dbg_frame(void);        /* the debugger's picture: its width */
void display_dbg_line(int y, uint16_t *dst);
void display_dma_changed(int at);
uint16_t display_clxdat(void);      /* CLXDAT read (and cleared) */
void display_clxcon(uint16_t value);

/* cia.c */
void cia_reset(void);
uint8_t cia_read(cia_t *c, int reg);
void cia_write(cia_t *c, int reg, uint8_t value);
void cia_sync(cia_t *c, int now);
void cia_tod_tick(cia_t *c);
void cia_flag(cia_t *c);            /* a falling edge on FLAG (CIA-B: the drives' index) */
void cia_serial_in(cia_t *c, uint8_t value);
void cia_rebase(cia_t *c, int frame_cycles);

/* input.c: the keyboard, the mouse, the joystick */
void input_reset(void);
void input_line(void);
void input_frame(void);            /* typed text */
void input_handshake(void);
bool input_reset_wanted(void);      /* Ctrl-Amiga-Amiga */
uint16_t input_joy0dat(void);
uint16_t input_joy1dat(void);
void input_joytest(uint16_t value);
uint8_t input_fire(void);           /* CIA-A PRA bits 7-6 */
uint16_t input_pot_low(void);       /* POTGOR bits a button pulls low */

/* audio.c: Paula's sound */
void audio_reset(void);
void audio_write(uint32_t reg, uint16_t value);
void audio_dma_changed(void);
void audio_line(void);

/* rtc.c: the battery-backed clock at $DC0000 (register 0-15) */
void rtc_init(int64_t offset, void (*changed)(int64_t offset), bool past_99);
uint8_t rtc_read(int reg);
void rtc_write(int reg, uint8_t value);

/* floppy.c: the drives, Paula's disk DMA */
void floppy_reset(void);
void floppy_control(uint8_t old_prb, uint8_t prb);
uint8_t floppy_status(void);    /* CIA-A PRA bits 5-2: /RDY /TK0 /WPRO /CHNG */
void floppy_sync(int at);       /* the disks turn up to this time */
void floppy_rebase(int frame_cycles);
void floppy_write(uint32_t reg, uint16_t value, int at);
uint16_t floppy_dskbytr(void);
const uint8_t *floppy_image(int drive, uint32_t *blocks);  /* its ADF (NULL: none, extended) */

/* space.c: the disks' free space for the web page */
void space_frame(void);                     /* once a frame, from the frame loop */

/* (Musashi's m68k_cycles_run(), inline: VPOSR polls ask often) */
static inline int ami_now(void)
{
    extern int m68ki_initial_cycles, m68ki_remaining_cycles;
    return ami.cycle + (ami.cpu_running ? (m68ki_initial_cycles - m68ki_remaining_cycles + ami.cpu_rem) >> ami.cpu_shift
                                        : 0);
}

/* The 68EC020 waits for the chip bus (its clocks): taken from the slice's
 * remaining cycles, it ends that much sooner and m68k_execute() counts
 * them as run (outside a slice, the next one starts its count afresh).
 * Inline: a call on each access to chip RAM was 18% of core 1.
 */
static inline void ami_cpu_wait(int clocks)
{
    extern int m68ki_remaining_cycles;
    m68ki_remaining_cycles -= clocks;
}

/* The 68000's accesses to the CIAs (VPA: 6800-style, by the E clock) end
 * with E's high phase: 10-19 clocks instead of a bus cycle's 4 (WinUAE's
 * cia_wait_pre/post: to 4 clocks past E's next start, then 6; its
 * interrupt acknowledge is a plain 4-clock read). The clocks more than 4
 * for one beginning at t.
 */
static inline int ami_e_wait(int t)
{
    int div = ((t + ami.e_phase) % 10 + 10) % 10;
    return (div ? 14 - div : 4) + 6 - 4;
}
