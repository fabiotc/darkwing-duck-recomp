/* 2C02 PPU.
 *
 * Catch-up model: the PPU runs only when the CPU touches a PPU register or
 * reaches a poll point, and is then advanced dot by dot to the current CPU
 * cycle. Each visible scanline is rendered in one go at dot 1 from the
 * "loopy" v/t/x scroll registers, which handles mid-frame scroll splits
 * (status bars) and sprite-0-hit timing correctly for the vast majority of
 * games. */
#include <string.h>

#include "nesrt/nesrt.h"

static struct {
    uint8_t ctrl, mask, status, oam_addr, buf, x, open_bus;
    bool w;
    uint16_t v, t;
    uint8_t oam[256], pal[32], ciram[2048];
    int line, dot, lines_per_frame;
    int s0_dot; /* dot of this line on which sprite 0 hits, or -1 */
    uint64_t dots, frame;
    bool pal_timing;
    uint8_t fb[NES_WIDTH * NES_HEIGHT];
} P;

void nes_ppu_reset(void) {
    memset(&P, 0, sizeof P);
    P.pal_timing = nes_cart_state.region == REGION_PAL;
    P.lines_per_frame = P.pal_timing ? 312 : 262;
    P.s0_dot = -1;
}

/* CPU cycles <-> PPU dots: NTSC runs 3 dots per cycle, PAL 3.2. */
static uint64_t cycles_to_dots(uint64_t cyc) { return P.pal_timing ? cyc * 16 / 5 : cyc * 3; }
static uint64_t dots_to_cycles(uint64_t d) { return P.pal_timing ? (d * 5 + 15) / 16 : (d + 2) / 3; }

/* ---- VRAM ------------------------------------------------------------- */

static uint16_t nt_index(uint16_t a) {
    int table = (a >> 10) & 3, page = 0;
    switch (nes_bus_state.mirroring) {
    case MIRROR_VERTICAL: page = table & 1; break;
    case MIRROR_HORIZONTAL: page = table >> 1; break;
    case MIRROR_SINGLE0: page = 0; break;
    case MIRROR_SINGLE1: page = 1; break;
    }
    return (uint16_t)(page * 0x400 + (a & 0x3FF));
}

static uint8_t pal_index(uint16_t a) {
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= 0x0F;
    return (uint8_t)a;
}

static inline uint8_t chr_read(uint16_t a) { return nes_bus_state.chr[(a >> 10) & 7][a & 0x3FF]; }

static uint8_t vram_read(uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) return chr_read(a);
    if (a < 0x3F00) return P.ciram[nt_index(a)];
    return P.pal[pal_index(a)];
}

static void vram_write(uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000) {
        if (nes_cart_state.chr_is_ram) nes_bus_state.chr[(a >> 10) & 7][a & 0x3FF] = v;
    } else if (a < 0x3F00) {
        P.ciram[nt_index(a)] = v;
    } else {
        P.pal[pal_index(a)] = v & 0x3F;
    }
}

/* ---- Scroll register helpers (see nesdev wiki "PPU scrolling") -------- */

static void inc_y(void) {
    if ((P.v & 0x7000) != 0x7000) {
        P.v += 0x1000;
        return;
    }
    P.v &= ~0x7000;
    int y = (P.v & 0x03E0) >> 5;
    if (y == 29) { y = 0; P.v ^= 0x0800; }
    else if (y == 31) y = 0;
    else y++;
    P.v = (uint16_t)((P.v & ~0x03E0) | (y << 5));
}

static void copy_x(void) { P.v = (uint16_t)((P.v & ~0x041F) | (P.t & 0x041F)); }
static void copy_y(void) { P.v = (uint16_t)((P.v & ~0x7BE0) | (P.t & 0x7BE0)); }

/* ---- Rendering -------------------------------------------------------- */

static void render_line(void) {
    const int y = P.line;
    uint8_t *out = &P.fb[y * NES_WIDTH];
    uint8_t bg[NES_WIDTH] = {0};
    uint8_t spr[NES_WIDTH] = {0}; /* bit7: behind bg, bit6: sprite 0, low 5: colour */
    P.s0_dot = -1;

    if (!(P.mask & 0x18)) {
        memset(out, P.pal[0], NES_WIDTH);
        return;
    }

    if (P.mask & 0x08) {
        uint8_t line[33 * 8];
        uint16_t v = P.v;
        for (int tile = 0; tile < 33; tile++) {
            uint8_t idx = vram_read(0x2000 | (v & 0x0FFF));
            uint8_t attr = vram_read(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
            uint8_t pal = (attr >> (((v >> 4) & 4) | (v & 2))) & 3;
            uint16_t pat = (uint16_t)(((P.ctrl & 0x10) << 8) | (idx << 4) | ((v >> 12) & 7));
            uint8_t lo = chr_read(pat), hi = chr_read(pat + 8);
            for (int b = 0; b < 8; b++) {
                int px = ((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1);
                line[tile * 8 + b] = px ? (uint8_t)((pal << 2) | px) : 0;
            }
            if ((v & 0x1F) == 31) v = (uint16_t)((v & ~0x1F) ^ 0x0400);
            else v++;
        }
        memcpy(bg, line + P.x, NES_WIDTH);
        if (!(P.mask & 0x02)) memset(bg, 0, 8);
    }

    if (P.mask & 0x10) {
        int h = (P.ctrl & 0x20) ? 16 : 8, found = 0;
        for (int i = 0; i < 64; i++) {
            const uint8_t *s = &P.oam[i * 4];
            int row = y - 1 - s[0];
            if (row < 0 || row >= h) continue;
            if (++found > 8) {
                P.status |= 0x20;
                break;
            }
            if (s[2] & 0x80) row = h - 1 - row;
            uint16_t addr;
            if (h == 16) {
                int tile = s[1] & 0xFE;
                if (row >= 8) { tile++; row -= 8; }
                addr = (uint16_t)(((s[1] & 1) << 12) | (tile << 4) | row);
            } else {
                addr = (uint16_t)(((P.ctrl & 0x08) << 9) | (s[1] << 4) | row);
            }
            uint8_t lo = chr_read(addr), hi = chr_read(addr + 8);
            for (int b = 0; b < 8; b++) {
                int x = s[3] + b;
                if (x >= NES_WIDTH) break;
                int bit = (s[2] & 0x40) ? b : 7 - b;
                int px = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!px || (spr[x] & 0x1F)) continue;
                spr[x] = (uint8_t)(0x10 | ((s[2] & 3) << 2) | px | ((s[2] & 0x20) << 2) | (i == 0 ? 0x40 : 0));
            }
        }
        if (!(P.mask & 0x04)) memset(spr, 0, 8);
    }

    const uint8_t gray = (P.mask & 1) ? 0x30 : 0x3F;
    for (int x = 0; x < NES_WIDTH; x++) {
        uint8_t b = bg[x], s = spr[x], c = 0;
        bool bg_opaque = b & 3, spr_opaque = s & 3;
        if (spr_opaque && bg_opaque && (s & 0x40) && x != 255 && P.s0_dot < 0) P.s0_dot = x + 1;
        if (spr_opaque && (!(s & 0x80) || !bg_opaque)) c = s & 0x1F;
        else if (bg_opaque) c = b;
        out[x] = P.pal[pal_index(c)] & gray;
    }
}

static void step(void) {
    const bool rendering = P.mask & 0x18;
    const int prerender = P.lines_per_frame - 1;
    if (P.line < 240) {
        if (P.dot == 1) render_line();
        if (P.dot == P.s0_dot) P.status |= 0x40;
        if (rendering) {
            if (P.dot == 256) inc_y();
            else if (P.dot == 257) copy_x();
        }
    } else if (P.line == 241 && P.dot == 1) {
        P.status |= 0x80;
        P.frame++;
        if (P.ctrl & 0x80) {
            nes_cpu_state.nmi_pending = true;
            nes_cpu_state.event_cycle = 0;
        }
        nes_frame_callback();
        if (nes_cpu_state.quit) nes_cpu_state.event_cycle = 0;
    } else if (P.line == prerender) {
        if (P.dot == 1) {
            P.status &= 0x1F;
            P.s0_dot = -1;
        }
        if (rendering) {
            if (P.dot == 257) copy_x();
            else if (P.dot >= 280 && P.dot <= 304) copy_y();
        }
    }
    P.dots++;
    if (++P.dot > 340) {
        P.dot = 0;
        if (++P.line >= P.lines_per_frame) P.line = 0;
    }
}

void nes_ppu_sync(void) {
    uint64_t target = cycles_to_dots(nes_cpu_state.cycles);
    while (P.dots < target) step();
}

uint64_t nes_ppu_next_event(void) {
    /* Poll once per scanline, just after dot 1 (where vblank/NMI begins). */
    uint64_t next = P.dots + (uint64_t)(341 - P.dot) + 2;
    return dots_to_cycles(next);
}

const uint8_t *nes_ppu_framebuffer(void) { return P.fb; }
uint64_t nes_ppu_frame_count(void) { return P.frame; }

/* ---- Registers -------------------------------------------------------- */

uint8_t nes_ppu_read(uint16_t reg) {
    uint8_t r = P.open_bus;
    switch (reg & 7) {
    case 2:
        r = (uint8_t)((P.status & 0xE0) | (P.open_bus & 0x1F));
        P.status &= 0x7F;
        P.w = false;
        break;
    case 4:
        r = P.oam[P.oam_addr];
        break;
    case 7: {
        uint16_t a = P.v & 0x3FFF;
        if (a < 0x3F00) {
            r = P.buf;
            P.buf = vram_read(a);
        } else {
            r = vram_read(a);
            P.buf = vram_read(a - 0x1000);
        }
        P.v = (uint16_t)(P.v + ((P.ctrl & 4) ? 32 : 1));
        break;
    }
    default: break;
    }
    P.open_bus = r;
    return r;
}

void nes_ppu_write(uint16_t reg, uint8_t v) {
    P.open_bus = v;
    switch (reg & 7) {
    case 0: {
        bool was = P.ctrl & 0x80;
        P.ctrl = v;
        P.t = (uint16_t)((P.t & 0xF3FF) | ((v & 3) << 10));
        if (!was && (v & 0x80) && (P.status & 0x80)) {
            nes_cpu_state.nmi_pending = true;
            nes_cpu_state.event_cycle = 0;
        }
        break;
    }
    case 1: P.mask = v; break;
    case 3: P.oam_addr = v; break;
    case 4: P.oam[P.oam_addr++] = v; break;
    case 5:
        if (!P.w) {
            P.t = (uint16_t)((P.t & ~0x001F) | (v >> 3));
            P.x = v & 7;
        } else {
            P.t = (uint16_t)((P.t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2));
        }
        P.w = !P.w;
        break;
    case 6:
        if (!P.w) {
            P.t = (uint16_t)((P.t & 0x00FF) | ((v & 0x3F) << 8));
        } else {
            P.t = (uint16_t)((P.t & 0xFF00) | v);
            P.v = P.t;
        }
        P.w = !P.w;
        break;
    case 7:
        vram_write(P.v, v);
        P.v = (uint16_t)(P.v + ((P.ctrl & 4) ? 32 : 1));
        break;
    }
}

void nes_ppu_oam_dma(const uint8_t *page) {
    for (int i = 0; i < 256; i++) P.oam[(uint8_t)(P.oam_addr + i)] = page[i];
}
