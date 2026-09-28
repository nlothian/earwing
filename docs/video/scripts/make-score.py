#!/usr/bin/env python3
"""
make-score.py -- original 30.000 s score + sound design for the "earwing" launch film.

Writes
    docs/video/assets/audio/score.wav              48 kHz, stereo, 24-bit PCM, exactly 1,440,000 frames
    docs/video/assets/audio/score-spectrogram.png  self-review image (full mix + zooms, cue markers)
and prints a QA report: ffmpeg ebur128 loudness / true peak, DC offset, click scan and a
measured-vs-intended cue onset table.

Requirements: python3 + numpy; ffmpeg on PATH (decodes the bundled SFX and measures loudness);
Pillow for the PNG (skipped if missing). Deterministic: every random source is seeded.

    python3 docs/video/scripts/make-score.py            # render + QA + PNG
    python3 docs/video/scripts/make-score.py --no-qa    # render only

Tempo grid (the edit is cut to it): 128 BPM, 4/4, beat = 0.46875 s, bar = 1.875 s,
16 bars = 30.000 s. Bar n starts at (n-1) * 1.875 s. Key: D major.

CUE TABLE ------------------------------------------------------------------------------------
 time (s)    bar.beat  sound                                                   picture
 0.000       1.1       Dmaj9 pad + low D drone swell up from silence, LP opening  dark reveal
 1.875       2.1       bell "bloom" A5, long reverb                                wordmark
 ~1.9-3.75             reversed reverb of the first pluck, sucks into 3.75
 3.750       3.1       mic pluck 1  D5   (panned L)                                mic 1 lights
 4.21875     3.2       mic pluck 2  F#5                                            mic 2
 4.6875      3.3       mic pluck 3  A5                                             mic 3
 5.15625     3.4       mic pluck 4  C#6  (panned R)                                mic 4
 5.625       4.1       soft low-passed kick on every beat, pad filter opens,
                       noise riser + reversed chord swell build into 7.5
 7.500       5.1       DROP: kick, sub, off-beat bass, hats/shaker, ping-pong arp  beamforming
 11.250      7.1       soft whoosh (sample, envelope peak on the cue)              waveforms align
 12.1875     7.3       warm swell: 4 detuned voices converge to unison + narrow    voice adds up
 13.125      8.1       motif A5-E6-D6 ... C#6 (4-3 suspension resolves at 14.0625) beam locks on
 15.000      9.1       breakdown: kick + sub out, arp/hats low-passed              private
 15.9375     9.3       soft UI pop (tuned B5)                                      UI pop
 16.875      10.1      kick + bass return (filter sweeps open over beat 9.4)
 18.750      11.1      low clean impact (replaces the kick) + strike-through swipe cloud crossed out
 20.625      12.1      full groove, brighter; claps on 2 & 4, 16th arp            who said what
 23.4375     13.3      key press 1 (hats drop out 23.40-23.86 so typing is clear) name typed
 23.55 / 23.66 / 23.78 key presses 2-4
 24.375      14.1      chime (E6 + B6 bell dyad)                                   label confirmed
 25.300                riser (noise sweep + reversed final chord) into 26.25; kick drops on 14.4
 26.250      15.1      END CARD: impact + Dmaj9 (wide) + D bass hit; everything else stops
 27.1875     15.3      final bell A6                                               tagline
 28.600-29.950         fade to true digital silence; every sample from 29.95 s on is exactly 0
----------------------------------------------------------------------------------------------
"""
import argparse
import os
import re
import subprocess
import wave
import zlib

import numpy as np

# =============================================================================================
# 0. PATHS, GRID, TARGETS
# =============================================================================================
HERE = os.path.dirname(os.path.abspath(__file__))
VIDEO_DIR = os.path.dirname(HERE)
OUT_DIR = os.path.join(VIDEO_DIR, "assets", "audio")
OUT_WAV = os.path.join(OUT_DIR, "score.wav")
OUT_PNG = os.path.join(OUT_DIR, "score-spectrogram.png")
SFX_DIR = os.environ.get(
    "EARWING_SFX_DIR", os.path.expanduser("~/.claude/skills/media-use/audio/assets/sfx"))

SR = 48_000
DUR = 30.0
N = int(round(DUR * SR))          # 1,440,000 frames
BPM = 128
BEAT = 60.0 / BPM                 # 0.46875 s
BAR = 4 * BEAT                    # 1.875 s
S16 = BEAT / 4                    # 0.1171875 s
SEED = 20260928

TARGET_LUFS = -14.0
LIMIT_DBTP = -1.35                # limiter ceiling; spec is <= -1.0 dBTP (margin for meter differences)
END_CARD = 26.25                  # bar 15, beat 1
FADE_START, FADE_END = 28.6, 29.95


def bt(bar, beat=0.0):
    """Time (s) of `beat` (0-based, may be fractional) within 1-based `bar`."""
    return (bar - 1) * BAR + beat * BEAT


def smp(t):
    return int(round(t * SR))


def mtof(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def rng_for(tag):
    """Independent, reproducible random stream per named part."""
    return np.random.default_rng([SEED, zlib.crc32(tag.encode())])


# Mix levels (dB). Tuned by measurement (see QA: per-stem loudness in the groove).
MIX = {
    "pad": -16.7, "drone": -16.7, "air": -33.0,
    "mic": -13.0, "bell": -9.3, "revswell": -7.5,
    "kick": -1.0, "sub": -7.4, "midbass": -7.4,
    "hat": -8.5, "shaker": -8.5, "clap": -5.0,
    "arp": -14.0, "motif": -14.0, "swell": -5.0,
    "riser": -7.0, "riser_sfx": -10.0, "downlifter": -15.0,
    "whoosh": -5.0, "pop": -9.0, "keys": -6.0, "chime": -11.5,
    "impact": 0.0, "impact_sfx": -10.0, "swipe": -14.0,
    "end_pad": -12.0, "end_struck": -14.0, "end_sub": -6.0, "end_thump": -2.5, "end_air": -9.0,
    "end_bell": -10.5,
    "rev_a": -3.5, "rev_b": -2.5, "delay": -4.0,
}


def db(x):
    return 10.0 ** (x / 20.0)


# =============================================================================================
# 1. DSP TOOLKIT (numpy only)
# =============================================================================================
def fast_len(n):
    """Smallest 5-smooth integer >= n (a fast FFT size)."""
    best = 1 << max(0, int(n - 1).bit_length())
    p5 = 1
    while p5 < best:
        p35 = p5
        while p35 < best:
            v = p35
            while v < n:
                v *= 2
            best = min(best, v)
            p35 *= 3
        p5 *= 5
    return best


def biquad(kind, f0, q=0.7071067811865476, gain_db=0.0):
    """RBJ-cookbook biquad -> (b, a), a[0] == 1."""
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * np.pi * f0 / SR
    c, s = np.cos(w0), np.sin(w0)
    al = s / (2 * q)
    if kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]; a = [1 + al, -2 * c, 1 - al]
    elif kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]; a = [1 + al, -2 * c, 1 - al]
    elif kind == "bp":
        b = [al, 0.0, -al]; a = [1 + al, -2 * c, 1 - al]
    elif kind == "peak":
        b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A]
    elif kind == "lowshelf":
        sa = 2 * np.sqrt(A) * al
        b = [A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c),
             A * ((A + 1) - (A - 1) * c - sa)]
        a = [(A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa]
    elif kind == "highshelf":
        sa = 2 * np.sqrt(A) * al
        b = [A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c),
             A * ((A + 1) + (A - 1) * c - sa)]
        a = [(A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa]
    else:
        raise ValueError(kind)
    b = np.array(b, float) / a[0]
    a = np.array(a, float) / a[0]
    return b, a


def butter(kind, f0, order=2):
    """Butterworth HP/LP (even order) as a cascade of biquads."""
    return [biquad(kind, f0, 1.0 / (2 * np.cos(np.pi * (2 * k - 1) / (2 * order))))
            for k in range(1, order // 2 + 1)]


def response(sections, n):
    """Complex response of a biquad cascade at the rfft bins of an n-point FFT."""
    z = np.exp(-2j * np.pi * np.arange(n // 2 + 1) / n)
    H = np.ones(n // 2 + 1, complex)
    for b, a in sections:
        H *= (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    return H


def filt(x, sections, tail=0.5):
    """Causal IIR filtering (biquad cascade), computed exactly in the frequency domain.
    The signal is zero-padded by `tail` seconds so the decaying impulse response does not wrap."""
    if not sections:
        return x
    x = np.asarray(x, float)
    L = x.shape[-1]
    n = fast_len(L + int(tail * SR))
    return np.fft.irfft(np.fft.rfft(x, n) * response(sections, n), n)[..., :L]


def hp(x, f, order=2):
    return filt(x, butter("hp", f, order))


def lp(x, f, order=2):
    return filt(x, butter("lp", f, order))


def zero_phase_band(x, lo=None, hi=None, order=4):
    """Zero-phase Butterworth-magnitude band filter (analysis only: no group delay)."""
    L = x.shape[-1]
    n = fast_len(L + SR // 4)
    f = np.fft.rfftfreq(n, 1 / SR)
    g = np.ones_like(f)
    if lo:
        g /= np.sqrt(1 + (lo / np.maximum(f, 1e-3)) ** (2 * order))
    if hi:
        g /= np.sqrt(1 + (f / hi) ** (2 * order))
    return np.fft.irfft(np.fft.rfft(x, n) * g, n)[..., :L]


def stft_filter(x, gain_fn, n=2048, hop=512):
    """Time-varying zero-phase filter. gain_fn(t_frames[F], f_bins[K]) -> gains[F, K].
    sqrt-Hann analysis/synthesis windows at 75 % overlap, exact overlap-add normalisation."""
    x = np.atleast_2d(np.asarray(x, float))
    C, L = x.shape
    win = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / n))
    pad = n
    total = L + 2 * pad
    nf = int(np.ceil((total - n) / hop)) + 1
    total = (nf - 1) * hop + n
    xp = np.zeros((C, total))
    xp[:, pad:pad + L] = x
    idx = hop * np.arange(nf)[:, None] + np.arange(n)[None, :]
    tc = (hop * np.arange(nf) + n / 2 - pad) / SR
    G = gain_fn(tc, np.fft.rfftfreq(n, 1 / SR))
    wsum = np.zeros(total)
    r = n // hop
    for k in range(r):
        m = len(range(k, nf, r))
        wsum[k * hop:k * hop + m * n] += np.tile(win * win, m)
    out = np.zeros((C, total))
    for ch in range(C):
        Y = np.fft.irfft(np.fft.rfft(xp[ch][idx] * win, axis=1) * G, n, axis=1) * win
        for k in range(r):
            blk = Y[k::r]
            out[ch, k * hop:k * hop + blk.size] += blk.reshape(-1)
    out /= np.maximum(wsum, 1e-9)
    return out[:, pad:pad + L]


def keyframes(t, pts, log=True):
    """Piecewise interpolation of [(time, value), ...] at times t (log-domain for frequencies)."""
    tt = np.array([p[0] for p in pts], float)
    vv = np.array([p[1] for p in pts], float)
    if log:
        return np.exp(np.interp(t, tt, np.log(vv)))
    return np.interp(t, tt, vv)


def lp_gain(fc, f, order=2):
    return 1.0 / np.sqrt(1.0 + (f[None, :] / fc[:, None]) ** (2 * order))


def resample_up(x, k):
    L = x.shape[-1]
    return np.fft.irfft(np.fft.rfft(x), L * k) * k


def resample_down(y, k):
    L = y.shape[-1] // k
    Y = np.fft.rfft(y)[..., :L // 2 + 1]
    return np.fft.irfft(Y, L) / k


def resample_ratio(x, ratio):
    """Band-limited resampling by FFT (ratio > 1 => shorter / higher pitch)."""
    L = x.shape[-1]
    L2 = int(round(L / ratio))
    X = np.fft.rfft(x)
    X2 = np.zeros(x.shape[:-1] + (L2 // 2 + 1,), complex)
    m = min(X.shape[-1], X2.shape[-1])
    X2[..., :m] = X[..., :m]
    return np.fft.irfft(X2, L2) * (L2 / L)


def fade_in(n):
    return 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / max(n, 1))


def fade_out(n):
    return 0.5 + 0.5 * np.cos(np.pi * (np.arange(n) + 1) / max(n, 1))


def env_asr(L, att, gate, rel):
    """Raised-cosine attack, sustain until `gate` s, raised-cosine release over `rel` s."""
    e = np.zeros(L)
    na, ng, nr = smp(att), smp(gate), smp(rel)
    na = max(1, min(na, ng))
    e[:na] = fade_in(na)
    e[na:ng] = 1.0
    seg = min(nr, L - ng)
    if seg > 0:
        e[ng:ng + seg] = fade_out(nr)[:seg]
    return e


def pan_gains(p):
    th = (np.clip(p, -1, 1) + 1) * np.pi / 4
    return np.cos(th) * np.sqrt(2), np.sin(th) * np.sqrt(2)


def to_stereo(sig, pan=0.0):
    if sig.ndim == 2:
        return sig
    gl, gr = pan_gains(pan)
    return np.vstack([sig * gl, sig * gr])


def place(bus, sig, t, gain=1.0, pan=0.0):
    """Mix a mono (panned) or stereo signal into `bus` starting at time t (may be negative)."""
    st = to_stereo(np.asarray(sig, float), pan) * gain
    s0 = smp(t)
    a, b = max(0, s0), min(bus.shape[1], s0 + st.shape[1])
    if b > a:
        bus[:, a:b] += st[:, a - s0:b - s0]


def place_ending_at(bus, sig, t_end, gain=1.0, pan=0.0):
    """Mix a signal so that its last sample lands just before t_end."""
    L = sig.shape[-1]
    place(bus, sig, (smp(t_end) - L) / SR, gain, pan)


def convolve(x, ir):
    """Mono-in (or stereo summed) -> stereo-out FFT convolution, truncated to len(x)."""
    x = np.asarray(x, float)
    if x.ndim == 2:
        x = 0.5 * (x[0] + x[1])
    L = x.shape[-1]
    n = fast_len(L + ir.shape[-1])
    return np.fft.irfft(np.fft.rfft(x, n)[None, :] * np.fft.rfft(ir, n), n)[:, :L]


def sliding_min_fwd(a, W):
    """out[n] = min(a[n : n+W]) (a is padded with its last value)."""
    L = len(a)
    b = np.concatenate([a, np.full(W, a[-1])])
    levels = [b]
    span = 1
    while span * 2 <= W:
        prev = levels[-1]
        levels.append(np.minimum(prev[:-span], prev[span:]))
        span *= 2
    top = levels[-1]
    return np.minimum(top[:L], top[W - span:W - span + L])


# =============================================================================================
# 2. INSTRUMENTS (band-limited: additive partials / band-limited wavetables, click-free envelopes)
# =============================================================================================
def partials(f0, L, spec, t0_phase=0.0):
    """Sum of exponentially decaying sines. spec = [(ratio, amp, tau_s), ...].
    Partials above 17 kHz are dropped, 12-17 kHz tapered, so nothing can alias."""
    t = np.arange(L) / SR
    y = np.zeros(L)
    for ratio, amp, tau in spec:
        f = f0 * ratio
        if f >= 17000 or amp == 0:
            continue
        taper = 1.0 if f < 12000 else (17000 - f) / 5000
        y += amp * taper * np.exp(-t / tau) * np.sin(2 * np.pi * f * t + t0_phase)
    return y


def noise_burst(rng, L, lo, hi, tau, order=2):
    n = rng.standard_normal(L)
    n = filt(n, butter("hp", lo, order) + butter("lp", hi, order), tail=0.05)
    t = np.arange(L) / SR
    return n * np.exp(-t / tau)


def glass_pluck(f0, vel=1.0, bright=1.0, decay=1.0, length=None, click=0.02, rng=None):
    """Clean, glassy pluck: harmonic partials with order-dependent decay, a faint inharmonic
    'ting' and a 2 ms band-passed noise tick. 1.2 ms raised-cosine attack, faded tail."""
    tau1 = 1.2 * decay
    length = length or min(4.5, 5.0 * tau1)
    L = smp(length)
    amps = [1.0, 0.55, 0.30, 0.16, 0.10, 0.06, 0.035]
    taus = [1.0, 0.55, 0.36, 0.25, 0.18, 0.13, 0.10]
    spec = [(k + 1, amps[k] * bright ** k, tau1 * taus[k]) for k in range(7)]
    spec.append((5.404, 0.045 * bright, 0.03 * decay + 0.012))
    y = partials(f0, L, spec)
    if click > 0 and rng is not None:
        nb = smp(0.004)
        tick = noise_burst(rng, nb, 2500, 7500, 0.0007)
        y[:nb] += click * tick / (np.abs(tick).max() + 1e-12)
    y[:smp(0.0012)] *= fade_in(smp(0.0012))
    nf = smp(0.06)
    y[-nf:] *= fade_out(nf)
    return vel * y


def bell(f0, vel=1.0, decay=1.0, length=6.0, shimmer_hz=0.35):
    """Warm additive bell; every partial is a slowly beating pair (bloom/shimmer)."""
    L = smp(length)
    spec = [(0.5, 0.10, 2.2), (1.0, 1.0, 2.6), (2.0, 0.34, 1.5), (3.0, 0.12, 0.85),
            (4.07, 0.09, 0.45), (5.43, 0.045, 0.25), (6.8, 0.02, 0.14)]
    spec = [(r, a, tau * decay) for r, a, tau in spec]
    y = 0.5 * partials(f0, L, spec)
    y += 0.5 * partials(f0 + shimmer_hz, L, spec)
    y[:smp(0.0025)] *= fade_in(smp(0.0025))
    nf = smp(0.3)
    y[-nf:] *= fade_out(nf)
    return vel * y


_TABLES = {}


def wavetable(f0, rolloff=5000.0, power=1.1, size=4096, fmax=16000.0, seed=0):
    """Band-limited saw-like single-cycle table (harmonics below fmax only)."""
    key = (round(f0, 3), rolloff, power, size, fmax, seed)
    if key not in _TABLES:
        K = max(1, int(fmax / (f0 * 1.02)))
        k = np.arange(1, K + 1)
        amp = (1.0 / k ** power) / np.sqrt(1 + (k * f0 / rolloff) ** 4)
        ph = np.random.default_rng([SEED, seed, K]).uniform(0, 2 * np.pi, K)
        spec = np.zeros(size // 2 + 1, complex)
        spec[1:K + 1] = amp * np.exp(1j * ph) * (size / 2)
        tab = np.fft.irfft(spec, size)
        _TABLES[key] = np.append(tab, tab[0])
    return _TABLES[key]


def play_table(tab, freq, phase0=0.0):
    size = len(tab) - 1
    ph = (phase0 + np.cumsum(freq) / SR) % 1.0
    pos = ph * size
    i = pos.astype(np.int64)
    fr = pos - i
    return tab[i] * (1 - fr) + tab[i + 1] * fr


def pad_note(midi, gate, att, rel, rng, voices=3, detune=7.0, spread=0.85,
             rolloff=4500.0, sine=0.35, env=None):
    """Warm detuned-saw pad voice stack with slow pitch drift; stereo."""
    f0 = mtof(midi)
    L = smp(gate + rel)
    t = np.arange(L) / SR
    out = np.zeros((2, L))
    for v in range(voices):
        u = 0.0 if voices == 1 else (v - (voices - 1) / 2) / ((voices - 1) / 2)
        drift = (2.0 * np.sin(2 * np.pi * rng.uniform(0.08, 0.16) * t + rng.uniform(0, 6.28))
                 + 1.2 * np.sin(2 * np.pi * rng.uniform(0.22, 0.37) * t + rng.uniform(0, 6.28)))
        f = f0 * 2 ** ((detune * u + drift) / 1200)
        tab = wavetable(f0, rolloff, 1.15, seed=v)
        s = play_table(tab, f, rng.random())
        gl, gr = pan_gains(spread * u)
        out[0] += s * gl
        out[1] += s * gr
    out /= voices
    out += sine * np.sin(2 * np.pi * f0 * t + rng.uniform(0, 6.28))[None, :]
    e = env if env is not None else env_asr(L, att, gate, rel)
    return out * e[None, :]


def kick(vel=1.0, f_hi=165.0, f_lo=50.0, pitch_tau=0.032, amp_tau=0.15, hold=0.018,
         length=0.6, click=0.05, rng=None):
    """Soft round kick: pitch-swept sine (+ faint 2nd harmonic) and a low-passed noise tick."""
    L = smp(length)
    t = np.arange(L) / SR
    f = f_lo + (f_hi - f_lo) * np.exp(-t / pitch_tau)
    ph = 2 * np.pi * np.cumsum(f) / SR
    amp = np.where(t < hold, 1.0, np.exp(-(t - hold) / amp_tau))
    y = amp * (np.sin(ph) + 0.07 * np.sin(2 * ph) * np.exp(-t / 0.05))
    if click > 0 and rng is not None:
        nb = smp(0.006)
        tick = noise_burst(rng, nb, 700, 3200, 0.0009)
        y[:nb] += click * tick / (np.abs(tick).max() + 1e-12)
    y[:smp(0.0006)] *= fade_in(smp(0.0006))
    nf = smp(0.04)
    y[-nf:] *= fade_out(nf)
    return vel * hp(y, 28)


def hat_voice(rng, closed=True):
    L = smp(0.12 if closed else 0.14)
    if closed:
        y = noise_burst(rng, L, 7200, 12500, 0.020)
        y[:smp(0.0004)] *= fade_in(smp(0.0004))
    else:  # shaker: softer attack, darker, a little longer
        y = noise_burst(rng, L, 3600, 9000, 0.028)
        y[:smp(0.006)] *= fade_in(smp(0.006))
    nf = smp(0.03)
    y[-nf:] *= fade_out(nf)
    return y / (np.abs(y).max() + 1e-12)


def clap(rng):
    L = smp(0.35)
    y = np.zeros(L)
    for k, dt in enumerate([0.0, 0.0085, 0.0175]):
        s = smp(dt)
        b = noise_burst(rng, smp(0.02), 900, 3200, 0.0035)
        b[:smp(0.0005)] *= fade_in(smp(0.0005))
        y[s:s + len(b)] += b * (0.8 + 0.1 * k)
    s = smp(0.02)
    tail = noise_burst(rng, L - s, 1000, 4200, 0.055)
    tail[:smp(0.002)] *= fade_in(smp(0.002))
    y[s:] += 0.9 * tail
    nf = smp(0.05)
    y[-nf:] *= fade_out(nf)
    return y / (np.abs(y).max() + 1e-12)


def sub_tone(f0_curve, amp, harm=(1.0, 0.16, 0.05)):
    """Additive sub (fundamental + phase-locked 2nd/3rd harmonics); f0_curve per sample."""
    ph = 2 * np.pi * np.cumsum(f0_curve) / SR
    y = np.zeros_like(ph)
    for k, a in enumerate(harm, start=1):
        y += a * np.sin(k * ph)
    return amp * y


def ui_pop(f_end=987.77):
    """Soft UI 'bloop': fast upward pitch glide into a tuned sine, 1 ms attack."""
    L = smp(0.22)
    t = np.arange(L) / SR
    f = f_end * (1 - 0.42 * np.exp(-t / 0.012))
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = (np.sin(ph) + 0.18 * np.sin(2 * ph) * np.exp(-t / 0.02)) * np.exp(-t / 0.045)
    y[:smp(0.001)] *= fade_in(smp(0.001))
    nf = smp(0.04)
    y[-nf:] *= fade_out(nf)
    return y


def noise_riser(rng, dur, f_start, f_end, curve=2.4, bw_oct=1.1):
    """Stereo noise swell whose band centre sweeps f_start -> f_end and whose level rises
    ~exponentially; ends with a 6 ms fade (so it stops exactly on the downbeat)."""
    L = smp(dur)
    x = rng.standard_normal((2, L))

    def g(tc, f):
        u = np.clip(tc / dur, 0, 1)
        fc = f_start * (f_end / f_start) ** (u ** 1.3)
        amp = u ** curve
        lf = np.log2(np.maximum(f, 20)[None, :] / fc[:, None])
        return amp[:, None] * np.exp(-0.5 * (lf / (bw_oct / 2)) ** 2)

    y = stft_filter(x, g, n=1024, hop=256)
    y[:, -smp(0.006):] *= fade_out(smp(0.006))[None, :]
    return y / (np.abs(y).max() + 1e-12)


def reverse_swell(dry, ir, length, fade=0.5):
    """Reversed reverb of `dry` (mono): the wet tail played backwards, cropped to `length` s."""
    L = len(dry)
    n = fast_len(L + ir.shape[1])
    wet = np.fft.irfft(np.fft.rfft(dry, n)[None, :] * np.fft.rfft(ir, n), n)[:, :L + ir.shape[1]]
    rev = wet[:, ::-1]
    m = smp(length)
    rev = rev[:, -m:].copy()
    rev[:, :smp(fade)] *= fade_in(smp(fade))[None, :]
    rev[:, -smp(0.004):] *= fade_out(smp(0.004))[None, :]
    return rev / (np.abs(rev).max() + 1e-12)


def make_ir(tag, rt_low=2.3, rt_high=1.0, length=3.2, predelay=0.022, er_gain=0.35,
            hp_f=200.0, lp_f=9500.0):
    """Synthetic stereo reverb IR: decorrelated noise with frequency-dependent exponential
    decay (RT60 rt_low @ 300 Hz -> rt_high @ 8 kHz), 25 ms density build-up, sparse early
    reflections, pre-delay, band-limited, unit energy per channel."""
    r = rng_for(tag)
    L = smp(length)
    x = r.standard_normal((2, L))

    def g(tc, f):
        ff = np.clip(f, 100, 16000)
        u = np.clip((np.log(ff) - np.log(300)) / (np.log(8000) - np.log(300)), 0, 1)
        rt = rt_low + (rt_high - rt_low) * u
        return np.exp(-6.9078 * np.maximum(tc, 0)[:, None] / rt[None, :])

    tail = stft_filter(x, g, n=1024, hop=256)
    tail[:, :smp(0.025)] *= fade_in(smp(0.025))[None, :]
    er = np.zeros((2, L))
    for k in range(10):
        dt = r.uniform(0.004, 0.065)
        ch = k % 2
        er[ch, smp(dt)] += r.choice([-1, 1]) * (1 - dt / 0.08) * r.uniform(0.6, 1.0)
    er = lp(er, 6500)
    ir = tail / np.sqrt((tail ** 2).sum(1, keepdims=True)) + er_gain * er
    ir = filt(ir, butter("hp", hp_f, 2) + butter("lp", lp_f, 2), tail=0.1)
    ir *= fade_out(L)[None, :] ** 0.25  # make sure the very end is exactly zero
    ir = np.concatenate([np.zeros((2, smp(predelay))), ir], axis=1)
    return ir / np.sqrt((ir ** 2).sum(1, keepdims=True))


def pingpong(x, delay_s, fb=0.38, taps=7, lp_f=3800.0, hp_f=350.0):
    """Ping-pong delay (wet only): repeat k goes to L for odd k, R for even k, each repeat
    darker (one more LP pass). Computed as a single frequency-domain multi-tap filter."""
    if x.ndim == 2:
        x = 0.5 * (x[0] + x[1])
    L = len(x)
    D = smp(delay_s)
    n = fast_len(L + D * taps + SR)
    X = np.fft.rfft(x, n)
    w = 2 * np.pi * np.arange(n // 2 + 1) / n
    Hl = response(butter("lp", lp_f, 2), n)
    HL = np.zeros(n // 2 + 1, complex)
    HR = np.zeros(n // 2 + 1, complex)
    for k in range(1, taps + 1):
        term = fb ** (k - 1) * Hl ** k * np.exp(-1j * w * D * k)
        if k % 2:
            HL += term
        else:
            HR += term
    Hh = response(butter("hp", hp_f, 2), n)
    y = np.vstack([np.fft.irfft(X * HL * Hh, n)[:L], np.fft.irfft(X * HR * Hh, n)[:L]])
    return y


def duck_env(times, depth, release=0.2, attack=0.004, n=N):
    """Sidechain gain curve: dips by `depth` (0..1) at each time, cosine recovery."""
    g = np.ones(n)
    na, nr = smp(attack), smp(release)
    shape = np.concatenate([fade_in(na), fade_out(nr)])
    for t in times:
        s0 = smp(t) - na
        a, b = max(0, s0), min(n, s0 + len(shape))
        if b > a:
            g[a:b] = np.minimum(g[a:b], 1 - depth * shape[a - s0:b - s0])
    return g


# =============================================================================================
# 3. BUNDLED SFX (decoded with ffmpeg; aligned by detected onset / peak, never assumed t=0)
# =============================================================================================
def load_sfx(name):
    path = os.path.join(SFX_DIR, name)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "2",
                          "-ar", str(SR), "-"], capture_output=True, check=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).reshape(-1, 2).T.astype(float)
    return x - x.mean(axis=1, keepdims=True)


def rms_env(m, win_s):
    w = max(1, smp(win_s))
    c = np.concatenate([[0.0], np.cumsum(m * m)])
    e = np.zeros(len(m))
    h = w // 2
    lo = np.clip(np.arange(len(m)) - h, 0, len(m))
    hi = np.clip(np.arange(len(m)) - h + w, 0, len(m))
    e = (c[hi] - c[lo]) / np.maximum(hi - lo, 1)
    return np.sqrt(np.maximum(e, 0))


def sfx_onset(x, thresh_db=-24.0):
    """First sample where the 2 ms RMS envelope rises within `thresh_db` of its maximum."""
    e = rms_env(x.mean(0), 0.002)
    return int(np.argmax(e > e.max() * db(thresh_db)))


def sfx_peak(x, win_s=0.02, band=None):
    m = x.mean(0)
    if band:
        m = zero_phase_band(m, *band)
    return int(np.argmax(rms_env(m, win_s)))


def trim_from(x, i, pre=0.001):
    """Drop everything before i - pre (with a short raised-cosine fade in)."""
    s = max(0, i - smp(pre))
    y = x[:, s:].copy()
    y[:, :smp(pre)] *= fade_in(smp(pre))[None, :]
    return y, i - s


# =============================================================================================
# 4. HARMONY / ARRANGEMENT DATA
# =============================================================================================
# (bar, beat, beats, name, pad voicing (MIDI), bass root (MIDI), arp notes (MIDI))
CHORDS = [
    (1, 0, 12, "Dmaj9", [57, 64, 66, 73], 38, None),
    (4, 0, 4, "Bm9", [57, 62, 66, 73], 35, None),
    (5, 0, 4, "Bm7", [57, 62, 66, 73], 35, [74, 78, 81, 85]),
    (6, 0, 4, "Gmaj9", [59, 62, 66, 69], 31, [74, 78, 81, 83]),
    (7, 0, 4, "Dadd9", [57, 64, 66, 69], 38, [74, 76, 78, 81]),
    (8, 0, 2, "Asus4", [57, 64, 69, 74], 33, [69, 74, 76, 81]),
    (8, 2, 2, "A", [57, 64, 69, 73], 33, [69, 73, 76, 81]),
    (9, 0, 4, "Gmaj9", [59, 62, 66, 69], 31, [74, 78, 81, 83]),
    (10, 0, 4, "Aadd9", [59, 61, 64, 69], 33, [71, 73, 76, 81]),
    (11, 0, 4, "Bm7", [57, 62, 66, 71], 35, [71, 74, 78, 81]),
    (12, 0, 4, "Gmaj9", [59, 62, 66, 69, 74], 31, [74, 78, 81, 83]),
    (13, 0, 4, "D/F#", [57, 62, 66, 69, 76], 30, [74, 78, 81, 86]),
    (14, 0, 2, "Em9", [55, 62, 66, 71, 74], 28, [71, 74, 78, 83]),
    (14, 2, 2, "A", [57, 61, 64, 69, 73], 33, [69, 73, 76, 81]),
]
END_CHORD = [50, 57, 64, 66, 73, 76, 81]          # Dmaj9, wide
END_STRUCK = [50, 62, 66, 69, 73, 76]
MIC_NOTES = [74, 78, 81, 85]                       # D5 F#5 A5 C#6
MIC_TIMES = [bt(3, b) for b in range(4)]           # 3.75, 4.21875, 4.6875, 5.15625
MIC_PANS = [-0.35, -0.12, 0.12, 0.35]
MOTIF = [(bt(8, 0), 81), (bt(8, 0.5), 88), (bt(8, 1), 86), (bt(8, 2), 85)]   # A5 E6 D6 .. C#6
KEY_TIMES = [23.4375, 23.55, 23.66, 23.78]
TYPING_WINDOW = (23.40, 23.86)


def chord_at(t):
    cur = None
    for c in CHORDS:
        if bt(c[0], c[1]) <= t + 1e-9:
            cur = c
    return cur


def chord_span(c):
    return bt(c[0], c[1]), bt(c[0], c[1]) + c[2] * BEAT


def tied_notes(chords, key):
    """Merge a pitch held across consecutive chords into one note: [(midi, t_on, t_off)]."""
    notes, active = [], {}
    for c in chords:
        t0, t1 = chord_span(c)
        pitches = key(c)
        pitches = pitches if isinstance(pitches, list) else [pitches]
        for p in list(active):
            if p not in pitches:
                notes.append((p, active.pop(p), t0))
        for p in pitches:
            active.setdefault(p, t0)
        last_end = t1
    for p, t0 in active.items():
        notes.append((p, t0, last_end))
    return sorted(notes, key=lambda n: (n[1], n[0]))


# Kick grid: bar 4 pre-drop (filtered), groove bars 5-8 and 10-14; none in the bar-9 breakdown,
# none at 18.75 (the impact takes that downbeat), none on 14.4 (breath before the end card).
PRE_KICKS = [bt(4, b) for b in range(4)]
KICKS = [bt(bar, b) for bar in list(range(5, 9)) + list(range(10, 15)) for b in range(4)
         if not (bar == 11 and b == 0) and not (bar == 14 and b == 3)]


# =============================================================================================
# 5. BUILD STEMS
# =============================================================================================
def build(verbose=True):
    stems = {}
    events = []            # (time, kind) of intended transients, used by the click scan

    def bus(name):
        return stems.setdefault(name, np.zeros((2, N)))

    def log(msg):
        if verbose:
            print(msg, flush=True)

    ir_a = make_ir("ir_a", rt_low=2.3, rt_high=1.0, length=3.2, predelay=0.022)
    ir_b = make_ir("ir_b", rt_low=3.4, rt_high=1.5, length=4.2, predelay=0.030, er_gain=0.25,
                   hp_f=160, lp_f=9000)
    sends_a, sends_b, sends_d = {}, {}, {}

    # ---- Pad (bars 1-14) ---------------------------------------------------------------
    log("  pad")
    r = rng_for("pad")
    pad = bus("pad")
    for midi, t0, t1 in tied_notes(CHORDS, lambda c: c[4]):
        first = t0 < 0.01
        att = 2.8 if first else 0.09
        rel = 0.04 if abs(t1 - END_CARD) < 1e-6 else 0.40
        place(pad, pad_note(midi, t1 - t0, att, rel, r), t0)
    t = np.arange(N) / SR
    pad_fc = [(0.0, 240), (1.875, 600), (3.6, 1300), (5.625, 1400), (7.45, 4500), (7.5, 4000),
              (20.55, 4000), (20.625, 4800), (26.25, 4800), (30.0, 4800)]
    stems["pad"] = pad = stft_filter(pad, lambda tc, f: lp_gain(keyframes(tc, pad_fc), f, 2))
    pad = filt(pad, butter("hp", 110, 4) + [biquad("peak", 450, 0.7, -3.0)])
    # level automation (groove = 1.0): swell from silence, sit under the plucks, open into the drop
    pad_gain = np.where(t < 3.2, (np.clip(t / 3.2, 0, 1)) ** 1.6, 1.0)
    pad_gain *= keyframes(t, [(0, 0.8), (5.625, 0.8), (7.45, 1.0), (14.99, 1.0), (15.0, 1.15),
                              (16.87, 1.15), (16.875, 1.0), (20.62, 1.0), (20.625, 1.05),
                              (30, 1.05)], log=False)
    stems["pad"] = pad * pad_gain[None, :]
    sends_a["pad"] = 0.22

    # ---- Air (intro breath) ------------------------------------------------------------
    r = rng_for("air")
    air = r.standard_normal((2, smp(7.5)))
    air = stft_filter(air, lambda tc, f: (np.clip(tc / 3.5, 0, 1) ** 2 * np.clip((7.5 - tc) / 1.5, 0, 1))[:, None]
                      * np.exp(-0.5 * (np.log2(np.maximum(f, 20) / 4200)[None, :] / 0.9) ** 2))
    place(bus("air"), air, 0.0)

    # ---- Drone (intro low D, then B in bar 4; hands over to the sub at the drop) ----------
    r = rng_for("drone")
    dr = bus("drone")
    for f0, t0, t1, att, rel in [(mtof(38), 0.0, bt(4), 3.0, 0.5), (mtof(35), bt(4), 7.5, 0.35, 0.06)]:
        L = smp(t1 - t0 + rel)
        y = sub_tone(np.full(L, f0), 1.0, (1.0, 0.25, 0.08)) + 0.3 * sub_tone(np.full(L, 2 * f0), 1.0, (1.0,))
        place(dr, y * env_asr(L, att, t1 - t0, rel), t0)
    stems["drone"] = lp(dr, 400)

    # ---- Bell bloom 1.875 --------------------------------------------------------------
    b = bell(mtof(81), 1.0, decay=1.0, length=6.0)
    place(bus("bell"), hp(b, 250), bt(2), pan=0.05)
    sends_a["bell"] = 0.55
    events.append((bt(2), "bell"))

    # ---- Four mic plucks ---------------------------------------------------------------
    r = rng_for("mic")
    for t0, m, p in zip(MIC_TIMES, MIC_NOTES, MIC_PANS):
        y = glass_pluck(mtof(m), 1.0, bright=1.0, decay=1.0, length=4.5, click=0.035, rng=r)
        place(bus("mic"), hp(y, 180), t0, pan=p)
        events.append((t0, "mic"))
    sends_a["mic"] = 0.30

    # ---- Reverse swell into 3.75 (reversed reverb of the first mic pluck) -------------
    dry = glass_pluck(mtof(74), 1.0, bright=1.0, decay=1.0, length=3.0, click=0.0)
    rs = reverse_swell(dry, ir_a, 1.8, fade=0.9)
    place_ending_at(bus("revswell"), hp(rs, 250), MIC_TIMES[0], gain=1.0)

    # ---- Bar 4 build: soft LP'd kicks, noise riser, reversed chord swell into 7.5 -------
    r = rng_for("prekick")
    for k, t0 in enumerate(PRE_KICKS):
        y = kick(vel=[0.5, 0.56, 0.62, 0.68][k], click=0.0, rng=r)
        y = lp(y, [260, 420, 700, 1200][k], 2)
        place(bus("kick"), y, t0)
        events.append((t0, "kick"))
    r = rng_for("riser1")
    place_ending_at(bus("riser"), hp(noise_riser(r, 1.875, 400, 5000), 300), 7.5, gain=1.0)
    dry = np.zeros(smp(3.0))
    for m in [59, 62, 66, 69, 74, 78]:
        dry += glass_pluck(mtof(m), 0.5, bright=0.8, decay=1.2, length=3.0, click=0.0)
    rs = reverse_swell(dry, ir_a, 1.7, fade=0.9)
    place_ending_at(bus("revswell"), hp(rs, 250), 7.5, gain=0.9)

    # riser.mp3: aligned by the end of its build (envelope peak of the >250 Hz band)
    rz = load_sfx("riser.mp3")
    rz = hp(rz, 280, 4)
    pk = sfx_peak(rz, 0.02, band=(250, None))
    for t_cue, length in [(7.5, 1.875), (END_CARD, 0.95)]:
        seg = rz[:, max(0, pk - smp(length)):pk].copy()
        seg[:, :smp(length * 0.6)] *= fade_in(smp(length * 0.6))[None, :]
        seg[:, -smp(0.006):] *= fade_out(smp(0.006))[None, :]
        place_ending_at(bus("riser_sfx"), seg / (np.abs(seg).max() + 1e-12), t_cue)

    # ---- Drop "air" (soft downlifter at 7.5) --------------------------------------------
    r = rng_for("down")
    L = smp(1.6)
    x = r.standard_normal((2, L))
    dl = stft_filter(x, lambda tc, f: (np.exp(-np.maximum(tc, 0) / 0.45))[:, None] * np.exp(
        -0.5 * (np.log2(np.maximum(f, 20)[None, :] / keyframes(tc, [(0, 9000), (1.6, 2200)])[:, None]) / 0.8) ** 2))
    dl[:, :smp(0.003)] *= fade_in(smp(0.003))[None, :]
    dl[:, -smp(0.05):] *= fade_out(smp(0.05))[None, :]
    place(bus("downlifter"), dl / np.abs(dl).max(), 7.5)
    sends_a["downlifter"] = 0.4

    # ---- Groove kick ----------------------------------------------------------------------
    log("  drums")
    r = rng_for("kick")
    for t0 in KICKS:
        place(bus("kick"), kick(vel=1.0, click=0.05, rng=r) * r.uniform(0.97, 1.0), t0)
        events.append((t0, "kick"))

    # ---- Hats + shaker (16ths, humanised velocity) ---------------------------------------
    r = rng_for("hats")
    hats = [hat_voice(r, True) for _ in range(6)]
    shks = [hat_voice(r, False) for _ in range(6)]
    for bar in range(5, 15):
        for step in range(16):
            t0 = bt(bar) + step * S16
            if TYPING_WINDOW[0] <= t0 <= TYPING_WINDOW[1] or t0 >= END_CARD - 1e-6:
                continue
            acc = [0.55, 0.30, 0.75, 0.34][step % 4]
            if bar == 14 and step >= 12:
                acc *= 1.0 + 0.12 * (step - 11)       # tiny lift into the end card
            place(bus("shaker"), shks[r.integers(6)], t0, gain=acc * r.uniform(0.86, 1.1), pan=-0.28)
            if step % 4 == 2:
                place(bus("hat"), hats[r.integers(6)], t0, gain=r.uniform(0.82, 1.0), pan=0.22)
            events.append((t0, "hat"))

    # ---- Claps (bars 12-14, beats 2 & 4) ------------------------------------------------
    r = rng_for("clap")
    for bar in (12, 13, 14):
        for b in (1, 3):
            if bar == 14 and b == 3:
                continue
            place(bus("clap"), hp(clap(r), 350), bt(bar, b), gain=r.uniform(0.9, 1.0), pan=0.04)
            events.append((bt(bar, b), "clap"))
    sends_a["clap"] = 0.30

    # ---- Sub + off-beat mid bass -----------------------------------------------------------
    log("  bass")
    groove = [c for c in CHORDS if c[0] >= 5]
    brk0, brk1 = bt(9), bt(10)                        # no sub in the bar-9 breakdown
    for midi, t0, t1 in tied_notes(groove, lambda c: c[5]):
        for a, b in ((t0, min(t1, brk0)), (max(t0, brk1), t1)):
            if b - a > 0.01:
                L = smp(b - a + 0.05)
                y = sub_tone(np.full(L, mtof(midi)), 1.0) * env_asr(L, 0.006, b - a, 0.045)
                place(bus("sub"), y, a)
    r = rng_for("midbass")
    for bar in list(range(5, 9)) + list(range(10, 15)):
        for b in range(4):
            t0 = bt(bar, b + 0.5)
            c = chord_at(t0)
            if t0 >= END_CARD - 0.2:
                continue
            f0 = mtof(c[5] + 12)
            L = smp(0.3)
            tt = np.arange(L) / SR
            y = 0.5 * (play_table(wavetable(f0, 700, 1.0, seed=11), np.full(L, f0 * 2 ** (4 / 1200)))
                       + play_table(wavetable(f0, 700, 1.0, seed=12), np.full(L, f0 * 2 ** (-4 / 1200)), 0.37))
            y *= np.exp(-tt / 0.11)
            y[:smp(0.0025)] *= fade_in(smp(0.0025))
            y[-smp(0.03):] *= fade_out(smp(0.03))
            place(bus("midbass"), y * r.uniform(0.9, 1.0), t0)
    stems["midbass"] = lp(hp(stems["midbass"], 60), 1400)

    # ---- Arp (ping-pong) ---------------------------------------------------------------
    log("  arp / motif")
    r = rng_for("arp")
    arp = bus("arp")
    pat8 = [0, 2, 1, 3, 2, 0, 3, 1]
    vel8 = [0.9, 0.62, 0.78, 0.6, 0.86, 0.6, 0.76, 0.64]
    pat16 = [0, 2, 1, 3, 2, 3, 1, 2, 0, 2, 1, 3, 2, 0, 3, 1]
    vel16 = [1.0, 0.5, 0.72, 0.5]
    arp_notes = []
    for bar in range(5, 15):
        if bar <= 11:
            for k in range(8):
                arp_notes.append((bt(bar, k * 0.5), pat8[k], vel8[k], 0.8))
        else:
            for k in range(16):
                arp_notes.append((bt(bar) + k * S16, pat16[k], vel16[k % 4] * 0.85, 0.9))
    for t0, idx, v, br in arp_notes:
        c = chord_at(t0)
        if c[6] is None or t0 >= END_CARD - 0.05:
            continue
        v *= r.uniform(0.9, 1.06)
        if 13.05 <= t0 < 14.9:
            v *= 0.5                          # make room for the motif
        if TYPING_WINDOW[0] - 0.05 <= t0 <= TYPING_WINDOW[1]:
            v *= 0.55                         # make room for the typing
        y = glass_pluck(mtof(c[6][idx]), v, bright=br, decay=0.28, length=1.2, click=0.012, rng=r)
        place(arp, y, t0, pan=r.uniform(-0.3, 0.3))
    arp_fc = [(0, 9000), (14.99, 9000), (15.0, 1100), (16.4, 1250), (16.875, 9000), (30, 9000)]
    arp = stft_filter(arp, lambda tc, f: lp_gain(keyframes(tc, arp_fc), f, 2))
    stems["arp"] = filt(arp, butter("hp", 250, 2) + [biquad("highshelf", 2500, 0.7, 3.0)])
    sends_a["arp"] = 0.18
    sends_d["arp"] = 0.55

    # ---- Motif at 13.125 ("beam locks on") ------------------------------------------------
    r = rng_for("motif")
    for k, (t0, m) in enumerate(MOTIF):
        y = glass_pluck(mtof(m), 1.0, bright=0.85, decay=1.1 if k == 3 else 0.7, length=3.5,
                        click=0.02, rng=r)
        y += 0.22 * bell(mtof(m + 12), 1.0, decay=0.5, length=3.5)[:len(y)]
        place(bus("motif"), hp(y, 250), t0, pan=[-0.1, 0.1, -0.05, 0.0][k])
        events.append((t0, "motif"))
    sends_a["motif"] = 0.32
    sends_d["motif"] = 0.35

    # ---- Warm swell at 12.1875: four detuned voices converge to unison and narrow -------
    r = rng_for("swell")
    L = smp(0.95)
    tt = np.arange(L) / SR
    conv = np.exp(-tt / 0.16)
    sw = np.zeros((2, L))
    for m, a in [(62, 1.0), (69, 0.7), (66, 0.35)]:
        for v, (cents, p) in enumerate([(-22, -0.8), (-8, -0.3), (8, 0.3), (22, 0.8)]):
            f = mtof(m) * 2 ** (cents * conv / 1200)
            ph = 2 * np.pi * np.cumsum(f) / SR + r.uniform(0, 6.28)
            y = a * (np.sin(ph) + 0.28 * np.sin(2 * ph) + 0.08 * np.sin(3 * ph))
            gl, gr = pan_gains(p * conv)
            sw[0] += y * gl
            sw[1] += y * gr
    att = 0.5 - 0.5 * np.cos(np.pi * np.clip(tt / 0.03, 0, 1))       # defined 30 ms onset ...
    rise = 0.35 + 0.65 * np.clip((tt - 0.03) / 0.42, 0, 1) ** 1.5      # ... then the swell
    fall = np.clip((0.95 - tt) / 0.5, 0, 1) ** 1.5
    e = att * rise * fall
    sw *= e[None, :]
    place(bus("swell"), hp(sw / np.abs(sw).max(), 150), 12.1875)
    sends_a["swell"] = 0.4
    events.append((12.1875, "swell"))

    # ---- SFX: whoosh 11.25 (peak aligned) -------------------------------------------------
    log("  sfx")
    wh = load_sfx("whoosh.mp3")
    wh = filt(wh, butter("hp", 180, 2) + butter("lp", 9000, 2), tail=0.2)
    pk = sfx_peak(wh, 0.02)
    place(bus("whoosh"), wh / np.abs(wh).max(), 11.25 - pk / SR)
    sends_a["whoosh"] = 0.15

    # ---- UI pop 15.9375 ----------------------------------------------------------------
    r = rng_for("pop")
    place(bus("pop"), hp(ui_pop(mtof(83)), 300), 15.9375, pan=0.1)
    sends_a["pop"] = 0.28
    events.append((15.9375, "pop"))

    # ---- Impact 18.75: synth sub hit (B1) + impact-bass-1 (onset aligned) + swipe ---------
    L = smp(1.8)
    tt = np.arange(L) / SR
    f = mtof(35) * (1 + 1.0 * np.exp(-tt / 0.035))
    y = sub_tone(f, 1.0, (1.0, 0.22, 0.07)) * np.where(tt < 0.03, 1, np.exp(-(tt - 0.03) / 0.42))
    y[:smp(0.0008)] *= fade_in(smp(0.0008))
    y[-smp(0.1):] *= fade_out(smp(0.1))
    place(bus("impact"), hp(y, 25), 18.75)
    events.append((18.75, "impact"))
    ib = load_sfx("impact-bass-1.mp3")
    on = sfx_onset(ib)
    ib, off = trim_from(ib, on)
    ib = ib[:, :smp(1.4)] * np.exp(-np.arange(smp(1.4)) / SR / 0.35)[None, :]
    ib[:, -smp(0.1):] *= fade_out(smp(0.1))[None, :]
    ib = filt(ib, butter("hp", 28, 2) + butter("lp", 900, 2), tail=0.2)
    place(bus("impact_sfx"), 0.5 * (ib[0] + ib[1]) / np.abs(ib).max(), 18.75 - off / SR)
    r = rng_for("swipe")
    Ls = smp(0.22)
    xs = r.standard_normal((2, Ls))
    swp = stft_filter(xs, lambda tc, f: (np.clip(tc / 0.03, 0, 1) * np.clip((0.22 - tc) / 0.15, 0, 1))[:, None]
                      * np.exp(-0.5 * (np.log2(np.maximum(f, 20)[None, :] / keyframes(tc, [(0, 5200), (0.22, 1300)])[:, None]) / 0.6) ** 2),
                      n=512, hop=128)
    swp[:, -smp(0.01):] *= fade_out(smp(0.01))[None, :]
    place(bus("swipe"), swp / np.abs(swp).max(), 18.75)
    sends_a["swipe"] = 0.3

    # ---- Key presses 23.4375 / .55 / .66 / .78 (onset aligned, 4 variations) --------------
    kp = load_sfx("key-press.mp3")
    kp = filt(kp, butter("hp", 160, 2) + butter("lp", 11000, 2), tail=0.1)
    on = sfx_onset(kp)
    kp, off = trim_from(kp, on)
    kp /= np.abs(kp).max()
    for t0, ratio, g, p in zip(KEY_TIMES, [1.0, 1.045, 0.97, 1.02], [1.0, 0.84, 0.92, 0.8],
                               [-0.08, 0.06, -0.03, 0.09]):
        y = resample_ratio(kp, ratio) if ratio != 1.0 else kp
        o = int(round(off / ratio))
        y = 0.5 * (y[0] + y[1])
        y[-smp(0.01):] *= fade_out(smp(0.01))
        place(bus("keys"), y, t0 - o / SR, gain=g, pan=p)
        events.append((t0, "key"))
    sends_a["keys"] = 0.08

    # ---- Chime 24.375 (E6 + B6 bell dyad) ------------------------------------------------
    ch = bell(mtof(88), 1.0, decay=0.55, length=3.5) + 0.55 * bell(mtof(95), 1.0, decay=0.45, length=3.5)
    place(bus("chime"), hp(ch, 400), 24.375, pan=0.12)
    sends_a["chime"] = 0.45
    events.append((24.375, "chime"))

    # ---- Riser 25.3 -> 26.25 --------------------------------------------------------------
    r = rng_for("riser2")
    place_ending_at(bus("riser"), hp(noise_riser(r, END_CARD - 25.3, 600, 5500, curve=2.0), 300),
                    END_CARD, gain=1.0)
    dry = np.zeros(smp(3.0))
    for m in END_STRUCK:
        dry += glass_pluck(mtof(m), 0.5, bright=0.8, decay=1.5, length=3.0, click=0.0)
    rs = reverse_swell(dry, ir_b, END_CARD - 25.3 + 0.35, fade=0.6)
    place_ending_at(bus("revswell"), hp(rs, 250), END_CARD, gain=1.0)

    # ---- END CARD 26.25 ------------------------------------------------------------------
    log("  end card")
    r = rng_for("end")
    gate = DUR - END_CARD
    L = smp(gate)
    tt = np.arange(L) / SR
    e = np.concatenate([fade_in(smp(0.025)), np.ones(L - smp(0.025))]) * np.exp(-tt / 4.0)
    endp = np.zeros((2, L))
    for m in END_CHORD:
        endp += pad_note(m, gate, 0.025, 0.0, r, voices=5, detune=11.0, spread=0.95,
                         rolloff=5500.0, sine=0.3, env=e)[:, :L]
    endp = stft_filter(endp, lambda tc, f: lp_gain(keyframes(tc, [(0, 5200), (1.0, 3600), (4.0, 1800)]), f, 2))
    place(bus("end_pad"), hp(endp, 120, 4), END_CARD)
    struck = np.zeros(smp(3.75))
    for m in END_STRUCK:
        struck += glass_pluck(mtof(m), 0.6, bright=0.75, decay=2.0, length=3.75, click=0.01, rng=r)
    place(bus("end_struck"), hp(struck, 140), END_CARD)
    f = mtof(38) * (1 + 0.6 * np.exp(-tt / 0.03))
    sub = sub_tone(f, 1.0, (1.0, 0.2, 0.06)) * np.where(tt < 0.04, 1, np.exp(-(tt - 0.04) / 1.3))
    sub[:smp(0.0008)] *= fade_in(smp(0.0008))
    place(bus("end_sub"), hp(sub, 25), END_CARD)
    place(bus("end_thump"), kick(vel=1.0, f_hi=130, f_lo=44, amp_tau=0.2, length=1.6, click=0.04, rng=r), END_CARD)
    ib = load_sfx("impact-bass-1.mp3")
    on = sfx_onset(ib)
    ib, off = trim_from(ib, on)
    ib = ib[:, :smp(0.9)] * np.exp(-np.arange(smp(0.9)) / SR / 0.2)[None, :]
    ib[:, -smp(0.1):] *= fade_out(smp(0.1))[None, :]
    ib = filt(ib, butter("hp", 28, 2) + butter("lp", 900, 2), tail=0.2)
    place(bus("impact_sfx"), 0.5 * (ib[0] + ib[1]) / np.abs(ib).max(), END_CARD - off / SR, gain=0.8)
    xa = r.standard_normal((2, smp(1.5)))
    air_b = filt(xa, butter("hp", 300, 2) + butter("lp", 4500, 2), tail=0.1) * np.exp(-np.arange(smp(1.5)) / SR / 0.3)[None, :]
    air_b[:, :smp(0.002)] *= fade_in(smp(0.002))[None, :]
    air_b[:, -smp(0.1):] *= fade_out(smp(0.1))[None, :]
    place(bus("end_air"), air_b / np.abs(air_b).max(), END_CARD)
    events += [(END_CARD, "impact")]
    b = bell(mtof(93), 1.0, decay=1.1, length=DUR - 27.1875)
    place(bus("end_bell"), hp(b, 300), 27.1875, pan=0.08)
    events.append((27.1875, "bell"))
    sends_b.update({"end_pad": 0.35, "end_struck": 0.4, "end_air": 1.0, "end_bell": 0.6})

    return stems, (sends_a, sends_b, sends_d), (ir_a, ir_b), events


# =============================================================================================
# 6. MIX (levels, sidechain, sends/returns, "everything stops" at the end card)
# =============================================================================================
BASS_STEMS = {"kick", "sub", "drone", "impact", "impact_sfx", "end_sub", "end_thump", "midbass"}
END_STEMS = {"end_pad", "end_struck", "end_sub", "end_thump", "end_air", "end_bell", "impact_sfx"}


def mix(stems, sends, irs, verbose=True):
    sends_a, sends_b, sends_d = sends
    ir_a, ir_b = irs
    t = np.arange(N) / SR

    # sidechain: pump pad / bass / arp from the kick (and duck the sub under the impacts)
    duck_bass = duck_env(KICKS, 0.5, release=0.21) * duck_env(PRE_KICKS, 0.3, release=0.2) \
        * duck_env([18.75], 0.9, release=0.7)
    duck_pad = duck_env(KICKS, 0.30, release=0.26) * duck_env(PRE_KICKS, 0.12, release=0.25)
    duck_arp = duck_env(KICKS, 0.16, release=0.18)
    sc = {"sub": duck_bass, "midbass": duck_env(KICKS, 0.4, release=0.15), "pad": duck_pad,
          "arp": duck_arp}

    # hats: low-passed and quieter in the bar-9 breakdown
    for k in ("hat", "shaker"):
        fc = [(0, 18000), (14.99, 18000), (15.0, 6000), (16.4, 6500), (16.875, 18000), (30, 18000)]
        stems[k] = stft_filter(stems[k], lambda tc, f, fc=fc: lp_gain(keyframes(tc, fc), f, 2))
        stems[k] *= keyframes(t, [(0, 1), (14.99, 1), (15.0, 0.6), (16.87, 0.6), (16.875, 1), (30, 1)], log=False)

    # the end-card gate: every non-end-card element (and the groove reverb/delay) stops at 26.25
    stop = np.ones(N)
    a, b = smp(END_CARD - 0.004), smp(END_CARD + 0.03)
    stop[a:b] = fade_out(b - a)
    stop[b:] = 0.0

    dry = {}
    for name, x in stems.items():
        y = x * db(MIX[name])
        if name in sc:
            y = y * sc[name][None, :]
        if name not in END_STEMS:
            y = y * stop[None, :]
        dry[name] = y

    rev_a_in = sum(dry[k] * v for k, v in sends_a.items())
    rev_b_in = sum(dry[k] * v for k, v in sends_b.items())
    del_in = sum(dry[k] * v for k, v in sends_d.items())
    if verbose:
        print("  reverb / delay", flush=True)
    ret_a = convolve(rev_a_in, ir_a)
    ret_a = filt(ret_a, butter("hp", 220, 2) + butter("lp", 8500, 2)) * duck_env(KICKS, 0.18, 0.25)[None, :]
    ret_a *= stop[None, :]
    ret_b = filt(convolve(rev_b_in, ir_b), butter("hp", 170, 2) + butter("lp", 9000, 2))
    delay = pingpong(del_in, 0.75 * BEAT, fb=0.38, taps=7, lp_f=3600, hp_f=350)
    delay *= stop[None, :]
    returns = {"rev_a": ret_a * db(MIX["rev_a"]), "rev_b": ret_b * db(MIX["rev_b"]),
               "delay": delay * db(MIX["delay"])}
    return dry, returns


# =============================================================================================
# 7. MASTER (mono lows, gentle glue, oversampled soft clip, true-peak limiter, loudness, fade)
# =============================================================================================
K_PRE = (np.array([1.53512485958697, -2.69169618940638, 1.19839281085285]),
         np.array([1.0, -1.69065929318241, 0.73248077421585]))
K_RLB = (np.array([1.0, -2.0, 1.0]), np.array([1.0, -1.99004745483398, 0.99007225036621]))


def lufs_blocks(x, block=0.4, hop=0.1):
    y = filt(x, [K_PRE, K_RLB], tail=1.0)
    blk, step = smp(block), smp(hop)
    c = np.concatenate([[0.0], np.cumsum((y * y).sum(0))])
    st = np.arange(0, y.shape[1] - blk + 1, step)
    z = (c[st + blk] - c[st]) / blk
    return z, st


def lufs_integrated(x):
    """ITU-R BS.1770-4 integrated loudness (48 kHz K-weighting, -70 / -10 LU gates)."""
    z, _ = lufs_blocks(x)
    l = -0.691 + 10 * np.log10(z + 1e-20)
    z1 = z[l > -70]
    rel = -0.691 + 10 * np.log10(z1.mean()) - 10
    z2 = z[(l > -70) & (l > rel)]
    return -0.691 + 10 * np.log10(z2.mean())


def true_peak_db(x):
    return 20 * np.log10(np.abs(resample_up(x, 4)).max() + 1e-12)


def mono_lows(x, f=150.0):
    m, s = 0.5 * (x[0] + x[1]), 0.5 * (x[0] - x[1])
    s = hp(s, f, 4)
    return np.vstack([m + s, m - s])


def glue_comp(x, thresh_db=-20.0, ratio=1.7, attack=0.02, release=0.25, knee=6.0, block=32):
    """Gentle feed-forward bus compressor (10 ms RMS detector with a 90 Hz sidechain high-pass so
    the sub does not pump the mix, soft knee)."""
    m = hp(0.5 * (x[0] + x[1]), 90, 2)
    ms = rms_env(m, 0.01) ** 2
    nb = len(m) // block
    lvl = 10 * np.log10(ms[:nb * block].reshape(nb, block).mean(1) + 1e-12)
    over = lvl - thresh_db
    s = 1 - 1 / ratio
    gr = np.where(over <= -knee / 2, 0.0,
                  np.where(over >= knee / 2, s * over, s * (over + knee / 2) ** 2 / (2 * knee)))
    aa, ar = np.exp(-block / (attack * SR)), np.exp(-block / (release * SR))
    g = np.empty(nb)
    v = 0.0
    for i in range(nb):
        c = aa if gr[i] > v else ar
        v = c * v + (1 - c) * gr[i]
        g[i] = v
    gdb = np.interp(np.arange(len(m)), (np.arange(nb) + 0.5) * block, g)
    return x * db(-gdb)[None, :], g


def soft_clip(x, thresh=0.8, os_=4):
    """tanh knee above `thresh`, run 4x oversampled so the new harmonics do not alias."""
    up = resample_up(x, os_)
    a = np.abs(up)
    k = 1 - thresh
    y = np.where(a <= thresh, up, np.sign(up) * (thresh + k * np.tanh((a - thresh) / k)))
    return resample_down(y, os_)


def tp_limiter(x, ceiling_db=LIMIT_DBTP, look=0.0015, release=0.06):
    """Look-ahead true-peak limiter: gain computed from the 4x-oversampled peak envelope,
    forward sliding-min over the look-ahead, block release, then a moving-average smoother
    (the smoothed gain never exceeds the required gain at any sample)."""
    c = db(ceiling_db)
    L = x.shape[1]
    pk = np.abs(resample_up(x, 4)).max(0).reshape(L, 4).max(1)
    pk = np.maximum(pk, np.concatenate([pk[1:], pk[-1:]]))
    greq = np.minimum(1.0, c / np.maximum(pk, 1e-12))
    W = smp(look)
    g1 = sliding_min_fwd(greq, W + 1)
    B = 16
    nb = int(np.ceil(L / B))
    gb = np.concatenate([g1, np.ones(nb * B - L)]).reshape(nb, B).min(1)
    rr = np.exp(-B / (release * SR))
    out = np.empty(nb)
    s = 1.0
    for i in range(nb):
        v = gb[i]
        s = v if v < s else v - (v - s) * rr
        out[i] = s
    g2 = np.repeat(out, B)[:L]
    cs = np.concatenate([np.full(W + 1, g2[0]), g2]).cumsum()
    g3 = (cs[W + 1:] - cs[:-(W + 1)]) / (W + 1)
    g3 = np.minimum(g3, greq)
    return x * g3[None, :], g3


def master_fade():
    """Unity until 28.6 s, raised-cosine^1.3 to exactly 0 at 29.95 s, zero afterwards."""
    fade = np.ones(N)
    a, b = smp(FADE_START), smp(FADE_END)
    u = np.arange(b - a) / (b - a)
    fade[a:b] = (0.5 + 0.5 * np.cos(np.pi * u)) ** 1.3
    fade[b:] = 0.0
    return fade


def master(mix_sum, gain_db):
    """pre-gain -> glue comp -> 4x soft clip -> true-peak limiter -> tail fade."""
    x = mix_sum * db(gain_db)
    x, gr = glue_comp(x, thresh_db=-19.0)
    x = soft_clip(x, thresh=0.82 * db(LIMIT_DBTP))
    x, g = tp_limiter(x)
    return x * master_fade()[None, :], gr, g


def render(verbose=True):
    print("building stems ...", flush=True)
    stems, sends, irs, events = build(verbose)
    print("mixing ...", flush=True)
    dry, returns = mix(stems, sends, irs, verbose)
    total = sum(dry.values()) + sum(returns.values())
    total = hp(total, 20, 2)
    total = mono_lows(total, 150.0)
    total = filt(total, [biquad("peak", 4500, 0.6, 2.5), biquad("highshelf", 11000, 0.7, -1.5)])
    print("mastering ...", flush=True)
    gain = TARGET_LUFS - lufs_integrated(total)
    for it in range(6):
        out, gr, g = master(total, gain)
        L = lufs_integrated(out)
        if verbose:
            grv = gr[smp(7.5) // 32:smp(26.2) // 32]
            print(f"  iter {it}: pre-gain {gain:+.2f} dB -> {L:.2f} LUFS, TP {true_peak_db(out):.2f} dBTP,"
                  f" glue GR groove mean {grv.mean():.1f} / max {gr.max():.1f} dB @ {gr.argmax() * 32 / SR:.2f} s,"
                  f" limiter max {-20 * np.log10(g.min()):.1f} dB @ {g.argmin() / SR:.2f} s", flush=True)
        if abs(L - TARGET_LUFS) < 0.02:
            break
        gain += (TARGET_LUFS - L)
    return out, dry, returns, stems, events


# =============================================================================================
# 8. WRITE 24-bit WAV
# =============================================================================================
def write_wav24(path, x):
    assert x.shape == (2, N), x.shape
    q = np.clip(np.round(x.T * 8388607.0), -8388608, 8388607).astype("<i4")
    b = q.reshape(-1, 1).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(SR)
        w.writeframes(b)


def read_wav24(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 3 and w.getframerate() == SR
        n, ch = w.getnframes(), w.getnchannels()
        raw = np.frombuffer(w.readframes(n), np.uint8).reshape(-1, 3)
    v = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16))
    v = np.where(v >= 1 << 23, v - (1 << 24), v)
    return (v.reshape(-1, ch).T / 8388608.0), n


# =============================================================================================
# 9. QA: ffmpeg loudness, DC, click scan, cue onsets, spectrogram
# =============================================================================================
def ffmpeg_ebur128(path):
    p = subprocess.run(["ffmpeg", "-nostats", "-hide_banner", "-i", path, "-af", "ebur128=peak=true",
                        "-f", "null", "-"], capture_output=True, text=True)
    s = p.stderr[p.stderr.rfind("Summary:"):]
    I = float(re.search(r"I:\s+(-?[\d.]+) LUFS", s).group(1))
    LRA = float(re.search(r"LRA:\s+(-?[\d.]+) LU", s).group(1))
    tp = float(re.search(r"True peak:\s+Peak:\s+(-?[\d.inf]+) dBFS", s).group(1))
    return I, LRA, tp


def onset_time(x, t_cue, band, search=(-0.04, 0.04), pre=0.03, post=0.006):
    """Onset = argmax of the energy ratio (next 6 ms) / (previous 30 ms) of a zero-phase
    band-limited signal; for a step onset the maximum sits exactly on the first new sample."""
    m = 0.5 * (x[0] + x[1]) if x.ndim == 2 else x
    a, b = smp(t_cue - 0.5), smp(t_cue + 0.5)
    seg = zero_phase_band(m[max(0, a):b], *band) if band else m[max(0, a):b]
    c = np.concatenate([[0.0], np.cumsum(seg * seg)])
    npre, npost, gap = smp(pre), smp(post), smp(0.0005)
    n = np.arange(smp(0.5 + search[0]), smp(0.5 + search[1]))
    e_post = (c[n + npost] - c[n]) / npost
    e_pre = (c[n - gap] - c[n - gap - npre]) / npre
    r = 10 * np.log10((e_post + 1e-18) / (e_pre + 1e-18))
    i = int(np.argmax(r))
    return (max(0, a) + n[i]) / SR, r[i]


CUE_CHECKS = [
    # (label, intended time, band (lo, hi), stem for the isolated check, mode)
    ("bell bloom A5", 1.875, (600, 5000), "bell", "onset"),
    ("mic pluck 1 D5", 3.75, (1200, 8000), "mic", "onset"),
    ("mic pluck 2 F#5", 4.21875, (1200, 8000), "mic", "onset"),
    ("mic pluck 3 A5", 4.6875, (1200, 8000), "mic", "onset"),
    ("mic pluck 4 C#6", 5.15625, (1200, 8000), "mic", "onset"),
    ("first soft kick", 5.625, (35, 200), "kick", "onset"),
    ("DROP kick", 7.5, (35, 200), "kick", "onset"),
    ("whoosh (peak)", 11.25, (200, 4000), "whoosh", "peak"),
    ("warm swell", 12.1875, (150, 1500), "swell", "onset"),
    ("motif A5", 13.125, (1500, 8000), "motif", "onset"),
    ("breakdown (kick out)", 15.0, None, None, "none"),
    ("UI pop", 15.9375, (600, 4000), "pop", "onset"),
    ("kick returns", 16.875, (35, 200), "kick", "onset"),
    ("impact", 18.75, (25, 150), "impact", "onset"),
    ("key 1", 23.4375, (3500, 12000), "keys", "onset"),
    ("key 2", 23.55, (3500, 12000), "keys", "onset"),
    ("key 3", 23.66, (3500, 12000), "keys", "onset"),
    ("key 4", 23.78, (3500, 12000), "keys", "onset"),
    ("chime", 24.375, (2500, 9000), "chime", "onset"),
    ("END impact", 26.25, (25, 150), "end_sub", "onset"),
    ("final bell A6", 27.1875, (1200, 9000), "end_bell", "onset"),
]


def peak_time(x, t_cue, band, win=0.25):
    m = 0.5 * (x[0] + x[1])
    a, b = smp(t_cue - win), smp(t_cue + win)
    seg = zero_phase_band(m[a:b], *band)
    e = rms_env(seg, 0.02)
    return (a + int(np.argmax(e))) / SR


def qa(out, dry, returns, stems, events):
    print("\n=== QA ===")
    I, LRA, tp = ffmpeg_ebur128(OUT_WAV)
    print(f"ffmpeg ebur128: integrated {I:.1f} LUFS, LRA {LRA:.1f} LU, true peak {tp:.1f} dBTP")
    x, n = read_wav24(OUT_WAV)
    print(f"file: {n} frames ({n / SR:.6f} s), 2 ch, 24-bit, {SR} Hz; "
          f"internal BS.1770 {lufs_integrated(x):.2f} LUFS, 4x TP {true_peak_db(x):.2f} dBTP")
    dc = x.mean(1)
    print(f"DC offset: L {dc[0]:+.2e}  R {dc[1]:+.2e} (full scale = 1.0)")
    zero_from = smp(FADE_END)
    print(f"samples from 29.95 s on exactly zero: {bool(np.all(x[:, zero_from:] == 0))}; "
          f"last 50 ms zero: {bool(np.all(x[:, -smp(0.05):] == 0))}; first sample {x[:, 0]}")
    # max sample-to-sample jump relative to what a band-limited signal of that level allows
    d = np.abs(np.diff(x, axis=1)).max()
    print(f"max |x[n]-x[n-1]| = {d:.4f} (peak {np.abs(x).max():.4f})")

    # stereo: lows must be mono, fold-down must not lose level
    mid, side = 0.5 * (x[0] + x[1]), 0.5 * (x[0] - x[1])
    lo_m = zero_phase_band(mid, None, 150, order=8)
    lo_s = zero_phase_band(side, None, 150, order=8)
    print(f"stereo: side/mid energy below 150 Hz {10 * np.log10(np.sum(lo_s ** 2) / np.sum(lo_m ** 2) + 1e-30):.1f} dB;"
          f" overall side/mid {10 * np.log10(np.sum(side ** 2) / np.sum(mid ** 2)):.1f} dB;"
          f" L/R correlation {np.corrcoef(x[0], x[1])[0, 1]:.2f};"
          f" mono fold-down {lufs_integrated(np.vstack([mid, mid])) - lufs_integrated(x):+.2f} LU")
    # click scan: HF (>12 kHz) 2 ms frames that jump > 18 dB above their 60 ms neighbourhood,
    # outside +-4 ms of any intended transient
    m = 0.5 * (x[0] + x[1])
    hfs = zero_phase_band(m, 12000, None)
    fr = smp(0.002)
    nfr = len(hfs) // fr
    lev = 10 * np.log10((hfs[:nfr * fr].reshape(nfr, fr) ** 2).mean(1) + 1e-20)
    k = 15
    padl = np.pad(lev, k, mode="edge")
    neigh = np.array([np.median(np.concatenate([padl[i:i + k], padl[i + k + 1:i + 2 * k + 1]])) for i in range(nfr)])
    spikes = np.where((lev - neigh > 18) & (lev > -80))[0]
    ev_t = np.array([e[0] for e in events])
    unmatched = [i * fr / SR for i in spikes if np.min(np.abs(ev_t - i * fr / SR)) > 0.004]
    print(f"click scan (HF spikes): {len(spikes)} spikes, {len(unmatched)} not at an intended transient"
          + (f": {[round(u, 4) for u in unmatched[:10]]}" if unmatched else ""))
    # tonal stems contain no partials above ~12 kHz, so a discontinuity (click) would show up as
    # broadband energy above 15 kHz. Stems get the master tail fade first (as heard).
    for name in ("pad", "mic", "bell", "arp", "sub", "drone", "motif", "swell", "end_pad", "end_struck",
                 "end_bell", "end_sub"):
        s = dry.get(name)
        if s is None:
            continue
        mm = 0.5 * (s[0] + s[1]) * master_fade()
        h = zero_phase_band(mm, 15000, None, order=8)
        pkh = 20 * np.log10(np.abs(h).max() + 1e-12)
        pks = 20 * np.log10(np.abs(mm).max() + 1e-12)
        print(f"  stem {name:10s} peak {pks:6.1f} dBFS, above 15 kHz {pkh:6.1f} dBFS ({pkh - pks:6.1f} dB rel)")

    # SFX must sit under the music: compare each SFX peak with the music-only peak around it
    sfx_names = ("whoosh", "keys", "impact_sfx", "riser_sfx", "pop", "chime", "swipe")
    music = sum(v for k, v in dry.items() if k not in sfx_names) + sum(returns.values())
    print("SFX peak vs music peak in the same window (pre-master; must be < 0 dB):")
    for name, t0 in [("whoosh", 11.25), ("pop", 15.9375), ("impact_sfx", 18.75), ("swipe", 18.75),
                     ("keys", 23.4375), ("chime", 24.375), ("riser_sfx", 26.0), ("impact_sfx", END_CARD)]:
        a_, b_ = smp(t0 - 0.3), smp(t0 + 0.5)
        ps = np.abs(dry[name][:, a_:b_]).max()
        pm = np.abs(music[:, a_:b_]).max()
        print(f"  {name:10s} @ {t0:7.4f}: {20 * np.log10(ps / pm):+5.1f} dB")

    print("\ncue onsets. mix: argmax of (6 ms after / 30 ms before) band energy ratio on the rendered file;"
          " stem: same ratio on the isolated, unfiltered element")
    print(f"  {'cue':22s} {'intended':>9s} {'mix':>9s} {'d(ms)':>6s} {'stem':>9s} {'d(ms)':>6s}")
    rise = 0.0
    rows = []
    for label, tc, band, stem, mode in CUE_CHECKS:
        if mode == "none":
            continue
        s = dry.get(stem)
        if mode == "peak":
            tm = peak_time(x, tc, band)
            ts = peak_time(s, tc, band)
        else:
            tm, rise = onset_time(x, tc, band)
            ts, _ = onset_time(s, tc, None)
        rows.append((label, tc, tm, ts))
        extra = f"   (rise {rise:+.1f} dB, band {band[0]}-{band[1]} Hz)" if mode == "onset" else "   (20 ms-RMS peak)"
        print(f"  {label:22s} {tc:9.4f} {tm:9.4f} {1000 * (tm - tc):+6.1f} {ts:9.4f} {1000 * (ts - tc):+6.1f}{extra}")
    # section energy checks
    def lvl(t0, t1, band=None):
        seg = m[smp(t0):smp(t1)]
        if band:
            seg = zero_phase_band(seg, *band)
        return 10 * np.log10(np.mean(seg ** 2) + 1e-20)
    print("\nsection RMS (dBFS): intro 0-3.75 %.1f | plucks 3.75-5.6 %.1f | build 5.6-7.5 %.1f | groove 7.5-15 %.1f |"
          " breakdown 15-16.9 %.1f | groove 16.9-26.25 %.1f | end 26.25-28.6 %.1f | fade 28.6-29.95 %.1f" % (
              lvl(0, 3.75), lvl(3.75, 5.6), lvl(5.6, 7.5), lvl(7.5, 15), lvl(15, 16.875), lvl(16.875, 26.25),
              lvl(26.25, 28.6), lvl(28.6, 29.95)))
    lows = [lvl(a, b, (30, 120)) for a, b in [(14.5, 15.0), (15.0, 16.8)]]
    print(f"  low band 30-120 Hz: bar 8 end {lows[0]:.1f} dB -> bar 9 {lows[1]:.1f} dB (kick/sub out)")
    # long-term spectrum in the groove (1/1-octave bands, relative to 125 Hz band)
    seg = m[smp(7.5):smp(26.2)]
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / SR)
    bands = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    e = [10 * np.log10(S[(f >= c / np.sqrt(2)) & (f < c * np.sqrt(2))].sum() + 1e-20) for c in bands]
    print("  groove octave spectrum (dB rel 125 Hz): " + " ".join(f"{c:g}:{v - e[2]:+.1f}" for c, v in zip(bands, e)))
    # per-stem loudness in the groove (pre-master, relative)
    print("  stem loudness bars 5-8 (pre-master LUFS, ungated):")
    a, b = smp(7.5), smp(15.0)
    allz = []
    for name, s in list(dry.items()) + list(returns.items()):
        z, _ = lufs_blocks(s[:, a:b])
        v = -0.691 + 10 * np.log10(z.mean() + 1e-20)
        if v > -60:
            allz.append((v, name))
    print("   " + ", ".join(f"{n} {v:.1f}" for v, n in sorted(allz, reverse=True)))
    return I, LRA, tp, rows


def render_png(x, path, events):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("Pillow not available; skipping PNG")
        return
    try:
        font = ImageFont.load_default(size=13)
        small = ImageFont.load_default(size=11)
    except TypeError:
        font = small = ImageFont.load_default()
    anchors = np.array([[0, 0, 4], [28, 16, 68], [79, 18, 123], [129, 37, 129], [181, 54, 122],
                        [229, 80, 100], [251, 135, 97], [254, 194, 135], [252, 253, 191]], float)

    def cmap(v):
        v = np.clip(v, 0, 1) * (len(anchors) - 1)
        i = np.minimum(v.astype(int), len(anchors) - 2)
        fr = (v - i)[..., None]
        return (anchors[i] * (1 - fr) + anchors[i + 1] * fr).astype(np.uint8)

    m = 0.5 * (x[0] + x[1])

    def spec(t0, t1, W, H, n=4096, fmin=25.0, fmax=20000.0, floor=-105.0, top=-15.0):
        a, b = smp(t0), smp(t1)
        cols = np.linspace(a, b, W, endpoint=False).astype(int)
        win = np.hanning(n)
        pad = np.concatenate([np.zeros(n), m, np.zeros(n)])
        frames = np.stack([pad[c + n // 2:c + n // 2 + n] for c in cols]) * win
        S = np.abs(np.fft.rfft(frames, axis=1)) * 2 / win.sum()
        fb = np.fft.rfftfreq(n, 1 / SR)
        fl = np.geomspace(fmin, fmax, H + 1)
        img = np.zeros((H, W))
        for r in range(H):
            sel = (fb >= fl[r]) & (fb < fl[r + 1])
            col = S[:, sel].max(1) if sel.any() else S[:, np.argmin(np.abs(fb - fl[r]))]
            img[H - 1 - r] = col
        dbv = 20 * np.log10(img + 1e-12)
        return cmap((dbv - floor) / (top - floor)), fl

    def panel(t0, t1, W, H, title, marks, floor=-105.0):
        rgb, fl = spec(t0, t1, W, H, floor=floor)
        im = Image.fromarray(rgb, "RGB")
        d = ImageDraw.Draw(im)
        for fr_ in (50, 100, 200, 500, 1000, 2000, 5000, 10000):
            y = H - 1 - int(np.log(fr_ / 25) / np.log(20000 / 25) * H)
            d.line([(0, y), (6, y)], fill=(200, 200, 200))
            d.text((8, y - 7), f"{fr_ // 1000}k" if fr_ >= 1000 else str(fr_), fill=(200, 200, 200), font=small)
        for bar in range(1, 17):
            tb = bt(bar)
            if t0 <= tb <= t1:
                xx = int((tb - t0) / (t1 - t0) * W)
                d.line([(xx, 0), (xx, H)], fill=(90, 90, 110))
        for tc, lab in marks:
            if t0 <= tc <= t1:
                xx = int((tc - t0) / (t1 - t0) * W)
                d.line([(xx, 0), (xx, 10)], fill=(80, 230, 255), width=2)
        d.text((W - 8 - 7 * len(title), 4), title, fill=(255, 255, 255), font=font)
        return im

    def wave_strip(t0, t1, W, H):
        im = Image.new("RGB", (W, H), (12, 12, 18))
        d = ImageDraw.Draw(im)
        a, b = smp(t0), smp(t1)
        edges = np.linspace(a, b, W + 1).astype(int)
        for i in range(W):
            s = m[edges[i]:max(edges[i] + 1, edges[i + 1])]
            lo_, hi_ = s.min(), s.max()
            r_ = np.sqrt(np.mean(s * s))
            d.line([(i, H / 2 - hi_ * H / 2), (i, H / 2 - lo_ * H / 2)], fill=(90, 150, 220))
            d.line([(i, H / 2 - r_ * H / 2), (i, H / 2 + r_ * H / 2)], fill=(170, 210, 255))
        for bar in range(1, 17):
            tb = bt(bar)
            if t0 <= tb <= t1:
                xx = int((tb - t0) / (t1 - t0) * W)
                d.line([(xx, 0), (xx, H)], fill=(70, 70, 90))
        return im

    marks = [(c[1], c[0]) for c in CUE_CHECKS]
    Wf, Hf = 2400, 640
    full = panel(0, DUR, Wf, Hf, "full mix 0-30 s (log f 25 Hz-20 kHz, -105..-15 dBFS)", marks)
    wf = wave_strip(0, DUR, Wf, 150)
    zw = Wf // 3 - 6
    z1 = panel(3.6, 5.6, zw, 360, "3.6-5.6 s: four mic plucks (-80..-15)", marks, floor=-80.0)
    z2 = panel(22.9, 24.9, zw, 360, "22.9-24.9 s: keys + chime (-80..-15)", marks, floor=-80.0)
    z3 = panel(25.2, 30.0, zw, 360, "25.2-30 s: riser, end card, fade (-80..-15)", marks, floor=-80.0)
    canvas = Image.new("RGB", (Wf, 30 + Hf + 26 + 150 + 30 + 360 + 10), (8, 8, 12))
    d = ImageDraw.Draw(canvas)
    d.text((10, 8), "earwing score - spectrogram (bar lines grey, cue markers cyan)", fill=(255, 255, 255), font=font)
    canvas.paste(full, (0, 30))
    y0 = 30 + Hf
    for s_ in range(0, 31):
        xx = int(s_ / DUR * Wf)
        d.line([(xx, y0), (xx, y0 + 5)], fill=(200, 200, 200))
        if s_ % 2 == 0:
            d.text((xx + 2, y0 + 6), f"{s_}s", fill=(200, 200, 200), font=small)
    canvas.paste(wf, (0, y0 + 26))
    y1 = y0 + 26 + 150 + 30
    for k, z in enumerate((z1, z2, z3)):
        canvas.paste(z, (k * (zw + 9), y1))
    canvas.save(path)
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-qa", action="store_true")
    ap.add_argument("--no-png", action="store_true")
    args = ap.parse_args()
    out, dry, returns, stems, events = render()
    write_wav24(OUT_WAV, out)
    print(f"wrote {OUT_WAV}")
    if not args.no_qa:
        qa(out, dry, returns, stems, events)
    if not args.no_png:
        render_png(read_wav24(OUT_WAV)[0], OUT_PNG, events)


if __name__ == "__main__":
    main()
