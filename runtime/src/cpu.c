/* CPU bus, interrupts and the dispatcher that runs recompiled code with the
 * interpreter as fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/recomp.h"

nes_cpu nes_cpu_state;
nes_stats nes_stats_state;
uint8_t nes_pad[2];

static nes_options opts;
static uint8_t pad_shift[2];
static bool pad_strobe;

/* ---- Trace: set of (bank, addr) block starts run by the interpreter ---- */

#define TRACE_SIZE (1u << 20)
static uint32_t *trace_set; /* open addressing, 0 = empty, key = bank<<16|addr|1<<31 */

static void trace_add(uint32_t key) {
    key |= 0x80000000u;
    uint32_t h = (key * 2654435761u) & (TRACE_SIZE - 1);
    while (trace_set[h]) {
        if (trace_set[h] == key) return;
        h = (h + 1) & (TRACE_SIZE - 1);
    }
    trace_set[h] = key;
    nes_stats_state.unique_misses++;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void trace_flush(void) {
    if (!trace_set || !opts.trace_path) return;
    /* Merge with what is already in the file. */
    FILE *f = fopen(opts.trace_path, "r");
    if (f) {
        char line[64];
        unsigned bank, addr;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%x:%x", &bank, &addr) == 2) trace_add(bank << 16 | addr);
        fclose(f);
    }
    uint32_t *keys = malloc(sizeof(uint32_t) * TRACE_SIZE);
    size_t n = 0;
    for (size_t i = 0; i < TRACE_SIZE; i++)
        if (trace_set[i]) keys[n++] = trace_set[i] & 0x7FFFFFFFu;
    qsort(keys, n, sizeof *keys, cmp_u32);
    f = fopen(opts.trace_path, "w");
    if (f) {
        fprintf(f, "# Block entry points executed by the interpreter (bank:address).\n");
        for (size_t i = 0; i < n; i++) fprintf(f, "%02X:%04X\n", keys[i] >> 16, keys[i] & 0xFFFF);
        fclose(f);
    }
    free(keys);
}

/* ---- Bus -------------------------------------------------------------- */

uint8_t nes_io_read(uint16_t a) {
    if (a < 0x4000) {
        nes_ppu_sync();
        return nes_ppu_read(a);
    }
    if (a == 0x4015) {
        nes_apu_sync();
        return nes_apu_read_status();
    }
    if (a == 0x4016 || a == 0x4017) {
        int i = a & 1;
        if (pad_strobe) return 0x40 | (nes_pad[i] & 1);
        uint8_t r = pad_shift[i] & 1;
        pad_shift[i] = (uint8_t)((pad_shift[i] >> 1) | 0x80);
        return 0x40 | r;
    }
    if (a >= 0x6000 && a < 0x8000) return nes_bus_state.prg_ram[a & 0x1FFF];
    return (uint8_t)(a >> 8); /* open bus */
}

void nes_io_write(uint16_t a, uint8_t v) {
    nes_cpu *c = &nes_cpu_state;
    if (a < 0x4000) {
        nes_ppu_sync();
        nes_ppu_write(a, v);
    } else if (a == 0x4014) {
        uint8_t page[256];
        for (int i = 0; i < 256; i++) page[i] = nes_read((uint16_t)(v << 8 | i));
        nes_ppu_sync();
        nes_ppu_oam_dma(page);
        c->cycles += 513 + (c->cycles & 1);
    } else if (a == 0x4016) {
        pad_strobe = v & 1;
        if (pad_strobe) {
            pad_shift[0] = nes_pad[0];
            pad_shift[1] = nes_pad[1];
        }
    } else if (a <= 0x4017) {
        nes_apu_sync();
        nes_apu_write(a, v);
        c->event_cycle = 0; /* IRQ state may have changed */
    } else if (a >= 0x6000 && a < 0x8000) {
        nes_bus_state.prg_ram[a & 0x1FFF] = v;
    } else if (a >= 0x8000) {
        nes_mapper_write(a, v);
    }
}

/* ---- Interrupts ------------------------------------------------------- */

static void interrupt(nes_cpu *c, uint16_t pc, uint16_t vector) {
    PUSH16(pc);
    PUSH(nes_get_p(c) | 0x20);
    c->fi = 1;
    c->pc = RD16(vector);
    c->cycles += 7;
}

int nes_poll(nes_cpu *c, uint16_t pc) {
    nes_ppu_sync();
    nes_apu_sync();
    c->event_cycle = nes_ppu_next_event();
    if (c->quit) {
        c->pc = pc;
        return 1;
    }
    if (c->nmi_pending) {
        c->nmi_pending = false;
        interrupt(c, pc, 0xFFFA);
        return 1;
    }
    if (nes_apu_irq()) {
        c->event_cycle = 0; /* level triggered: keep polling while asserted */
        if (!c->fi) {
            interrupt(c, pc, 0xFFFE);
            return 1;
        }
    }
    return 0;
}

int nes_illegal_opcode(nes_cpu *c) {
    static int reported;
    if (reported++ < 16)
        fprintf(stderr, "nesrt: illegal opcode $%02X at $%04X (bank %d), treated as NOP\n",
                RD(c->pc), c->pc, nes_bus_state.prg_bank16[(c->pc >> 14) & 1]);
    CYC(2);
    c->pc++;
    POLL();
    return 1;
}

/* ---- Execution -------------------------------------------------------- */

double nes_cpu_hz(void) {
    return nes_cart_state.region == REGION_PAL ? 1662607.0 : 1789773.0;
}

double nes_frame_hz(void) {
    return nes_cart_state.region == REGION_PAL ? 50.007 : 60.0988;
}

void nes_power_on(const nes_options *opt) {
    opts = *opt;
    nes_cart_state.region = opt->region;
    nes_cpu *c = &nes_cpu_state;
    memset(c, 0, sizeof *c);
    memset(&nes_bus_state.ram, 0, sizeof nes_bus_state.ram);
    c->s = 0xFD;
    c->fi = 1;
    c->cycles = 7;
    nes_ppu_reset();
    nes_apu_reset(48000);
    c->pc = RD16(0xFFFC);
    if (opts.trace_path) trace_set = calloc(TRACE_SIZE, sizeof(uint32_t));
}

void nes_run(void) {
    nes_cpu *c = &nes_cpu_state;
    while (!c->quit) {
        if (!opts.interpret_only && c->pc >= 0x8000) {
            int w = (c->pc >> 14) & 1, bank = nes_bus_state.prg_bank16[w];
            nes_recomp_fn f = bank < nes_recomp_bank_count ? nes_recomp_table[bank][w] : NULL;
            if (f) {
                nes_stats_state.recomp_entries++;
                if (f(c) == NES_EXIT) continue;
            }
        }
        /* Not recompiled: interpret up to the next control transfer. */
        if (trace_set && c->pc >= 0x8000)
            trace_add((uint32_t)nes_bus_state.prg_bank16[(c->pc >> 14) & 1] << 16 | c->pc);
        nes_stats_state.interp_blocks++;
        do nes_stats_state.interp_insns++;
        while (!nes_interp_step(c) && !c->quit);
    }
}

void nes_shutdown(void) { trace_flush(); }
