# Auto-start: how it works, and the two macOS traps

`./install.sh --auto-start` sets the n12 up so it comes back by itself after a
reboot, with no Terminal. This page explains what it installs and the two
non-obvious macOS behaviours it has to work around — both of which will bite
anyone who tries to do the same thing by hand.

## What gets installed

Two launchd jobs:

| job | type | runs | why |
|---|---|---|---|
| `com.n12monterey.kexts` | LaunchDaemon (`/Library/LaunchDaemons`) | `try_kextload.sh --bypass` at boot, as root | the Yamaha kexts are **non-persistent** — macOS unloads them at shutdown, so something must reload them every boot |
| `com.n12monterey.agent` | LaunchAgent (`~/Library/LaunchAgents`) | `n12_launchd.sh` at login, as you | builds the CoreAudio device (per-user) and makes it the Mac's output |

They run from a **copy** of the tools in
`~/Library/Application Support/n12-monterey`, not from the checkout you cloned.
Remove everything with `./install.sh --uninstall`.

## Trap 1 — Privacy (TCC): a launchd job cannot read `~/Documents`

If a job's program lives under `~/Documents`, `~/Desktop` or `~/Downloads`,
launchd starts it and it immediately fails:

```
/bin/zsh: can't open input file: /Users/you/Documents/.../tools/n12_on.sh
```

macOS protects those three folders, and a launchd-spawned process has no
permission to read them (they only work from an interactive Terminal, which the
user has granted access to). The job *loads* fine and then dies — which looks like
"the auto-start silently does nothing".

**The fix used here:** `install.sh` copies the tools to
`~/Library/Application Support/n12-monterey` (not protected) and points the jobs
there. A pleasant side effect: you can move or delete the checkout afterwards
without breaking the auto-start.

## Trap 2 — launchd reaps a job's leftover processes

`tools/n12_on.sh` starts `n12_agent` in the background and exits. That is correct
for a Terminal, but **launchd kills a job's remaining processes when the job's
main process exits**. So a job that runs `n12_on.sh` brings the device up and,
the instant the script returns, the agent is killed and the device disappears.

**The fix used here:** the login job runs `tools/n12_launchd.sh`, a thin wrapper
that calls `n12_on.sh --default` and then **stays alive for as long as the agent
runs** (it waits on `/tmp/n12_agent.pid`). Keeping the main process alive keeps the
agent alive — and because the wrapper exits when the agent does, `tools/n12_off.sh`
still stops it properly.

## Testing without a reboot

The login job is easy to exercise on the spot:

```sh
launchctl kickstart -k "gui/$(id -u)/com.n12monterey.agent"
sleep 20
tools/n12_fmt          # expect: 14 outputs, 16 inputs
```

The kext daemon is only meaningful at a **real reboot** (that is the moment the
kexts have been unloaded and need reloading).

## Where to look when it goes wrong

| file | what it tells you |
|---|---|
| `/tmp/n12_monterey.kexts.log` | the boot-time kext load — "load command succeeded", or `Code=27` (Apple is asking you to click **Allow** in System Settings → Privacy & Security) |
| `/tmp/n12_monterey.agent.log` | the login-time bring-up — the agent's normal output, or a read error (Trap 1) |
| `launchctl print "gui/$(id -u)/com.n12monterey.agent"` | the job's `program` (must be `…/Application Support/n12-monterey/tools/n12_launchd.sh`), `state`, and `runs` |

If `kextstat | grep -i yamaha` is empty after a reboot, the daemon did not load
the kexts — check its log, and remember the first load after install may need the
one-time **Yamaha Corporation → Allow** click, followed by another reboot.

## The other place auto-start can go wrong: privacy prompts

The very first time either kext loads, macOS may refuse and offer an **Allow**
button in **System Settings → Privacy & Security**. That is Apple's kext-consent
gate, not this project; click it and reboot once more. After that it is silent.

## Doing it by hand (if you prefer)

The two plists are small; `install.sh` writes them with your paths substituted.
If you would rather write them yourself, the essential bits are:

* daemon: `ProgramArguments = [$SUPPORT/tools/try_kextload.sh, --bypass]`,
  `RunAtLoad = true`, where `$SUPPORT` is the Application Support copy;
* agent: `ProgramArguments = [$SUPPORT/tools/n12_launchd.sh]`, `RunAtLoad = true`.

Load them with `sudo launchctl bootstrap system <daemon.plist>` and
`launchctl bootstrap "gui/$(id -u)" <agent.plist>`.
