# Elad Spectrum - Developer Documentation

Technical documentation for developers working on the Elad Spectrum application.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         GTK4 Application                             │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐  │
│  │ SpectrumWidget  │  │ WaterfallWidget │  │    Control Bar      │  │
│  │  (GtkDrawingArea)│  │ (GtkDrawingArea)│  │  (Labels, Spins)    │  │
│  └────────┬────────┘  └────────┬────────┘  └─────────────────────┘  │
│           │                    │                                     │
│           └──────────┬─────────┘                                     │
│                      │                                               │
│              ┌───────▼───────┐                                       │
│              │   main.c      │◄──── Settings (load/save)             │
│              │  (app_data_t) │◄──── Bandplan (JSON load)             │
│              └───────┬───────┘                                       │
└──────────────────────┼───────────────────────────────────────────────┘
                       │
         ┌─────────────┼─────────────┬─────────────┐
         │             │             │             │
         ▼             ▼             ▼             ▼
┌─────────────┐ ┌─────────────┐ ┌─────────────┐ ┌───────────────┐
│ USB Thread  │ │ CAT Control │ │ GPIO Thread │ │  TCP Servers   │
│ (libusb)    │ │ (serial)    │ │ (libgpiod)  │ │  CAT + IQ     │
│  ── OR ──   │ │  ── OR ──   │ └─────────────┘ │   ── AND ──   │
│ IQ Client   │ │ CAT Client  │   (Pi only)     │  TCP Clients   │
│ (TCP input) │ │ (TCP input) │                  │  IQ + CAT      │
└──────┬──────┘ └──────┬──────┘                  └───────┬───────┘
       │               │                                 │
       ▼               ▼                                 ▼
┌─────────────┐ ┌─────────────┐                  Network clients
│ FFT Process │ │ /dev/ttyUSB0│                  (SWLDemodTool, etc.)
│ (FFTW3)     │ │  or remote  │
└─────────────┘ │  CAT server │
       │        └─────────────┘
       ▼
  FDM-DUO USB
  Endpoint 0x86
  or remote IQ server
```

## Module Descriptions

### Core Modules

#### `main.c` - Application Entry Point
- GTK4 application lifecycle management
- Window creation and layout
- Thread coordination (USB, CAT poll, GPIO)
- Timer-based display refresh (~30 FPS) reads cached CAT data (no I/O)
- Background CAT poll thread for serial/network I/O (~300ms interval)
- Settings persistence with debounced auto-save

**Key Data Structure:**
```c
typedef struct {
    GtkApplication *app;
    GtkWidget *window, *spectrum, *waterfall;
    usb_device_t *usb;
    fft_processor_t *fft;
    cat_control_t *cat;
    bandplan_t bandplan;
    pthread_t usb_thread;
    atomic_int running, usb_connected;
    // ... display parameters, settings
} app_data_t;
```

#### `app_state.h` - Shared Constants and Types
- FFT size (4096 samples)
- Buffer sizes
- Sample rate (192 kHz default)
- IQ sample structure
- Double-buffer for thread-safe data exchange

### Hardware Interface Modules

#### `usb_device.c/h` - USB Communication
Handles all communication with the FDM-DUO via libusb.

**Key Functions:**
| Function | Description |
|----------|-------------|
| `usb_device_new()` | Create handler, init libusb context |
| `usb_device_open()` | Find device, claim interface, init FIFO |
| `usb_device_start_streaming()` | Submit async bulk transfers |
| `usb_device_handle_events()` | Process libusb events (call from thread) |
| `usb_device_check_disconnected()` | Detect device removal |

**USB Protocol:**
- Vendor ID: `0x1721`, Product ID: `0x061a`
- RF Data Endpoint: `0x86` (bulk IN)
- Sample Format: 24-bit signed IQ pairs (6 bytes per sample)
- Transfer Size: 12288 bytes (2048 samples)

**Reconnection Handling:**
1. Transfer errors set disconnected flag
2. USB thread detects flag, closes device
3. Polls for device every 1 second
4. On reconnect: 3s stabilization delay, reinit FIFO

#### `cat_control.c/h` - CAT Serial Control
Kenwood TS-480 compatible CAT protocol via serial port.

**Commands Used:**
| Command | Response | Description |
|---------|----------|-------------|
| `IF;` | `IF...;` | Information (freq, mode, VFO) |
| `RF0;` | `RF0xx;` | Filter bandwidth (SSB/CW) |
| `RF1;` | `RF1x;` | Filter bandwidth (AM) |
| `RF2;` | `RF2x;` | Filter bandwidth (FM) |

**Serial Settings:** 38400 baud, 8N1, no flow control

#### `rotary_encoder.c/h` - GPIO Rotary Encoder (Pi Only)
Optional dual encoder support using libgpiod.

**Encoder 1 (GPIO 17/27/22):** Parameter control
- Rotation: Adjust ref/range values
- Button: Cycle active parameter

**Encoder 2 (GPIO 5/6/13):** Zoom/Pan control
- Rotation: Zoom in/out or pan left/right
- Button: Toggle zoom/pan mode

### Network Modules

#### `cat_server.c/h` - TCP CAT Command Server
Bidirectional passthrough for Kenwood CAT commands over TCP.

- Accepts up to 8 concurrent clients
- Each client gets a dedicated handler thread
- Commands are serialized through the shared `cat_mutex`
- Can forward to either serial `cat_control` or network `cat_client` backend
- Enabled via `-c PORT` CLI option

#### `iq_server.c/h` - TCP IQ Streaming Server
Broadcasts raw IQ sample data to network clients for external processing.

- Taps into the USB bulk transfer callback in `main.c`
- Sends 16-byte header on connection (magic `"ELAD"`, sample rate, format)
- Streams continuous `int32_t` IQ pairs (8 bytes/sample, little-endian)
- Accepts up to 8 concurrent clients
- Clients that can't keep up are disconnected
- Enabled via `-i PORT` CLI option

#### `iq_client.c/h` - TCP IQ Input Client
Connects to a remote IQ server and feeds data into the local FFT pipeline.

- Background thread with auto-reconnect (1s retry)
- Reads ELAD protocol header (magic, sample rate, format)
- Streams 12288-byte chunks into the same callback as USB bulk transfers
- Enabled via `-I HOST:PORT` CLI option (replaces USB input)

#### `cat_client.c/h` - TCP CAT Input Client
Connects to a remote CAT server for frequency/mode polling.

- Synchronous TCP connection (protected by `cat_mutex`)
- Same command/response protocol as serial CAT (Kenwood TS-480)
- Implements `get_freq_mode()` and `get_filter_bw()` with same parsing
- Auto-reconnects on polling cycle if disconnected
- Enabled via `-C HOST:PORT` CLI option (replaces serial input)

### Signal Processing Modules

#### `fft_processor.c/h` - FFT Computation
FFTW3-based spectrum analysis with averaging.

**Processing Pipeline:**
```
USB Data → Unpack 24-bit IQ → Accumulate Buffer →
Apply Window → FFT → Magnitude² → dB Conversion →
3-Frame Average → Output Spectrum
```

**Key Parameters:**
- FFT Size: 4096 samples
- Window: Blackman-Harris (excellent sidelobe rejection)
- Averaging: 3 frames (reduces noise floor by ~4.8 dB)
- Resolution: 46.9 Hz/bin at 192 kHz sample rate

#### `bandplan.c/h` - Band Plan Loading
Loads amateur radio band definitions from JSON.

**Data Structure:**
```c
typedef struct {
    char name[32];
    int64_t lower_bound;  // Hz
    int64_t upper_bound;  // Hz
    band_tag_t tag;       // HAMRADIO, BROADCAST, etc.
} band_entry_t;
```

**File Search Order:**
1. `./resources/bands-r1.json` (development)
2. `/usr/share/elad-spectrum/bands-r1.json` (installed)

### Display Modules

#### `spectrum_widget.c/h` - Spectrum Display
Custom GtkDrawingArea for real-time spectrum visualization.

**Drawing Order:**
1. Black background
2. Band overlays (colored rectangles on x-axis)
3. Grid lines (10x10)
4. Axis labels (dB left, frequency bottom)
5. Spectrum trace (cyan line with fill)
6. Center frequency marker (red line/arrow)
7. Overlay text (frequency, mode, filter)

**Zoom/Pan Support:**
- Zoom levels: 1x, 2x, 4x, 8x, 16x
- Pan: Bin offset from center
- All elements track zoom/pan correctly

#### `waterfall_widget.c/h` - Waterfall Display
Scrolling spectrogram with direct pixel rendering.

**Implementation:**
- Ring buffer of spectrum lines (256 lines)
- Cairo image surface for efficient rendering
- Color mapping: blue (weak) → cyan → green → yellow → red (strong)
- Time labels: Local (left) and UTC (right)

**Bandwidth Indicators:**
- Dashed vertical lines showing filter edges
- Mode-aware positioning (USB upper, LSB lower, CW/AM symmetric)
- Orange for CW resonator modes, red for others

#### `settings.c/h` - Settings Persistence
INI-style configuration file handling.

**Config Location:** `~/.config/elad-spectrum/settings.conf`

**Auto-save Behavior:**
- 3-second debounce after changes
- Also saves on window close
- Creates directory if needed

## Threading Model

```
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│    Main Thread   │     │  USB Thread OR   │     │   GPIO Thread    │
│    (GTK4 UI)     │     │  IQ Client Thread│     │   (Pi only)      │
├──────────────────┤     ├──────────────────┤     ├──────────────────┤
│ • Event loop     │     │ • Bulk transfers │     │ • Poll encoders  │
│ • Display update │     │   or TCP recv    │     │ • Debounce       │
│ • Read cached CAT│◄────│ • FFT processing │     │ • Callbacks      │
│ • Settings save  │     │ • Data callback  │     │                  │
│                  │     │ • Reconnection   │     │                  │
└──────────────────┘     └──────────────────┘     └──────────────────┘
         ▲                        │
         │     GMutex protection  │
         └────────────────────────┘

┌──────────────────┐     ┌──────────────────┐
│  CAT Poll Thread │     │  TCP Servers     │
├──────────────────┤     ├──────────────────┤
│ • Serial/TCP I/O │     │ • CAT passthrough│
│ • Freq/mode poll │     │ • IQ broadcast   │
│ • Filter poll    │     │ • DM command     │
│ • Cache results  │     │ • Client threads │
│   (every ~300ms) │     │                  │
└──────────────────┘     └──────────────────┘
```

The CAT poll thread runs all serial/network CAT I/O in the background,
writing results to cached fields in `app_data_t`. The GTK main thread
reads the cache atomically (microseconds) instead of blocking on serial
I/O (100-300ms per poll). This prevents display freezes, especially on
Raspberry Pi.

**Synchronization:**
- `GMutex` (`spectrum_mutex`) protects spectrum data buffer
- `GMutex` (`cat_cache_mutex`) protects cached CAT results (freq, mode, vfo, filter)
- `atomic_int` for flags (running, connected, ready, cat_cache_valid)
- `atomic_int_least64_t` for demod timestamp (safe on 32-bit ARM)
- `pthread_mutex_t` (`cat_mutex`) serializes serial port access between CAT poll thread and TCP server client handlers
- `pthread_mutex_t` (`fd_mutex`) protects IQ client socket fd between recv thread and stop
- `pthread_mutex_t` (`clients_mutex`) protects client fd arrays in both TCP servers; add/remove are atomic with count check
- TCP server client handlers own their fd lifecycle (close on exit); server stop uses `shutdown()` only to avoid double-close
- IQ broadcast uses `MSG_DONTWAIT` with 1MB `SO_SNDBUF`; transient `EAGAIN` drops the chunk instead of disconnecting (50 consecutive drops = disconnect)

## Build System

### Meson Configuration

**Dependencies:**
| Dependency | Purpose | Required |
|------------|---------|----------|
| gtk4 | GUI framework | Yes |
| libusb-1.0 | USB communication | Yes |
| fftw3 | FFT computation | Yes |
| json-glib-1.0 | Band plan loading | Yes |
| libgpiod | Rotary encoder | No (Pi only) |

**Install Dependencies:**
```bash
# Debian/Ubuntu
sudo apt install libgtk-4-dev libusb-1.0-0-dev libfftw3-dev libjson-glib-dev meson ninja-build

# Manjaro/Arch Linux
sudo pacman -S gtk4 libusb fftw json-glib meson ninja

# Raspberry Pi (optional, for rotary encoder support)
sudo apt install libgpiod-dev        # Debian/Ubuntu
sudo pacman -S libgpiod              # Manjaro/Arch
```

**Conditional Compilation:**
```meson
if gpiod_dep.found()
  src_files += 'src/rotary_encoder.c'
  add_project_arguments('-DHAVE_GPIOD', language: 'c')
endif
```

### Adding New Features

1. **New Module:**
   - Create `src/module.c` and `src/module.h`
   - Add to `src_files` in `meson.build`
   - Include header in `main.c`

2. **New Dependency:**
   - Add `dep = dependency('name')` in `meson.build`
   - Add to `deps` array
   - Add to `debian/control` Build-Depends
   - Add to `PKGBUILD` depends/makedepends

3. **New Settings:**
   - Add field to `app_settings_t` in `settings.h`
   - Update `settings_load()` and `settings_save()`
   - Update `settings_init_defaults()`

## Code Conventions

### Naming
- Types: `snake_case_t` (e.g., `usb_device_t`)
- Functions: `module_verb_noun()` (e.g., `usb_device_open()`)
- Constants: `UPPER_SNAKE_CASE`
- GTK types: Follow GLib conventions

### Memory Management
- Use GLib allocators (`g_malloc`, `g_free`) for GTK code
- Standard allocators (`malloc`, `free`) for non-GTK modules
- Always check allocation results
- Free in reverse order of allocation

### Error Handling
- Return 0 for success, negative for error
- Use `fprintf(stderr, ...)` for error messages
- Prefix messages with module name (e.g., "CAT:", "USB:")

## Debugging Tips

### USB Issues
```bash
# Check device presence
lsusb | grep 1721

# Monitor USB traffic
sudo modprobe usbmon
sudo wireshark  # Select usbmonX interface

# Check permissions
ls -la /dev/bus/usb/*/
```

### CAT Issues
```bash
# Test serial port
stty -F /dev/ttyUSB0 38400 cs8 -cstopb -parenb
echo "IF;" > /dev/ttyUSB0
cat /dev/ttyUSB0
```

### GTK Issues
```bash
# Enable GTK debug output
GTK_DEBUG=interactive ./build/elad-spectrum

# Check for memory leaks
G_DEBUG=gc-friendly G_SLICE=always-malloc valgrind ./build/elad-spectrum
```

## Performance Considerations

- FFT runs in USB thread to minimize latency
- Display updates at ~30 FPS (33ms timer)
- Waterfall uses direct pixel manipulation (no scaling)
- Band overlay uses pre-filtered visible bands only
- Settings save is debounced (3 second delay)

## Future Improvements

- [ ] Configurable FFT size
- [ ] Multiple sample rate support
- [ ] Audio interface integration
- [ ] Frequency markers/annotations
- [ ] Spectrum recording/playback
