#!/usr/bin/env python3
"""Offline analysis of a gulltoppr /raw.wav capture (4 channels, 24-bit).

    curl -o cap.wav 'http://gulltoppr.local:81/raw.wav?s=5'
    python3 tools/analyze_raw.py cap.wav              # levels + pair lags
    python3 tools/analyze_raw.py cap.wav --event      # loudest 100 ms (clap test)
    python3 tools/analyze_raw.py cap.wav --doa        # SRP-PHAT per frame
    python3 tools/analyze_raw.py cap.wav --polar      # beam response vs angle
    python3 tools/analyze_raw.py --selftest           # check this script

Conventions match code/audio_config.h: theta is measured from broadside,
positive toward Mic 4 (+x); "lead" of mic j over mic i is how many samples
earlier j hears the sound. The WAV holds capture channels (ch1 = I2S0 left =
Mic 1 ... ch4 = I2S1 right = Mic 4); --map says which geometry position each
channel's mic sits at, exactly like /control?map=.

Needs numpy only; --plot additionally needs matplotlib.
"""
import argparse
import struct
import sys

import numpy as np

C = 343.0            # m/s, as SPEED_OF_SOUND in audio_config.h
BAND_LO_HZ = 300.0
ALIAS_MARGIN = 0.96
NFFT, HOP = 512, 256

# Keep in sync with GEOMETRY_* in code/audio_config.h (metres).
SQ_R = 0.040 / np.sqrt(2)
GEOMETRIES = {
    "linear": np.array([[-0.045, 0], [-0.015, 0], [0.015, 0], [0.045, 0]]),
    "square": np.array([[-SQ_R, 0], [0, SQ_R], [0, -SQ_R], [SQ_R, 0]]),
}


# ---------- WAV I/O ----------
def read_wav(path):
    """PCM 16/24/32-bit, plain or WAVE_FORMAT_EXTENSIBLE. Returns (fs, float array [n, ch])."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        sys.exit(f"{path}: missing fmt or data chunk")
    tag, ch, fs, _, align, bits = fmt
    if tag not in (1, 0xFFFE):
        sys.exit(f"{path}: not PCM (format tag {tag:#x})")
    n = len(pcm) // align
    raw = np.frombuffer(pcm[:n * align], dtype=np.uint8).reshape(n, ch, bits // 8)
    if bits == 24:
        v = (raw[..., 0].astype(np.int32) | (raw[..., 1].astype(np.int32) << 8)
             | (raw[..., 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - (1 << 24), v)
        x = v / float(1 << 23)
    elif bits == 16:
        x = raw.view("<i2").reshape(n, ch) / 32768.0
    elif bits == 32:
        x = raw.view("<i4").reshape(n, ch) / float(1 << 31)
    else:
        sys.exit(f"{path}: unsupported {bits}-bit samples")
    return fs, x.astype(np.float64)


def write_wav24(path, fs, x):
    n, ch = x.shape
    v = np.clip(np.round(x * (1 << 23)), -(1 << 23), (1 << 23) - 1).astype(np.int32)
    b = np.stack([(v & 0xFF), (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=-1).astype(np.uint8)
    pcm = b.tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, ch, fs, fs * ch * 3, ch * 3, 24) + b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(hdr + pcm)


# ---------- geometry ----------
class Geometry:
    def __init__(self, pos, fs):
        self.pos, self.fs = pos, fs
        self.linear = bool(np.allclose(pos[:, 1], pos[0, 1]))
        d = [np.hypot(*(pos[i] - pos[j])) for i in range(4) for j in range(i + 1, 4)]
        self.min_spacing, self.aperture = min(d), max(d)
        self.alias_hz = C / (2 * self.min_spacing)
        self.band_hi = min(self.alias_hz * ALIAS_MARGIN, fs / 2)

    def delays(self, theta_deg):
        """tau[m] for each angle: seconds earlier than the array centre. [..., 4]"""
        t = np.radians(np.asarray(theta_deg, dtype=float))[..., None]
        return (self.pos[:, 0] * np.sin(t) + self.pos[:, 1] * np.cos(t)) / C

    def scan_angles(self, step=1.0):
        if self.linear:
            return np.arange(-90, 90 + step / 2, step)
        return np.arange(-180, 180, step)


# ---------- STFT helpers ----------
def stft(x):
    """x [n, ch] → X [frames, ch, bins], periodic sqrt-Hann, like the firmware."""
    w = np.sin(np.pi * np.arange(NFFT) / NFFT)
    n = (x.shape[0] - NFFT) // HOP + 1
    if n < 1:
        sys.exit("segment shorter than one FFT frame")
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(n)[:, None]
    frames = x[idx] * w[None, :, None]            # [frames, NFFT, ch]
    return np.fft.rfft(frames, axis=1).transpose(0, 2, 1)


def band_mask(fs, lo, hi, nfft=NFFT):
    f = np.fft.rfftfreq(nfft, 1 / fs)
    return f, (f >= lo) & (f <= hi)


def gcc_lead(X, i, j, fs, lo, hi, max_lag, up=64):
    """Samples j leads i (tau_j - tau_i), from the frame-averaged cross-spectrum,
    PHAT-weighted, band-limited, upsampled by `up` for sub-sample resolution."""
    # X_i·conj(X_j) = |S|²·e^{j2πf(τ_i − τ_j)}: a delay of τ_j − τ_i samples,
    # so its inverse DFT peaks at +lead.
    cs = np.mean(X[:, i, :] * np.conj(X[:, j, :]), axis=0)
    f, m = band_mask(fs, lo, hi)
    g = np.where(m, cs / (np.abs(cs) + 1e-30), 0)
    r = np.fft.irfft(g, NFFT * up) * up
    lags = np.fft.fftfreq(NFFT * up, 1 / (NFFT * up)) / up
    keep = np.abs(lags) <= max_lag
    k = int(np.argmax(np.where(keep, r, -np.inf)))
    y0, y1, y2 = r[k - 1], r[k], r[(k + 1) % len(r)]   # parabola through the peak
    den = y0 - 2 * y1 + y2
    off = 0.5 * (y0 - y2) / den if den < 0 else 0.0
    return lags[k] + off / up, r[k] / max(m.sum(), 1) * NFFT / 2


def srp_scan(X, geo, fs, angles):
    """Per-frame SRP-PHAT scores [frames, angles] over the DOA band."""
    f, m = band_mask(fs, BAND_LO_HZ, geo.band_hi)
    Xb = X[:, :, m]
    Xn = Xb / (np.abs(Xb) + 1e-30)
    tau = geo.delays(angles)                      # [angles, 4]
    fb = f[m]
    score = np.zeros((X.shape[0], len(angles)))
    for i in range(4):
        for j in range(i + 1, 4):
            G = Xn[:, i, :] * np.conj(Xn[:, j, :])                        # [frames, bins]
            ph = np.exp(-2j * np.pi * fb[None, :] * (tau[:, i] - tau[:, j])[:, None])  # [angles, bins]
            score += np.real(G @ ph.T)
    return score / (6 * m.sum())


# ---------- analyses ----------
def pick_event(x, fs, win_s=0.1):
    e = np.sum(x ** 2, axis=1)
    w = max(int(win_s * fs), 1)
    s = np.convolve(e, np.ones(w), mode="valid")
    k = int(np.argmax(s))
    pad = NFFT
    return max(k - pad, 0), min(k + w + pad, x.shape[0])


def report_levels(x):
    print("channel  rms dBFS  peak dBFS      dc")
    for c in range(x.shape[1]):
        v = x[:, c]
        rms = 10 * np.log10(np.mean(v ** 2) + 1e-20)
        pk = 20 * np.log10(np.max(np.abs(v)) + 1e-12)
        print(f"  ch{c + 1}    {rms:7.1f}   {pk:7.1f}  {np.mean(v):+.1e}")


def report_lags(X, geo, fs, pos_of_ch):
    print(f"\npair lags, GCC-PHAT over {BAND_LO_HZ:.0f}-{geo.band_hi:.0f} Hz "
          f"(+ = the second mic hears it first):")
    print("  pair        lead (samples)     us   coherence  implied angle")
    for i in range(4):
        for j in range(i + 1, 4):
            pi_, pj = pos_of_ch[i], pos_of_ch[j]
            d = geo.pos[pj] - geo.pos[pi_]
            max_lag = np.hypot(*d) / C * fs + 2
            lead, coh = gcc_lead(X, i, j, fs, BAND_LO_HZ, geo.band_hi, max_lag)
            ang = ""
            if geo.linear and abs(d[0]) > 1e-6:
                s = lead / fs * C / d[0]
                ang = f"{np.degrees(np.arcsin(np.clip(s, -1, 1))):+6.1f} deg" + (" (clipped)" if abs(s) > 1 else "")
            tag = "  <- crosses I2S0/I2S1" if (i < 2) != (j < 2) else ""
            print(f"  ch{i + 1}-ch{j + 1}      {lead:+7.3f}      {lead / fs * 1e6:+7.1f}     {coh:5.2f}   {ang}{tag}")


def report_doa(X, geo, fs):
    angles = geo.scan_angles(1.0)
    sc = srp_scan(X, geo, fs, angles)
    energy = np.sum(np.abs(X) ** 2, axis=(1, 2))
    gate = energy > np.median(energy) * 10 ** 0.8     # ~8 dB over the median frame
    peak = sc.max(axis=1)
    conf = peak - sc.mean(axis=1)
    est = angles[np.argmax(sc, axis=1)]
    print(f"\nSRP-PHAT per frame ({len(est)} frames, {gate.sum()} above the energy gate):")
    for name, sel in (("all frames", np.ones_like(gate)), ("gated frames", gate)):
        if sel.sum() == 0:
            continue
        e = est[sel]
        print(f"  {name:12s}: median {np.median(e):+6.1f} deg, mean {np.mean(e):+6.1f}, "
              f"std {np.std(e):5.1f}, confidence median {np.median(conf[sel]):.2f}")
    return est, conf


def report_polar(X, geo, fs, pos_of_ch, plot=None):
    """Delay-and-sum output power vs steering angle, per 1/3-octave band."""
    Xp = np.zeros_like(X)
    for c in range(4):
        Xp[:, pos_of_ch[c], :] = X[:, c, :]
    f = np.fft.rfftfreq(NFFT, 1 / fs)
    angles = geo.scan_angles(5.0)
    tau = geo.delays(angles)                                # [angles, 4]
    centres = [500, 1000, 2000, 3000, 4000, 5000]
    resp = {}
    for fc in centres:
        m = (f >= fc / 2 ** (1 / 6)) & (f <= fc * 2 ** (1 / 6)) & (f < fs / 2)
        if not m.any():
            continue
        w = np.exp(-2j * np.pi * f[m][None, None, :] * tau[:, :, None]) / 4   # [angles, 4, bins]
        Y = np.einsum("amb,tmb->tab", w, Xp[:, :, m])                        # [frames, angles, bins]
        p = np.mean(np.abs(Y) ** 2, axis=(0, 2))
        resp[fc] = 10 * np.log10(p / p.max() + 1e-20)
    print("\ndelay-and-sum response vs steering angle (dB re the best angle):")
    print("  angle " + "".join(f"{fc:>8d}" for fc in resp))
    for k, a in enumerate(angles):
        if a % 15 == 0:
            print(f"  {a:+5.0f} " + "".join(f"{resp[fc][k]:8.1f}" for fc in resp))
    if plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("  (matplotlib not installed; no plot)")
            return
        ax = plt.subplot(projection="polar")
        ax.set_theta_zero_location("N")
        ax.set_theta_direction(-1)          # + toward Mic 4, clockwise from broadside
        for fc, r in resp.items():
            ax.plot(np.radians(angles), np.maximum(r, -30), label=f"{fc} Hz")
        if geo.linear:
            ax.set_thetamin(-90)
            ax.set_thetamax(90)
        ax.set_rlim(-30, 0)
        ax.legend(loc="lower left", fontsize=7)
        plt.savefig(plot, dpi=120)
        print(f"  polar plot written to {plot}")


# ---------- self-test ----------
def selftest():
    """Synthesise a 4-channel plane wave with exact fractional delays, write it
    as 24-bit, and check the lags and DOA come back."""
    import os
    import tempfile
    fs, n = 16000, 1 << 15
    geo = Geometry(GEOMETRIES["linear"], fs)
    rng = np.random.default_rng(1)
    S = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / fs)
    ok = True
    for theta in (-50.0, 0.0, 30.0, 90.0):
        tau = geo.delays(theta)
        x = np.stack([np.fft.irfft(S * np.exp(2j * np.pi * f * t), n) for t in tau], axis=1)
        x *= 0.05 / np.std(x)
        path = os.path.join(tempfile.gettempdir(), "gulltoppr_selftest.wav")
        write_wav24(path, fs, x)
        fs2, y = read_wav(path)
        X = stft(y)
        lead, _ = gcc_lead(X, 0, 3, fs2, BAND_LO_HZ, geo.band_hi, 6)
        expect = (tau[3] - tau[0]) * fs
        sc = srp_scan(X, geo, fs2, geo.scan_angles(1.0))
        doa = np.median(geo.scan_angles(1.0)[np.argmax(sc, axis=1)])
        good = abs(lead - expect) < 0.05 and abs(doa - theta) <= 2
        ok &= good
        print(f"  theta {theta:+5.0f}: ch4 leads ch1 {lead:+.3f} (expect {expect:+.3f}), "
              f"DOA {doa:+.0f}  {'ok' if good else 'FAIL'}")
    print("selftest " + ("passed" if ok else "FAILED"))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--geometry", choices=GEOMETRIES, default="linear")
    ap.add_argument("--map", default="1234", help="geometry position (1-4) of each channel's mic")
    ap.add_argument("--start", type=float, help="segment start, seconds")
    ap.add_argument("--dur", type=float, help="segment length, seconds")
    ap.add_argument("--event", action="store_true", help="analyse only the loudest 100 ms (clap test)")
    ap.add_argument("--doa", action="store_true", help="SRP-PHAT per frame")
    ap.add_argument("--polar", action="store_true", help="delay-and-sum response vs angle")
    ap.add_argument("--plot", help="write the polar plot here (needs matplotlib)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.wav:
        ap.error("need a WAV file (or --selftest)")
    if sorted(a.map) != list("1234"):
        ap.error("--map must be a permutation of 1234")
    pos_of_ch = [int(ch) - 1 for ch in a.map]

    fs, x = read_wav(a.wav)
    if x.shape[1] != 4:
        sys.exit(f"expected 4 channels, got {x.shape[1]}")
    geo = Geometry(GEOMETRIES[a.geometry], fs)
    print(f"{a.wav}: {x.shape[0] / fs:.2f} s at {fs} Hz, geometry {a.geometry} "
          f"({geo.aperture * 1000:.0f} mm aperture, aliasing above {geo.alias_hz:.0f} Hz)")
    if a.start is not None or a.dur is not None:
        s0 = int((a.start or 0) * fs)
        s1 = x.shape[0] if a.dur is None else s0 + int(a.dur * fs)
        x = x[s0:s1]
    if a.event:
        s0, s1 = pick_event(x, fs)
        print(f"loudest event: {s0 / fs:.3f}-{s1 / fs:.3f} s")
        x = x[s0:s1]

    report_levels(x)
    x = x - np.mean(x, axis=0)
    X = stft(x)
    report_lags(X, geo, fs, pos_of_ch)
    if a.doa:
        Xp = np.zeros_like(X)
        for c in range(4):
            Xp[:, pos_of_ch[c], :] = X[:, c, :]
        report_doa(Xp, geo, fs)
    if a.polar or a.plot:
        report_polar(X, geo, fs, pos_of_ch, a.plot)


if __name__ == "__main__":
    main()
