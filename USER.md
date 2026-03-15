# Elad Spectrum - User Guide

A spectrum analyzer and waterfall display for the Elad FDM-DUO SDR transceiver.

## Quick Start

### Prerequisites

1. **Elad FDM-DUO** transceiver connected via USB
2. **Linux** system (tested on Ubuntu 24.04, Manjaro, and Raspberry Pi OS)

### Installation

**From Debian Package (Recommended):**
```bash
sudo dpkg -i elad-spectrum_1.0.0_*.deb
sudo apt-get install -f  # Install any missing dependencies
```

**From Source (Debian/Ubuntu):**
```bash
sudo apt install libgtk-4-dev libusb-1.0-0-dev libfftw3-dev libjson-glib-dev meson ninja-build
git clone https://github.com/dielectric-coder/EladSpectrum.git
cd EladSpectrum
meson setup build
meson compile -C build
```

**From PKGBUILD (Manjaro/Arch Linux):**
```bash
sudo pacman -S gtk4 libusb fftw json-glib meson ninja
git clone https://github.com/dielectric-coder/EladSpectrum.git
cd EladSpectrum
makepkg -si
```

**From Source (Manjaro/Arch Linux):**
```bash
sudo pacman -S gtk4 libusb fftw json-glib meson ninja
git clone https://github.com/dielectric-coder/EladSpectrum.git
cd EladSpectrum
meson setup build
meson compile -C build
```

### USB Permissions

To run without root, add a udev rule:

```bash
sudo tee /etc/udev/rules.d/99-elad.rules << 'EOF'
# Elad FDM-DUO
SUBSYSTEM=="usb", ATTR{idVendor}=="1721", ATTR{idProduct}=="061a", MODE="0666"
EOF
sudo udevadm control --reload-rules
```

Then reconnect the radio.

### Serial Port Access

Add your user to the dialout group for CAT control:
```bash
sudo usermod -aG dialout $USER
```
Log out and back in for the change to take effect.

## Running the Application

```bash
# Desktop mode (1024x768 window)
elad-spectrum

# Fullscreen mode
elad-spectrum -f

# Raspberry Pi 5" LCD (800x480)
elad-spectrum -p

# Pi fullscreen (recommended for embedded)
elad-spectrum -p -f
```

## User Interface

```
┌─────────────────────────────────────────────────────────┐
│                    VFO A                                │
│  ┌───────────────────────────────────────────────────┐  │
│  │ dB    14.200000 MHz  USB 2.4k                     │  │
│  │  0 ┼──────────────────────────────────────────────│  │
│  │-20 ┼─────────────╱╲──────────────────────────────│  │
│  │-40 ┼────────────╱  ╲─────────────────────────────│  │
│  │-60 ┼───────────╱    ╲────────────────────────────│  │
│  │-80 ┼──────────╱      ╲───────────────────────────│  │
│  │    └──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──│  │
│  │      ████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░│  │
│  └───────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────┐  │
│  │ LOCAL 14:32:15                      UTC 19:32:15  │  │
│  │ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  │
│  │ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  │
│  │ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│              ● Ref [-30] Rng [120]                      │
└─────────────────────────────────────────────────────────┘
```

### Display Elements

#### Status Indicator
| Symbol | Color | Meaning |
|--------|-------|---------|
| ● | Green | Connected and streaming |
| ○ | Gray | Disconnected or searching |
| ● | Red | Error state |

#### Spectrum Display
- **Cyan trace**: Real-time spectrum (signal strength vs frequency)
- **Red vertical line**: Tuned frequency marker
- **Grid**: 10 divisions horizontal and vertical
- **Overlay text**: Current frequency, mode, and filter bandwidth

#### Frequency Axis Band Overlay
Colored rectangles on the x-axis indicate radio bands:
| Color | Band Type | Examples |
|-------|-----------|----------|
| Green | Amateur Radio | 40m, 20m, 15m, 10m |
| Orange | Broadcast | AM, Shortwave |
| Gray | Other | CB, Marine, Air |

#### Waterfall Display
- **Scrolling spectrogram**: Signal history over time
- **Time labels**: Local time (left), UTC (right)
- **Dashed lines**: Filter bandwidth indicators
- **Color scale**: Blue (weak) → Cyan → Green → Yellow → Red (strong)

### Controls

#### Desktop Mode
| Control | Function |
|---------|----------|
| **Ref** spinner | Reference level (top of display, dB) |
| **Rng** spinner | Dynamic range (dB span) |
| `+` / `=` | Zoom in |
| `-` | Zoom out |
| Left / Right arrow | Pan left / right (when zoomed) |
| Home | Reset zoom and pan |

#### Pi Mode
In Pi mode (`-p`), the control bar shows a compact display:
```
● SP.REF -30  SP.RNG 120  WF.REF -30  WF.RNG 120  SPAN 192  OFS +0
```

Parameters are controlled via rotary encoders:

**Encoder 1 (Left):**
- **Rotation**: Adjust highlighted parameter
- **Button**: Cycle through parameters (SP.REF → SP.RNG → WF.REF → WF.RNG)

**Encoder 2 (Right):**
- **Rotation**: Zoom in/out or pan left/right
- **Button**: Toggle between Zoom and Pan mode

### Zoom Levels

| Zoom | Span | Resolution |
|------|------|------------|
| 1x | 192 kHz | 46.9 Hz/bin |
| 2x | 96 kHz | 23.4 Hz/bin |
| 4x | 48 kHz | 11.7 Hz/bin |
| 8x | 24 kHz | 5.9 Hz/bin |
| 16x | 12 kHz | 2.9 Hz/bin |

## Remote CAT Control

The application can expose a TCP server for remote CAT command access. By default it listens on **localhost only** (127.0.0.1).

### Command-line Options

```bash
# Start CAT server on port 4532 (localhost only)
elad-spectrum -c 4532

# Start CAT server on all interfaces (LAN access)
elad-spectrum -c 4532 -l any
```

| Option | Description |
|--------|-------------|
| `-c, --cat-port PORT` | Start TCP CAT server on PORT |
| `-i, --iq-port PORT` | Start TCP IQ streaming server on PORT |
| `-I, --iq-input HOST:PORT` | Connect to remote IQ server (replaces USB) |
| `-C, --cat-input HOST:PORT` | Connect to remote CAT server (replaces serial) |
| `-l, --cat-listen ADDR` | Listen address: `localhost` (default) or `any` |

## IQ Streaming Server

The application can stream raw IQ sample data over TCP for external processing by tools such as **elad-demod** (TUI demodulator).

```bash
# Start with IQ server on port 4533 and CAT server on port 4532
elad-spectrum -i 4533 -c 4532

# Start with IQ server accessible from LAN
elad-spectrum -i 4533 -c 4532 -l any
```

The IQ server sends a 16-byte header on connection (magic, sample rate, format), then streams continuous raw 32-bit signed IQ pairs at the radio's sample rate (192 kHz). Up to 8 clients can connect simultaneously.

## Network Input Mode

Instead of connecting directly to a radio via USB, the application can receive IQ and CAT data from a remote EladSpectrum instance over TCP. This is useful for:
- Remote monitoring of a radio on another machine
- Chaining multiple spectrum displays
- Feeding data to tools like SWLDemodTool via a local relay

```bash
# Connect to remote IQ and CAT servers
elad-spectrum -I fdmduopi.local:4533 -C fdmduopi.local:4532

# Connect to remote and re-serve locally (for chaining)
elad-spectrum -I fdmduopi.local:4533 -C fdmduopi.local:4532 -i 4534 -c 4535

# IQ only (no CAT — frequency/mode won't be displayed)
elad-spectrum -I 192.168.1.10:4533
```

Both IQ and CAT clients auto-reconnect if the remote server goes down.

### Testing with netcat

```bash
echo "IF;" | nc localhost 4532
```

### Remote Access via SSH

To control the radio from another machine while keeping the server on localhost (recommended):

```bash
# On the remote machine, create an SSH tunnel:
ssh -L 4532:localhost:4532 user@radio-host

# Then in another terminal on the remote machine:
echo "IF;" | nc localhost 4532
```

This is more secure than using `-l any` since the connection is encrypted and requires SSH authentication.

### Listening on All Interfaces

If you prefer direct LAN access without SSH:

```bash
elad-spectrum -c 4532 -l any
```

**Note:** There is no authentication on the CAT server. Anyone on the network can send commands to the radio when using `-l any`. Only use this on trusted networks.

## Radio Integration

### Automatic Frequency Tracking
The application reads frequency, mode, and VFO selection from the radio via CAT commands. When you tune the radio:
- Spectrum display centers on new frequency
- Overlay updates with frequency and mode
- Band overlay highlights current band
- Bandwidth lines track filter settings

### Supported Modes
| Mode | Bandwidth Display |
|------|-------------------|
| USB | Upper sideband line |
| LSB | Lower sideband line |
| CW | Symmetric around center |
| CW-R | Symmetric around center |
| AM | Symmetric around center |
| FM | Wide or Narrow bandwidth |

### Filter Bandwidth
The application shows your current filter bandwidth:
- **Dashed lines** on waterfall indicate filter edges
- **Orange lines** for CW resonator filters (100&1, etc.)
- **Red lines** for other filter types

## Settings

Settings are automatically saved to `~/.config/elad-spectrum/settings.conf`

| Setting | Description | Default |
|---------|-------------|---------|
| spectrum_ref | Spectrum reference level | -30 dB |
| spectrum_range | Spectrum dynamic range | 120 dB |
| waterfall_ref | Waterfall reference level | -30 dB |
| waterfall_range | Waterfall dynamic range | 120 dB |
| zoom_level | Horizontal zoom | 1x |
| pan_offset | Pan position | 0 (center) |

Settings auto-save 3 seconds after any change.

## Reconnection Behavior

The application handles radio power cycling automatically:

1. **Radio powers off**: Status changes to gray, display freezes
2. **Radio powers on**: Application detects USB reconnection
3. **3 second delay**: Waits for radio FPGA to initialize
4. **Resumes streaming**: Display updates, status turns green

No manual intervention required.

## Troubleshooting

### "Cannot open FDM-DUO device"
- Check USB cable connection
- Verify udev rules are installed
- Try `lsusb | grep 1721` to confirm device is visible

### "CAT: Cannot open /dev/ttyUSB0"
- Radio may not be powered on
- Check dialout group membership
- Verify serial port: `ls -la /dev/ttyUSB*`

### No spectrum display
- Check status indicator (should be green)
- Radio may be in standby mode
- Try power cycling the radio

### Band overlay not showing
- Verify band plan file exists
- Check console for "Bandplan: Loaded X bands" message
- Current frequency may be outside defined bands

### Waterfall display frozen
- USB connection may have dropped
- Check status indicator
- Application should auto-reconnect when radio returns

## ITU Regions

The application includes band definitions for all ITU regions:

| File | Region | Coverage |
|------|--------|----------|
| bands-r1.json | Region 1 | Europe, Africa, Middle East |
| bands-r2.json | Region 2 | Americas |
| bands-r3.json | Region 3 | Asia, Oceania |

Default is Region 1. To use a different region, copy the desired file:
```bash
cp /usr/share/elad-spectrum/bands-r2.json ~/.config/elad-spectrum/bands.json
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `+` / `=` | Zoom in |
| `-` | Zoom out |
| Left arrow | Pan left (when zoomed) |
| Right arrow | Pan right (when zoomed) |
| Home | Reset zoom and pan to 1x |

Keyboard shortcuts are available in desktop mode. In Pi mode, use the rotary encoders instead.

## Tips for Best Results

1. **Noise Floor**: Adjust Ref level so noise floor is near bottom of display
2. **Dynamic Range**: Use 80-100 dB for most conditions, 120 dB for weak signals
3. **Zoom**: Use higher zoom levels to see signal details
4. **Waterfall Speed**: ~15 lines/second (depends on FFT averaging)

## Getting Help

- **Issues**: https://github.com/dielectric-coder/EladSpectrum/issues
- **Documentation**: See CLAUDE.md for technical details
- **Developer Info**: See DEVELOPER.md for architecture

## License

This software is provided as-is for amateur radio use.

73 de VE2EXB
