
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
