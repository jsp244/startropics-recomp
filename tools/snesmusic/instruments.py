#!/usr/bin/env python3
"""tools/snesmusic/instruments.py - make the remastered music's instruments (runtime/snesmusic_bank.c).

The remastered music (runtime/snesmusic.c) plays the game's own notes on the SNES's sound chip,
the S-DSP, which plays 4-bit ADPCM samples (BRR) from its 64 KiB of RAM. The instruments are
synthesized here, so no recorded sample is involved: a steel drum, a marimba, a plucked electric
bass, a shaker and a conga. Each pitched one is an attack (its strike or pluck, a whole number of
periods long) followed by one period that loops while the DSP's envelope lets it ring out; the
drums play once. The bank is the sound RAM from $0300: the sample directory (4 bytes an
instrument: start and loop address), then the samples from $0400.

BRR: blocks of 16 samples, each with a shift and one of four prediction filters. The first block of
a sample and the first block of its loop use filter 0, which needs no earlier samples, so every pass
through a loop decodes the same (with prediction across the loop point, the decoder's history never
settles and a one-period loop buzzes). The loop starts where a block of the period is quietest,
since that block is coded without prediction.

The noise is seeded, so the bank is the same on every run. Needs numpy.

usage: instruments.py [OUT.c]      (default: runtime/snesmusic_bank.c next to this tool)"""
import math
import os
import sys

import numpy as np

SR = 32000
DIR, SAMPLES = 0x0300, 0x0400
rng = np.random.default_rng(7)


# ---- synthesis
def onepole_lp(x, fc):
    a = math.exp(-2 * math.pi * fc / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def onepole_hp(x, fc):
    return x - onepole_lp(x, fc)


def harm(amps, n, P):
    out = np.zeros(len(n))
    for k, a in amps.items():
        out += a * np.sin(2 * np.pi * k * n / P)
    return out


def steel():
    """steel drum: fundamental, octave and twelfth; a bright, slightly inharmonic strike"""
    P, periods = 128, 30                      # 250 Hz at pitch $1000; 120 ms attack
    n = np.arange(P * periods)
    t = n / SR
    fade = (1 - n / len(n)) ** 2              # attack-only parts reach zero at the loop
    sus = {1: 1.0, 2: 0.62, 3: 0.24, 4: 0.10, 5: 0.05, 6: 0.025}
    x = harm(sus, n, P)
    for k in range(3, 11):
        x += 0.45 / k ** 0.6 * np.exp(-t / (0.006 + 0.06 / k)) * np.sin(2 * np.pi * k * n / P) * fade
    for r, a, tau in ((2.76, 0.30, 0.035), (4.18, 0.16, 0.02), (5.93, 0.10, 0.012)):
        x += a * np.exp(-t / tau) * np.sin(2 * np.pi * r * (SR / P) * t) * fade
    x += onepole_lp(rng.standard_normal(len(n)), 2000) * np.exp(-t / 0.0015) * 0.35 * fade
    x *= np.minimum(1, t / 0.0012)
    return x, harm(sus, np.arange(P), P), SR / P


def marimba():
    """marimba: a rosewood bar's fundamental with its 4th and 10th partials dying fast"""
    P, periods = 128, 16
    n = np.arange(P * periods)
    t = n / SR
    fade = (1 - n / len(n)) ** 2
    sus = {1: 1.0, 4: 0.03}
    x = harm(sus, n, P)
    x += 0.75 * np.exp(-t / 0.018) * np.sin(2 * np.pi * 4 * n / P) * fade
    x += 0.30 * np.exp(-t / 0.005) * np.sin(2 * np.pi * 10 * n / P) * fade
    x += onepole_lp(rng.standard_normal(len(n)), 1500) * np.exp(-t / 0.0012) * 0.4 * fade
    x *= np.minimum(1, t / 0.0008)
    return x, harm(sus, np.arange(P), P), SR / P


def bass():
    """plucked electric bass: a round body, a bright pluck"""
    P, periods = 128, 8                       # 250 Hz at pitch $1000
    n = np.arange(P * periods)
    t = n / SR
    fade = (1 - n / len(n)) ** 2
    sus = {k: (1.0 if k == 1 else 0.55 / k ** 1.5) for k in range(1, 11)}
    x = harm(sus, n, P)
    for k in range(2, 25):
        x += 0.5 / k ** 0.9 * np.exp(-t / (0.003 + 0.02 / k)) * np.sin(2 * np.pi * k * n / P) * fade
    x += onepole_lp(rng.standard_normal(len(n)), 1500) * np.exp(-t / 0.001) * 0.5 * fade
    x *= np.minimum(1, t / 0.001)
    return x, harm(sus, np.arange(P), P), SR / P


def shaker():
    """shaker: high-passed noise, a short swell and fall"""
    L = 2560
    t = np.arange(L) / SR
    x = onepole_hp(onepole_hp(rng.standard_normal(L), 2000), 2000)
    x *= np.minimum(1, t / 0.004) * np.exp(-np.maximum(0, t - 0.004) / 0.025)
    return x, None, None


def conga():
    """conga: a drumhead's tone falling in pitch, and the slap of the hand"""
    L = 6400
    t = np.arange(L) / SR
    f = 205 + 70 * np.exp(-t / 0.012)
    ph = 2 * np.pi * np.cumsum(f) / SR
    x = np.sin(ph) * np.exp(-t / 0.075) + 0.25 * np.sin(1.52 * ph) * np.exp(-t / 0.03)
    x += onepole_lp(rng.standard_normal(L), 2500) * np.exp(-t / 0.003) * 0.5
    x *= np.minimum(1, t / 0.0007)
    return x, None, None


# ---- BRR (the decoder's arithmetic as runtime/snesdsp.c does it)
def pred(f, o, oo):
    if f == 1:
        return o + ((-o) >> 4)
    if f == 2:
        return 2 * o + ((3 * -o) >> 5) - oo + (oo >> 4)
    if f == 3:
        return 2 * o + ((13 * -o) >> 6) - oo + ((3 * oo) >> 4)
    return 0


def dec(nib, shift, p):
    s = ((nib << shift) >> 1) + p
    s = -0x8000 if s < -0x8000 else (0x7fff if s > 0x7fff else s)
    v = (s * 2) & 0xffff
    return v - 0x10000 if v >= 0x8000 else v


def enc_block(x16, old, older, filters):
    """the shift and filter (of those allowed) that code 16 samples best after old, older"""
    best = None
    peak = max(abs(int(v)) for v in x16)
    smin = max(0, int(math.log2(max(1, peak))) - 4)
    for f in filters:
        for sh in range(max(0, smin - 2), 13):
            o, oo, err, nibs = old, older, 0, []
            step = (1 << sh) / 2
            for tv in x16:
                p = pred(f, o, oo)
                guess = int(round((tv / 2 - p) / step))
                bv, bn, be = None, None, None
                for nb in (guess - 1, guess, guess + 1):
                    if nb < -8 or nb > 7:
                        continue
                    v = dec(nb, sh, p)
                    e = (v - tv) ** 2
                    if be is None or e < be:
                        bv, bn, be = v, nb, e
                if bn is None:
                    bn = max(-8, min(7, guess))
                    bv = dec(bn, sh, p)
                    be = (bv - tv) ** 2
                err += be
                nibs.append(bn)
                oo, o = o, bv >> 1
                if best is not None and err >= best[0]:
                    break
            else:
                if best is None or err < best[0]:
                    best = (err, f, sh, nibs, o, oo)
    return best


def brr(x16, loop_block):
    blocks = [x16[i:i + 16] for i in range(0, len(x16), 16)]
    out = []
    o = oo = 0
    for bi, b in enumerate(blocks):
        r = enc_block(b, o, oo, (0,) if bi == 0 or bi == loop_block else (0, 1, 2, 3))
        out.append(r)
        o, oo = r[4], r[5]
    data = bytearray()
    for bi, (_, f, sh, nibs, _, _) in enumerate(out):
        hdr = (sh << 4) | (f << 2)
        if bi == len(out) - 1:
            hdr |= 1 | (2 if loop_block is not None else 0)     # end, and loop back
        data.append(hdr)
        for k in range(0, 16, 2):
            data.append(((nibs[k] & 15) << 4) | (nibs[k + 1] & 15))
    return bytes(data)


def to16(x, peak=17000):
    x = np.asarray(x, dtype=float)
    return np.round(x / max(1e-9, np.max(np.abs(x))) * peak).astype(int)


#   C name          maker    ADSR1 ADSR2  (runtime/snesmusic.h's instrument numbers, in order)
INSTRUMENTS = [("steel drum", steel, 0xCF, 0xB0),
               ("marimba", marimba, 0xEF, 0x72),
               ("bass", bass, 0xDF, 0xAD),
               ("shaker", shaker, 0x8F, 0xE0),
               ("conga", conga, 0x8F, 0xE0)]


def build():
    aram = bytearray(0x10000)
    adr = SAMPLES
    insts = []
    for srcn, (name, maker, a1, a2) in enumerate(INSTRUMENTS):
        att, loop, base = maker()
        if loop is not None:
            r = min(range(len(loop) // 16), key=lambda k: np.max(np.abs(loop[16 * k:16 * k + 16])))
            att = np.concatenate([att, loop[:16 * r]])
            loop = np.roll(loop, -16 * r)
            x16 = list(to16(np.concatenate([att, loop])))
            lb = len(att) // 16
            data = brr(x16, lb)
            loop_adr = adr + lb * 9
        else:
            x16 = list(to16(np.concatenate([att, np.zeros(32)])))     # a silent block to end on
            x16 += [0] * ((16 - len(x16) % 16) % 16)
            data = brr(x16, None)
            loop_adr = adr
        aram[adr:adr + len(data)] = data
        aram[DIR + 4 * srcn:DIR + 4 * srcn + 4] = bytes([adr & 255, adr >> 8, loop_adr & 255, loop_adr >> 8])
        insts.append((name, srcn, a1, a2, base))
        adr = (adr + len(data) + 15) & ~15
    return bytes(aram[DIR:adr]), insts


def write_c(path, bank, insts):
    lines = ["/*",
             " * snesmusic_bank.c - the remastered music's instruments: BRR samples for the S-DSP and their",
             " * envelopes. Made by tools/snesmusic/instruments.py, which synthesizes them (no recorded",
             " * samples); do not edit.",
             " */",
             '#include "snesmusic.h"',
             "",
             "/* the sound RAM from SNESMUSIC_BANK_ADDR: the sample directory, then the samples */",
             "const uint8_t snesmusic_bank[%d] = {" % len(bank)]
    for i in range(0, len(bank), 16):
        lines.append("    " + " ".join("0x%02X," % b for b in bank[i:i + 16]))
    lines += ["};",
              "const unsigned snesmusic_bank_size = sizeof snesmusic_bank;",
              "",
              "/* sample number, ADSR1, ADSR2, the pitch the sample plays at rate $1000 (0: drums, played at $1000) */",
              "const snesmusic_inst_t snesmusic_insts[SNESMUSIC_INSTS] = {"]
    for name, srcn, a1, a2, base in insts:
        lines.append("    { %d, 0x%02X, 0x%02X, %.1f },   /* %s */" % (srcn, a1, a2, base or 0.0, name))
    lines.append("};")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "runtime", "snesmusic_bank.c")
    bank, insts = build()
    write_c(out, bank, insts)
    print("%s: %d bytes of sound RAM ($%04X-$%04X), %d instruments" % (out, len(bank), DIR, DIR + len(bank) - 1, len(insts)))
