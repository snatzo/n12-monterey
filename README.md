# n12-monterey

Use a **Yamaha n-series mixer (n12 / n8)** as a normal, working CoreAudio device
on modern macOS — **Monterey and later** — with all of its channels mapped the way
the original Yamaha software mapped them.

If you have an n12 on a modern Mac, you have probably found that it *does not
appear at all* in System Settings → Sound or Audio MIDI Setup. That is because the
component that used to bring it to life, Yamaha's **connection manager** (a 32-bit
app), cannot run on a 64-bit-only macOS. Yamaha's kernel driver and HAL are
64-bit and load fine — they are just never *asked* to build the device, because the
only thing that used to ask was 32-bit.

This project is a **64-bit replacement for that connection manager**, plus the
channel mapping it performed, rebuilt for the way the desk actually behaves.

> **Unofficial.** Not affiliated with or endorsed by Yamaha or Steinberg. Built by
> reverse-engineering the installed driver for interoperability. **It contains no
> Yamaha code, binaries, or documentation** — you install Yamaha's driver yourself.

## What you get

With the tools running, the desk behaves like it did on macOS 10.13:

| CoreAudio channels | desk |
|---|---|
| **outputs 1–2** | the desk's **main bus** — ordinary macOS audio is heard here with **nothing pressed on the desk** (no channel strip, no `DAW` button) |
| **outputs 3–10** | desk channels **1–8** (mono strips) |
| **outputs 11–14** | desk channels **9–12** (stereo channels) |
| **inputs 1–12** | the desk's per-channel direct outs |
| **inputs 13–14** | the desk's REC out |
| **inputs 15–16** | the desk's **stereo bus** — record your desk mix here |

So, exactly as on Mojave:

* macOS system audio, Music, QuickTime, browsers and **Logic/VLC** all play through
  the desk, with the **main meters** moving and no strip involved;
* a DAW can send each track to its own desk strip by using outputs **3, 4, 5 …**
  (Logic reserves 1–2 as its "stereo out", so the first pair it offers is 3‑4 = desk
  channels 1‑2);
* pressing **`DAW`** on a strip makes it take the computer's signal — the desk
  resets every strip to analog input (`A.IN`) at power-up, so this is per-session.

## Requirements

* macOS **10.15 (Catalina) or later**; developed and tested on **Monterey**.
* The official **Yamaha Steinberg FW Driver** installed (it provides the kernel
  extensions and the HAL plug-in this project drives). *Not supplied here.*
* The **Xcode command line tools** (`xcode-select --install`), to build the tools.
* An Intel Mac was used for development; the tools are plain C and should build on
  Apple Silicon too, though that is untested.

## Quick start

```sh
git clone <this repo>
cd n12-monterey
./install.sh                 # builds the tools
sudo tools/try_kextload.sh --bypass      # load the Yamaha kexts (once per boot)
tools/n12_on.sh --default    # bring the n12 up and make it the Mac's output
```

See **[INSTALL.md](INSTALL.md)** for the full walkthrough, the auto-start option
(`./install.sh --auto-start`), and how to undo everything.

## How it works

The full story is in **[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)** and the
reverse-engineering notes in
**[docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md)**. In one paragraph:

The Yamaha kernel driver exposes a user client that builds an audio device on
request. Yamaha's (32‑bit) manager made that request and also wired each of the
desk's channels to a specific isochronous "subchannel". This project makes the
same request from a small 64‑bit tool, and — the interesting part — reconstructs
the per‑channel wiring by measuring it: which CoreAudio output lands on which desk
channel. Two CoreAudio settings ("preferred stereo pair" and "channel layout") have
to be pointed at the same place, or AudioUnit apps and AudioQueue apps disagree
about where stereo goes.

## What works / what does not

* ✅ The n12 as a multi-channel CoreAudio device on Monterey (14 out, 16 in).
* ✅ macOS audio through the desk's main bus; DAW-track → desk-strip routing;
  recording the desk's stereo bus back.
* ✅ AudioUnit apps (Logic, VLC, browsers) **and** AudioQueue apps (Music,
  QuickTime, system sounds).
* ⚠️ Only one n-series device at a time is assumed.
* ⚠️ Sample rate is 44.1 kHz (the desk's rate).
* ⚠️ The desk's own wiring/config is never written; if the desk were ever
  factory-reset, restoring it would need Yamaha's own software on a period Mac.

## Tools

`tools/` holds the C sources. The important ones:

| tool | what it does |
|---|---|
| `n12_agent` | the connection manager: builds and holds the device |
| `n12_on.sh` / `n12_off.sh` | start / stop it, with the right settings |
| `n12_layout` | points the device's stereo pair at the main bus |
| `n12_default` | makes the n12 the Mac's default output |
| `n12_fmt` | dumps the device's channels and formats |
| `n12_play` / `sysplay` | play tones/files over the two CoreAudio client paths |
| `n12_capture` | record from the desk and report per-channel levels |
| others | diagnostics and the reverse-engineering probes |

## Licence

Apache-2.0 — see [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
