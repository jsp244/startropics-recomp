/*
 * snesmusic.h - the remastered music: the game's own notes, as its sound engine starts them, played
 * on the SNES's sound chip (snesdsp.c) with instruments made for it, while the sound effects stay
 * on the NES's. See snesmusic.c.
 */
#ifndef SNESMUSIC_H
#define SNESMUSIC_H

#include <stdbool.h>
#include <stdint.h>

/* the instruments (snesmusic_bank.c, made by tools/snesmusic/instruments.py) */
#define SNESMUSIC_BANK_ADDR 0x0300           /* where the bank goes in the DSP's RAM */
typedef struct {
    uint8_t srcn, adsr1, adsr2;
    double  base_hz;                         /* the pitch the sample plays at rate $1000; 0: a drum, played at $1000 */
} snesmusic_inst_t;
enum { SM_STEEL, SM_MARIMBA, SM_BASS, SM_SHAKER, SM_CONGA, SNESMUSIC_INSTS };
extern const uint8_t snesmusic_bank[];
extern const unsigned snesmusic_bank_size;
extern const snesmusic_inst_t snesmusic_insts[SNESMUSIC_INSTS];

/* the loaded cartridge has the sound engine the music knows (StarTropics) */
bool snesmusic_supported(void);
/* on: the engine's notes go to the DSP and the NES's music channels leave the APU's output */
void snesmusic_enable(bool on);
bool snesmusic_enabled(void);
/* forget the notes in flight, after a state load, a rewind step or a pause: the voices fade out
   and the music goes on from the next notes */
void snesmusic_reset(void);
/* after each frame whose sound is heard: its notes go to the DSP, which makes the frame's samples */
void snesmusic_frame(void);
/* n samples of the NES's (mono) and the music's, resampled to rate, mixed into n stereo pairs */
void snesmusic_mix(const int16_t *nes, int16_t *out, uint32_t n, uint32_t rate);
/* testing: sees the DSP's own output (stereo pairs, 32,000 a second) as it is made */
extern void (*snesmusic_tap)(const int16_t *samples, int n);

#endif
