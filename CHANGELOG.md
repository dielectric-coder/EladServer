# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Bug Fixes

- **Double parameter toggle on encoder button press** — Each button press on encoder 1 advanced the active parameter by two steps instead of one. The rotary encoder poll callback was calling `rotary_encoder_toggle_param()` internally, and then the button callback in main.c toggled it again. Removed the redundant toggle from `rotary_encoder.c`.

- **Zoom level clamping mismatch** — The spectrum and waterfall widgets clamped zoom to a maximum of 8x, but the encoder code in main.c allowed up to 16x. Increased the widget clamp from 8 to 16 to match the documented zoom levels (1x, 2x, 4x, 8x, 16x).

- **Division by zero in frequency labels** — The spectrum widget frequency label code divided by `spectrum_size`, which is zero before the first FFT data arrives. Changed to use the `FFT_SIZE` constant, consistent with the band overlay code.

- **Integer division precision loss** — `sample_rate / zoom_level` was computed as integer division before assignment to a double, losing fractional precision at higher zoom levels. Added explicit `(double)` casts.

- **Overlay text truncation** — The `overlay_mode` buffer was 16 bytes but could receive up to 32 bytes (mode name + filter string). Increased to 32 bytes. Also increased `overlay_text` from 64 to 68 bytes to prevent snprintf truncation warnings.

- **Misleading `(void)length` suppression** — The USB data callback marked `length` as unused with `(void)length` but then passed it to `fft_processor_process()`. Removed the incorrect suppression.

### Security

- **TCP CAT server now binds to localhost by default** — The CAT command server previously bound to `INADDR_ANY` (all network interfaces) with no authentication. Changed default to `INADDR_LOOPBACK` (127.0.0.1). Use `-l any` to listen on all interfaces for LAN access.

- **Added `-l, --cat-listen` CLI flag** — Allows choosing between `localhost` (default, secure) and `any` (all interfaces, for LAN access). Remote access via SSH tunneling is documented as the recommended approach.

- **Checked write() return values in CAT server** — Client socket `write()` calls in the CAT server were ignoring return values. Added error checks that close the connection on write failure.

### Thread Safety

- **Made USB `streaming` flag atomic** — The `streaming` field in `usb_device_t` was a plain `int` accessed from both the USB transfer callback (libusb thread context) and the main thread via `start_streaming`/`stop_streaming`. Changed to `atomic_int` with proper `atomic_load`/`atomic_store` operations, consistent with `disconnected` and `transfers_pending`.

### Code Cleanup

- **Removed unused structs from `app_state.h`** — Removed `iq_sample_t`, `spectrum_buffer_t`, and `app_state_t` which were remnants of an earlier design. The header now only contains the shared constants (`FFT_SIZE`, `WATERFALL_LINES`, `USB_BUFFER_SIZE`, `DEFAULT_SAMPLE_RATE`).

- **Removed unused `waterfall_data` field** — The `waterfall_data` ring buffer in the waterfall widget was allocated and freed but never used for rendering (the widget uses a cairo surface for direct pixel manipulation). Removed the field and all references.

- **Removed legacy encoder pin defines** — Removed unused backward-compatibility defines (`ENCODER_CLK_PIN`, `ENCODER_DT_PIN`, `ENCODER_SW_PIN`) from `rotary_encoder.h`.

- **Updated deprecated GTK API** — Replaced `gtk_css_provider_load_from_data()` with `gtk_css_provider_load_from_string()` to fix deprecation warning with GTK 4.

## [1.0.0] - 2025

### Features

- USB device handling with async bulk transfers from endpoint 0x86
- 4096-point FFT with Blackman-Harris window using FFTW3
- 3-frame spectrum averaging for noise reduction
- Live spectrum analyzer with cyan trace and grid
- Waterfall display with direct pixel rendering and time axis
- Local and UTC time display overlay on waterfall
- Adjustable reference level and dynamic range (independent for spectrum and waterfall)
- Red tuned frequency marker line with off-screen arrow indicators
- CAT control via serial port for frequency, mode, VFO, and filter bandwidth
- VFO A/B indicator in spectrum frame label
- Bandwidth indicator lines on waterfall (red for most modes, orange for CW resonator)
- Mode-aware bandwidth positioning (USB/LSB/CW/AM/FM/data modes)
- Band plan overlay on frequency axis (green for ham, orange for broadcast, gray for other)
- ITU Region 1, 2, and 3 band plan files
- Horizontal zoom (1x-16x) and pan with rotary encoder support
- Dual rotary encoder support for Raspberry Pi (parameter control + zoom/pan)
- Pi mode for 5" LCD displays (800x480, dark theme, compact controls)
- Fullscreen mode
- Automatic USB reconnection with FPGA re-initialization
- Settings persistence with debounced auto-save
- TCP CAT command server for remote control by external applications
- Arch/Manjaro PKGBUILD and Debian packaging
