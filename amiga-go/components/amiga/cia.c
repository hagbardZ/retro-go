/*
 * The two 8520 CIAs. CIA-A: the ROM overlay and the power LED, the fire
 * buttons and the drives' status lines, the keyboard's serial port; its
 * interrupt is Paula's INT2 (PORTS). CIA-B: the drives' control lines, the
 * serial port's handshake; INT6 (EXTER).
 *
 * The timers count the E clock (the 68000's clock / 10) and are brought up
 * to date when they're read or written and when one underflows with its
 * interrupt enabled (an event in the frame loop). TOD counts the vertical
 * sync on CIA-A, the horizontal on CIA-B.
 */
#include <string.h>
#include "amiga_internal.h"

cia_t cia[2];

enum { PRA, PRB, DDRA, DDRB, TALO, TAHI, TBLO, TBHI, TODLO, TODMID, TODHI, UNUSED, SDR, ICR, CRA, CRB };

#define CR_START        0x01
#define CR_RUNMODE      0x08        /* one-shot */
#define CR_LOAD         0x10
#define CRA_SPMODE      0x40
#define CRB_INMODE      0x60
#define CRB_ALARM       0x80

#define ICR_TA          0x01
#define ICR_TB          0x02
#define ICR_ALRM        0x04
#define ICR_SP          0x08
#define ICR_FLG         0x10

/* E clock edges in (from, to] */
static inline int e_ticks(int from, int to)
{
    return (to + ami.e_phase) / 10 - (from + ami.e_phase) / 10;
}

/* The time of the n-th E clock edge after t */
static inline int e_edge(int t, int n)
{
    return ((t + ami.e_phase) / 10 + n) * 10 - ami.e_phase;
}

static void check_irq(cia_t *c)
{
    bool irq = (c->icr & c->icr_mask) != 0;
    if (irq && !c->irq) {
        ami_set_intreq(c->index ? INT_EXTER : INT_PORTS);
    }
    c->irq = irq;
}

static inline uint8_t port_out(uint8_t pr, uint8_t ddr)
{
    return (pr & ddr) | (uint8_t)~ddr;      /* inputs pulled high */
}

/* Count a timer down by ticks: the underflows (after counting through 0) */
static int count(uint16_t *counter, uint16_t latch, uint8_t *cr, int ticks)
{
    if (!(*cr & CR_START) || ticks <= 0) {
        return 0;
    }
    uint32_t c = *counter;
    if ((uint32_t)ticks <= c) {
        *counter = c - ticks;
        return 0;
    }
    ticks -= c + 1;
    if (*cr & CR_RUNMODE) {
        *cr &= ~CR_START;
        *counter = latch;
        return 1;
    }
    uint32_t period = (uint32_t)latch + 1;
    *counter = latch - ticks % period;
    return 1 + ticks / period;
}

static bool tb_counts_ta(const cia_t *c)
{
    return (c->crb & CRB_INMODE) == 0x40 || (c->crb & CRB_INMODE) == 0x60;
}

static void plan(cia_t *c)
{
    c->next = NEVER;
    if ((c->icr_mask & ICR_TA) && (c->cra & CR_START) && !(c->cra & 0x20)) {
        c->next = e_edge(c->last, c->ta + 1);
    }
    if ((c->icr_mask & ICR_TB) && (c->crb & CR_START)) {
        int t = NEVER;
        if ((c->crb & CRB_INMODE) == 0) {
            t = e_edge(c->last, c->tb + 1);
        } else if (tb_counts_ta(c) && (c->cra & CR_START) && !(c->cra & 0x20) && !(c->cra & CR_RUNMODE)) {
            int64_t n = (int64_t)c->ta + 1 + (int64_t)c->tb * ((int64_t)c->ta_latch + 1);
            if (n < 0x7FFFFFF) {
                t = e_edge(c->last, (int)n);
            }
        }
        if (t < c->next) {
            c->next = t;
        }
    }
}

void cia_sync(cia_t *c, int now)
{
    int ticks = e_ticks(c->last, now);
    c->last = now;
    if (ticks > 0) {
        int ta_under = 0;
        if (!(c->cra & 0x20)) {
            ta_under = count(&c->ta, c->ta_latch, &c->cra, ticks);
            if (ta_under) {
                c->icr |= ICR_TA;
            }
        }
        int tb_ticks = (c->crb & CRB_INMODE) == 0 ? ticks : tb_counts_ta(c) ? ta_under : 0;
        if (count(&c->tb, c->tb_latch, &c->crb, tb_ticks)) {
            c->icr |= ICR_TB;
        }
        check_irq(c);
    }
    plan(c);
}

void cia_rebase(cia_t *c, int frame_cycles)
{
    c->last -= frame_cycles;
    if (c->next != NEVER) {
        c->next -= frame_cycles;
    }
}

void cia_tod_tick(cia_t *c)
{
    if (c->tod_halted) {
        return;
    }
    c->tod = (c->tod + 1) & 0xFFFFFF;
    if (c->tod == c->alarm) {
        c->icr |= ICR_ALRM;
        check_irq(c);
    }
}

/* A byte shifted in from outside (CIA-A: the keyboard) */
void cia_serial_in(cia_t *c, uint8_t v)
{
    c->sdr = v;
    c->icr |= ICR_SP;
    check_irq(c);
}

void cia_flag(cia_t *c)
{
    c->icr |= ICR_FLG;
    check_irq(c);
}

static uint8_t port_a_in(const cia_t *c)
{
    if (c->index == 0) {
        /* the fire buttons, the drives' lines, LED and OVL pulled up */
        return input_fire() | 0x03 | floppy_status();
    }
    return 0xFF;
}

uint8_t cia_read(cia_t *c, int reg)
{
    switch (reg) {
    case PRA:
        return (c->pra & c->ddra) | (port_a_in(c) & ~c->ddra);
    case PRB:
        return port_out(c->prb, c->ddrb);
    case DDRA:
        return c->ddra;
    case DDRB:
        return c->ddrb;
    case TALO:
        cia_sync(c, ami_now());
        return c->ta & 0xFF;
    case TAHI:
        cia_sync(c, ami_now());
        return c->ta >> 8;
    case TBLO:
        cia_sync(c, ami_now());
        return c->tb & 0xFF;
    case TBHI:
        cia_sync(c, ami_now());
        return c->tb >> 8;
    case TODLO: {
        uint32_t t = c->tod_latched ? c->tod_latch : c->tod;
        c->tod_latched = false;
        return t & 0xFF;
    }
    case TODMID:
        return ((c->tod_latched ? c->tod_latch : c->tod) >> 8) & 0xFF;
    case TODHI:
        if (!c->tod_latched) {
            c->tod_latched = true;
            c->tod_latch = c->tod;
        }
        return (c->tod_latch >> 16) & 0xFF;
    case SDR:
        return c->sdr;
    case ICR: {
        cia_sync(c, ami_now());
        uint8_t v = c->icr | (c->irq ? 0x80 : 0);
        c->icr = 0;
        c->irq = false;
        return v;
    }
    case CRA:
        /* (a one-shot's START clears as it underflows: polled, without
         * its interrupt there's no event to bring it up to date) */
        cia_sync(c, ami_now());
        return c->cra;
    case CRB:
        cia_sync(c, ami_now());
        return c->crb;
    default:
        return 0xFF;
    }
}

void cia_write(cia_t *c, int reg, uint8_t v)
{
    switch (reg) {
    case PRA:
    case DDRA:
        if (reg == PRA) {
            c->pra = v;
        } else {
            c->ddra = v;
        }
        if (c->index == 0) {
            bool ovl = (port_out(c->pra, c->ddra) & 1) != 0;
            if (ovl != ami.ovl) {
                ami.ovl = ovl;
                mem_update_map();
            }
        }
        break;
    case PRB:
    case DDRB: {
        uint8_t old = port_out(c->prb, c->ddrb);
        if (reg == PRB) {
            c->prb = v;
        } else {
            c->ddrb = v;
        }
        if (c->index == 1) {
            floppy_control(old, port_out(c->prb, c->ddrb));
        }
        break;
    }
    case TALO:
        cia_sync(c, ami_now());
        c->ta_latch = (c->ta_latch & 0xFF00) | v;
        break;
    case TAHI:
        cia_sync(c, ami_now());
        c->ta_latch = (c->ta_latch & 0x00FF) | (v << 8);
        /* a stopped timer loads; a one-shot one also starts */
        if (c->cra & CR_RUNMODE) {
            c->ta = c->ta_latch;
            c->cra |= CR_START;
        } else if (!(c->cra & CR_START)) {
            c->ta = c->ta_latch;
        }
        plan(c);
        ami_cpu_wake();
        break;
    case TBLO:
        cia_sync(c, ami_now());
        c->tb_latch = (c->tb_latch & 0xFF00) | v;
        break;
    case TBHI:
        cia_sync(c, ami_now());
        c->tb_latch = (c->tb_latch & 0x00FF) | (v << 8);
        if (c->crb & CR_RUNMODE) {
            c->tb = c->tb_latch;
            c->crb |= CR_START;
        } else if (!(c->crb & CR_START)) {
            c->tb = c->tb_latch;
        }
        plan(c);
        ami_cpu_wake();
        break;
    case TODLO:
    case TODMID:
    case TODHI: {
        int shift = (reg - TODLO) * 8;
        uint32_t *t = (c->crb & CRB_ALARM) ? &c->alarm : &c->tod;
        *t = (*t & ~(0xFFu << shift)) | ((uint32_t)v << shift);
        if (!(c->crb & CRB_ALARM)) {
            if (reg == TODHI) {
                c->tod_halted = true;
            } else if (reg == TODLO) {
                c->tod_halted = false;
            }
        }
        break;
    }
    case SDR:
        c->sdr = v;                 /* (shifted out to nobody) */
        break;
    case ICR:
        cia_sync(c, ami_now());
        if (v & 0x80) {
            c->icr_mask |= v & 0x1F;
        } else {
            c->icr_mask &= ~(v & 0x1F);
        }
        check_irq(c);
        plan(c);
        ami_cpu_wake();
        break;
    case CRA:
        cia_sync(c, ami_now());
        if (v & CR_LOAD) {
            c->ta = c->ta_latch;
        }
        if (c->index == 0 && (c->cra & CRA_SPMODE) && !(v & CRA_SPMODE)) {
            input_handshake();      /* KDAT was pulled low */
        }
        c->cra = v & ~CR_LOAD;
        plan(c);
        ami_cpu_wake();
        break;
    case CRB:
        cia_sync(c, ami_now());
        if (v & CR_LOAD) {
            c->tb = c->tb_latch;
        }
        c->crb = v & ~CR_LOAD;
        plan(c);
        ami_cpu_wake();
        break;
    default:
        break;
    }
}

void cia_reset(void)
{
    for (int i = 0; i < 2; i++) {
        cia_t *c = &cia[i];
        uint32_t tod = c->tod;
        memset(c, 0, sizeof(*c));
        c->index = i;
        c->tod = tod;
        c->ta = c->tb = c->ta_latch = c->tb_latch = 0xFFFF;
        c->last = ami_now();
        c->next = NEVER;
    }
}
