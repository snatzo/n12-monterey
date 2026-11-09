# How it works

This is what the tools in `tools/` actually do, and why each piece is needed. It
describes the *interface* this project uses (method selectors and structure
layouts), reconstructed for interoperability — no Yamaha source or binary is
included or reproduced.

## The problem in one line

The Yamaha driver builds the n12's audio device **only when a user-space program
asks it to**, and the program that used to ask (Yamaha's connection manager) is
32-bit and will not run on 64-bit-only macOS. So the driver loads, the hardware is
present, and yet no audio device appears.

## The stack on the machine

Reading back the loaded libraries (`DYLD_PRINT_LIBRARIES`) and the driver's
internal names, the pieces are:

```
YamahaFWCM  ->  YamahaFWEnabler  ->  YamahaFWHAL   ->  the desk (FireWire / mLAN)
                                         |
                                         +-> YamahaFWUserLib -> YamahaFWAudioDriver (user client)
```

* **`YamahaFWAudioDriver.kext`** — the kernel driver. It publishes a
  `YamahaFWStreamDevice` service and a user client with the methods that build
  audio devices, streams and isochronous parameters.
* **`YamahaFWHAL.bundle`, `YamahaFWEnabler.framework`, `YamahaFWCM.app`** — the
  user-space control chain. `YamahaFWCM.app` is the *connection manager*; it is
  the 32-bit piece that no longer runs, and it is the only thing that calls the
  driver to build the device.

The driver and the HAL are 64-bit and load fine. Only the "please build the
device" request is missing. `tools/n12_agent` makes that request.

## Building the audio device (the calls `n12_agent` makes)

All calls go to the `YamahaFWStreamDevice` user client, identified by selector
number. In order:

| selector | method | what it does |
|---|---|---|
| 0 | open | opens the user client |
| 3 | `sCreateAudioDevice` | creates the audio device; a 48-byte structure of GUID + name buffers |
| 76 | `sSetAvailableSampleRates` | tells the driver which sample rates a client may select |
| 34 | `sCreateIsochStreamParam` | creates one isochronous stream parameter per direction |
| 45 | `sSetIsochStreamChannel` | programs the isochronous client's channel |
| 11 | `sCreateAudioStream` | creates an audio stream (one per channel; see below) |
| 43 | `sSetSYTSynchChannel` | slaves the node's clock to the desk's transmit channel |
| 61 | `sRequestStartStreams` | starts the isochronous DMA |
| 9 | `sStartAudioDevice` | starts the IOAudioEngine CoreAudio talks to |

Two details matter and were learned the hard way:

* **`sStartAudioDevice` must be called** — the device is only published once the
  engine is started. Skipping it (there is a `--no-devstart` switch) makes the
  device vanish from CoreAudio entirely.
* **The structure fields are not what they look like.** In the 40-byte
  `sCreateAudioStream` structure:

  | offset | meaning |
  |---|---|
  | `+0x00` | audio-device index (from `sCreateAudioDevice`) |
  | `+0x04` | isochronous-stream index (from `sCreateIsochStreamParam`) |
  | `+0x08` | **isochronous subchannel** this stream transmits/receives on |
  | `+0x0c` | **1-based channel number** (and, via its maximum, the device's channel count) |
  | `+0x10` | direction (0 = output, 1 = input) |
  | `+0x18`/`+0x20` | stream-name size and pointer |

  The device's total channel count is the **maximum `+0x0c`, rounded up to even**.
  Many streams may share one isochronous parameter — the vendor's own manager made
  sixteen that way — as long as no two streams claim the same channel number.

## The desk's channel map

The n12 moves 16 channels each way over one isochronous stream, as 16
*subchannels*. The desk's block diagram gives the wiring:

| isochronous subchannel (0-based) | desk `DAW IN` | where it goes |
|---|---|---|
| 0–1 | 1–2 | **the desk's main bus — no channel strip** |
| 2–5 | 3–6 | desk channels **9–12** |
| 6–13 | 7–14 | desk channels **1–8** |
| 14–15 | 15–16 | (spare on the receive side) |

So "send a track to desk channel 1" means *transmit on subchannel 6* — which is
why the device's channel order looks odd: to present the channels the way Mojave
did, an output stream is created per channel with the matching `+0x08`:

```
--out-map=0,1,6,7,8,9,10,11,12,13,2,3,4,5
```

which makes CoreAudio outputs 1–2 the **main bus**, 3–10 the eight mono strips,
and 11–14 the four stereo strips. Recording is the mirror: `--in-map=0..15` makes
Mac inputs *n* the desk's `DAW OUT n`, so the stereo bus (DAW OUT 15/16) lands on
**Mac inputs 15/16**.

The main bus (subchannels 0–1) is why **ordinary macOS audio is heard with nothing
pressed on the desk**: it bypasses every strip. A strip only takes the computer's
signal when its `[INPUT SELECT]` is set to `DAW` — and the desk resets every strip
to the analog input at power-up.

## The two CoreAudio "where is stereo" settings

There are two, and they are honoured by different parts of CoreAudio:

* **`kAudioDevicePropertyPreferredChannelsForStereo`** — the *preferred pair*.
  The **AudioUnit** output path uses this (Logic, VLC, Safari/YouTube, and the
  `sysplay` tool).
* **`kAudioDevicePropertyPreferredChannelLayout`** — the *channel layout*. The
  **AudioQueue** path uses this (`afplay`, **QuickTime Player**, **Music**, and the
  system's own sounds).

The Yamaha driver publishes a layout that marks channels 7–8 as Left/Right. Leave
it there and the AudioQueue path sends its stereo to desk channels 5–6 (strips,
needing `DAW`), while the AudioUnit path goes to channels 1–2 (the main bus) — the
classic symptom "Music and QuickTime are silent but Logic works".

`tools/n12_layout` moves the Left/Right labels to channels 1–2 so both paths agree,
and `n12_on.sh` runs it on every start (the layout is rebuilt with the device).

## Clock

The Mac follows the desk's clock. `n12_agent` slaves the node's presentation-time
clock to the channel it receives on (the desk's transmit channel), which is the
desk's own clock; otherwise the node free-runs and beats slowly against the desk.

## Why the kexts must be reloaded each boot

Yamaha's kernel extensions are non-persistent: macOS unloads them at shutdown.
`tools/try_kextload.sh` loads them (needs `sudo`). `install.sh --auto-start` can
install a small root daemon that does this at boot.
