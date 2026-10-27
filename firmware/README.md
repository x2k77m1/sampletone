# firmware/ — put the stock Digitone OS here

This repository redistributes **no firmware** (Elektron's copyright), so this
folder starts empty and `build.sh` stops until you add the stock file.

## What it wants

Elektron's **Digitone OS 1.44** update file for the **Digitone mk1 / Digitone
Keys** — the `.syx` you would normally send to the unit with Transfer. It is on
Elektron's download page under Digitone (and Digitone Keys; one file serves
both). SampleTone targets 1.44 exactly: the hook sites and measured addresses
it is built from are that release's.

```
firmware/Digitone_and_Digitone_Keys_OS1.44.syx

sha256  d4f200d04484333d82822db7744e6484d0def8f2db8ddf55ee2b780cc13c9659
```

`build.sh` checks that hash before building, so a wrong version (1.43, or
another product) is refused with a clear message instead of a linker error. The
expected hash is read from your elekloader checkout
(`elekloader/devices/digitone_mk1.py`), not hardcoded here.

With exactly one `.syx` in the folder it is used automatically; with several,
pass the one you want:

```sh
sh build.sh --firmware firmware/Digitone_and_Digitone_Keys_OS1.44.syx
```

Everything in this folder except this README is ignored by git, and the build
output (`out/`, `*.syx`, `*.elemod`) is ignored too, so neither the stock file
nor anything derived from it can be committed by accident.

## Flashing, and getting out of trouble

Send the built image in `out/` to the unit with Elektron Transfer over USB (or
any SysEx tool). If anything looks wrong, the bootloader was never touched, so
the stock file restores the unit completely:

1. hold **FUNC** while powering on (the startup menu appears),
2. press **TRIG 4** (OS UPGRADE),
3. send the stock `.syx` from this folder.
