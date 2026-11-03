/*
 * Run the Amiga on the host: boot a Kickstart for a number of frames and
 * write the last picture as a PPM image.
 *
 *   ./ami_host <kickstart.rom | folder of them> [frames] [out.ppm]
 *
 * An RP9 in AMI_DF0 or AMI_HD0 without AMI_MODEL: the machine its manifest
 * names (model, memory; given a folder, its Kickstart by FS-UAE's names,
 * amiga-os-130.rom ...).
 *
 * Environment: AMI_CHIP_KB (512), AMI_SLOW_KB (512; 0 for none), AMI_FAST_KB
 * (0; 1024-8192: an autoconfig board), AMI_ECS,
 * AMI_NTSC, AMI_SERIAL (print what's sent on the serial port), AMI_EXT=<rom>
 * (AROS's extension ROM at $E00000),
 * AMI_SHOTS=<n> (a picture every n frames, <out>-<frame>.ppm), AMI_LOG=<n>
 * (the machine's state every n frames), AMI_RING=<n> (the last n
 * instructions at the end), AMI_STOP=<pc>[:<frame>], AMI_TRACE=<pc>:<n>,
 * AMI_DIS=<addr hex>:<n> (disassemble and stop), AMI_EXC (exceptions other than interrupts; "all" with them),
 * AMI_EXCSTOP=<vector>[:<frame>] (stop at that exception, as AMI_STOP), AMI_LOGREGS
 * (AMI_LOG's lines with the display's registers),
 * AMI_DEBUG=<port> (serve the debugger, in real time), AMI_MODEL (0 A500, 1
 * A2000, 2 A600), AMI_HD0, AMI_HD1 (HDF images, written: use copies),
 * AMI_NEWHD=<path>:<MB> (a blank hard disk made first), AMI_DF0, AMI_DF1
 * (ADF images in the drives: written to!), AMI_INSERT=<frame>:<drive>:<adf>
 * [;...] (later; an empty name ejects), AMI_INPUT=<frame>:m:<dx>:<dy>:<buttons>
 * | <frame>:k:<code hex>:<1|0> | <frame>:j:<bits hex> | <frame>:t:<text>
 * [;...] (the mouse, a key, the joystick at that frame, text typed from
 * then on, "\n" for Return; a mouse move of more than 100 is spread over
 * the frames after), AMI_WAV=<file> (the sound), AMI_DISKLOG
 * (each disk DMA), AMI_DUMP=<addr hex>:<bytes>:<file> (memory at the end).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "amiga.h"
#include "m68k.h"
#include "emudbg.h"

static uint32_t s_frame;
static uint64_t s_insns;

static void disassemble(uint32_t pc, int count)
{
    char buf[100];
    for (int i = 0; i < count; i++) {
        unsigned int len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("  $%06lx  %s\n", (unsigned long)pc, buf);
        pc += len;
    }
}

#define RING_SIZE 4096
static uint32_t s_ring[RING_SIZE];
static uint32_t s_ring_pos;

static void show_ring(int n)
{
    if (n > RING_SIZE) {
        n = RING_SIZE;
    }
    for (uint32_t i = s_ring_pos - n; i != s_ring_pos; i++) {
        disassemble(s_ring[i % RING_SIZE], 1);
    }
}

/* The last instructions and the registers, then the end */
static void stop_here(void)
{
    show_ring(60);
    printf("D0-7:");
    for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) {
        printf(" %08x", m68k_get_reg(NULL, r));
    }
    printf("\nA0-7:");
    for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) {
        printf(" %08x", m68k_get_reg(NULL, r));
    }
    printf("\n");
    exit(0);
}

void m68k_host_instruction(unsigned int pc)
{
    s_insns++;
    s_ring[s_ring_pos++ % RING_SIZE] = pc;

    static uint32_t stop_pc = 1;
    static uint32_t stop_after;
    if (stop_pc == 1) {
        const char *e = getenv("AMI_STOP");
        stop_pc = e ? strtoul(e, NULL, 16) : 0;
        stop_after = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (stop_pc && pc == stop_pc && s_frame >= stop_after) {
        printf("-- reached $%06x at frame %lu after:\n", pc, (unsigned long)s_frame);
        stop_here();
    }

    static uint32_t trace_pc = 1;
    static int trace_n, trace_left;
    if (trace_pc == 1) {
        const char *e = getenv("AMI_TRACE");
        trace_pc = e ? strtoul(e, NULL, 16) : 0;
        trace_n = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 100;
    }
    if (trace_pc && pc == trace_pc && trace_left == 0) {
        trace_left = trace_n;
    }
    if (trace_left > 0) {
        trace_left--;
        disassemble(pc, 1);
    }
}

static void exception(unsigned int vector)
{
    static int mode = -1;
    static unsigned stop_vector, stop_after;
    if (mode < 0) {
        const char *e = getenv("AMI_EXC");
        mode = e ? (strcmp(e, "all") == 0 ? 2 : 1) : 0;
        e = getenv("AMI_EXCSTOP");
        if (e) {
            sscanf(e, "%u:%u", &stop_vector, &stop_after);
        }
    }
    if (stop_vector && vector == stop_vector && s_frame >= stop_after) {
        printf("-- exception %u at PC $%06x (frame %lu), SR %04x, after:\n", vector,
               m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, (unsigned long)s_frame,
               m68k_get_reg(NULL, M68K_REG_SR));
        stop_here();
    }
    if (mode == 0 || (mode == 1 && vector >= 24 && vector < 32)) {
        return;
    }
    printf("-- exception %u at PC $%06x (frame %lu), SR %04x\n", vector,
           m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, (unsigned long)s_frame,
           m68k_get_reg(NULL, M68K_REG_SR));
}

static void disk_dma(uint16_t len, uint16_t sync, uint16_t adkcon, int cylinder, int side, uint32_t pt)
{
    printf("-- disk %s %u words, sync %04x%s, cylinder %d side %d, to $%06x (frame %lu)\n",
           (len & 0x4000) ? "write" : "read", len & 0x3FFF, sync, (adkcon & 0x0400) ? "" : " (not used)",
           cylinder, side, (unsigned)pt, (unsigned long)s_frame);
}

static void serial(int ch)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("AMI_SERIAL") != NULL;
    }
    if (on) {
        putchar(ch);
        fflush(stdout);
    }
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    amiga_screen_signature();       /* (shows the latest frame) */
    int width = amiga_screen_width(), height = amiga_screen_height();
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    static uint16_t line[2 * AMIGA_SCREEN_WIDTH];
    for (int y = 0; y < height; y++) {
        amiga_render_line_rgb565(y, line);
        for (int x = 0; x < width; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                               (uint8_t)((c & 31) * 255 / 31) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* AMI_WAV: 16-bit stereo at AMIGA_AUDIO_HZ */
static FILE *s_wav;
static uint32_t s_wav_frames;

static void wav_header(void)
{
    uint32_t bytes = s_wav_frames * 4, rate = AMIGA_AUDIO_HZ;
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2);
    w = 2; memcpy(h + 22, &w, 2);
    memcpy(h + 24, &rate, 4);
    v = rate * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_wav);
    fseek(s_wav, 0, SEEK_END);
}

static void wav_drain(void)
{
    int16_t buf[2 * 1024];
    int n;
    while ((n = amiga_audio_render(buf, 1024)) > 0) {
        if (s_wav) {
            fwrite(buf, 4, (size_t)n, s_wav);
            s_wav_frames += (uint32_t)n;
        }
    }
}

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        exit(1);
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

/* The clock set by the Amiga (the board saves the offset in NVS) */
static void rtc_changed(int64_t offset)
{
    printf("clock offset now %lld s\n", (long long)offset);
}

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e ? atoi(e) : def;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <kickstart.rom> [frames] [out.ppm]\n", argv[0]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 500;
    const char *out = argc > 3 ? argv[3] : "screen.ppm";

    /* the machine an RP9 names (the board's setup rows: ECS with more than
     * 512 KB chip RAM or Kickstart 2.04, the A600's and A1200's own)
     */
    amiga_rp9_machine_t rm;
    const char *pkg = NULL;
    for (int i = 0; i < 2 && !pkg; i++) {
        const char *p = getenv(i ? "AMI_DF0" : "AMI_HD0");
        if (p && strlen(p) > 4 && strcasecmp(p + strlen(p) - 4, ".rp9") == 0) {
            pkg = p;
        }
    }
    bool from_rp9 = !getenv("AMI_MODEL") && pkg && amiga_rp9_machine(pkg, &rm);

    amiga_config_t cfg = { 0 };
    cfg.model = from_rp9 ? rm.model : (amiga_model_t)env_int("AMI_MODEL", AMIGA_A500);
    cfg.ecs = from_rp9 ? rm.model >= AMIGA_A600 || rm.chip_kb > 512 || rm.kickstart == 204 : getenv("AMI_ECS") != NULL;
    cfg.ntsc = getenv("AMI_NTSC") != NULL;
    cfg.chip_size = (from_rp9 ? rm.chip_kb : env_int("AMI_CHIP_KB", 512)) * 1024;
    cfg.chip = malloc(cfg.chip_size);
    cfg.slow_size = (from_rp9 ? rm.slow_kb : env_int("AMI_SLOW_KB", 512)) * 1024;
    cfg.slow = cfg.slow_size ? malloc(cfg.slow_size) : NULL;
    cfg.fast_size = (from_rp9 ? rm.fast_kb : env_int("AMI_FAST_KB", 0)) * 1024;
    cfg.fast = cfg.fast_size ? malloc(cfg.fast_size) : NULL;
    struct stat st;
    char kick[600];
    snprintf(kick, sizeof(kick), "%s", argv[1]);
    if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
        int k = from_rp9 ? rm.kickstart : 130;
        const char *name = k == 120 ? "120" : k == 130 ? "130" : k == 204 ? "204" : k == 205 ? "205-a600"
                           : k == 300                                           ? "300-a1200"
                           : cfg.model == AMIGA_A1200                           ? "310-a1200"
                                                                                : "310-a600";
        snprintf(kick, sizeof(kick), "%s/amiga-os-%s.rom", argv[1], name);
    }
    if (from_rp9) {
        printf("%s: %s, Kickstart %d.%d, %d KB chip, %d KB slow, %d KB fast RAM (%s)\n", rm.title, rm.system,
               rm.kickstart / 100, rm.kickstart % 100 / 10, rm.chip_kb, rm.slow_kb, rm.fast_kb, kick);
    }
    cfg.kickstart = load(kick, &cfg.kickstart_size);
    cfg.rom = malloc(cfg.kickstart_size);
    if (getenv("AMI_EXT")) {
        uint32_t ext_size = 0;
        cfg.extension = load(getenv("AMI_EXT"), &ext_size);
        cfg.ext_rom = malloc(512 * 1024);
        if (ext_size != 512 * 1024) {
            fprintf(stderr, "AMI_EXT: not 512 KB\n");
            return 1;
        }
    }
    cfg.serial = serial;
    cfg.rtc_changed = rtc_changed;
    amiga_exception_hook = exception;
    if (getenv("AMI_DISKLOG")) {
        amiga_disk_hook = disk_dma;
    }
    if (!amiga_init(&cfg)) {
        return 1;
    }

    /* AMI_DIS=<addr hex>:<n>: disassemble n instructions (the ROM is mapped
     * after reset) and stop */
    if (getenv("AMI_DIS")) {
        unsigned pc = 0, count = 20;
        sscanf(getenv("AMI_DIS"), "%x:%u", &pc, &count);
        for (unsigned i = 0; i < count; i++) {
            char text[100];
            unsigned n = m68k_disassemble(text, pc, cfg.model == AMIGA_A1200 ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
            printf("  $%06x  %s\n", pc, text);
            pc += n;
        }
        return 0;
    }

    for (int d = 0; d < AMIGA_FLOPPIES; d++) {
        char name[8];
        snprintf(name, sizeof(name), "AMI_DF%d", d);
        if (getenv(name) && !amiga_floppy_insert(d, getenv(name))) {
            fprintf(stderr, "%s: can't use %s\n", name, getenv(name));
            return 1;
        }
    }
    if (getenv("AMI_NEWHD")) {
        char path[400];
        unsigned mb = 0;
        if (sscanf(getenv("AMI_NEWHD"), "%399[^:]:%u", path, &mb) != 2 ||
            !amiga_hd_create(path, mb * 1048576u / 516096u * 516096u)) {
            fprintf(stderr, "AMI_NEWHD: can't make %s\n", getenv("AMI_NEWHD"));
            return 1;
        }
    }
    for (int d = 0; d < AMIGA_HARD_DISKS; d++) {
        char name[8];
        snprintf(name, sizeof(name), "AMI_HD%d", d);
        if (getenv(name) && !amiga_hd_insert(d, getenv(name))) {
            fprintf(stderr, "%s: can't use %s\n", name, getenv(name));
            return 1;
        }
    }

    emudbg_host_run("AMI_DEBUG", amiga_run_frame, amiga_screen_hz());

    if (getenv("AMI_WAV")) {
        s_wav = fopen(getenv("AMI_WAV"), "wb");
        if (s_wav) {
            wav_header();
            amiga_audio_enable(true);
        }
    }

    int shots = env_int("AMI_SHOTS", 0), log_every = env_int("AMI_LOG", 0);
    clock_t t0 = clock();
    const char *inserts = getenv("AMI_INSERT");
    const char *inputs = getenv("AMI_INPUT");
    int move_x = 0, move_y = 0, buttons = 0;
    for (s_frame = 0; s_frame < (uint32_t)frames; s_frame++) {
        for (const char *p = inputs; p && *p;) {
            unsigned at = 0;
            char kind = 0;
            int a = 0, b = 0, c = 0;
            int n = sscanf(p, "%u:%c:%x:%x", &at, &kind, (unsigned *)&a, (unsigned *)&b);
            if (n >= 2 && at == s_frame) {
                if (kind == 'm' && sscanf(p, "%u:m:%d:%d:%d", &at, &a, &b, &c) == 4) {
                    move_x += a;
                    move_y += b;
                    buttons = c;
                } else if (kind == 'k' && n == 4) {
                    amiga_key((uint8_t)a, b != 0);
                } else if (kind == 'j' && n >= 3) {
                    amiga_joystick((uint8_t)a);
                } else if (kind == 't') {
                    char text[256];
                    size_t len = 0;
                    for (const char *t = strchr(strchr(p, ':') + 1, ':') + 1; *t && *t != ';' && len < sizeof(text) - 1;
                         t++) {
                        if (t[0] == '\\' && t[1] == 'n') {
                            text[len++] = '\n';
                            t++;
                        } else {
                            text[len++] = *t;
                        }
                    }
                    text[len] = '\0';
                    amiga_type(text);
                }
            }
            const char *end = strchr(p, ';');
            p = end ? end + 1 : NULL;
        }
        {
            int dx = move_x > 100 ? 100 : move_x < -100 ? -100 : move_x;
            int dy = move_y > 100 ? 100 : move_y < -100 ? -100 : move_y;
            move_x -= dx;
            move_y -= dy;
            amiga_mouse(dx, dy, (uint8_t)buttons);
        }
        for (const char *p = inserts; p && *p;) {
            unsigned at = 0;
            int drive = 0, used = 0;
            char path[400] = "";
            if (sscanf(p, "%u:%d:%n", &at, &drive, &used) < 2) {
                break;
            }
            const char *name = p + used, *end = strchr(name, ';');
            size_t n = end ? (size_t)(end - name) : strlen(name);
            if (at == s_frame) {
                snprintf(path, sizeof(path), "%.*s", (int)n, name);
                if (n == 0) {
                    amiga_floppy_eject(drive);
                } else if (!amiga_floppy_insert(drive, path)) {
                    fprintf(stderr, "can't insert %s\n", path);
                }
            }
            p = end ? end + 1 : NULL;
        }
        amiga_run_frame();
        wav_drain();
        if (log_every && (s_frame + 1) % log_every == 0) {
            amiga_status_t st;
            amiga_get_status(&st);
            printf("-- frame %lu: PC $%06lx SR %04x INTENA %04x INTREQ %04x DMACON %04x%s\n",
                   (unsigned long)st.frames, (unsigned long)st.pc, st.sr, st.intena, st.intreq,
                   st.dmacon, st.halted ? " HALTED" : "");
            if (getenv("AMI_LOGREGS")) {
                char regs[200];
                amiga_display_regs(regs, sizeof(regs));
                printf("   %s\n", regs);
            }
        }
        if (shots && (s_frame + 1) % shots == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s-%04lu.ppm", (int)(strlen(out) - 4), out,
                     (unsigned long)(s_frame + 1));
            write_ppm(path);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d frames in %.2f s (%.0f%% of real time), %.1f M instructions\n", frames, secs,
           secs > 0 ? frames / (double)amiga_screen_hz() / secs * 100 : 0, s_insns / 1e6);
    amiga_hd_status_t hs;
    amiga_hd_get_status(&hs);
    for (int d = 0; d < AMIGA_HARD_DISKS; d++) {
        if (amiga_hd_path(d)) {
            printf("DH%d: %lu sectors read, %lu written\n", d, (unsigned long)hs.reads[d], (unsigned long)hs.writes[d]);
        }
    }
    for (int d = 0; d < AMIGA_FLOPPIES + AMIGA_HARD_DISKS; d++) {
        uint64_t free_bytes, size;
        if (amiga_disk_space(d, &free_bytes, &size)) {
            printf("%s%d: %llu bytes free of %llu\n", d < AMIGA_FLOPPIES ? "DF" : "DH",
                   d < AMIGA_FLOPPIES ? d : d - AMIGA_FLOPPIES, (unsigned long long)free_bytes,
                   (unsigned long long)size);
        }
    }
    amiga_mem_free_t mem[3];
    if (amiga_mem_free(&mem[0], &mem[1], &mem[2])) {
        static const char *const kinds[] = { "chip", "slow", "fast" };
        for (int k = 0; k < 3; k++) {
            if (mem[k].found) {
                printf("%s RAM: %lu bytes free, largest block %lu\n", kinds[k], (unsigned long)mem[k].free,
                       (unsigned long)mem[k].largest);
            }
        }
    }
    if (getenv("AMI_RING")) {
        show_ring(atoi(getenv("AMI_RING")));
    }
    write_ppm(out);
    amiga_floppy_flush();
    if (getenv("AMI_DUMP")) {
        unsigned addr = 0, len = 0;
        char file[400] = "";
        if (sscanf(getenv("AMI_DUMP"), "%x:%u:%399s", &addr, &len, file) == 3) {
            FILE *f = fopen(file, "wb");
            for (unsigned i = 0; f && i < len; i++) {
                fputc(amiga_peek8(addr + i), f);
            }
            if (f) {
                fclose(f);
            }
        }
    }
    if (s_wav) {
        wav_header();
        fclose(s_wav);
    }
    return 0;
}
