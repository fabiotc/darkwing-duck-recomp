/* CPU bus, interrupts and the dispatcher that runs recompiled code with the
 * interpreter as fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/recomp.h"

nes_cpu nes_cpu_state;
nes_stats nes_stats_state;
uint8_t nes_pad[2];
uint8_t nes_cheats[64];
uint32_t nes_marks;

int nes_marker_index(const char *id) {
    for (int i = 0; i < nes_marker_count; i++)
        if (!strcmp(nes_marker_defs[i].id, id)) return i;
    return -1;
}

static nes_options opts;
static struct {
    uint8_t shift[2];
    bool strobe;
} pads;

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
        if (pads.strobe) return 0x40 | (nes_pad[i] & 1);
        uint8_t r = pads.shift[i] & 1;
        pads.shift[i] = (uint8_t)((pads.shift[i] >> 1) | 0x80);
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
        pads.strobe = v & 1;
        if (pads.strobe) {
            pads.shift[0] = nes_pad[0];
            pads.shift[1] = nes_pad[1];
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
    if (c->stop) {
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

/* Apply an enabled cheat patch at c->pc, mirroring what the recompiled code
 * does. Returns -1 if none applies, else what nes_interp_step would return. */
static int interp_patch(nes_cpu *c) {
    for (int i = 0; i < nes_patch_count; i++) {
        const nes_patch_def *p = &nes_patch_defs[i];
        if (p->addr != c->pc || !nes_cheats[p->cheat] ||
            p->bank != nes_bus_state.prg_bank16[(c->pc >> 14) & 1])
            continue;
        if (p->kind == NES_PATCH_POKE) {
            if (p->unless >= 0 && RAM[p->ram[0]] == p->unless) return -1;
            uint8_t v = nes_cheat_defs[p->cheat].values[nes_cheats[p->cheat] - 1];
            for (int k = 0; k < p->ram_count; k++) RAM[p->ram[k]] = v;
            return -1; /* then run the instruction normally */
        }
        if (p->kind == NES_PATCH_SKIP) {
            CYC(p->cycles);
            c->pc = (uint16_t)(c->pc + p->size);
            return 0;
        }
        CYC(6); /* behave like RTS */
        c->pc = (uint16_t)(POP16() + 1);
        POLL();
        return 1;
    }
    return -1;
}

static int interp_step(nes_cpu *c) {
    for (int i = 0; i < nes_marker_count; i++)
        if (nes_marker_defs[i].addr == c->pc && nes_marker_defs[i].bank == nes_bus_state.prg_bank16[(c->pc >> 14) & 1])
            nes_marks |= 1u << i;
    if (nes_patch_count) {
        int r = interp_patch(c);
        if (r >= 0) return r;
    }
    return nes_interp_step(c);
}

/* Interpret one block, reporting every instruction that changes the watched byte. */
static void watch_block(nes_cpu *c) {
    int done;
    do {
        uint16_t pc = c->pc;
        int bank = nes_bus_state.prg_bank16[(pc >> 14) & 1];
        uint8_t before = RAM[opts.watch_addr & 0x7FF];
        done = interp_step(c);
        uint8_t after = RAM[opts.watch_addr & 0x7FF];
        if (before != after)
            fprintf(stderr, "watch $%04X: %02X -> %02X by %02X:%04X (frame %llu)\n", opts.watch_addr, before,
                    after, bank, pc, (unsigned long long)nes_ppu_frame_count());
    } while (!done && !c->stop);
}

static nes_safe_point_fn safe_fn;
static void *safe_user;

void nes_at_safe_point(nes_safe_point_fn fn, void *user) {
    safe_fn = fn;
    safe_user = user;
    nes_cpu_state.stop = true;
    nes_cpu_state.event_cycle = 0; /* poll (and so stop) at the next control transfer */
}

void nes_run(void) {
    nes_cpu *c = &nes_cpu_state;
    for (;;) {
        if (c->stop || c->quit) {
            c->stop = false;
            if (safe_fn) {
                nes_safe_point_fn fn = safe_fn;
                safe_fn = NULL;
                fn(safe_user);
            }
            if (c->quit) break;
            continue;
        }
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
        if (opts.watch_addr >= 0) {
            watch_block(c);
            continue;
        }
        do nes_stats_state.interp_insns++;
        while (!interp_step(c) && !c->stop);
    }
}

void nes_shutdown(void) { trace_flush(); }

void *nes_pads_state(size_t *size) {
    *size = sizeof pads;
    return &pads;
}
