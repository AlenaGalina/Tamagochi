"""Lo-fi treatment for short sounds, and a synthesized lo-fi e-piano chord.

Usage:
  python3 tools/lofi.py in.ogg out.wav     # process a sample
  python3 tools/lofi.py --start out.wav    # pomodoro start: an Fmaj9 rolled upward
  python3 tools/lofi.py --finish out.wav   # pomodoro done: G13 resolving to Cmaj9

The chain: 16 kHz mono, slow tape wobble, soft low-pass and high-pass (a small
speaker can't play bass anyway), gentle saturation, a short echo and a little
vinyl crackle. Output is 16 kHz mono 16-bit WAV, ready for tools/gen_sounds.py.
"""
import sys
import numpy as np
import soundfile as sf

RATE = 16000
rng = np.random.default_rng(7)


def resample(x, sr, rate=RATE, wobble=True):
    n = int(len(x) * rate / sr)
    t = np.arange(n) / rate
    speed = 1.0
    if wobble:  # tape wow (0.55 Hz) and flutter (6 Hz)
        speed = 1 + 0.004 * np.sin(2 * np.pi * 0.55 * t) + 0.0008 * np.sin(2 * np.pi * 6.1 * t)
    pos = np.cumsum(speed * sr / rate)
    pos = pos[pos < len(x) - 1]
    return np.interp(pos, np.arange(len(x)), x)


def one_pole(x, cutoff, high=False):
    a = np.exp(-2 * np.pi * cutoff / RATE)
    y, s = np.empty_like(x), 0.0
    for i, v in enumerate(x):
        s = (1 - a) * v + a * s
        y[i] = v - s if high else s
    return y


def lofi(x):
    x = one_pole(one_pole(x, 3000), 3000)          # soft low-pass
    x = one_pole(x, 220, high=True)                # drop rumble
    x = np.tanh(2.2 * x / (np.abs(x).max() + 1e-9)) / np.tanh(2.2)  # warm saturation
    x = np.concatenate([x, np.zeros(int(0.45 * RATE))])
    d = int(0.13 * RATE)
    for i in range(d, len(x)):                      # short echo
        x[i] += 0.22 * x[i - d]
    crackle = np.zeros_like(x)
    clicks = rng.random(len(x)) < 18 / RATE         # ~18 dust clicks a second
    crackle[clicks] = rng.uniform(-1, 1, clicks.sum()) * 0.10
    crackle = one_pole(crackle, 2500) + rng.normal(0, 0.006, len(x))
    x = x + crackle
    fade = int(0.3 * RATE)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x / np.abs(x).max() * 0.9


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


# (MIDI note, start in seconds, velocity)
START = [(65, 0.00, 1.0), (69, 0.11, 0.9), (72, 0.24, 0.9), (76, 0.35, 0.85), (79, 0.50, 0.8)]
FINISH = [(65, 0.00, 0.7), (71, 0.05, 0.7), (76, 0.10, 0.7), (81, 0.15, 0.6),        # G13 (F B E A)
          (64, 0.62, 0.9), (67, 0.70, 0.85), (71, 0.78, 0.85), (74, 0.86, 0.8), (84, 1.10, 0.7)]  # Cmaj9, high C


def epiano(notes, length):
    """A Rhodes-like FM electric piano playing the given notes."""
    t = np.arange(int(length * RATE)) / RATE
    out = np.zeros_like(t)
    for midi, start, vel in notes:
        f = hz(midi)
        u = np.clip(t - start, 0, None)
        on = t >= start
        index = 1.6 * np.exp(-u * 5)                    # bright attack, mellow tail
        tine = 0.25 * np.exp(-u * 18) * np.sin(2 * np.pi * f * 7.0 * u)  # the bell-like 'tine'
        tone = np.sin(2 * np.pi * f * u + index * np.sin(2 * np.pi * f * u)) + tine
        env = np.exp(-u * 1.6) * np.minimum(1, u / 0.004)
        out += on * env * tone * vel
    return out


def main():
    if sys.argv[1] in ("--start", "--finish"):
        notes, length = (START, 2.2) if sys.argv[1] == "--start" else (FINISH, 3.0)
        x = resample(epiano(notes, length), RATE)
        sf.write(sys.argv[2], lofi(x).astype(np.float32), RATE, subtype="PCM_16")
        return
    x, sr = sf.read(sys.argv[1])
    if x.ndim > 1:
        x = x.mean(axis=1)
    sf.write(sys.argv[2], lofi(resample(x, sr)).astype(np.float32), RATE, subtype="PCM_16")


main()
