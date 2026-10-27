# samples/ — put your samples here

This repository ships **no samples** (the ones used for testing belong to their
owners), so this folder starts empty and `build.sh` stops until you add some.
Its contents are ignored by git: your audio stays yours.

```sh
# 1. drop WAV/AIFF files in here
# 2. put Elektron's stock Digitone OS 1.44 .syx in ../firmware/
sh ../build.sh                       # bake, build, and write out/sampletone-1.44-1.0a.syx
```

That bakes everything in here into the mod, builds the OS image, and prints
which SAMP value selects which sample. **There is no runtime loading**: the
samples are compiled into the mod, so changing them means baking and flashing
again. Transfer and the +Drive cannot be used for this from a machine mod — a
mod is a patch to the OS image and has no file API.

## The recommended format

**Mono, 48 kHz, 16-bit PCM WAV.** In that format nothing is processed at all:
the frames are copied into the mod verbatim, and the bake report says
`verbatim` per file. Anything else is converted, and not always the way you
would want:

- 24/32-bit and 8-bit are converted to 16-bit.
- Any rate other than the bake rate is linearly interpolated (no anti-alias
  filter). Pre-convert with a real resampler if it matters.
- **Stereo is a choice, because the machine's voice is mono** — one 32-sample
  block per voice; the Digitone's pan, chorus, delay and reverb all come after
  it, so a machine cannot output two channels. `--downmix mid` (the default)
  sums both channels at half gain, which is what a centred mono mix sounds
  like; content that is hard-panned lands 6 dB down, and content that is out of
  phase cancels. If your sample is a hard-panned stereo pair, prefer
  `--downmix left` or `--downmix right`, or bake the two channels as separate
  samples and run them on two tracks panned left and right.

## Rules and limits

- Files are baked **sorted by filename**, and a sample's 5-character name comes
  from its file stem (`Kick 707 1.wav` → `KICK7`), so renaming changes both the
  order and the name on the overlay.
- At most 128 files, and at most **1,200,000 bytes of PCM** in one build
  (12.5 s at 48 kHz mono 16-bit) — that is the room left in core-dn1 4.0's
  1.25 MB mod area once the code is paid for. Over the cap, the bake refuses and
  names the total; `--trim SECONDS`, a lower `--rate`, or fewer files bring it
  down.
- One `--root` note (default 60, C4) applies to the whole bake, and it is the
  note at which a sample plays back at its recorded pitch.
- The SAMP knob's 0–127 range is spread over the pack, so a few knob values can
  select the same sample; the bake prints the ranges.

## Options

`sh ../build.sh` passes these to `bake.py`:

| option | what |
|---|---|
| `--downmix mid\|left\|right` | which channel(s) of a stereo source to keep |
| `--rate HZ` | bake rate, default 48000 (the Digitone's render rate) |
| `--root NOTE` | MIDI note at which the bake plays at pitch, default 60 |
| `--trim SECONDS` | keep only the first SECONDS of each sample |
| `--fade MS` | fade-out at the end of a trimmed sample, default 5 ms |
| `--max-bytes N` | raise the cap, if you know the build has room |

To bake without building (no firmware needed, nothing in the tree touched):

```sh
sh ../build.sh --bake-only
```

`python3 bake.py --pack samples/` does the same bake on its own and writes
`samples.c`/`samples.h` in the repository root — useful when you are building
the mod by hand. The two files in the repository are the empty placeholder,
which is what `build.sh` puts back after a build: no sample ships with this
mod, so the table in the tree is yours once you bake.
