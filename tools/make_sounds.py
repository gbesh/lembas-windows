"""Lembas sound scheme: soft harp plucks, small bells and a low horn. All synthesised here.

Usage: python make_sounds.py <out_dir>
"""
import sys
import wave
from pathlib import Path

import numpy as np

SR = 44100
rng = np.random.default_rng(1954)


def note(name):
    names = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}
    n = names[name[0]]
    i = 1
    if name[1] in "#b":
        n += 1 if name[1] == "#" else -1
        i = 2
    octave = int(name[i:])
    return 440.0 * 2 ** ((n + (octave - 4) * 12) / 12)


def pluck(freq, dur, bright=0.5):
    """Karplus–Strong string: a harp-like pluck."""
    n = int(SR * dur)
    period = max(2, int(SR / freq))
    buf = rng.uniform(-1, 1, period)
    buf -= buf.mean()
    out = np.zeros(n)
    decay = 0.996
    for i in range(n):
        out[i] = buf[i % period]
        nxt = buf[(i + 1) % period]
        buf[i % period] = decay * (bright * buf[i % period] + (1 - bright) * 0.5 * (buf[i % period] + nxt))
    env = np.minimum(1, np.arange(n) / (0.004 * SR))
    return out * env


def bell(freq, dur):
    t = np.arange(int(SR * dur)) / SR
    partials = [(1, 1.0, 1.6), (2.0, 0.5, 2.4), (2.76, 0.35, 3.2), (5.4, 0.18, 5.0), (8.9, 0.08, 7.0)]
    s = sum(a * np.sin(2 * np.pi * freq * r * t) * np.exp(-t * k) for r, a, k in partials)
    return s * np.minimum(1, t / 0.002)


def horn(freq, dur):
    t = np.arange(int(SR * dur)) / SR
    s = sum((1 / h) * np.sin(2 * np.pi * freq * h * t + 0.3 * h) for h in range(1, 9))
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5 * t)
    s = sum((1 / h) * np.sin(2 * np.pi * freq * h * t * vib) for h in range(1, 9))
    env = np.minimum(1, t / 0.12) * np.exp(-np.maximum(0, t - 0.35) * 5)
    # gentle low-pass
    k = 0.12
    y = np.zeros_like(s)
    for i in range(1, len(s)):
        y[i] = y[i - 1] + k * (s[i] - y[i - 1])
    return y * env


def place(parts, total):
    out = np.zeros(int(SR * total))
    for start, sig, gain in parts:
        i = int(SR * start)
        j = min(len(out), i + len(sig))
        out[i:j] += sig[: j - i] * gain
    return out


def reverb(x, mix=0.25):
    out = x.copy()
    for d, g in ((0.031, 0.5), (0.047, 0.4), (0.071, 0.32), (0.113, 0.25), (0.157, 0.18), (0.229, 0.12)):
        k = int(SR * d)
        out[k:] += x[:-k] * g * mix * 2
    return out


def finish(x, peak=0.5):
    x = reverb(x)
    # remove any DC offset with a gentle high-pass
    y = np.zeros_like(x)
    a = 0.995
    for i in range(1, len(x)):
        y[i] = a * (y[i - 1] + x[i] - x[i - 1])
    x = y
    fade = int(SR * 0.08)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x / (np.max(np.abs(x)) + 1e-9) * peak


def write(path, x):
    data = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    stereo = np.repeat(data[:, None], 2, axis=1)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(stereo.tobytes())


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    sounds = {
        # a three-note harp figure
        "Lembas_Notify.wav": finish(place([(0.00, pluck(note("D5"), 1.2), 1), (0.09, pluck(note("F#5"), 1.1), .9), (0.18, pluck(note("A5"), 1.2), .9)], 1.5), .45),
        # one small bell
        "Lembas_Chime.wav": finish(place([(0, bell(note("E6"), 1.4), 1)], 1.4), .38),
        # two falling bells
        "Lembas_Warning.wav": finish(place([(0, bell(note("A5"), 1.0), 1), (0.16, bell(note("E5"), 1.2), .9)], 1.4), .42),
        # a low horn
        "Lembas_Error.wav": finish(place([(0, horn(note("D3"), 0.9), 1), (0.0, horn(note("A3"), 0.9), .35)], 1.0), .5),
        "Lembas_Connect.wav": finish(place([(0, pluck(note("A4"), .8), 1), (0.1, pluck(note("E5"), .9), 1)], 1.0), .42),
        "Lembas_Disconnect.wav": finish(place([(0, pluck(note("E5"), .8), 1), (0.1, pluck(note("A4"), .9), 1)], 1.0), .42),
        # a longer welcome: rising harp over a soft bell
        "Lembas_Logon.wav": finish(place(
            [(i * 0.13, pluck(note(n), 1.8), .85) for i, n in enumerate(["D4", "A4", "D5", "F#5", "A5", "D6"])] +
            [(0.7, bell(note("D6"), 1.8), .25)], 2.6), .45),
    }
    for name, x in sounds.items():
        write(out / name, x)
    print("sounds written to", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sounds")
