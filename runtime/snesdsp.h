/*
 * snesdsp.h - the SNES's sound chip, the S-DSP: eight voices of BRR samples from its 64 KiB of RAM,
 * each with an ADSR or GAIN envelope, Gaussian interpolation, noise, pitch modulation and an echo
 * with an 8-tap FIR filter, mixed to stereo at 32,000 samples a second. LakeSnes's emulation
 * (snesdsp.c; its licence is there). Driven directly through its registers: no SPC700 program.
 */
#ifndef SNESDSP_H
#define SNESDSP_H

#include <stdbool.h>
#include <stdint.h>

#define SDSP_RATE 32000

typedef struct {
    uint16_t pitch, pitchCounter;
    bool     pitchModulation;
    int16_t  decodeBuffer[12];
    uint8_t  bufferOffset, srcn;
    uint16_t decodeOffset;
    uint8_t  blockOffset, brrHeader;
    bool     useNoise;
    uint8_t  startDelay;
    uint8_t  adsrRates[4];          /* attack, decay, sustain, gain */
    uint8_t  adsrState;             /* 0 attack, 1 decay, 2 sustain, 3 release */
    uint8_t  sustainLevel, gainSustainLevel;
    bool     useGain;
    uint8_t  gainMode;
    bool     directGain;
    uint16_t gainValue, preclampGain, gain;
    bool     keyOn, keyOff;
    int16_t  sampleOut;
    int8_t   volumeL, volumeR;
    bool     echoEnable;
} sdsp_voice_t;

typedef struct {
    uint8_t*     aram;              /* the 64 KiB of sound RAM: samples, their directory, the echo buffer */
    uint8_t      ram[0x80];         /* the registers */
    sdsp_voice_t channel[8];
    uint16_t     counter, dirPage;
    bool         evenCycle, mute, reset;
    int8_t       masterVolumeL, masterVolumeR;
    int16_t      sampleOutL, sampleOutR, echoOutL, echoOutR;
    int16_t      noiseSample;
    uint8_t      noiseRate;
    bool         echoWrites;
    int8_t       echoVolumeL, echoVolumeR, feedbackVolume;
    uint16_t     echoBufferAdr, echoDelay, echoLength, echoBufferIndex;
    uint8_t      firBufferIndex;
    int8_t       firValues[8];
    int16_t      firBufferL[8], firBufferR[8];
} sdsp_t;

/* power-on state (muted, in reset: write FLG ($6C) to start it); aram must point at 64 KiB */
void sdsp_reset(sdsp_t *dsp);
/* a register write, as the SPC700 makes them through $F2/$F3 */
void sdsp_write(sdsp_t *dsp, uint8_t adr, uint8_t val);
/* n samples: L, R pairs into out */
void sdsp_run(sdsp_t *dsp, int16_t *out, int n);

#endif
