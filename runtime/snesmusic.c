/*
 * snesmusic.c - the remastered music (snesmusic.h).
 *
 * StarTropics' sound engine (in the fixed bank) plays six streams of notes: 0 pulse 1, 1 pulse 2,
 * 2 sound effects on pulse 2, 3 noise, 4 sound effects on noise, 5 triangle. A song takes streams
 * 0, 1, 3 and 5, with its data at $A000-$BFFF; a sound effect takes 2 or 4 (or another stream its
 * header names), with its data at $C000-$DFFF, and while one plays on 2 or 4 the engine keeps the
 * music of 1 or 3 off that channel. Every note and every rest of a stream passes through the
 * engine's routine at $FD1A, which writes $4015 with the stream in X: the stream's channel bit set
 * for a note, clear for a rest (it is the one writer that stores the same value in the engine's copy
 * of $4015, $7160, just before; the recompiled code keeps no program counter to tell it by). By the
 * end of the frame the note's period and volume are in the APU registers.
 *
 * The music is taken from there and played on the S-DSP with the instruments of the prototype that
 * was chosen: pulse 1 on a steel drum and pulse 2 on a marimba (two voices each, so a note rings on
 * while the next one sounds), the triangle on a plucked bass an octave lower, released when the
 * triangle's own note ends, and the noise on a shaker (its high settings) or a conga, with the
 * DSP's echo. Notes are keyed on a frame late: a voice about to be reused is released (8 ms) the
 * frame before, without a click. The channels that play music leave the NES's output
 * (apu_mute_mask); sound effects, and the DMC, stay on the NES.
 *
 * Run-ahead runs frames that are never heard: a frame's notes are kept only until the next frame
 * starts (sys.frames_run), and only snesmusic_frame, called after the frames that are heard, plays
 * them.
 */
#include "snesmusic.h"
#include "snesdsp.h"
#include "nes.h"
#include <math.h>
#include <string.h>

#define CPU_HZ 1789773.0
#define ENG_4015 0x7160             /* the engine's copy of $4015 */
#define ENG_STATE 0x7170            /* per stream: $FF when idle */
#define ENG_DATA_HI 0x717C          /* per stream: the high byte of its data's address */
#define MUS_GAIN 0.257              /* the DSP's mix to the loudness of the NES's music */
/* the frame after a pulse's rest the engine writes the silent channel's period again; the first
   prototype took that for a note, a soft repeat of the last one, and that is the sound chosen.
   0 plays only the notes the NES plays */
#ifndef REPEAT_AFTER_REST
#define REPEAT_AFTER_REST 1
#endif

void (*snesmusic_tap)(const int16_t *samples, int n);

/* the engine's routine at $FD11-$FD3C and its tables at $FDE3-$FDF4 (each stream's channel bit, the
   bits left for a rest, its register offset): only this engine's notes are taken */
static const uint8_t sig_code[] = {
    0xAC, 0x6A, 0x71, 0xBD, 0xE9, 0xFD, 0x2D, 0x60, 0x71, 0xE0, 0x01, 0xF0, 0x0C, 0x8D, 0x60, 0x71,
    0x8D, 0x15, 0x40, 0xA9, 0x00, 0x99, 0x61, 0x71, 0x60, 0xA0, 0xFF, 0xCC, 0x72, 0x71, 0xD0, 0x0B,
    0x8D, 0x60, 0x71, 0x8D, 0x15, 0x40, 0xA9, 0x00, 0x8D, 0x62, 0x71, 0x60 };
static const uint8_t sig_tables[] = {
    0x01, 0x02, 0x02, 0x08, 0x08, 0x04, 0x0E, 0x0D, 0x0D, 0x07, 0x07, 0x0B, 0x00, 0x04, 0x04, 0x0C, 0x0C, 0x08 };
static const uint8_t stream_bit[6] = { 0x01, 0x02, 0x02, 0x08, 0x08, 0x04 };

/* the instruments' parts: volume (of 127) and pan (-1 left .. 1 right) */
#define STEEL_VOL 88
#define STEEL_PAN -0.25
#define MARIMBA_VOL 72
#define MARIMBA_PAN 0.35
#define BASS_VOL 72
#define SHAKER_VOL 110
#define SHAKER_PAN -0.5
#define CONGA_VOL 56
#define CONGA_PAN 0.4
/* the voices: steel drum 0-1, marimba 2-3, bass 4, shaker 5, conga 6 */
#define V_BASS 4
#define V_SHAKER 5
#define V_CONGA 6

/* the DSP's setup: samples' directory, an 8 KiB echo buffer at $C000 (64 ms), a soft low-pass on
   the echo (FIR), feedback, levels; echo on the steel drum, the marimba and the conga */
static const uint8_t fir[8] = { 0x0C, 0x21, 0x2B, 0x2B, 0x13, 0xFE, 0xF3, 0xF9 };

enum { EV_NONE, EV_NOTE, EV_REST };

typedef struct {
    bool    on;
    int     voice;
    double  hz;
    int     vol;       /* the channel's volume register (pulses, noise) */
    int     idx;       /* noise period index */
    uint8_t lin;       /* the triangle's $4008 */
} note_t;

static bool enabled;
static uint32_t fcount;
static uint8_t sound_ram[0x10000];
static sdsp_t dsp;
static uint8_t regs[0x18];          /* the APU registers as last written */
static uint8_t ev[6];               /* this frame's events per stream */
static bool all_off;                /* this frame switched every channel off ($4015 = 0) */
static uint64_t ev_frame;           /* the frame (sys.frames_run) they belong to */
static uint64_t rest_frame[2];      /* the frame of the pulses' last rest */
static note_t pend[6];              /* last frame's notes, keyed on now */
static int alt[2];                  /* the voice pulse 1 / pulse 2 takes next (0 or 1 of its two) */
static int follow[6];               /* the voice playing a stream's held note (its pitch follows the channel), or -1 */
static uint16_t pitch_now[8];       /* each voice's pitch register */
static bool voice_set[8];           /* the voice's sample and envelope are written */
static uint32_t bass_off_at;
static uint64_t last_cycles;
static double dsp_acc;
static bool kof_all;

#define FIFO 4096                   /* stereo pairs from the DSP waiting to be mixed */
static int16_t fifo[FIFO][2];
static uint32_t f_w, f_r;
static double f_pos;
static bool f_primed;

static void w(uint8_t reg, uint8_t val) { sdsp_write(&dsp, reg, val); }

bool snesmusic_supported(void) {
    if (!cart.prg) return false;
    for (unsigned i = 0; i < sizeof sig_code; i++) if (nes_peek((uint16_t)(0xFD11 + i)) != sig_code[i]) return false;
    for (unsigned i = 0; i < sizeof sig_tables; i++) if (nes_peek((uint16_t)(0xFDE3 + i)) != sig_tables[i]) return false;
    return true;
}

bool snesmusic_enabled(void) { return enabled; }

/* stream s is playing a song (not idle, not a sound effect) */
static bool song_stream(int s) {
    return nes_peek((uint16_t)(ENG_STATE + s)) != 0xFF && (nes_peek((uint16_t)(ENG_DATA_HI + s)) & 0xE0) == 0xA0;
}

/* the channels the song has now: they leave the NES's output */
static void update_mask(void) {
    uint8_t m = 0;
    if (song_stream(0)) m |= 0x01;
    if (nes_peek(ENG_STATE + 2) == 0xFF && song_stream(1)) m |= 0x02;
    if (song_stream(5)) m |= 0x04;
    if (nes_peek(ENG_STATE + 4) == 0xFF && song_stream(3)) m |= 0x08;
    apu_mute_mask = m;
}

static void new_frame_events(void) {
    if (ev_frame != sys.frames_run) {
        memset(ev, 0, sizeof ev);
        all_off = false;
        ev_frame = sys.frames_run;
    }
}

static void on_apu_write(uint16_t a, uint8_t v) {
    if (a < 0x4000 || a > 0x4017) return;
    new_frame_events();
    regs[a - 0x4000] = v;
    if (a == 0x4015) {
        if (nes_peek(ENG_4015) == v && cpu.x < 6) {
            int s = cpu.x;
            if (s != 2 && s != 4 && song_stream(s)) ev[s] = (v & stream_bit[s]) ? EV_NOTE : EV_REST;
            if (s < 2 && ev[s] == EV_REST) rest_frame[s] = sys.frames_run;
        }
        if (!(v & 0x0F)) {             /* everything off: the engine stopping all its streams (a new song, a new game) */
            bool idle = true;
            for (int s = 0; s < 6; s++) idle = idle && nes_peek((uint16_t)(ENG_STATE + s)) == 0xFF;
            if (idle) all_off = true;
        }
    } else if (REPEAT_AFTER_REST && (a == 0x4003 || a == 0x4007)) {
        int s = a == 0x4003 ? 0 : 1;
        if (ev[s] == EV_NONE && rest_frame[s] + 1 == sys.frames_run && song_stream(s) && (s == 0 || nes_peek(ENG_STATE + 2) == 0xFF))
            ev[s] = EV_NOTE;
    }
    update_mask();
}

static void start_dsp(void) {
    memset(sound_ram, 0, sizeof sound_ram);
    memcpy(sound_ram + SNESMUSIC_BANK_ADDR, snesmusic_bank, snesmusic_bank_size);
    dsp.aram = sound_ram;
    sdsp_reset(&dsp);
    w(0x6C, 0x60);                     /* muted, no echo writes, while setting up */
    w(0x5D, SNESMUSIC_BANK_ADDR >> 8);
    w(0x6D, 0xC0);
    w(0x7D, 4);
    for (int i = 0; i < 8; i++) w((uint8_t)((i << 4) | 0x0F), fir[i]);
    w(0x0D, 0x48);                     /* echo feedback */
    w(0x2C, 0x26);
    w(0x3C, 0x26);                     /* echo volume */
    w(0x0C, 0x6C);
    w(0x1C, 0x6C);                     /* main volume */
    w(0x3D, 0);
    w(0x2D, 0);
    w(0x4D, 0x4F);                     /* echo: voices 0-3 and 6 */
    w(0x5C, 0);
    w(0x6C, 0x00);                     /* go */
    memset(voice_set, 0, sizeof voice_set);
    memset(pitch_now, 0, sizeof pitch_now);
}

void snesmusic_reset(void) {
    memset(pend, 0, sizeof pend);
    memset(ev, 0, sizeof ev);
    for (int s = 0; s < 6; s++) follow[s] = -1;
    all_off = false;
    bass_off_at = 0;
    kof_all = true;                    /* the voices fade out at the next frame */
    last_cycles = 0;
}

void snesmusic_enable(bool on) {
    on = on && snesmusic_supported();
    if (on == enabled) return;
    enabled = on;
    apu_write_hook = on ? on_apu_write : NULL;
    apu_mute_output = on;
    apu_mute_mask = 0;
    if (on) {
        start_dsp();
        snesmusic_reset();
        kof_all = false;
        memset(regs, 0, sizeof regs);
        f_w = f_r = 0;
        f_pos = 0;
        f_primed = false;
        dsp_acc = 0;
        fcount = 0;
        alt[0] = alt[1] = 0;
        update_mask();
    }
}

static int round_half_even(double x) { return (int)rint(x); }

static void vol_lr(double v, double pan, uint8_t *l, uint8_t *r) {
    *l = (uint8_t)(round_half_even(v * (1 - fmax(0.0, pan))) & 255);
    *r = (uint8_t)(round_half_even(v * (1 + fmin(0.0, pan))) & 255);
}

static uint16_t pitch_of(int inst, double hz) {
    double base = snesmusic_insts[inst].base_hz;
    if (base <= 0) return 0x1000;
    int p = round_half_even(hz / base * 4096);
    return (uint16_t)(p < 1 ? 1 : (p > 0x3FFF ? 0x3FFF : p));
}

static void set_pitch(int v, uint16_t p) {
    w((uint8_t)((v << 4) | 2), (uint8_t)(p & 255));
    w((uint8_t)((v << 4) | 3), (uint8_t)(p >> 8));
    pitch_now[v] = p;
}

static void key_on(int v, int inst, double hz, double vol, double pan) {
    const snesmusic_inst_t *in = &snesmusic_insts[inst];
    if (!voice_set[v]) {
        w((uint8_t)((v << 4) | 4), in->srcn);
        w((uint8_t)((v << 4) | 5), in->adsr1);
        w((uint8_t)((v << 4) | 6), in->adsr2);
        voice_set[v] = true;
    }
    set_pitch(v, pitch_of(inst, hz));
    uint8_t l, r;
    vol_lr(vol, pan, &l, &r);
    w((uint8_t)((v << 4) | 0), l);
    w((uint8_t)((v << 4) | 1), r);
}

static double pulse_hz(int c) {
    int per = regs[c * 4 + 2] | ((regs[c * 4 + 3] & 7) << 8);
    return CPU_HZ / (16.0 * (per + 1));
}

static double tri_hz(void) {
    int per = regs[0x0A] | ((regs[0x0B] & 7) << 8);
    return CPU_HZ / (32.0 * (per + 1));
}

/* the streams in the order their notes are written: pulse 1, pulse 2, triangle, noise */
static const int order[4] = { 0, 1, 5, 3 };

static void play_frame(void) {
    uint8_t kon = 0, kof = 0;
    fcount++;
    if (kof_all) { kof = 0xFF; kof_all = false; }
    if (all_off) {
        kof = 0xFF;
        memset(pend, 0, sizeof pend);
        for (int s = 0; s < 6; s++) follow[s] = -1;
        bass_off_at = 0;
    }

    /* this frame's notes: their voices are released now, for the notes to start next frame */
    note_t now[6];
    memset(now, 0, sizeof now);
    for (int k = 0; k < 4; k++) {
        int s = order[k];
        if (ev[s] == EV_REST) { follow[s] = -1; if (s == 5) kof |= 1 << V_BASS; }
        if (ev[s] != EV_NOTE) continue;
        note_t *n = &now[s];
        n->on = true;
        follow[s] = -1;
        if (s == 0 || s == 1) {
            n->voice = s * 2 + alt[s];
            alt[s] ^= 1;
            n->hz = pulse_hz(s);
            n->vol = regs[s * 4] & 15;
        } else if (s == 5) {
            n->voice = V_BASS;
            n->hz = tri_hz() / 2;          /* the bass an octave below the triangle */
            n->lin = regs[0x08];
        } else {
            n->idx = regs[0x0E] & 15;
            n->voice = n->idx <= 6 ? V_SHAKER : V_CONGA;
            n->vol = regs[0x0C] & 15;
        }
        kof |= (uint8_t)(1 << n->voice);
    }

    /* last frame's notes */
    for (int k = 0; k < 4; k++) {
        int s = order[k];
        note_t *n = &pend[s];
        if (!n->on) continue;
        if (s == 0 || s == 1) {
            int peak = n->vol > (regs[s * 4] & 15) ? n->vol : (regs[s * 4] & 15);
            double vel = pow(fmin(1.0, peak / (s == 0 ? 7.0 : 4.0)), 0.7);
            if (s == 0) key_on(n->voice, SM_STEEL, n->hz, STEEL_VOL * vel, STEEL_PAN);
            else key_on(n->voice, SM_MARIMBA, n->hz, MARIMBA_VOL * vel, MARIMBA_PAN);
        } else if (s == 5) {
            key_on(V_BASS, SM_BASS, n->hz, BASS_VOL, 0.0);
            /* released when the triangle's linear counter runs out (quarter frames), unless held */
            bass_off_at = (n->lin & 0x80) ? 0 : fcount + (uint32_t)((n->lin & 0x7F) <= 4 ? 1 : ((n->lin & 0x7F) + 3) / 4);
        } else {
            double vel = fmin(1.0, n->vol / 5.0);
            if (n->voice == V_SHAKER) key_on(V_SHAKER, SM_SHAKER, 0, SHAKER_VOL * vel, SHAKER_PAN);
            else key_on(V_CONGA, SM_CONGA, 0, CONGA_VOL * vel, CONGA_PAN);
        }
        kon |= (uint8_t)(1 << n->voice);
        if (s != 3 && ev[s] == EV_NONE) follow[s] = n->voice;
    }
    if (bass_off_at && fcount == bass_off_at) { kof |= 1 << V_BASS; bass_off_at = 0; }
    w(0x5C, (uint8_t)(kof & ~kon));
    if (kon) w(0x4C, kon);

    /* a held note's pitch follows its channel (vibrato, slides) while the song has the channel */
    for (int k = 0; k < 3; k++) {
        int s = order[k];
        int v = follow[s];
        if (v < 0 || ev[s] != EV_NONE || !(apu_mute_mask & (s == 5 ? 0x04 : 1 << s))) continue;
        uint16_t p = s == 5 ? pitch_of(SM_BASS, tri_hz() / 2) : pitch_of(s == 0 ? SM_STEEL : SM_MARIMBA, pulse_hz(s));
        if (p != pitch_now[v]) set_pitch(v, p);
    }
    memcpy(pend, now, sizeof pend);
}

void snesmusic_frame(void) {
    if (!enabled) return;
    new_frame_events();
    play_frame();

    /* the frame's samples: as many as the CPU cycles it took */
    uint64_t cyc = cpu.cycles;
    if (!last_cycles || cyc <= last_cycles || cyc - last_cycles > 4 * 29781) last_cycles = cyc - 29781;
    dsp_acc += (double)(cyc - last_cycles) * SDSP_RATE / CPU_HZ;
    last_cycles = cyc;
    int k = (int)dsp_acc;
    dsp_acc -= k;
    static int16_t buf[2 * 2048];
    if (k > 2048) k = 2048;
    sdsp_run(&dsp, buf, k);
    if (snesmusic_tap) snesmusic_tap(buf, k);
    if (f_w - f_r + (uint32_t)k > FIFO - 4) { f_r = f_w - 16; f_pos = 0; }     /* nobody mixed for a while */
    for (int i = 0; i < k; i++) {
        fifo[f_w & (FIFO - 1)][0] = buf[2 * i];
        fifo[f_w & (FIFO - 1)][1] = buf[2 * i + 1];
        f_w++;
    }
}

/* 4-point Hermite interpolation between y1 and y2 */
static inline double hermite(double y0, double y1, double y2, double y3, double t) {
    double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

static inline int16_t clamp16(double v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

void snesmusic_mix(const int16_t *nes, int16_t *out, uint32_t n, uint32_t rate) {
    if (!enabled) {
        for (uint32_t i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = nes[i];
        return;
    }
    double step = (double)SDSP_RATE / (rate ? rate : 48000);
    if (!f_primed && f_w - f_r >= 16) f_primed = true;    /* a few samples' margin for the interpolation */
    for (uint32_t i = 0; i < n; i++) {
        double l = 0, r = 0;
        if (f_primed && f_w - f_r >= 4) {
            const int16_t *a = fifo[(f_r - 1) & (FIFO - 1)], *b = fifo[f_r & (FIFO - 1)];
            const int16_t *c = fifo[(f_r + 1) & (FIFO - 1)], *d = fifo[(f_r + 2) & (FIFO - 1)];
            l = hermite(a[0], b[0], c[0], d[0], f_pos);
            r = hermite(a[1], b[1], c[1], d[1], f_pos);
            f_pos += step;
            while (f_pos >= 1.0) { f_pos -= 1.0; f_r++; }
        }
        out[2 * i] = clamp16(nes[i] + l * MUS_GAIN);
        out[2 * i + 1] = clamp16(nes[i] + r * MUS_GAIN);
    }
}
