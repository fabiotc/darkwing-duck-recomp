/* Helpers used by generated code (recompiled windows and the interpreter).
 * Keep these semantics in sync with tools/nesrecomp/semantics.py. */
#ifndef NESRT_RECOMP_H
#define NESRT_RECOMP_H

#include "nesrt.h"

/* Return values of a recompiled window function. */
#define NES_EXIT 0 /* c->pc left this window (or the bank changed) */
#define NES_MISS 1 /* c->pc is in this window but was not recompiled */

typedef int (*nes_recomp_fn)(nes_cpu *c);

/* Provided by the generated recomp_table.c. */
extern const int nes_recomp_bank_count;
extern const uint32_t nes_recomp_prg_crc32;
extern const nes_recomp_fn nes_recomp_table[][2];

/* Cheats: runtime-toggleable patches declared in the game config. */
typedef struct {
    const char *id, *name, *description;
    int choice_count;            /* 0 = simple on/off cheat */
    const char *const *choices;  /* menu entries */
    const uint8_t *values;       /* value poked for each choice */
} nes_cheat_def;
enum { NES_PATCH_RETURN = 1, NES_PATCH_SKIP = 2, NES_PATCH_POKE = 3 };
typedef struct {
    uint8_t bank;
    uint16_t addr;
    uint8_t cheat, kind, cycles, size;
    uint8_t ram_count;
    uint16_t ram[4]; /* NES_PATCH_POKE targets */
    int16_t unless;  /* poke only if RAM[ram[0]] != unless (-1 = always) */
} nes_patch_def;
extern const int nes_cheat_count;
extern const nes_cheat_def nes_cheat_defs[];
extern const int nes_patch_count;
extern const nes_patch_def nes_patch_defs[];
/* Markers: code locations that tell the frontend what the game is doing. */
typedef struct {
    const char *id;
    uint8_t bank;
    uint16_t addr;
} nes_marker_def;
extern const int nes_marker_count;
extern const nes_marker_def nes_marker_defs[];
extern uint32_t nes_marks; /* bit i set when marker i ran; cleared by the frontend */
int nes_marker_index(const char *id); /* -1 if unknown */

extern uint8_t nes_cheats[]; /* 0 = off, else 1 or (choice index + 1); indexed like nes_cheat_defs */

/* Runtime entry points used by generated code. */
int nes_poll(nes_cpu *c, uint16_t pc); /* 1 if control was redirected */
int nes_interp_step(nes_cpu *c);
int nes_illegal_opcode(nes_cpu *c);

#define RAM (nes_bus_state.ram)
#define RD(a) nes_read((uint16_t)(a))
#define WR(a, v) nes_write((uint16_t)(a), (uint8_t)(v))
#define PRG(a) (nes_bus_state.prg[((a) >> 13) & 3][(a) & 0x1FFF])
#define RD16(a) ((uint16_t)(RD(a) | (RD((uint16_t)((a) + 1)) << 8)))
#define ZP16(z) ((uint16_t)(RAM[(uint8_t)(z)] | (RAM[(uint8_t)((z) + 1)] << 8)))
#define CYC(n) (c->cycles += (uint64_t)(n))
#define NZ(v) do { uint8_t nz_ = (uint8_t)(v); c->fz = nz_ == 0; c->fn = nz_ >> 7; } while (0)

#define PUSH(v) (RAM[0x100 | c->s--] = (uint8_t)(v))
#define POP() (RAM[0x100 | ++c->s])
#define PUSH16(v) do { uint16_t p16_ = (uint16_t)(v); PUSH(p16_ >> 8); PUSH(p16_ & 0xFF); } while (0)
#define POP16() (nes_pop16(c))

#define IN_WINDOW(pc) ((((pc) >> 14) & 3) == 2 + W)
#define BANK_CHANGED (nes_bus_state.prg_bank16[W] != BANK)

/* Interrupt poll at a control transfer to a statically known label. */
#define POLL_GOTO(t, label) do { \
        if (c->cycles >= c->event_cycle && nes_poll(c, (t))) goto dispatch; \
        goto label; \
    } while (0)
/* Control transfer to an address only known at runtime (or outside the window). */
#define POLL_JUMP(t) do { \
        c->pc = (uint16_t)(t); \
        if (c->cycles >= c->event_cycle) nes_poll(c, c->pc); \
        goto dispatch; \
    } while (0)
/* Interpreter poll point (c->pc already updated). */
#define POLL() do { if (c->cycles >= c->event_cycle) nes_poll(c, c->pc); } while (0)

static inline uint16_t nes_pop16(nes_cpu *c) {
    uint8_t lo = POP();
    uint8_t hi = POP();
    return (uint16_t)(lo | (hi << 8));
}

static inline uint8_t nes_get_p(const nes_cpu *c) {
    return (uint8_t)(c->fc | (c->fz << 1) | (c->fi << 2) | (c->fd << 3) |
                     (c->fv << 6) | (c->fn << 7));
}

static inline void nes_set_p(nes_cpu *c, uint8_t p) {
    c->fc = p & 1; c->fz = (p >> 1) & 1; c->fi = (p >> 2) & 1;
    c->fd = (p >> 3) & 1; c->fv = (p >> 6) & 1; c->fn = p >> 7;
}

/* The 2A03 has no decimal mode, so ADC/SBC are always binary. */
static inline void op_adc(nes_cpu *c, uint8_t v) {
    unsigned t = (unsigned)c->a + v + c->fc;
    c->fv = (~(c->a ^ v) & (c->a ^ t) & 0x80) != 0;
    c->fc = t > 0xFF;
    c->a = (uint8_t)t;
    NZ(c->a);
}

static inline void op_cmp(nes_cpu *c, uint8_t r, uint8_t v) {
    c->fc = r >= v;
    NZ(r - v);
}

static inline void op_bit(nes_cpu *c, uint8_t v) {
    c->fz = (c->a & v) == 0;
    c->fv = (v >> 6) & 1;
    c->fn = v >> 7;
}

static inline uint8_t op_asl(nes_cpu *c, uint8_t v) { c->fc = v >> 7; v <<= 1; NZ(v); return v; }
static inline uint8_t op_lsr(nes_cpu *c, uint8_t v) { c->fc = v & 1; v >>= 1; NZ(v); return v; }
static inline uint8_t op_rol(nes_cpu *c, uint8_t v) {
    uint8_t r = (uint8_t)((v << 1) | c->fc); c->fc = v >> 7; NZ(r); return r;
}
static inline uint8_t op_ror(nes_cpu *c, uint8_t v) {
    uint8_t r = (uint8_t)((v >> 1) | (c->fc << 7)); c->fc = v & 1; NZ(r); return r;
}
static inline uint8_t op_inc(nes_cpu *c, uint8_t v) { v++; NZ(v); return v; }
static inline uint8_t op_dec(nes_cpu *c, uint8_t v) { v--; NZ(v); return v; }

#endif
