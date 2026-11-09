# Reverse-engineering notes

How the behaviour in [HOW-IT-WORKS.md](HOW-IT-WORKS.md) was worked out, so the
next person can re-derive or extend it. **No Yamaha code, binaries, decompilation
dumps, or documentation are reproduced or included in this repository** — the
notes below describe *method* and *observed behaviour* only.

## The situation

On a period machine (macOS 10.13, Mojave) the desk works with Yamaha's own
software. On modern macOS it does not appear at all. The driver loads; the
user-space connection manager is the piece that no longer runs. So the goal was to
find out exactly what that manager asked the driver to do, and reproduce it.

## Technique 1 — see what is actually loaded

`DYLD_PRINT_LIBRARIES=1 open …` on a running app prints every image dyld loads,
including dlopen'ed ones. That revealed the components that never appear in a
static `otool -L`, in particular a library that lives *inside the audio kext's
`PlugIns` folder* and is loaded at runtime — the owner of the audio-device calls.
That single trick turned "the vendor manager" from a black box into a named set of
components.

## Technique 2 — interpose the user client

The driver is reached through an IOKit user client, so a small interposer was
built that wraps `IOServiceOpen` and the `IOConnectCall…` family and logs, for
every call: the service, the **selector number**, the scalar inputs, and the first
bytes of the structure input. Run on the Mojave machine with Yamaha's own software
driving the desk, it captured the manager's entire conversation with the driver:
which selectors, in what order, and the contents of the structures.

Two things came out of that capture and both were essential:

* the **selector numbers** (the driver's method table), which map to
  `sCreateAudioDevice`, `sCreateAudioStream`, `sCreateIsochStreamParam`, and so on;
* the shape of the stream structure, with a field that **varied per call** — the
  first big clue that the manager was creating many streams, one per channel,
  rather than one wide stream.

A caveat worth repeating: in one capture the returned object *indices* differed
from a later run, so a captured structure is a **shape, not constants** — only the
relationships replay.

## Technique 3 — read the driver's handlers

The driver's machine code was analysed with Ghidra in headless mode (scripted
import + decompile to text), focusing on the handler behind each selector. This
answered the questions a capture cannot:

* what each field of the stream structure **means** (the subchannel and the
  1-based channel-number at `+0x08`/`+0x0c`);
* the rule for the device's channel count (**the maximum channel number, rounded up
  to even**);
* why some structures were rejected — a non-zero channel number is required when an
  isochronous reference is present, and two streams may not claim the same channel
  number.

This is analysis of a binary that is already installed on the machine, for the
purpose of interoperating with it; none of the analysis output is redistributed.

## Technique 4 — measure the real device

Static reading tells you what the driver does with a value; it does not tell you
**where on the desk** that lands. That was measured directly: play a tone on one
CoreAudio output at a time (`tools/n12_play --channel=N`) and watch which desk
meter lights. That produced the actual subchannel → desk-channel map, which
differed from a naive reading of the block diagram.

This step also produced the project's most repeated lesson: **several "nothing
happened" results were the desk, not the code.** The desk resets every strip's
`[INPUT SELECT]` to analog at power-up, so a strip you are feeding shows nothing
until you press `DAW`. A negative result is only meaningful with the desk in a
known state.

## Technique 5 — bisection on the host side

The two CoreAudio "where is stereo" settings were found by bisection. AudioUnit
apps worked and AudioQueue apps did not, on the *same* device. Comparing the
device's properties against a known-good device (the built-in output) showed the
layout marking a different pair of channels as Left/Right than the preferred-pair
property. Moving the layout labels to match made both app families route the same
way.

## Things that cost time (so they need not cost yours)

* **The device appears only after `sStartAudioDevice`.** Without it, the device is
  not published at all — and any sound you hear in that state is the Mac's own
  output, not the desk. That looks exactly like "it works but silent", and is a
  trap.
* **The channel map is permuted.** "Output 1 → desk channel 1" is *not* the
  vendor's arrangement; the main bus and the strips interleave. Measure it.
* **Many streams, one isochronous parameter.** The natural guess ("one stream per
  channel needs one parameter per channel") is wrong; the driver accepts many
  streams on one parameter, and *that* is the vendor's shape.
* **Rejection codes are informative.** `0xe00002c2` (`kIOReturnBadArgument`) meant
  "channel count is zero"; `0xe0000001` meant "that channel number is already
  taken". Reading the handler turned these from mysteries into messages.
