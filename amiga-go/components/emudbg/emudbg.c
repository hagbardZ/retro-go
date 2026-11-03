/*
 * emudbg: the debugger's state and its commands (see emudbg.h and
 * docs/debugger.md). Platform independent; the platform's file serves it.
 *
 * Two tasks meet here: the emulation task in emudbg_hit() and
 * emudbg_frame_end(), the network task in emudbg_command(). The state is
 * under emudbg_os_lock(); emudbg_hit() looks without it first (it runs
 * before every instruction of a page with a breakpoint, or every one while
 * stepping out).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emudbg.h"

#if EMUDBG_ENABLED

#define MAX_CPUS        4
#define MAX_BPS         128             /* breakpoints per CPU */
#define MAP_BYTES       8192            /* 65536 bits: addresses (16-bit) or pages (24-bit) */
#define MAX_READ        4096            /* bytes per read command */
#define MAX_DISASM      64              /* instructions per disasm command */
#define OUT_SIZE        (64 * 1024)     /* an answer (screen rows) */
#define SCREEN_BYTES    (44 * 1024)     /* RGB565 per screen answer */
#define MAX_TYPE        256             /* characters per type command */
#define PAUSE_FRAMES    3               /* a pause no CPU took (WAI, STOP): stop between frames */

typedef enum { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT } step_mode_t;

typedef struct {
    uint32_t bp[MAX_BPS];
    volatile int bp_count;
    uint8_t *maps[2];                   /* the core reads one while the other is built */
    int map_cur;
    const uint8_t *given;               /* the map the core has */
    int sp_reg, pc_reg;                 /* register indexes (-1: none) */
} cpu_state_t;

static const emudbg_machine_t *s_machine;
static cpu_state_t s_cpu[MAX_CPUS];
static uint8_t *s_ones;                 /* every bit set: stop anywhere */

static volatile bool s_client;
static volatile bool s_stopped;         /* the emulation task waits in emudbg_hit() */
static volatile bool s_quiet;           /* no stops until the frame ends (reset, suspend) */
static volatile bool s_suspended;       /* the runtime paused the emulation */
static volatile bool s_pause_req;
static volatile int s_pause_frames;     /* frames since the pause was asked for */
static volatile bool s_reset_req, s_reset_stop;
static volatile int s_frames_left;      /* "frames N": stop when it reaches 0 */
static volatile uint32_t s_frame_count;

#define MAX_ENTRIES     32              /* interrupt handlers a step runs through */

static struct {
    volatile step_mode_t mode;
    int cpu;
    uint32_t pc;                        /* where the step began */
    uint32_t sp;                        /* the stack then */
    bool last_return;                   /* the instruction before was a return */
    bool in_irq;                        /* in an interrupt that came during the step */
    uint32_t irq_sp;                    /* the stack at its handler's start */
    bool irq_last_return;
    uint32_t entries[MAX_ENTRIES];      /* the interrupt handlers */
    int entry_count;
} s_step;

static struct {
    int cpu;
    uint32_t pc;
    const char *reason;
} s_stop;

/* ---- Output ---- */

typedef struct {
    char *buf;
    size_t len, size;
    bool done;                          /* a whole answer (an error) */
} out_t;

static char *s_out_buf;                 /* the network task's (command answers) */

static void out_printf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_printf(out_t *o, const char *fmt, ...)
{
    if (o->len >= o->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n < o->size - o->len ? (size_t)n : o->size - o->len - 1;
    }
}

static void out_str(out_t *o, const char *s)
{
    out_printf(o, "\"");
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out_printf(o, "\\%c", ch);
        } else if (ch < 0x20) {
            out_printf(o, "\\u%04x", ch);
        } else {
            out_printf(o, "%c", ch);
        }
    }
    out_printf(o, "\"");
}

static void send_out(out_t *o)
{
    emudbg_os_send(o->buf, o->len);
}

/* ---- CPUs ---- */

static int cpu_count(void)
{
    return s_machine ? s_machine->cpu_count : 0;
}

static emudbg_cpu_t *cpu_at(int i)
{
    return &s_machine->cpus[i];
}

static int cpu_index(const emudbg_cpu_t *cpu)
{
    for (int i = 0; i < cpu_count(); i++) {
        if (cpu_at(i) == cpu) {
            return i;
        }
    }
    return -1;
}

static uint32_t addr_mask(const emudbg_cpu_t *cpu)
{
    return cpu->arch->addr_bits >= 32 ? 0xFFFFFFFFu : (1u << cpu->arch->addr_bits) - 1;
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t a)
{
    a &= addr_mask(cpu);
    return cpu->peek ? cpu->peek(cpu->ctx, a) : cpu->arch->peek(cpu, a);
}

static void poke(emudbg_cpu_t *cpu, uint32_t a, uint8_t v)
{
    a &= addr_mask(cpu);
    if (cpu->poke) {
        cpu->poke(cpu->ctx, a, v);
    } else {
        cpu->arch->poke(cpu, a, v);
    }
}

static uint32_t reg_value(int i, int reg)
{
    if (reg < 0) {
        return 0;
    }
    emudbg_cpu_t *cpu = cpu_at(i);
    return cpu->arch->get_reg(cpu, reg);
}

static uint32_t stack_value(int i)
{
    emudbg_cpu_t *cpu = cpu_at(i);
    return cpu->arch->stack ? cpu->arch->stack(cpu) : reg_value(i, s_cpu[i].sp_reg);
}

static bool is_bp(int i, uint32_t pc)
{
    cpu_state_t *st = &s_cpu[i];
    int n = st->bp_count;
    for (int k = 0; k < n; k++) {
        if (st->bp[k] == pc) {
            return true;
        }
    }
    return false;
}

static void map_set(const emudbg_cpu_t *cpu, uint8_t *map, uint32_t a)
{
    uint32_t bit = cpu->arch->addr_bits > 16 ? (a >> 8) & 0xFFFF : a & 0xFFFF;
    map[bit >> 3] |= 1 << (bit & 7);
}

/* Give each core the map it needs now (under the lock) */
static void apply_maps(void)
{
    for (int i = 0; i < cpu_count(); i++) {
        cpu_state_t *st = &s_cpu[i];
        emudbg_cpu_t *cpu = cpu_at(i);
        const uint8_t *map = NULL;
        if (s_suspended || s_quiet || !s_client) {
            map = NULL;
        } else if (s_pause_req || (s_step.mode != STEP_NONE && s_step.cpu == i)) {
            map = s_ones;
        } else if (st->bp_count > 0) {
            uint8_t *m = st->maps[st->map_cur ^ 1];
            memset(m, 0, MAP_BYTES);
            for (int k = 0; k < st->bp_count; k++) {
                map_set(cpu, m, st->bp[k]);
            }
            st->map_cur ^= 1;
            map = m;
        }
        if (map != st->given) {
            st->given = map;
            cpu->arch->set_map(cpu, map);
        }
    }
}

/* ---- Stopping (the emulation task) ---- */

static void stopped_event(void)
{
    char buf[256];
    out_t o = { buf, 0, sizeof(buf), false };
    char where[96] = "";
    if (s_machine->where) {
        s_machine->where(where, sizeof(where));
    }
    out_printf(&o, "{\"event\":\"stopped\",\"reason\":\"%s\",\"cpu\":%d,\"pc\":%lu,\"where\":",
               s_stop.reason, s_stop.cpu, (unsigned long)s_stop.pc);
    out_str(&o, where);
    out_printf(&o, "}");
    send_out(&o);
}

/* Stop the machine here until the debugger lets it go (lock held: released) */
static void stop(int i, uint32_t pc, const char *reason)
{
    s_stopped = true;
    s_stop.cpu = i;
    s_stop.pc = pc;
    s_stop.reason = reason;
    s_pause_req = false;
    s_frames_left = 0;
    s_step.mode = STEP_NONE;
    apply_maps();
    stopped_event();
    emudbg_os_unlock();
    while (s_stopped && !s_quiet && !s_suspended) {
        emudbg_os_wait(100);
    }
}

/* A step's instruction on its CPU (the emulation task, without the
 * lock: the network task changes s_step only while stopped): stop here?
 * Interrupts that come meanwhile run through (their handler's start is
 * one of the vectors), and returns count by the stack: a call stepped
 * over may take its parameters inline (ProDOS, PRIMM, GS/OS) and return
 * further on.
 */
static bool step_hit(int i, emudbg_cpu_t *cpu, uint32_t pc)
{
    uint32_t sp = stack_value(i);
    if (s_step.in_irq) {
        if (!(s_step.irq_last_return && sp > s_step.irq_sp)) {
            s_step.irq_last_return = cpu->arch->insn(cpu, pc) == EMUDBG_INSN_RETURN;
            return false;
        }
        /* Back from the handler. At the step's instruction: the interrupt
         * came before it ran. Elsewhere it was a call after all (RST $38
         * goes where IM 1 interrupts go): a return just happened.
         */
        s_step.in_irq = false;
        bool before = pc == s_step.pc;
        if (s_step.mode == STEP_INTO) {
            return !before;
        }
        s_step.last_return = !before;
    } else {
        for (int k = 0; k < s_step.entry_count; k++) {
            if (s_step.entries[k] == pc) {
                s_step.in_irq = true;
                s_step.irq_sp = sp;
                s_step.irq_last_return = false;
                return false;
            }
        }
    }
    switch (s_step.mode) {
    case STEP_INTO:
        return true;
    case STEP_OVER:
    case STEP_OUT:
        if (s_step.last_return && (s_step.mode == STEP_OVER ? sp >= s_step.sp : sp > s_step.sp)) {
            return true;
        }
        s_step.last_return = cpu->arch->insn(cpu, pc) == EMUDBG_INSN_RETURN;
        return false;
    default:
        return false;
    }
}

void emudbg_hit(emudbg_cpu_t *cpu, uint32_t pc)
{
    if (s_quiet || s_suspended || s_machine == NULL || !s_client) {
        return;
    }
    int i = cpu_index(cpu);
    if (i < 0) {
        return;
    }
    const char *reason = NULL;
    if (s_pause_req) {
        reason = "pause";
    } else if (s_step.mode != STEP_NONE && s_step.cpu == i && step_hit(i, cpu, pc)) {
        reason = "step";
    } else if (is_bp(i, pc)) {
        reason = "breakpoint";
    } else {
        return;
    }
    emudbg_os_lock();
    if (s_quiet || s_suspended || !s_client) {
        emudbg_os_unlock();
        return;
    }
    stop(i, pc, reason);
}

void emudbg_frame_end(void)
{
    s_frame_count++;
    if (s_machine == NULL || !(s_quiet || s_reset_req || s_frames_left > 0 || s_pause_req)) {
        return;
    }
    emudbg_os_lock();
    if (s_quiet) {
        s_quiet = false;
        apply_maps();
    }
    if (s_reset_req) {
        s_reset_req = false;
        if (s_machine->reset) {
            s_machine->reset();
        }
        if (s_reset_stop && s_client) {
            s_pause_req = true;
            s_pause_frames = 0;
        }
        s_reset_stop = false;
        apply_maps();
    }
    if (!s_client || s_suspended) {
        emudbg_os_unlock();
        return;
    }
    if (s_frames_left > 0 && --s_frames_left == 0) {
        stop(0, reg_value(0, s_cpu[0].pc_reg), "frames");
        return;
    }
    if (s_pause_req && ++s_pause_frames >= PAUSE_FRAMES) {
        /* No CPU ran an instruction (waiting for an interrupt, stopped) */
        stop(0, reg_value(0, s_cpu[0].pc_reg), "pause");
        return;
    }
    emudbg_os_unlock();
}

/* ---- The runtime's side ---- */

static bool alloc_maps(void)
{
    if (s_ones == NULL) {
        s_ones = emudbg_os_alloc(MAP_BYTES);
        if (s_ones == NULL) {
            return false;
        }
        memset(s_ones, 0xFF, MAP_BYTES);
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        for (int k = 0; k < 2; k++) {
            if (s_cpu[i].maps[k] == NULL) {
                s_cpu[i].maps[k] = emudbg_os_alloc(MAP_BYTES);
                if (s_cpu[i].maps[k] == NULL) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void machine_event(void)
{
    char buf[160];
    out_t o = { buf, 0, sizeof(buf), false };
    out_printf(&o, "{\"event\":\"machine\",\"name\":");
    out_str(&o, s_machine ? s_machine->name : NULL);
    out_printf(&o, "}");
    send_out(&o);
}

void emudbg_set_machine(const emudbg_machine_t *machine)
{
    if (!alloc_maps()) {
        return;
    }
    emudbg_os_lock();
    s_machine = machine;
    if (machine && machine->cpu_count > MAX_CPUS) {
        s_machine = NULL;
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_state_t *st = &s_cpu[i];
        st->bp_count = 0;
        st->given = NULL;
        st->sp_reg = st->pc_reg = -1;
        if (i < cpu_count()) {
            const emudbg_arch_t *a = cpu_at(i)->arch;
            for (int r = 0; r < a->reg_count; r++) {
                if ((a->regs[r].flags & EMUDBG_REG_SP) && st->sp_reg < 0) {
                    st->sp_reg = r;
                }
                if ((a->regs[r].flags & EMUDBG_REG_PC) && st->pc_reg < 0) {
                    st->pc_reg = r;
                }
            }
            cpu_at(i)->arch->set_map(cpu_at(i), NULL);
        }
    }
    s_step.mode = STEP_NONE;
    s_pause_req = false;
    s_frames_left = 0;
    s_reset_req = false;
    apply_maps();
    emudbg_os_unlock();
    if (s_client) {
        machine_event();
    }
}

void emudbg_rearm(void)
{
    emudbg_os_lock();
    for (int i = 0; i < cpu_count(); i++) {
        s_cpu[i].given = NULL;
        cpu_at(i)->arch->set_map(cpu_at(i), NULL);
    }
    apply_maps();
    emudbg_os_unlock();
}

void emudbg_suspend(void)
{
    emudbg_os_lock();
    s_suspended = true;
    bool was_stopped = s_stopped;
    s_stopped = false;
    apply_maps();
    emudbg_os_unlock();
    emudbg_os_wake();
    if (was_stopped && s_client) {
        static const char ev[] = "{\"event\":\"continued\",\"reason\":\"paused\"}";
        emudbg_os_send(ev, sizeof(ev) - 1);
    }
}

void emudbg_resume(void)
{
    emudbg_os_lock();
    s_suspended = false;
    apply_maps();
    emudbg_os_unlock();
}

bool emudbg_stopped(void)
{
    return s_stopped;
}

bool emudbg_connected(void)
{
    return s_client;
}

/* Forget everything the client set and let the machine run */
static void detach(void)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        s_cpu[i].bp_count = 0;
    }
    s_step.mode = STEP_NONE;
    s_pause_req = false;
    s_frames_left = 0;
    s_reset_stop = false;
    s_stopped = false;
    apply_maps();
}

void emudbg_client(bool connected)
{
    emudbg_os_lock();
    s_client = connected;
    detach();
    emudbg_os_unlock();
    emudbg_os_wake();
}

/* ---- Commands (the network task) ---- */

static const char *state_name(void)
{
    if (s_machine == NULL) {
        return "none";
    }
    if (s_stopped) {
        return "stopped";
    }
    return s_suspended ? "paused" : "running";
}

/* A number: decimal, 0x or $ hex */
static bool parse_num(const char *s, uint32_t *v)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end;
    unsigned long n;
    if (s[0] == '$') {
        n = strtoul(s + 1, &end, 16);
    } else {
        n = strtoul(s, &end, 0);
    }
    if (*end != '\0') {
        return false;
    }
    *v = (uint32_t)n;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void reply_error(out_t *o, const char *id, const char *msg)
{
    o->len = 0;
    out_printf(o, "{\"id\":%s,\"ok\":false,\"error\":", id);
    out_str(o, msg);
    out_printf(o, "}");
    o->done = true;
}

static void stop_info(out_t *o)
{
    out_printf(o, ",\"stop\":{\"reason\":\"%s\",\"cpu\":%d,\"pc\":%lu}",
               s_stop.reason, s_stop.cpu, (unsigned long)s_stop.pc);
}

static void cmd_hello(out_t *o)
{
    out_printf(o, ",\"protocol\":%d,\"machine\":", EMUDBG_PROTOCOL);
    out_str(o, s_machine ? s_machine->name : NULL);
    out_printf(o, ",\"state\":\"%s\",\"frame\":%lu,\"cpus\":[", state_name(), (unsigned long)s_frame_count);
    for (int i = 0; i < cpu_count(); i++) {
        const emudbg_cpu_t *cpu = cpu_at(i);
        const emudbg_arch_t *a = cpu->arch;
        out_printf(o, "%s{\"name\":", i ? "," : "");
        out_str(o, cpu->name);
        out_printf(o, ",\"arch\":\"%s\",\"addr_bits\":%d,\"regs\":[", a->arch, a->addr_bits);
        for (int r = 0; r < a->reg_count; r++) {
            const emudbg_reg_t *reg = &a->regs[r];
            out_printf(o, "%s{\"name\":\"%s\",\"bits\":%d,\"flags\":%d", r ? "," : "",
                       reg->name, reg->bits, reg->flags);
            if (reg->flag_names) {
                out_printf(o, ",\"flag_names\":\"%s\"", reg->flag_names);
            }
            out_printf(o, "}");
        }
        out_printf(o, "]}");
    }
    out_printf(o, "]");
    if (s_stopped) {
        stop_info(o);
    }
}

/* Let the stopped machine go on (lock held) */
static void go(void)
{
    s_stopped = false;
    apply_maps();
    emudbg_os_wake();
}

static bool cmd_step(out_t *o, const char *id, const char *cmd, int i)
{
    if (!s_stopped) {
        reply_error(o, id, "not stopped");
        return false;
    }
    emudbg_cpu_t *cpu = cpu_at(i);
    uint32_t pc = reg_value(i, s_cpu[i].pc_reg);
    s_step.cpu = i;
    s_step.pc = pc;
    s_step.sp = stack_value(i);
    s_step.in_irq = false;
    s_step.entry_count = cpu->arch->interrupts ? cpu->arch->interrupts(cpu, s_step.entries, MAX_ENTRIES) : 0;
    emudbg_insn_t kind = cpu->arch->insn(cpu, pc);
    s_step.last_return = kind == EMUDBG_INSN_RETURN;
    if (strcmp(cmd, "step") == 0) {
        s_step.mode = STEP_INTO;
    } else if (strcmp(cmd, "next") == 0) {
        s_step.mode = kind == EMUDBG_INSN_CALL ? STEP_OVER : STEP_INTO;
    } else {
        s_step.mode = STEP_OUT;
    }
    go();
    return true;
}

static void cmd_regs(out_t *o, int i)
{
    emudbg_cpu_t *cpu = cpu_at(i);
    out_printf(o, ",\"regs\":[");
    for (int r = 0; r < cpu->arch->reg_count; r++) {
        out_printf(o, "%s%lu", r ? "," : "", (unsigned long)cpu->arch->get_reg(cpu, r));
    }
    out_printf(o, "]");
}

static bool cmd_setreg(out_t *o, const char *id, int i, const char *name, const char *value)
{
    emudbg_cpu_t *cpu = cpu_at(i);
    uint32_t v;
    if (!parse_num(value, &v)) {
        reply_error(o, id, "bad value");
        return false;
    }
    for (int r = 0; r < cpu->arch->reg_count; r++) {
        const emudbg_reg_t *reg = &cpu->arch->regs[r];
        if (strcasecmp(reg->name, name) == 0) {
            if (reg->flags & EMUDBG_REG_RO) {
                reply_error(o, id, "read only");
                return false;
            }
            if (reg->bits < 32) {
                v &= (1u << reg->bits) - 1;
            }
            cpu->arch->set_reg(cpu, r, v);
            out_printf(o, ",\"value\":%lu", (unsigned long)cpu->arch->get_reg(cpu, r));
            return true;
        }
    }
    reply_error(o, id, "no such register");
    return false;
}

static bool cmd_read(out_t *o, const char *id, int i, const char *a1, const char *a2)
{
    uint32_t addr, len;
    if (!parse_num(a1, &addr) || !parse_num(a2, &len) || len > MAX_READ) {
        reply_error(o, id, "usage: read cpu addr len (len <= 4096)");
        return false;
    }
    emudbg_cpu_t *cpu = cpu_at(i);
    out_printf(o, ",\"data\":\"");
    for (uint32_t k = 0; k < len; k++) {
        out_printf(o, "%02x", peek(cpu, addr + k));
    }
    out_printf(o, "\"");
    return true;
}

static bool cmd_write(out_t *o, const char *id, int i, const char *a1, const char *hex)
{
    uint32_t addr;
    size_t n = hex ? strlen(hex) : 0;
    if (!parse_num(a1, &addr) || n % 2) {
        reply_error(o, id, "usage: write cpu addr hexbytes");
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (hexval(hex[k]) < 0) {
            reply_error(o, id, "bad hex");
            return false;
        }
    }
    emudbg_cpu_t *cpu = cpu_at(i);
    for (size_t k = 0; k < n / 2; k++) {
        poke(cpu, addr + (uint32_t)k, (uint8_t)(hexval(hex[2 * k]) << 4 | hexval(hex[2 * k + 1])));
    }
    out_printf(o, ",\"count\":%u", (unsigned)(n / 2));
    return true;
}

static bool cmd_disasm(out_t *o, const char *id, int i, const char *a1, const char *a2)
{
    uint32_t addr, count;
    if (!parse_num(a1, &addr) || !parse_num(a2, &count) || count > MAX_DISASM) {
        reply_error(o, id, "usage: disasm cpu addr count (count <= 64)");
        return false;
    }
    emudbg_cpu_t *cpu = cpu_at(i);
    uint32_t mask = addr_mask(cpu);
    out_printf(o, ",\"lines\":[");
    for (uint32_t k = 0; k < count; k++) {
        char text[64];
        addr &= mask;
        int len = cpu->arch->disassemble(cpu, addr, text, sizeof(text));
        if (len < 1) {
            len = 1;
        }
        out_printf(o, "%s[%lu,\"", k ? "," : "", (unsigned long)addr);
        for (int b = 0; b < len; b++) {
            out_printf(o, "%02x", peek(cpu, addr + (uint32_t)b));
        }
        out_printf(o, "\",");
        out_str(o, text);
        out_printf(o, "]");
        addr += (uint32_t)len;
    }
    out_printf(o, "]");
    return true;
}

static void out_base64(out_t *o, const uint8_t *p, size_t n)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t k = 0; k < n && o->len + 5 < o->size; k += 3) {
        uint32_t v = (uint32_t)p[k] << 16 | (k + 1 < n ? (uint32_t)p[k + 1] << 8 : 0) | (k + 2 < n ? p[k + 2] : 0);
        char *q = o->buf + o->len;
        q[0] = tab[v >> 18];
        q[1] = tab[(v >> 12) & 63];
        q[2] = k + 1 < n ? tab[(v >> 6) & 63] : '=';
        q[3] = k + 2 < n ? tab[v & 63] : '=';
        q[4] = '\0';
        o->len += 4;
    }
}

/* screen: the size; screen y n: n lines from y, RGB565 little-endian, base64 */
static bool cmd_screen(out_t *o, const char *id, char **args, int argc)
{
    if (s_machine->screen == NULL || s_machine->render_line == NULL) {
        reply_error(o, id, "the machine shows no picture here");
        return false;
    }
    int w = 0, h = 0;
    s_machine->screen(&w, &h);
    out_printf(o, ",\"width\":%d,\"height\":%d", w, h);
    if (argc == 0) {
        return true;
    }
    uint32_t y, n;
    if (argc < 2 || !parse_num(args[0], &y) || !parse_num(args[1], &n) || w <= 0 || w > 2048) {
        reply_error(o, id, "usage: screen [y count]");
        return false;
    }
    uint32_t most = SCREEN_BYTES / (uint32_t)(w * 2);
    if (n > most) {
        n = most;
    }
    if (y >= (uint32_t)h) {
        n = 0;
    } else if (y + n > (uint32_t)h) {
        n = (uint32_t)h - y;
    }
    static uint16_t *pixels;
    if (pixels == NULL) {
        pixels = emudbg_os_alloc(SCREEN_BYTES);
        if (pixels == NULL) {
            reply_error(o, id, "no memory");
            return false;
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        s_machine->render_line((int)(y + k), pixels + k * (uint32_t)w);
    }
    out_printf(o, ",\"y\":%lu,\"count\":%lu,\"data\":\"", (unsigned long)y, (unsigned long)n);
    out_base64(o, (const uint8_t *)pixels, (size_t)n * (size_t)w * 2);
    out_printf(o, "\"");
    return true;
}

/* type <hex>: the text, hex-encoded (Return is 0a) */
static bool cmd_type(out_t *o, const char *id, const char *hex)
{
    if (s_machine->type == NULL) {
        reply_error(o, id, "the machine can't type here");
        return false;
    }
    char text[MAX_TYPE + 1];
    size_t n = hex ? strlen(hex) : 0;
    if (n % 2 || n / 2 > MAX_TYPE) {
        reply_error(o, id, "usage: type hextext (up to 256 characters)");
        return false;
    }
    for (size_t k = 0; k < n / 2; k++) {
        int hi = hexval(hex[2 * k]), lo = hexval(hex[2 * k + 1]);
        if (hi < 0 || lo < 0) {
            reply_error(o, id, "bad hex");
            return false;
        }
        text[k] = (char)(hi << 4 | lo);
    }
    text[n / 2] = '\0';
    s_machine->type(text);
    return true;
}

static bool cmd_bps(out_t *o, const char *id, int i, char **args, int argc)
{
    if (argc > MAX_BPS) {
        reply_error(o, id, "too many breakpoints");
        return false;
    }
    uint32_t list[MAX_BPS];
    uint32_t mask = addr_mask(cpu_at(i));
    for (int k = 0; k < argc; k++) {
        if (!parse_num(args[k], &list[k])) {
            reply_error(o, id, "bad address");
            return false;
        }
        list[k] &= mask;
    }
    cpu_state_t *st = &s_cpu[i];
    st->bp_count = 0;
    memcpy(st->bp, list, sizeof(uint32_t) * argc);
    st->bp_count = argc;
    apply_maps();
    out_printf(o, ",\"count\":%d", argc);
    return true;
}

#define MAX_ARGS    (MAX_BPS + 4)

void emudbg_command(char *line)
{
    if (s_out_buf == NULL) {
        s_out_buf = emudbg_os_alloc(OUT_SIZE);
        if (s_out_buf == NULL) {
            return;
        }
    }
    out_t out = { s_out_buf, 0, OUT_SIZE, false }, *o = &out;
    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *p = strtok(line, " \t\r\n"); p && argc < MAX_ARGS; p = strtok(NULL, " \t\r\n")) {
        argv[argc++] = p;
    }
    if (argc < 2) {
        return;
    }
    const char *id = argv[0];
    for (const char *p = id; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return;
        }
    }
    const char *cmd = argv[1];
    char **args = argv + 2;
    argc -= 2;

    emudbg_os_lock();
    out_printf(o, "{\"id\":%s,\"ok\":true", id);
    bool ok = true;
    /* Commands with a CPU: its index first */
    int cpu = -1;
    static const char *const with_cpu[] = { "step", "next", "out", "regs", "setreg", "read", "write",
                                            "disasm", "bps" };
    for (size_t k = 0; k < sizeof(with_cpu) / sizeof(with_cpu[0]); k++) {
        if (strcmp(cmd, with_cpu[k]) == 0) {
            uint32_t n;
            if (argc < 1 || !parse_num(args[0], &n) || (int)n >= cpu_count()) {
                reply_error(o, id, s_machine ? "no such cpu" : "no machine");
                ok = false;
            } else {
                cpu = (int)n;
                args++;
                argc--;
            }
            break;
        }
    }
    if (!ok) {
        /* (the error is in o) */
    } else if (strcmp(cmd, "hello") == 0) {
        cmd_hello(o);
    } else if (strcmp(cmd, "state") == 0) {
        out_printf(o, ",\"state\":\"%s\",\"frame\":%lu", state_name(), (unsigned long)s_frame_count);
        if (s_stopped) {
            stop_info(o);
        }
    } else if (s_machine == NULL) {
        reply_error(o, id, "no machine (the setup screen is open)");
    } else if (strcmp(cmd, "pause") == 0) {
        if (!s_stopped) {
            s_pause_req = true;
            s_pause_frames = 0;
            apply_maps();
        }
    } else if (strcmp(cmd, "continue") == 0) {
        s_step.mode = STEP_NONE;
        if (s_stopped) {
            go();
        }
    } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "next") == 0 || strcmp(cmd, "out") == 0) {
        cmd_step(o, id, cmd, cpu);
    } else if (strcmp(cmd, "frames") == 0) {
        uint32_t n;
        if (argc < 1 || !parse_num(args[0], &n) || n == 0) {
            reply_error(o, id, "usage: frames n");
        } else {
            s_frames_left = (int)n;
            s_step.mode = STEP_NONE;
            s_pause_req = false;
            if (s_stopped) {
                go();
            }
        }
    } else if (strcmp(cmd, "reset") == 0) {
        if (s_machine->reset == NULL) {
            reply_error(o, id, "the machine can't be reset from here");
        } else {
            s_reset_req = true;
            s_reset_stop = argc > 0 && strcmp(args[0], "stop") == 0;
            s_pause_req = false;
            s_step.mode = STEP_NONE;
            s_frames_left = 0;
            s_quiet = true;             /* the rest of the frame runs freely */
            if (s_stopped) {
                go();
            } else {
                apply_maps();
            }
        }
    } else if (strcmp(cmd, "detach") == 0) {
        detach();
        emudbg_os_wake();
    } else if (strcmp(cmd, "regs") == 0) {
        if (!s_stopped) {
            reply_error(o, id, "not stopped");
        } else {
            cmd_regs(o, cpu);
        }
    } else if (strcmp(cmd, "setreg") == 0) {
        if (!s_stopped) {
            reply_error(o, id, "not stopped");
        } else if (argc < 2) {
            reply_error(o, id, "usage: setreg cpu name value");
        } else {
            cmd_setreg(o, id, cpu, args[0], args[1]);
        }
    } else if (strcmp(cmd, "read") == 0) {
        cmd_read(o, id, cpu, argc > 0 ? args[0] : NULL, argc > 1 ? args[1] : NULL);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write(o, id, cpu, argc > 0 ? args[0] : NULL, argc > 1 ? args[1] : "");
    } else if (strcmp(cmd, "disasm") == 0) {
        cmd_disasm(o, id, cpu, argc > 0 ? args[0] : NULL, argc > 1 ? args[1] : NULL);
    } else if (strcmp(cmd, "bps") == 0) {
        cmd_bps(o, id, cpu, args, argc);
    } else if (strcmp(cmd, "screen") == 0) {
        cmd_screen(o, id, args, argc);
    } else if (strcmp(cmd, "type") == 0) {
        cmd_type(o, id, argc > 0 ? args[0] : NULL);
    } else {
        reply_error(o, id, "unknown command");
    }
    if (!o->done) {
        out_printf(o, "}");
    }
    /* Under the lock: an answer goes out before the stop it lets happen */
    send_out(o);
    emudbg_os_unlock();
}

#else

void emudbg_hit(emudbg_cpu_t *cpu, uint32_t pc)
{
    (void)cpu;
    (void)pc;
}

#endif
