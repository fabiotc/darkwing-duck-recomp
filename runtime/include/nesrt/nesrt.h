/* nesrt - runtime for statically recompiled NES games.
 *
 * The recompiled code only implements the CPU program. Everything else the
 * game talks to (PPU, APU, controllers, mapper) lives here, together with the
 * dispatcher that connects recompiled windows and the fallback interpreter.
 */
#ifndef NESRT_H
#define NESRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CPU ------------------------------------------------------------- */

typedef struct nes_cpu {
    uint8_t a, x, y, s;
    uint8_t fc, fz, fi, fd, fv, fn; /* status flags, each 0 or 1 */
    uint16_t pc;
    uint64_t cycles;      /* CPU cycles since power-on */
    uint64_t event_cycle; /* call nes_poll() once cycles reaches this */
    bool nmi_pending;
    bool quit;
} nes_cpu;

extern nes_cpu nes_cpu_state;

/* ---- Bus / cartridge ------------------------------------------------- */

typedef enum { MIRROR_HORIZONTAL, MIRROR_VERTICAL, MIRROR_SINGLE0, MIRROR_SINGLE1 } nes_mirroring;
typedef enum { REGION_NTSC, REGION_PAL } nes_region;

typedef struct nes_bus {
    uint8_t ram[0x800];
    uint8_t prg_ram[0x2000];
    const uint8_t *prg[4];  /* 8 KiB slots at $8000/$A000/$C000/$E000 */
    int prg_bank16[2];      /* 16 KiB PRG bank visible at $8000 and $C000 */
    uint8_t *chr[8];        /* 1 KiB pattern slots at PPU $0000-$1FFF */
    nes_mirroring mirroring;
} nes_bus;

extern nes_bus nes_bus_state;

typedef struct nes_cart {
    uint8_t *prg;
    size_t prg_size;
    uint8_t *chr;
    size_t chr_size;
    bool chr_is_ram;
    int mapper;
    nes_region region;
    nes_mirroring mirroring;
    uint32_t prg_crc32;
} nes_cart;

extern nes_cart nes_cart_state;

/* Load an iNES / NES 2.0 image. Returns false and fills err on failure. */
bool nes_cart_load(const char *path, char *err, size_t errlen);
bool nes_mapper_init(char *err, size_t errlen);
void nes_mapper_write(uint16_t addr, uint8_t value);

uint8_t nes_io_read(uint16_t addr);
void nes_io_write(uint16_t addr, uint8_t value);

static inline uint8_t nes_read(uint16_t a) {
    if (a < 0x2000) return nes_bus_state.ram[a & 0x7FF];
    if (a >= 0x8000) return nes_bus_state.prg[(a >> 13) & 3][a & 0x1FFF];
    return nes_io_read(a);
}

static inline void nes_write(uint16_t a, uint8_t v) {
    if (a < 0x2000) nes_bus_state.ram[a & 0x7FF] = v;
    else nes_io_write(a, v);
}

/* ---- PPU / APU / input ----------------------------------------------- */

#define NES_WIDTH 256
#define NES_HEIGHT 240

void nes_ppu_reset(void);
void nes_ppu_sync(void);                 /* catch the PPU up to the CPU */
uint8_t nes_ppu_read(uint16_t reg);
void nes_ppu_write(uint16_t reg, uint8_t v);
void nes_ppu_oam_dma(const uint8_t *page);
uint64_t nes_ppu_next_event(void);       /* CPU cycle of the next PPU event */
const uint8_t *nes_ppu_framebuffer(void); /* 256x240 NES palette indices */
uint64_t nes_ppu_frame_count(void);

void nes_apu_reset(int sample_rate);
void nes_apu_sync(void);
uint8_t nes_apu_read_status(void);
void nes_apu_write(uint16_t reg, uint8_t v);
bool nes_apu_irq(void);
/* Drain generated audio (mono int16). Returns the number of samples copied. */
size_t nes_apu_take_samples(int16_t *out, size_t max);

/* Controller state, bit order: A B Select Start Up Down Left Right. */
enum { BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
       BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80 };
extern uint8_t nes_pad[2];

/* ---- Execution ------------------------------------------------------- */

typedef struct nes_options {
    bool interpret_only;   /* ignore recompiled code (reference mode) */
    const char *trace_path; /* record block starts the interpreter executes */
    nes_region region;
} nes_options;

/* Called by the PPU once per frame at the start of vblank. The frontend
 * presents the picture, polls input and paces emulation here. Set
 * nes_cpu_state.quit to stop. */
extern void nes_frame_callback(void);

void nes_power_on(const nes_options *opt);
void nes_run(void); /* runs until nes_cpu_state.quit */
double nes_cpu_hz(void);
double nes_frame_hz(void);

/* Execution statistics, for coverage reports. */
typedef struct nes_stats {
    uint64_t recomp_entries;  /* times recompiled code was entered */
    uint64_t interp_blocks;   /* blocks run by the fallback interpreter */
    uint64_t interp_insns;
    uint64_t unique_misses;   /* distinct addresses the interpreter ran */
} nes_stats;
extern nes_stats nes_stats_state;
void nes_shutdown(void); /* flushes the trace file */

#ifdef __cplusplus
}
#endif
#endif
