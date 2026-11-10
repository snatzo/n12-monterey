
---

## 75. The device "type" unlocks multi-rate (2026-11-10)

The 48-byte `sCreateAudioDevice` structure's field at **`+0x0c`** — which we had
been sending as 0 and calling an "index" — is the **device type**:
`createAudioDeviceFunction` passes it to `YamahaFWAudioDevice::setType`, which
writes `device+0xfc`; `createAudioEngine` then copies it to `engine+0x1c8`, and
`createNewAudioStream` only walks the **multi-rate** list when that value is 1
(otherwise it advertises the single initial rate).

So with the field set to 0 the n12 offered **44100 only**; with **1** it offers
all four (`n12ctl` reading the live device):

```
type 0 :  available: 44100..44100
type 1 :  available: 44100..44100 48000..48000 88200..88200 96000..96000
```

The other fields of the same structure map as: `+0x00` guid -> `setGUID`,
`+0x08` -> `setVendorID`, `+0x10` name -> `setDeviceNameStr`, `+0x20` vendor ->
`setVendorNameStr`.

`n12_agent` now sends **type 1** by default (`--dev-type=N` overrides it), so
CoreAudio advertises 44.1/48/88.2/96 kHz. Switching the rate itself (vs merely
offering it) is still open: setting `kAudioDevicePropertyNominalSampleRate`
returns success but does not move the rate, so the driver's own rate-change path
(and the desk's clock) has to be driven - see the companion `n12_control_panel`
project, `docs/FINDINGS.md`.

---

## 76. MIDI works: the desk's CoreMIDI ports come back (2026-11-10)

The n12's MIDI is the last piece, and it is created **through the audio driver**,
then published by Yamaha's CoreMIDI driver plugin (`YamahaFWMIDI.plugin`, a
64-bit-capable CFPlugIn already installed and loaded by `MIDIServer`).

`n12_agent` now builds the MIDI half too:

```
sCreateMIDIDevice                   -> 0x0  index=34     (selector 18, 64B struct)
sSetMIDIDeviceModelID(34,1048590)   -> 0x0               (selector 67)
sCreateMIDIPlug(out, "mLAN Network 1") -> 0x0            (selector 27, 32B struct)
sCreateMIDIPlug(in , "mLAN Network 1") -> 0x0
sStartMIDIDevice(34)                -> 0x0               (selector 25)
```

CoreMIDI then shows the desk:

```
sources:      n12 mLAN Network 1
destinations: n12 mLAN Network 1
```

### The two things that made it work

1. **`sSetMIDIDeviceModelID(idx, 1048590)`.** The Mojave user-client capture
   holds the exact call (`scalMethod sel 67 in[2] 3 1048590`). It writes the MIDI
   device's `+0x1c`. Without it the CoreMIDI plugin opens the device's user client
   but publishes **no endpoints**.
2. **Restarting `MIDIServer` after creating the device.** The plugin builds its
   port list when it *loads*, so a device created after `MIDIServer` started is
   invisible. `MIDIRestart()` was not enough; bouncing `MIDIServer` (it relaunches
   on demand) is what makes the ports appear. `n12_agent` does this once, after
   starting the MIDI device.

`--no-midi` builds the audio device only. `tools/n12_midi_dev` remains the
standalone way to experiment with the MIDI sequence.

### Not disturbing MIDI Studio (2026-11-10, later)

`MIDIServer` is **on demand**: it starts when a MIDI client connects and quits
when idle. So the agent must not blindly `killall MIDIServer` on every start - and
"are the ports present?" is not a safe test either, because with the server down
CoreMIDI reports no ports at all. The rule the agent now uses is:

* if `MIDIServer` is **not running**, leave it: it will load the plugin with the
  device present on next use;
* if it **is running and already has the desk's ports**, leave it;
* only if it **is running without** the desk's ports, restart it once.

That keeps the desk's MIDI working without closing MIDI Studio (or any other MIDI
app) out from under the user.
