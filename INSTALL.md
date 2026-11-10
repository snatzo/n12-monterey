# Installing n12-monterey

Everything here runs in your own user account except one `sudo` step that loads
Yamaha's kernel extensions. Nothing is installed system-wide.

## 0. Prerequisites

### The Xcode command line tools (to build)

```sh
xcode-select --install      # if this says "already installed", you are set
```

### Yamaha's driver (the kernel side)

This project drives Yamaha's driver; it does not include it. Install the official
**Yamaha Steinberg FW Driver** for your n-series device (the 64-bit build for
Monterey or later). After installing, confirm the pieces are present:

```sh
ls /Library/Extensions/YamahaFW*.kext
ls "/Library/Application Support/Yamaha/FWDriver"
```

You should see several `YamahaFW….kext` files and a `FWDriver` folder. If not, the
driver is not installed and nothing here can help.

## 1. Build

```sh
cd n12-monterey
./install.sh
```

That builds the tools into `tools/`. (Prefer to do it by hand? `make -C tools`.)

## 2. Load the Yamaha kexts (once per boot)

The Yamaha kexts are **not persistent** — macOS unloads them at shutdown, so they
must be loaded after every reboot. This is the one step that needs your password:

```sh
sudo tools/try_kextload.sh --bypass
```

Expected: it reports the Yamaha kexts loaded. The script uses the copy already
installed in `/Library/Extensions` if there is one, otherwise it extracts the
kexts from a Yamaha package you point it at with `PKG=/path/to/….pkg`.

> Want this automatic? `./install.sh --auto-start` installs a small root daemon
> that loads the kexts at boot, and a login item that starts the device. See
> *Auto-start after reboot* below.

## 3. Start the device

Make sure the desk is **powered on and connected** by FireWire, then:

```sh
tools/n12_on.sh --default
```

`--default` also points the Mac's output at the n12, so ordinary system audio
immediately comes out of the desk. (Without `--default`, the device appears and
you choose it yourself in System Settings → Sound.)

You should see the device come up:

```sh
tools/n12_fmt
#   output: 14 buffer(s), total 14 channel(s)
#   input : 16 buffer(s), total 16 channel(s)
```

## 4. Check it by ear

```sh
tools/n12_play --seconds=5 --channel=0    # 1 kHz on CoreAudio output 1 -> desk main bus, LEFT
tools/n12_play --seconds=5 --channel=1    #                                -> desk main bus, RIGHT
```

You should hear a tone, left then right, and the desk's **main meters** should
move — with **nothing pressed** on the desk.

## Using it with a DAW (e.g. Logic)

* Set the DAW's output device to **n12**. Logic shows it with "stereo out" on
  channels **1–2** (the main bus); a track's first selectable output pair is
  **3–4**, which is desk channels **1–2**.
* To send a track to a desk strip, choose its output pair (3‑4 → desk ch 1‑2,
  5‑6 → desk ch 3‑4, …) and press **`DAW`** on that strip.
* To record the desk's mix, record a stereo track from **inputs 15–16** (the desk's
  stereo bus).

Remember: the desk sets every strip to analog input (`A.IN`) at power-up, so
press `DAW` on the strips you are feeding each session. The main bus (outputs 1–2)
never needs it.

## Auto-start after reboot

```sh
./install.sh --auto-start           # installs the launchd jobs (sudo for the daemon)
```

This installs two jobs:

* `~/Library/LaunchAgents/com.n12monterey.agent.plist` — brings the device up at
  login and makes it the Mac's output;
* `/Library/LaunchDaemons/com.n12monterey.kexts.plist` — loads the Yamaha kexts at
  boot (this is the one that needs your password to install).

### Why the jobs run from a copy, not from this folder

macOS adds two wrinkles that the installer handles for you, and it helps to know
they exist:

1. **Privacy (TCC).** A launchd job is *not allowed to read files in `~/Documents`,
   `~/Desktop` or `~/Downloads`* — it fails with `can't open input file`. So the
   installer puts a copy of the tools in
   `~/Library/Application Support/n12-monterey` (not protected) and points the
   jobs there. Moving or deleting this checkout later will not break the
   auto-start.

2. **launchd reaps the job's leftover processes.** `tools/n12_on.sh` starts the
   agent in the background and exits — perfect for a Terminal, but launchd kills
   a job's remaining processes when the job's main process exits, so the device
   would vanish. The login job therefore runs `tools/n12_launchd.sh`, a thin
   wrapper that brings the device up and then **stays alive for as long as the
   agent runs**.

To test the login job without logging out:

```sh
launchctl kickstart -k "gui/$(id -u)/com.n12monterey.agent"
sleep 20
tools/n12_fmt          # expect 14 out / 16 in
```

The kext daemon only takes effect at a real **reboot** (it is what makes the n12
present again after the kexts unload at shutdown).

Remove both jobs again with `./install.sh --uninstall`.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `n12_agent` says "service YamahaFWStreamDevice not found" | the kexts are not loaded → step 2 |
| nothing in System Settings → Sound | desk powered off / FireWire unplugged, or kexts not loaded |
| you can hear Logic/VLC but **not** Music/QuickTime | run `tools/n12_layout` (`n12_on.sh` does this) |
| a strip is silent | the strip's `[INPUT SELECT]` is on `A.IN` — press `DAW` |
| audio drifts or clicks | `n12_on.sh` slaves the Mac to the desk's clock; make sure only the n12 is the Mac's output |
| after reboot the device is missing | the kexts did not load — check `/tmp/n12_monterey.kexts.log`, or run `sudo tools/try_kextload.sh --bypass` |
| the login job does nothing | check `/tmp/n12_monterey.agent.log`; it must not say `can't open input file` (see *Auto-start* above) |

Diagnostics that come in handy:

```sh
tools/n12_fmt          # what channels/formats CoreAudio sees
tools/n12_ports        # the desk's port names, if present
tools/n12_watch 5      # per-channel levels while something plays
tools/n12_capture      # record from the n12 and show per-channel levels
```

## Uninstalling

```sh
./install.sh --uninstall    # remove the launchd jobs AND the installed copy
tools/n12_off.sh            # stop the running agent now
rm -rf n12-monterey         # remove the checkout (and its built tools)
```

`--uninstall` removes both job plists and the
`~/Library/Application Support/n12-monterey` copy. Yamaha's driver is untouched by
any of this; remove it with Yamaha's own uninstaller if you want it gone.
