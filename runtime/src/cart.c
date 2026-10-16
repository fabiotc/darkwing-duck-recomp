/* Cartridge loading and mappers (NROM, MMC1, UxROM, CNROM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/nesrt.h"

nes_cart nes_cart_state;
nes_bus nes_bus_state;

uint32_t nes_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

bool nes_cart_load(const char *path, char *err, size_t errlen) {
    nes_cart *cart = &nes_cart_state;
    free(cart->prg);
    free(cart->chr);
    cart->prg = cart->chr = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    uint8_t h[16];
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "NES\x1a", 4) != 0) {
        fclose(f);
        snprintf(err, errlen, "%s is not an iNES ROM", path);
        return false;
    }
    bool nes2 = (h[7] & 0x0C) == 0x08;
    size_t prg = (size_t)h[4] * 0x4000, chr = (size_t)h[5] * 0x2000;
    cart->mapper = (h[6] >> 4) | (h[7] & 0xF0);
    if (nes2) {
        prg |= (size_t)(h[9] & 0x0F) << 22;
        chr |= (size_t)(h[9] & 0xF0) << 17;
        cart->mapper |= (h[8] & 0x0F) << 8;
        cart->region = (h[12] & 3) == 1 ? REGION_PAL : REGION_NTSC;
    } else {
        cart->region = (h[9] & 1) ? REGION_PAL : REGION_NTSC;
    }
    cart->mirroring = (h[6] & 1) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
    if (h[6] & 0x04) fseek(f, 512, SEEK_CUR); /* trainer */

    cart->prg_size = prg;
    cart->prg = malloc(prg);
    cart->chr_is_ram = chr == 0;
    cart->chr_size = chr ? chr : 0x2000;
    cart->chr = calloc(1, cart->chr_size);
    bool ok = prg > 0 && fread(cart->prg, 1, prg, f) == prg &&
              (chr == 0 || fread(cart->chr, 1, chr, f) == chr);
    fclose(f);
    if (!ok) {
        snprintf(err, errlen, "%s is truncated", path);
        return false;
    }
    cart->prg_crc32 = nes_crc32(cart->prg, prg);
    return true;
}

/* ---- Banking helpers -------------------------------------------------- */

static void set_prg16(int window, int bank) {
    int count = (int)(nes_cart_state.prg_size / 0x4000);
    bank %= count;
    nes_bus_state.prg_bank16[window] = bank;
    nes_bus_state.prg[window * 2] = nes_cart_state.prg + bank * 0x4000;
    nes_bus_state.prg[window * 2 + 1] = nes_cart_state.prg + bank * 0x4000 + 0x2000;
}

static void set_chr4(int half, int bank) {
    int count = (int)(nes_cart_state.chr_size / 0x1000);
    bank %= count;
    for (int i = 0; i < 4; i++)
        nes_bus_state.chr[half * 4 + i] = nes_cart_state.chr + bank * 0x1000 + i * 0x400;
}

static void set_chr8(int bank) {
    set_chr4(0, bank * 2);
    set_chr4(1, bank * 2 + 1);
}

/* ---- MMC1 (mapper 1) -------------------------------------------------- */

static struct {
    uint8_t shift, count, control, chr0, chr1, prg;
    uint64_t last_write_cycle;
} mmc1;

static void mmc1_update(void) {
    static const nes_mirroring mir[4] = {MIRROR_SINGLE0, MIRROR_SINGLE1, MIRROR_VERTICAL,
                                         MIRROR_HORIZONTAL};
    nes_bus_state.mirroring = mir[mmc1.control & 3];
    int last = (int)(nes_cart_state.prg_size / 0x4000) - 1;
    int bank = mmc1.prg & 0x0F;
    switch ((mmc1.control >> 2) & 3) {
    case 0:
    case 1: set_prg16(0, bank & ~1); set_prg16(1, bank | 1); break;
    case 2: set_prg16(0, 0); set_prg16(1, bank); break;
    case 3: set_prg16(0, bank); set_prg16(1, last); break;
    }
    if (mmc1.control & 0x10) {
        set_chr4(0, mmc1.chr0);
        set_chr4(1, mmc1.chr1);
    } else {
        set_chr8(mmc1.chr0 >> 1);
    }
}

static void mmc1_write(uint16_t addr, uint8_t v) {
    /* MMC1 ignores a write on the cycle right after another one; this is what
     * makes the read-modify-write reset idiom (INC $FFFF) work. */
    uint64_t now = nes_cpu_state.cycles;
    bool ignore = now == mmc1.last_write_cycle;
    mmc1.last_write_cycle = now;
    if (ignore) return;
    if (v & 0x80) {
        mmc1.shift = mmc1.count = 0;
        mmc1.control |= 0x0C;
        mmc1_update();
        return;
    }
    mmc1.shift |= (uint8_t)((v & 1) << mmc1.count);
    if (++mmc1.count < 5) return;
    switch ((addr >> 13) & 3) {
    case 0: mmc1.control = mmc1.shift; break;
    case 1: mmc1.chr0 = mmc1.shift; break;
    case 2: mmc1.chr1 = mmc1.shift; break;
    case 3: mmc1.prg = mmc1.shift; break;
    }
    mmc1.shift = mmc1.count = 0;
    mmc1_update();
}

/* ---- Generic ---------------------------------------------------------- */

bool nes_mapper_init(char *err, size_t errlen) {
    int last = (int)(nes_cart_state.prg_size / 0x4000) - 1;
    nes_bus_state.mirroring = nes_cart_state.mirroring;
    set_prg16(0, 0);
    set_prg16(1, last);
    set_chr8(0);
    switch (nes_cart_state.mapper) {
    case 0: case 2: case 3:
        return true;
    case 1:
        memset(&mmc1, 0, sizeof mmc1);
        mmc1.control = 0x0C;
        mmc1.last_write_cycle = UINT64_MAX;
        mmc1_update();
        return true;
    default:
        snprintf(err, errlen, "mapper %d is not supported", nes_cart_state.mapper);
        return false;
    }
}

void nes_mapper_write(uint16_t addr, uint8_t v) {
    switch (nes_cart_state.mapper) {
    case 1: mmc1_write(addr, v); break;
    case 2: set_prg16(0, v); break;               /* UxROM */
    case 3: set_chr8(v & nes_bus_state.prg[(addr >> 13) & 3][addr & 0x1FFF]); break; /* CNROM, bus conflict */
    default: break;
    }
}

void *nes_mapper_state(size_t *size) {
    *size = sizeof mmc1;
    return &mmc1;
}
