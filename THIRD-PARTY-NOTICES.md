# Third-party notices

## What this project bundles

**Nothing.** Every file in this repository is original work licensed under the
Apache License 2.0 (see `LICENSE`). No third-party source code, binaries,
documentation, or model weights are included.

## What it depends on (not bundled)

This project only *drives* software you install yourself; it does not contain or
redistribute any of it.

| dependency | role | licence / terms |
|---|---|---|
| **Yamaha Steinberg FW Driver** (`YamahaFWAudioDriver.kext`, `YamahaFWAsync.kext`, and the `YamahaFW…` frameworks/bundles) | the kernel driver and HAL this project talks to | Yamaha/Steinberg proprietary; obtained from Yamaha. Not included here. |
| **IOFireWireFamily** (`IOFireWireLib`) | Apple's FireWire stack | Apple system software |
| **CoreAudio / AudioToolbox / IOKit / CoreMIDI** | Apple frameworks linked by the tools | Apple system software |

Because the Yamaha driver's connection-manager component is 32-bit only, it cannot
run on 64-bit-only macOS; this project replaces that component. It does not
replace, modify, or redistribute Yamaha's driver.

## Trademarks

"Yamaha", "Steinberg", and "n12"/"n8" are trademarks of their respective owners.
They are used here only to describe interoperability, and this project is not
affiliated with or endorsed by Yamaha or Steinberg.

## Build tooling

The tools are plain C built with Apple `clang` from the Xcode command line tools.
No third-party libraries are compiled in.
