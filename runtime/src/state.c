/* Save states: the whole machine (CPU, RAM, PPU, APU, mapper, controller
 * ports) written as a sequence of size-checked blocks. Must only be called at
 * a safe point (see nes_at_safe_point). */
#include <stdio.h>
#include <string.h>

#include "nesrt/nesrt.h"

void *nes_ppu_state(size_t *size);
void *nes_apu_state(size_t *size);
void *nes_mapper_state(size_t *size);
void *nes_pads_state(size_t *size);

#define STATE_MAGIC "NESRTSV1"

/* Bank pointers can't be saved directly; store offsets into PRG/CHR. */
typedef struct {
    uint8_t ram[0x800];
    uint8_t prg_ram[0x2000];
    uint32_t prg_off[4];
    int32_t prg_bank16[2];
    uint32_t chr_off[8];
    int32_t mirroring;
} bus_snapshot;

typedef struct {
    void *data;
    size_t size;
} block;

static int collect(block *b, bus_snapshot *bus) {
    int n = 0;
    b[n++] = (block){&nes_cpu_state, sizeof nes_cpu_state};
    b[n++] = (block){bus, sizeof *bus};
    b[n].data = nes_ppu_state(&b[n].size), n++;
    b[n].data = nes_apu_state(&b[n].size), n++;
    b[n].data = nes_mapper_state(&b[n].size), n++;
    b[n].data = nes_pads_state(&b[n].size), n++;
    b[n++] = (block){nes_pad, sizeof nes_pad}; /* buttons held when the state was saved */
    if (nes_cart_state.chr_is_ram) b[n++] = (block){nes_cart_state.chr, nes_cart_state.chr_size};
    return n;
}

bool nes_state_save(const char *path, char *err, size_t errlen) {
    nes_ppu_sync();
    nes_apu_sync();
    bus_snapshot bus;
    nes_bus *nb = &nes_bus_state;
    memcpy(bus.ram, nb->ram, sizeof bus.ram);
    memcpy(bus.prg_ram, nb->prg_ram, sizeof bus.prg_ram);
    for (int i = 0; i < 4; i++) bus.prg_off[i] = (uint32_t)(nb->prg[i] - nes_cart_state.prg);
    for (int i = 0; i < 8; i++) bus.chr_off[i] = (uint32_t)(nb->chr[i] - nes_cart_state.chr);
    bus.prg_bank16[0] = nb->prg_bank16[0];
    bus.prg_bank16[1] = nb->prg_bank16[1];
    bus.mirroring = nb->mirroring;

    block b[8];
    int n = collect(b, &bus);
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "can't write %s", path);
        return false;
    }
    bool ok = fwrite(STATE_MAGIC, 1, 8, f) == 8 &&
              fwrite(&nes_cart_state.prg_crc32, sizeof(uint32_t), 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        uint32_t size = (uint32_t)b[i].size;
        ok = fwrite(&size, sizeof size, 1, f) == 1 && fwrite(b[i].data, 1, b[i].size, f) == b[i].size;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errlen, "error writing %s", path);
    return ok;
}

bool nes_state_load(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "no save state in this slot");
        return false;
    }
    char magic[8];
    uint32_t crc;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, STATE_MAGIC, 8) != 0 ||
        fread(&crc, sizeof crc, 1, f) != 1 || crc != nes_cart_state.prg_crc32) {
        fclose(f);
        snprintf(err, errlen, "save state is for a different game or ROM");
        return false;
    }
    /* Read everything into scratch copies first so a bad file changes nothing. */
    bus_snapshot bus;
    block b[8];
    int n = collect(b, &bus);
    static uint8_t scratch[1 << 20];
    size_t used = 0;
    bool ok = true;
    for (int i = 0; ok && i < n; i++) {
        uint32_t size;
        ok = fread(&size, sizeof size, 1, f) == 1 && size == b[i].size && used + size <= sizeof scratch &&
             fread(scratch + used, 1, size, f) == size;
        used += ok ? size : 0;
    }
    fclose(f);
    if (!ok) {
        snprintf(err, errlen, "save state was made by a different version of the game");
        return false;
    }
    used = 0;
    for (int i = 0; i < n; i++) {
        memcpy(b[i].data, scratch + used, b[i].size);
        used += b[i].size;
    }

    nes_bus *nb = &nes_bus_state;
    memcpy(nb->ram, bus.ram, sizeof bus.ram);
    memcpy(nb->prg_ram, bus.prg_ram, sizeof bus.prg_ram);
    for (int i = 0; i < 4; i++) nb->prg[i] = nes_cart_state.prg + bus.prg_off[i];
    for (int i = 0; i < 8; i++) nb->chr[i] = nes_cart_state.chr + bus.chr_off[i];
    nb->prg_bank16[0] = bus.prg_bank16[0];
    nb->prg_bank16[1] = bus.prg_bank16[1];
    nb->mirroring = (nes_mirroring)bus.mirroring;

    nes_cpu_state.quit = false;
    nes_cpu_state.stop = false;
    nes_cpu_state.event_cycle = 0;
    return true;
}
