# MIDICollider

A Raspberry Pi 3B+ as a standalone SuperCollider instrument: a USB MIDI keyboard
goes in, audio comes out, and nothing runs except a Linux kernel, a custom PID 1
and three processes.

```
USB keyboard ──raw MIDI──► midid ──OSC (localhost)──► scsynth ──► DAC
                                                         ▲
laptop ──TCP :9000──► recvd ──► /data (synthdefs, samples, config)
```

| Program | Role |
|---|---|
| `src/init.c` | PID 1: mounts, CPU governor, static IP, supervises the other three |
| `scsynth` | SuperCollider server, bound to 127.0.0.1 only |
| `src/midid.c` | reads `/dev/snd/midiC*D*` (no alsa-lib), sends notes to scsynth as OSC |
| `src/recvd.c` | receives files over Ethernet into the data partition |

`init`, `midid` and `recvd` depend only on libc, so they build as static musl binaries.

## SD card layout

| Partition | FS | Mounted | Contents |
|---|---|---|---|
| p1 | FAT | `/boot`, read-only | Pi firmware, `config.txt`, `cmdline.txt`, kernel, initramfs |
| p2 | FAT | `/data`, read-write | `midid.conf`, `scsynth.args`, `synthdefs/`, `samples/`, `log.txt` |

`data/` in this repo is a template for p2. Both partitions are FAT, so you can
edit them on any laptop. Files reach `/data` either way:

- **Copy them to the card** (large sample sets, the initial setup).
- **Send them over Ethernet** while experimenting: `tools/pi put <file>`.

recvd writes atomically (temp file, fsync, rename). The only other writes are
the boot log, each line synced as it's written. Pulling the power is safe
except during an upload; at worst it cuts off the end of `log.txt`.

## Workflow

On the laptop, in sclang:

```supercollider
"~/Develop/MIDICollider/tools/pi.scd".standardizePath.load;   // or open it and run it
~piPut.(SynthDef(\pad, { |freq=440, gate=1, amp=0.2| ... }));
```

`~piPut` adds the def locally (to audition it), writes the `.scsyndef` and uploads
it. The Pi loads it into scsynth immediately and again on every boot.

From a shell:

```sh
tools/pi put data/midid.conf      # midid notices the change by itself
tools/pi put data/scsynth.args
tools/pi restart                  # restart scsynth + midid, e.g. after new args
tools/pi ls synthdefs
tools/pi del synthdefs/pad.scsyndef
tools/pi log                      # this boot's log (tools/pi log old: the previous one)
tools/pi get midid.conf my.conf   # download a file
```

`PI_HOST` (default `10.0.0.2`) selects the Pi. Change the Pi's address with
`mc_ip=10.0.0.5/24` in `cmdline.txt`, or `mc_ip=off`. Give the laptop's Ethernet
port an address in the same subnet, e.g. `10.0.0.1/24`.

### SynthDef convention

midid creates one node per key and passes these controls:
`freq`, `note`, `vel` (0..1), `amp` (vel²), `bend` (semitones), `gate` (1, then 0 on
release), `buf`/`rate`/`svel` when `sample` lines are configured, and any control a `cc`
line maps to. **The def must free itself** (`doneAction: Done.freeSelf`) once the gate
closes. See `tools/synthdefs.scd` for examples and `data/midid.conf` for the config
format: default/per-channel synth, program change map, CC map, bend range and samples.

### Piano

The default synth is `piano`: the [Salamander Grand Piano](https://freepats.zenvoid.org/Piano/acoustic-grand-piano.html)
(Yamaha C5, by Alexander Holm, CC BY 3.0), sampled every minor third. It uses 4 of the
16 velocity layers, cut to 9 s each, because scsynth keeps samples in RAM as
32-bit float (about 360 MB). The samples aren't in this repo: `make image` runs
`make piano` first, which downloads the pack (707 MB, checked against a SHA-256),
extracts the 4 layers and runs `tools/prepare-piano`. That writes
`data/samples/piano/` and the `sample` lines in `data/midid.conf`. For longer notes:
`LENGTH=12 tools/prepare-piano dl/SalamanderGrandPiano-SFZ+FLAC-V3+20200602`. midid
picks the nearest sample in pitch and, of its layers, the one recorded nearest to the
played velocity; the `piano` def scales it by `vel/svel` to fill the gap between layers.
scsynth loads the samples at startup, which takes a while on the Pi: keys stay silent
until they're in.

### recvd protocol

One command per TCP connection, with a one-line header, e.g. `PUT <size> <path>\n` followed by
the bytes. See the comment at the top of `src/recvd.c`. There's no authentication:
it's meant for a direct cable.

## Testing on the laptop

```sh
make
mkdir -p /tmp/mc && cp -r data/* /tmp/mc/ && rm /tmp/mc/scsynth.args
PATH=$PWD/build:$PATH MC_DATA=/tmp/mc build/init     # supervise mode (not PID 1)
PI_HOST=127.0.0.1 tools/pi ls                        # in another terminal
```

To test without a keyboard, make a FIFO and point midid at its directory:
`mkfifo /tmp/midi/midiC9D0; midid -d /tmp/mc -m /tmp/midi`, then write raw MIDI
bytes to it. A real keyboard works too, as long as nothing else (PipeWire, a2jmidid)
holds its `/dev/snd/midiC*` open.

## Building the SD card image

On Debian/Ubuntu, install what Buildroot and the piano preparation need:

```sh
sudo apt install build-essential git rsync bc cpio unzip file wget curl ffmpeg
```

Then, from a fresh clone:

```sh
make image                 # clones Buildroot 2026.02.3, prepares the piano, builds
make flash DEV=/dev/mmcblk0   # writes and verifies the card; check the device with lsblk
```

The first build downloads about 1.3 GB (600 MB of toolchain and sources, 707 MB
of piano) and needs about 6 GB of disk. Later runs only rebuild what changed:
`make image` always rebuilds the `src/` programs.
Use `make -C output menuconfig` to change the Buildroot configuration.

`buildroot-external/` is a standard `BR2_EXTERNAL` tree:

| Path | What |
|---|---|
| `configs/midicollider_rpi3_defconfig` | Pi 3B+ 64-bit, Bootlin musl toolchain, no BusyBox/shell/init system, initramfs-only root |
| `package/supercollider/` | scsynth 3.14.1 + core UGens only (server subtree, PortAudio→ALSA, no sclang/Qt) |
| `package/midicollider/` | `src/` built statically: `/init`, `/usr/bin/midid`, `/usr/bin/recvd` |
| `board/rpi3/linux.config` | trimmed kernel config, no modules (generated, see below) |
| `board/rpi3/linux.fragment` | what we add to `bcm2711_defconfig`: ALSA, USB audio, onboard jack, HiFiBerry DAC/DAC+; no Wi-Fi/BT |
| `board/rpi3/config.txt`, `cmdline.txt` | firmware config, serial console on GPIO 14/15, `mc_ip=` |
| `board/rpi3/genimage.cfg` | p1 boot FAT (64 MB), p2 data FAT (1 GB) seeded from `data/` |
| `patches/linux/` | kernel patches, applied on top of the Raspberry Pi tree (see below) |

`make synthdefs` recompiles `tools/synthdefs.scd` into `data/synthdefs/`, so the
image boots with something to play.

**Kernel config:** `linux.config` is `bcm2711_defconfig` plus `linux.fragment`,
with every module and a list of unused subsystems removed (Pi 4 hardware, ext4/NFS,
KVM, netfilter, tracing, USB storage...), and module support itself turned off.
After changing the fragment or the trim list, or bumping the kernel, regenerate it:

```sh
buildroot-external/board/rpi3/trim-kernel-config.sh
make -C output linux-reconfigure all
```

For one-off changes, `make -C output linux-menuconfig` followed by
`make -C output linux-update-defconfig` also works, but the script won't know
about them.

**Kernel patches:** `patches/linux/0001-ALSA-usb-audio-don-t-resubmit-MIDI-URBs-at-once-on-E.patch`
fixes a keyboard going silent after it's switched off and on. While a
bus-powered keyboard powers down it answers with stalls (`-EPIPE`), and
snd-usb-audio resubmitted those URBs at once and printed `urb status -32` for
each. On the Pi that loop keeps CPU 0, which handles all interrupts, too busy
to process the hub's disconnect, so the keyboard stayed silent until it was
replugged. With the patch the URBs are retried every 100 ms without a message,
the disconnect comes through and midid reopens the keyboard by itself.
After a kernel bump, check the patch still applies: `make -C output linux-dirclean linux`.

**Debugging:** you don't need a serial cable.

- **ACT LED (green):** blinks in a heartbeat pattern (two quick blinks, then a
  pause) once init is running and supervising the services. If it never does,
  the kernel or init didn't get that far.
- **`log.txt` on the data partition:** the kernel log, init's messages and every
  service's output, with uptime stamps. The previous boot's is `log.old.txt`.
  Put the card in the laptop, or use `tools/pi log` over Ethernet.
  Once a minute init adds a line like
  `init: stats: 52.1 C, 1400 MHz, load 9.0% (irq 0.4%, softirq 1.1%), active, top: scsynth 31.0%, ...`:
  SoC temperature, CPU clock, load across all four cores (interrupt time
  separately, as it belongs to no process), whether it's idle, and the busiest
  processes, each as a share of one core like `top`.
- **Idle:** after `idle` minutes without MIDI input (`midid.conf`, default 15,
  0 = never), midid has init switch the CPUs to the powersave governor
  (600 MHz). The next key switches them back; the log shows
  `init: idle, CPUs on powersave` / `init: awake, CPUs on performance`.
  scsynth keeps running: pausing it stops the audio stream, and the onboard
  jack clicks when the stream starts again.
- **Watchdog:** midid pings scsynth once a second, and scsynth answers from
  its audio thread. If scsynth stops answering for 10 s without exiting,
  midid logs `midid: no reply from scsynth for 10 s, asking init to restart it`,
  and init kills scsynth (`init: scsynth (pid N) hung, killing it`) and
  starts a new one, which then reloads the samples.
- **HDMI:** the console is on the screen as well.
- **Serial**, if you have a 3.3 V USB-serial adapter: GPIO 14/15, 115200 baud.

## Still to do

- First boot on real hardware: check the audio device name scsynth reports and
  put it in `scsynth.args` (`-H`).
- Static scsynth: SuperCollider 3.14.1 doesn't expose `STATIC_PLUGINS` in CMake
  yet (only `develop` does), so the UGens stay `.so` plugins for now.
- `PREEMPT_RT` instead of `PREEMPT`, if latency needs it.
- midid: per-channel CCs (they're global now) and multiple sample sets (one set is shared by all synths).
