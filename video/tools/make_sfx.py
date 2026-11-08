#!/usr/bin/env python3
"""Three action sounds for the film: a tick, a click and a whoosh.

Each one is noise shaped by a formula, nothing sampled.  They sit on visible
causes only: the crosses landing, the sector box filling, the paper cut to the
desk photo, the screen switching to ONLINE.
"""
import wave

import numpy as np

SR = 48000
rng = np.random.default_rng(7)


def save(name, sig, peak=0.55):
    sig = sig / np.max(np.abs(sig)) * peak
    pcm = (np.stack([sig, sig], axis=1) * 32767).astype("<i2")
    with wave.open(f"assets/audio/{name}.wav", "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"assets/audio/{name}.wav  {len(sig) / SR:.3f}s")


def bandpass_noise(n, lo, hi):
    """Crude FFT band-pass: keep the bins between lo and hi."""
    nz = rng.standard_normal(n)
    spec = np.fft.rfft(nz)
    freqs = np.fft.rfftfreq(n, 1 / SR)
    mask = np.zeros_like(freqs)
    band = (freqs >= lo) & (freqs <= hi)
    mask[band] = 1.0
    # soft edges so the burst does not ring
    edge = np.exp(-((freqs - lo) / (lo * 0.5 + 1)) ** 2)
    mask *= np.where(freqs < lo, edge, 1.0)
    return np.fft.irfft(spec * mask, n)


# tick: 4 ms of band-limited noise with a 24 ms tail
n = int(0.06 * SR)
tt = np.arange(n) / SR
tick = bandpass_noise(n, 2200, 9000) * np.exp(-tt * 130.0)
save("tick", tick, 0.5)

# click: a touch lower and rounder, for a state change on the screen
n = int(0.09 * SR)
tt = np.arange(n) / SR
click = bandpass_noise(n, 900, 4200) * np.exp(-tt * 70.0)
click += np.sin(2 * np.pi * 420 * tt) * np.exp(-tt * 55.0) * 0.5
save("click", click, 0.5)

# whoosh: 0.5 s band-pass that opens up and closes again
n = int(0.5 * SR)
tt = np.arange(n) / SR
nz = rng.standard_normal(n)
# sweep a resonance by mixing two fixed bands with a crossfade
low = bandpass_noise(n, 180, 900)
high = bandpass_noise(n, 900, 5200)
k = np.sin(np.pi * tt / 0.5) ** 1.4
body = low * (1 - k * 0.7) + high * (k * 0.9)
shape = np.minimum(1.0, tt / 0.05) * np.exp(-np.maximum(tt - 0.28, 0) * 9.0)
save("whoosh", body * shape, 0.5)
