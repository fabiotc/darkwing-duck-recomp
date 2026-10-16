/* 2A03 APU: two pulse channels, triangle, noise, DMC and the frame counter.
 * Like the PPU it is caught up lazily to the CPU cycle counter. */
#include <string.h>

#include "nesrt/nesrt.h"

static const uint8_t LENGTH[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                   12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const uint8_t DUTY[4] = {0x40, 0x60, 0x78, 0x9F};
static const uint8_t TRIANGLE[32] = {15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
                                     0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15};
static const uint16_t NOISE_NTSC[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
static const uint16_t NOISE_PAL[16] = {4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778};
static const uint16_t DMC_NTSC[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
static const uint16_t DMC_PAL[16] = {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50};

typedef struct {
    bool start, loop, constant;
    uint8_t period, divider, decay, volume;
} envelope;

typedef struct {
    bool enabled;
    uint8_t length;
    envelope env;
    uint8_t duty, seq;
    uint16_t period, timer;
    bool sweep_enabled, sweep_negate, sweep_reload;
    uint8_t sweep_period, sweep_shift, sweep_divider;
    int ones_complement; /* pulse 1 negates with one's complement */
} pulse;

static struct {
    pulse p[2];
    struct {
        bool enabled, control, linear_reload;
        uint8_t length, linear, linear_period, seq;
        uint16_t period, timer;
    } tri;
    struct {
        bool enabled, mode;
        uint8_t length;
        envelope env;
        uint16_t period, timer, lfsr;
    } noise;
    struct {
        bool enabled, irq_enabled, loop, irq, silence;
        uint8_t level, buffer, shift, bits;
        bool buffer_full;
        uint16_t rate, timer, addr, cur_addr, len, remaining;
    } dmc;
    bool five_step, irq_inhibit, frame_irq;
    uint32_t frame_cycle;
    uint64_t cycle;
    bool pal;
    /* output */
    double sample_step, sample_acc, mix_sum, hp_prev_in, hp_prev_out;
    uint32_t mix_count;
    int16_t ring[16384];
    size_t head, tail;
} A;

void nes_apu_reset(int sample_rate) {
    memset(&A, 0, sizeof A);
    A.pal = nes_cart_state.region == REGION_PAL;
    A.p[0].ones_complement = 1;
    A.noise.lfsr = 1;
    A.noise.period = A.pal ? NOISE_PAL[0] : NOISE_NTSC[0];
    A.dmc.rate = A.pal ? DMC_PAL[0] : DMC_NTSC[0];
    A.sample_step = (double)sample_rate / nes_cpu_hz();
    A.cycle = nes_cpu_state.cycles;
}

/* ---- Units ------------------------------------------------------------ */

static void env_clock(envelope *e) {
    if (e->start) {
        e->start = false;
        e->decay = 15;
        e->divider = e->period;
    } else if (e->divider == 0) {
        e->divider = e->period;
        if (e->decay) e->decay--;
        else if (e->loop) e->decay = 15;
    } else {
        e->divider--;
    }
}

static uint8_t env_out(const envelope *e) { return e->constant ? e->volume : e->decay; }

static uint16_t sweep_target(const pulse *p) {
    uint16_t delta = p->period >> p->sweep_shift;
    if (!p->sweep_negate) return (uint16_t)(p->period + delta);
    return (uint16_t)(p->period - delta - p->ones_complement);
}

static bool pulse_muted(const pulse *p) {
    return p->period < 8 || (!p->sweep_negate && sweep_target(p) > 0x7FF);
}

static void quarter_frame(void) {
    env_clock(&A.p[0].env);
    env_clock(&A.p[1].env);
    env_clock(&A.noise.env);
    if (A.tri.linear_reload) A.tri.linear = A.tri.linear_period;
    else if (A.tri.linear) A.tri.linear--;
    if (!A.tri.control) A.tri.linear_reload = false;
}

static void half_frame(void) {
    for (int i = 0; i < 2; i++) {
        pulse *p = &A.p[i];
        if (!p->env.loop && p->length) p->length--;
        if (p->sweep_divider == 0 && p->sweep_enabled && p->sweep_shift && !pulse_muted(p))
            p->period = sweep_target(p);
        if (p->sweep_divider == 0 || p->sweep_reload) {
            p->sweep_divider = p->sweep_period;
            p->sweep_reload = false;
        } else {
            p->sweep_divider--;
        }
    }
    if (!A.tri.control && A.tri.length) A.tri.length--;
    if (!A.noise.env.loop && A.noise.length) A.noise.length--;
}

static void frame_counter_clock(void) {
    /* Step positions in CPU cycles. */
    static const uint32_t NTSC[5] = {7457, 14913, 22371, 29829, 37281};
    static const uint32_t PAL[5] = {8313, 16627, 24939, 33253, 41565};
    const uint32_t *s = A.pal ? PAL : NTSC;
    uint32_t c = ++A.frame_cycle;
    if (c == s[0] || c == s[2]) {
        quarter_frame();
    } else if (c == s[1]) {
        quarter_frame();
        half_frame();
    } else if (!A.five_step && c == s[3]) {
        quarter_frame();
        half_frame();
        if (!A.irq_inhibit) A.frame_irq = true;
        A.frame_cycle = 0;
    } else if (A.five_step && c == s[4]) {
        quarter_frame();
        half_frame();
        A.frame_cycle = 0;
    }
}

static void dmc_restart(void) {
    A.dmc.cur_addr = A.dmc.addr;
    A.dmc.remaining = A.dmc.len;
}

static void dmc_clock(void) {
    if (!A.dmc.buffer_full && A.dmc.remaining) {
        A.dmc.buffer = nes_read(A.dmc.cur_addr);
        A.dmc.buffer_full = true;
        A.dmc.cur_addr = A.dmc.cur_addr == 0xFFFF ? 0x8000 : (uint16_t)(A.dmc.cur_addr + 1);
        if (--A.dmc.remaining == 0) {
            if (A.dmc.loop) dmc_restart();
            else if (A.dmc.irq_enabled) A.dmc.irq = true;
        }
    }
    if (A.dmc.timer) {
        A.dmc.timer--;
        return;
    }
    A.dmc.timer = (uint16_t)(A.dmc.rate - 1);
    if (!A.dmc.silence) {
        if (A.dmc.shift & 1) { if (A.dmc.level <= 125) A.dmc.level += 2; }
        else if (A.dmc.level >= 2) A.dmc.level -= 2;
    }
    A.dmc.shift >>= 1;
    if (A.dmc.bits) A.dmc.bits--;
    if (A.dmc.bits == 0) {
        A.dmc.bits = 8;
        A.dmc.silence = !A.dmc.buffer_full;
        if (A.dmc.buffer_full) {
            A.dmc.shift = A.dmc.buffer;
            A.dmc.buffer_full = false;
        }
    }
}

static float mix(void) {
    int p = 0;
    for (int i = 0; i < 2; i++) {
        const pulse *q = &A.p[i];
        if (q->length && !pulse_muted(q) && ((DUTY[q->duty] << q->seq) & 0x80)) p += env_out(&q->env);
    }
    int t = TRIANGLE[A.tri.seq];
    int n = (A.noise.length && !(A.noise.lfsr & 1)) ? env_out(&A.noise.env) : 0;
    int d = A.dmc.level;
    float pulse_out = p ? 95.88f / (8128.0f / p + 100.0f) : 0.0f;
    float tnd_in = t / 8227.0f + n / 12241.0f + d / 22638.0f;
    float tnd_out = tnd_in > 0 ? 159.79f / (1.0f / tnd_in + 100.0f) : 0.0f;
    return pulse_out + tnd_out;
}

static void tick(void) {
    frame_counter_clock();

    /* Triangle runs at CPU rate; pulse and noise at APU rate (every other cycle). */
    if (A.tri.timer == 0) {
        A.tri.timer = A.tri.period;
        if (A.tri.length && A.tri.linear && A.tri.period > 1) A.tri.seq = (A.tri.seq + 1) & 31;
    } else {
        A.tri.timer--;
    }
    if (A.cycle & 1) {
        for (int i = 0; i < 2; i++) {
            pulse *q = &A.p[i];
            if (q->timer == 0) {
                q->timer = q->period;
                q->seq = (q->seq + 1) & 7;
            } else {
                q->timer--;
            }
        }
    }
    if (A.noise.timer == 0) {
        A.noise.timer = A.noise.period;
        int fb = (A.noise.lfsr & 1) ^ ((A.noise.lfsr >> (A.noise.mode ? 6 : 1)) & 1);
        A.noise.lfsr = (uint16_t)((A.noise.lfsr >> 1) | (fb << 14));
    } else {
        A.noise.timer--;
    }
    dmc_clock();

    A.mix_sum += mix();
    A.mix_count++;
    A.sample_acc += A.sample_step;
    if (A.sample_acc >= 1.0) {
        A.sample_acc -= 1.0;
        double in = A.mix_sum / A.mix_count;
        A.mix_sum = 0;
        A.mix_count = 0;
        /* ~37 Hz high-pass to remove the DC offset of the mixer. */
        double out = 0.995 * (A.hp_prev_out + in - A.hp_prev_in);
        A.hp_prev_in = in;
        A.hp_prev_out = out;
        double s = out * 40000.0;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        size_t next = (A.head + 1) % (sizeof A.ring / sizeof A.ring[0]);
        if (next != A.tail) {
            A.ring[A.head] = (int16_t)s;
            A.head = next;
        }
    }
    A.cycle++;
}

void nes_apu_sync(void) {
    /* The APU stands still during overclock lines so the music keeps its speed. */
    nes_ppu_sync();
    uint64_t target = nes_cpu_state.cycles - nes_ppu_paused_cycles();
    while (A.cycle < target) tick();
}

size_t nes_apu_take_samples(int16_t *out, size_t max) {
    size_t n = 0;
    const size_t cap = sizeof A.ring / sizeof A.ring[0];
    while (n < max && A.tail != A.head) {
        out[n++] = A.ring[A.tail];
        A.tail = (A.tail + 1) % cap;
    }
    return n;
}

bool nes_apu_irq(void) { return A.frame_irq || A.dmc.irq; }

uint8_t nes_apu_read_status(void) {
    uint8_t r = (uint8_t)((A.p[0].length ? 1 : 0) | (A.p[1].length ? 2 : 0) | (A.tri.length ? 4 : 0) |
                          (A.noise.length ? 8 : 0) | (A.dmc.remaining ? 0x10 : 0) |
                          (A.frame_irq ? 0x40 : 0) | (A.dmc.irq ? 0x80 : 0));
    A.frame_irq = false;
    return r;
}

static void env_write(envelope *e, uint8_t v) {
    e->loop = v & 0x20;
    e->constant = v & 0x10;
    e->period = e->volume = v & 0x0F;
}

void nes_apu_write(uint16_t reg, uint8_t v) {
    if (reg <= 0x4007) {
        pulse *q = &A.p[(reg >> 2) & 1];
        switch (reg & 3) {
        case 0: q->duty = v >> 6; env_write(&q->env, v); break;
        case 1:
            q->sweep_enabled = v & 0x80;
            q->sweep_period = (v >> 4) & 7;
            q->sweep_negate = v & 0x08;
            q->sweep_shift = v & 7;
            q->sweep_reload = true;
            break;
        case 2: q->period = (uint16_t)((q->period & 0x700) | v); break;
        case 3:
            q->period = (uint16_t)((q->period & 0xFF) | ((v & 7) << 8));
            if (q->enabled) q->length = LENGTH[v >> 3];
            q->seq = 0;
            q->env.start = true;
            break;
        }
        return;
    }
    switch (reg) {
    case 0x4008:
        A.tri.control = v & 0x80;
        A.tri.linear_period = v & 0x7F;
        break;
    case 0x400A: A.tri.period = (uint16_t)((A.tri.period & 0x700) | v); break;
    case 0x400B:
        A.tri.period = (uint16_t)((A.tri.period & 0xFF) | ((v & 7) << 8));
        if (A.tri.enabled) A.tri.length = LENGTH[v >> 3];
        A.tri.linear_reload = true;
        break;
    case 0x400C: env_write(&A.noise.env, v); break;
    case 0x400E:
        A.noise.mode = v & 0x80;
        A.noise.period = (A.pal ? NOISE_PAL : NOISE_NTSC)[v & 0x0F];
        break;
    case 0x400F:
        if (A.noise.enabled) A.noise.length = LENGTH[v >> 3];
        A.noise.env.start = true;
        break;
    case 0x4010:
        A.dmc.irq_enabled = v & 0x80;
        A.dmc.loop = v & 0x40;
        A.dmc.rate = (A.pal ? DMC_PAL : DMC_NTSC)[v & 0x0F];
        if (!A.dmc.irq_enabled) A.dmc.irq = false;
        break;
    case 0x4011: A.dmc.level = v & 0x7F; break;
    case 0x4012: A.dmc.addr = (uint16_t)(0xC000 | (v << 6)); break;
    case 0x4013: A.dmc.len = (uint16_t)((v << 4) | 1); break;
    case 0x4015:
        A.p[0].enabled = v & 1;
        A.p[1].enabled = v & 2;
        A.tri.enabled = v & 4;
        A.noise.enabled = v & 8;
        A.dmc.enabled = v & 0x10;
        if (!A.p[0].enabled) A.p[0].length = 0;
        if (!A.p[1].enabled) A.p[1].length = 0;
        if (!A.tri.enabled) A.tri.length = 0;
        if (!A.noise.enabled) A.noise.length = 0;
        if (!A.dmc.enabled) A.dmc.remaining = 0;
        else if (!A.dmc.remaining) dmc_restart();
        A.dmc.irq = false;
        break;
    case 0x4017:
        A.five_step = v & 0x80;
        A.irq_inhibit = v & 0x40;
        if (A.irq_inhibit) A.frame_irq = false;
        A.frame_cycle = 0;
        if (A.five_step) {
            quarter_frame();
            half_frame();
        }
        break;
    default: break;
    }
}

void *nes_apu_state(size_t *size) {
    *size = sizeof A;
    return &A;
}
