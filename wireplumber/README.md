# EVO4 Audio Setup — PipeWire + WirePlumber

Arch Linux, PipeWire 1.6+, WirePlumber 0.5+.

## The Problem

The EVO4 exposes 4 USB audio channels but has only 2 physical inputs and 2 physical
outputs. The 4 channels map as:

- CH1/CH2 (FL/FR) - main output / mic input
- CH3/CH4 (RL/RR) - device loopback bus (output feeds back into capture)

Without configuration, PipeWire sees a "Surround 4.0" device and upmixes stereo
audio across all 4 channels, causing volume and channel-mapping issues.

## Signal Flow

### Playback

```
Application (stereo)
    |
    v
alsa_output.usb-Audient_EVO4-00.analog-surround-40  (raw ALSA 4ch sink)
    channelmix.upmix = false  ->  stereo apps only write FL/FR (CH1/CH2)
    |
    v
EVO4 DAC -> Headphones / Speakers

evo4_loopback_output (virtual 2ch sink, loopback module)
    |
    v
alsa_output  [CH3/CH4 - RL/RR]  ->  EVO4 loopback bus input
```

### Capture

```
EVO4 ADC <- Mic/Line inputs (physical)
    |
    v
alsa_input.usb-Audient_EVO4-00.analog-surround-40  (raw ALSA 4ch source)
    |
    +-- CH1/CH2 (FL/FR) --> evo4_mic             (virtual 2ch source, physical mics)
    +-- CH3/CH4 (RL/RR) --> evo4_loopback_capture (virtual 2ch source, loopback bus)
```

## Config Files

| File | Install location | Purpose |
|------|-----------------|---------|
| `evo4-stereo.conf` | `~/.config/pipewire/pipewire.conf.d/` | Three loopback modules: `evo4_loopback_output`, `evo4_mic`, `evo4_loopback_capture` |
| `51-evo4.conf` | `~/.config/wireplumber/wireplumber.conf.d/` | Disables UCM, disables idle suspension (prevents clicks), disables upmix on output, renames ALSA nodes |
| `evo4-setup.sh` | `~/.local/bin/` | Sets EVO4 nodes as default sink/source via `wpctl` |
| `evo4-setup.service` | `~/.config/systemd/user/` | Runs `evo4-setup.sh` at login |

## Volume Control

The raw ALSA sink (`EVO4 Hardware (4ch)`) is the default sink and PipeWire
drives the EVO4's hardware mixer through it: media keys, `wpctl set-volume`,
pavucontrol, the physical knob and `evoctl set volume` all move the same
control. This relies on `evo_raw` renaming the control to `Master Playback
Volume` and on the `snd-usb-audio` quirk installed by `kmod/install.sh` (see
the top-level README).

Notes:

- The sink is 4-channel; RL/RR (the loopback pair) are kept at 0 so stereo
  apps never leak into the loopback bus. Use absolute values with `wpctl`
  (`wpctl set-volume ID 0.6`); relative steps such as `1%-` are computed
  against the muted channels by WirePlumber 0.5.x and collapse the volume to 0.
- If volume changes only affect PipeWire and not the hardware after a
  hot-plug, WirePlumber probed the card before the rename. The udev wait rule
  prevents that; as a fallback `systemctl --user restart wireplumber` re-probes.

## Installation

```bash
bash wireplumber/install.sh
```

Backs up existing configs, installs all files, restarts the audio stack, and sets defaults.

To apply defaults manually (e.g. after reconnecting the device):

```bash
evo4-setup.sh
```

## Troubleshooting

```bash
wpctl status                      # check default sink/source (marked with *)
pw-cli dump Node | grep -A5 evo4  # verify node properties
pw-top                            # live PipeWire graph activity
lsusb | grep Audient              # confirm USB connection
aplay -l | grep EVO4              # confirm ALSA sees the device
```
