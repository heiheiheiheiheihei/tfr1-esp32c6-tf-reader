#!/usr/bin/env python3
"""TFR-1 intro bed: a 120 BPM pulse bed synthesised from scratch.

Nothing is sampled; every element is a formula over seconds, so the file is
reproducible and unlicensed.  Written as a WAV, then the film mixes effects
on separate tracks.
"""
import math
import wave

import numpy as np

SR = 48000
DUR = 54.0
N = int(SR * DUR)
t = np.arange(N) / SR
BPM = 120.0
BEAT = 60.0 / BPM  # 0.5 s

rng = np.random.default_rng(20261108)

mix = np.zeros(N, dtype=np.float64)


def add(sig, at, gain=1.0):
    i = int(at * SR)
    if i >= N:
        return
    n = min(len(sig), N - i)
    mix[i:i + n] += sig[:n] * gain


def env(n, attack, decay, curve=3.0):
    a = int(attack * SR)
    d = max(n - a, 1)
    e = np.ones(n)
    if a:
        e[:a] = np.linspace(0, 1, a) ** 0.6
    e[a:] = np.exp(-np.linspace(0, curve, d)) * (1 - 0.0)
    return e


def kick(dur=0.34, f0=118.0, f1=42.0):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    f = f1 + (f0 - f1) * np.exp(-tt * 26.0)
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) * np.exp(-tt * 9.0)
    click = rng.standard_normal(n) * np.exp(-tt * 320.0) * 0.25
    return body * 0.9 + click


def hat(dur=0.05, decay=90.0):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    nz = rng.standard_normal(n)
    # crude high-pass: difference of consecutive samples
    hp = np.diff(np.concatenate([[0.0], nz]))
    return hp * np.exp(-tt * decay) * 0.5


def bass(freq, dur, gain=1.0):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    ph = 2 * np.pi * freq * tt
    s = np.sin(ph) * 0.7 + np.sin(2 * ph) * 0.22 + np.sin(3 * ph) * 0.06
    e = np.minimum(1.0, tt / 0.012) * np.exp(-np.maximum(tt - 0.05, 0) * 5.0)
    return s * e * gain


def pad(freqs, dur, atk=0.55, rel=0.9, detune=0.0018):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    s = np.zeros(n)
    for f in freqs:
        for k in (-1, 0, 1):
            s += np.sin(2 * np.pi * f * (1 + k * detune) * tt + rng.random() * 6.28)
    s /= (len(freqs) * 3)
    e = np.minimum(1.0, tt / atk) * np.minimum(1.0, np.maximum(0, (dur - tt)) / rel)
    return s * e


def riser(dur, f0=180.0, f1=1400.0):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    f = f0 * (f1 / f0) ** (tt / dur)
    ph = 2 * np.pi * np.cumsum(f) / SR
    tone = np.sin(ph) * 0.35
    nz = rng.standard_normal(n)
    # cheap moving high-pass: blend noise with its difference
    hp = np.diff(np.concatenate([[0.0], nz]))
    blend = np.linspace(0.15, 0.9, n)
    s = tone * (1 - blend) + (nz * 0.5 + hp * 0.9) * blend * 0.5
    return s * (np.linspace(0, 1, n) ** 1.6) * 0.9


def impact(dur=0.9):
    n = int(dur * SR)
    tt = np.arange(n) / SR
    f = 30 + 70 * np.exp(-tt * 18.0)
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) * np.exp(-tt * 4.2)
    tail = rng.standard_normal(n) * np.exp(-tt * 12.0) * 0.18
    return body * 0.95 + tail


# ---- arrangement -----------------------------------------------------------
# 0-13: sparse. one kick a bar, a drone, nothing else: this is the setup, not a drop
for b in range(0, 26):
    at = b * BEAT
    if b % 4 == 0:
        add(kick(0.3, 110, 45), at, 0.55)

n = N
drone = (np.sin(2 * np.pi * 55.0 * t) * 0.15 + np.sin(2 * np.pi * 82.5 * t) * 0.05)
drone += np.sin(2 * np.pi * 164.81 * t) * 0.02
drone *= 0.85 + 0.15 * np.sin(2 * np.pi * 0.25 * t)
duck = np.ones(n)
d0, d1 = int(7.2 * SR), int(9.4 * SR)
duck[d0:d1] = np.linspace(1, 0.15, d1 - d0)
duck[int(9.4 * SR):int(13 * SR)] = np.linspace(0.15, 1, int(13 * SR) - int(9.4 * SR))
mix += drone * duck

add(riser(3.4), 9.5, 0.30)

# 13-27: kick, hats on the 8th, A1 bass on the offbeat, a quiet mid pluck so a
# phone speaker still has something to play
mid = [440.0, 523.25, 659.26, 523.25]
for b in range(26, 54):
    at = b * BEAT
    add(kick(0.3, 118, 44), at, 0.6)
    add(hat(), at + BEAT / 2, 0.5)
    if b >= 38:
        add(hat(0.04, 140), at + BEAT / 4, 0.24)
    add(bass(55.0 if b % 2 else 65.41, 0.32), at + BEAT / 2, 0.5)
    if b % 2 == 1:
        add(bass(mid[(b // 2) % 4], 0.16, 0.16), at + BEAT / 2 + 0.02, 0.5)

# 27-35: pad enters (A minor)
add(pad([110.0, 130.81, 164.81], 8.4, atk=0.9), 27.0, 0.5)
for b in range(54, 70):
    at = b * BEAT
    add(kick(0.3, 120, 43), at, 0.55)
    add(hat(), at + BEAT / 2, 0.42)

# 35-38.5: riser into the payoff
add(riser(3.5), 35.0, 0.44)
for b in range(70, 77):
    at = b * BEAT
    add(kick(0.28, 124, 42), at, 0.5)

# 38.5 the number lands, full for four beats
add(impact(), 38.5, 0.95)
add(pad([55.0, 110.0, 164.81, 220.0], 3.5, atk=0.3), 38.5, 0.45)
for b in range(77, 84):
    at = b * BEAT
    add(kick(0.3, 116, 42), at, 0.45)
    if b % 2:
        add(hat(), at + BEAT / 2, 0.4)

# 42-48: strip back under the disclaimer
add(pad([110.0, 164.81], 6.0, atk=1.2), 42.0, 0.4)
for b in range(84, 96, 4):
    add(kick(0.34, 100, 40), b * BEAT, 0.34)

# 48-54: resolve and hold under the end card
add(pad([110.0, 130.81, 164.81, 220.0], 6.2, atk=0.8, rel=2.6), 48.0, 0.62)
add(impact(1.2), 48.0, 0.45)

# ---- master ----------------------------------------------------------------
mix = np.tanh(mix * 1.25) * 0.92
fade = np.ones(N)
f0 = int(50.0 * SR)
fade[f0:] = np.linspace(1, 0, N - f0) ** 1.4
fade_out = int(53.4 * SR)
fade[fade_out:] = np.linspace(fade[fade_out], 0, N - fade_out)
mix *= fade

peak = float(np.max(np.abs(mix)))
mix = mix / peak * 0.7
stereo = np.stack([mix, mix], axis=1)
pcm = (stereo * 32767).astype("<i2")

with wave.open("assets/audio/bed.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())

print("wrote assets/audio/bed.wav", pcm.shape[0] / SR, "s peak", peak)
