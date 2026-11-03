/* Disk accesses between line ends, with a generated blank ADF. No ROM needed. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "amiga_internal.h"

static uint8_t chip[512 * 1024];
static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", message); } \
} while (0)

void m68k_host_instruction(unsigned int pc) { (void)pc; }

static void setup(const char *path)
{
    amiga_floppy_eject(0);
    memset(&ami, 0, sizeof(ami));
    memset(chip, 0x5A, sizeof(chip));
    ami.chip = chip;
    ami.chip_size = sizeof(chip);
    ami.chip_mask = sizeof(chip) - 1;
    floppy_reset();
    CHECK(amiga_floppy_insert(0, path), "insert blank disk");
    ami.dmacon = DMA_EN | DMA_DSK;
}

static void arm(int at)
{
    floppy_write(0x020, 0, at);
    floppy_write(0x022, 0x1000, at);
    floppy_write(0x024, 0x8002, at);
    floppy_write(0x024, 0x8002, at);
}

int main(void)
{
    char path[] = "/tmp/esp32ami-floppy-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    int sized = ftruncate(fd, 901120);
    close(fd);
    if (sized) { perror("ftruncate"); unlink(path); return 1; }

    setup(path);
    floppy_control(0xFF, 0x77);          /* DF0 selected, motor on, side 0 */
    arm(300);                          /* first word (clock 224) already passed */
    floppy_sync(454);
    CHECK(chip_read16(0x1000) == 0xAAAA, "word at clock 448 reaches DMA");
    CHECK(chip_read16(0x1002) == 0x5A5A, "DMA must not capture the word before it started");
    CHECK(!(ami.intreq & INT_DSKBLK), "two-word DMA must still be active");
    floppy_sync(454);
    CHECK(chip_read16(0x1002) == 0x5A5A, "syncing the same time must not advance DMA");
    floppy_sync(400);
    CHECK(chip_read16(0x1002) == 0x5A5A, "a CPU overshoot must not move the disk backwards");
    floppy_rebase(454);                 /* same phase on the next frame */
    floppy_sync(218);                   /* absolute clock 672: the next word */
    CHECK(chip_read16(0x1002) == 0xAAAA, "DMA continues across a frame boundary");
    CHECK(ami.intreq & INT_DSKBLK, "second word completes DMA");

    /* A DSKSYNC write covers the words that went by up to the write: the
     * new sync must not be taken back over them when the line is next
     * brought up to date. The gap's words are all $AAAA, so a sync written
     * at clock 700 would match the words at 448 and 672 if the disk were
     * left behind at 300. */
    setup(path);
    floppy_control(0xFF, 0x77);         /* DF0 selected, motor on, side 0 */
    floppy_sync(300);                   /* the word at clock 224 has gone */
    ami.intreq = 0;
    floppy_write(0x07E, 0xAAAA, 700);   /* ... and those at 448 and 672 */
    floppy_sync(701);                   /* no word falls between the two */
    CHECK(!(ami.intreq & INT_DSKSYN), "a late DSKSYNC must not match the words before it");
    CHECK(!(floppy_dskbytr() & 0x1000), "nor leave DSKBYTR's word-equal set");
    floppy_sync(900);                   /* the word at clock 896 */
    CHECK(ami.intreq & INT_DSKSYN, "the next word matches the new sync");
    CHECK(floppy_dskbytr() & 0x1000, "DSKBYTR reports the new sync's match");

    amiga_floppy_eject(0);
    unlink(path);
    puts(failures ? "floppy checks failed" : "all floppy timing checks passed");
    return failures != 0;
}
