# Elad Spectrum

A spectrum analyzer application for the Elad FDM-DUO Software Defined Radio transceiver.

> **Note:** This project is a work in progress.

## Overview

The FDM-DUO exposes three USB interfaces:

1. **Sound Card** - Audio I/O for transmit/receive audio
2. **Serial Port** - CAT (Computer Aided Transceiver) control commands
3. **Sampling Data Port** - Raw sampled RF data from the antenna (typically 192 KHz bandwidth)

This application provides a spectrum analyzer and waterfall display using the sampling data port, with CAT control for frequency and mode synchronization.

## Features

- Live spectrum analyzer with 4096-point FFT
- Waterfall display with time axis
- CAT control via serial port (Kenwood TS-480 compatible)
- Frequency and mode overlay display
- VFO A/B indicator
- Adjustable reference level and dynamic range
- Status indicator (colored circle: green=connected, gray=disconnected)
- Automatic reconnection after radio power cycle
- Dual rotary encoder support for Raspberry Pi (optional)

## Dependencies

### Debian/Ubuntu

```bash
sudo apt install libgtk-4-dev libusb-1.0-0-dev libfftw3-dev meson ninja-build
```

### Raspberry Pi CM5

```bash
sudo apt install libgtk-4-dev libusb-1.0-0-dev libfftw3-dev meson ninja-build
```

On Raspberry Pi OS, you may also need:

```bash
sudo apt install libwayland-dev libxkbcommon-dev
```

For rotary encoder support (optional):

```bash
sudo apt install libgpiod-dev
```

## Build

```bash
meson setup build
meson compile -C build
```

## Build .deb Package

To build a Debian package for installation:

```bash
# Install build dependencies
sudo apt install debhelper devscripts

# Build the package
dpkg-buildpackage -us -uc -b

# The .deb file will be created in the parent directory
ls ../*.deb
```

Install the package:

```bash
sudo dpkg -i ../elad-spectrum_*.deb
```

## Run

```bash
# Normal windowed mode (1024x768)
./build/elad-spectrum

# Fullscreen mode
./build/elad-spectrum -f

# Raspberry Pi 5" LCD (800x480)
./build/elad-spectrum -p

# Pi fullscreen (recommended for embedded use)
./build/elad-spectrum -p -f

# Help
./build/elad-spectrum -h
```

## Command-line Options

| Option | Description |
|--------|-------------|
| `-f, --fullscreen` | Start in fullscreen mode |
| `-p, --pi` | Set window size to 800x480 (5" LCD), enable rotary encoder |
| `-c, --cat-port PORT` | Start TCP CAT server on PORT (e.g., 4532) |
| `-i, --iq-port PORT` | Start TCP IQ streaming server on PORT (e.g., 4533) |
| `-l, --cat-listen ADDR` | Server listen address: `localhost` (default) or `any` |
| `-h, --help` | Show help message |

### Raspberry Pi Usage

The `-p` and `-f` options are designed for running on a Raspberry Pi with a small display. For embedded use with a 5" LCD (800x480), use both options together:

```bash
./build/elad-spectrum -p -f
```

This provides a fullscreen interface optimized for the Pi's display size.

## Rotary Encoders (Raspberry Pi)

Optional dual GPIO rotary encoders for hands-free control:

- **Encoder 1**: Parameter control (Ref/Range for spectrum and waterfall)
  - Button press cycles through parameters
  - Rotation adjusts current parameter value
- **Encoder 2**: Zoom/Pan control
  - Button press toggles zoom/pan mode
  - Rotation adjusts zoom level (1x/2x/4x) or pans the display

See `CLAUDE.md` for detailed GPIO pin assignments and usage.

## Hardware Requirements

- Elad FDM-DUO transceiver
- USB connection to the radio
- Serial port access (`/dev/ttyUSB0`) for CAT control

## IQ Streaming

The application can stream raw IQ sample data over TCP for external demodulation and analysis:

```bash
# Start with IQ server on port 4533 and CAT server on port 4532
./build/elad-spectrum -i 4533 -c 4532
```

The companion **elad-demod** TUI application connects to this stream for AM/SSB/CW demodulation with audio output. See the [elad-demod](https://github.com/mikewam/elad-demod) project.

## Future Features

- **USB Audio to Power Amp HAT** - Pipe USB audio from the radio to a power amplifier HAT, with optional DSP processing

## Notes

- USB interfaces may require root access or udev rules for user access
- Serial port requires `dialout` group membership or root access
- The FDM-DUO FPGA must be initialized before data streaming begins
- Application automatically reconnects if the radio is power cycled (3 second stabilization delay)

## License

This project is provided as-is for amateur radio experimentation.
