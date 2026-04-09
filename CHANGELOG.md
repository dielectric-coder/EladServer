# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [1.1.0] - 2026-04-09

### Features

- **TCP IQ streaming server** — New `-i PORT` option starts a TCP server that broadcasts raw IQ sample data to network clients. Streams continuous 32-bit signed IQ pairs at the radio's sample rate (192 kHz). Sends a 16-byte header on connection with magic, sample rate, and format info. Supports up to 8 concurrent clients. Used by the companion **elad-demod** TUI demodulator.

### Bug Fixes

- **SIGPIPE crash in IQ server** — `send_header()` used `write()` instead of `send(..., MSG_NOSIGNAL)`. A client disconnecting during header send would kill the entire process via SIGPIPE.

- **Division by zero in spectrum display** — `calc_visible_range` could produce `visible_bins == 1` at extreme zoom, causing division by zero in the spectrum draw function. Added guards for `zoom_level < 1`, `spectrum_size < 1`, and `visible_bins < 2`.

- **Use-after-free on CAT server shutdown** — Client handler threads were detached and could access the server struct after `cat_server_free()` destroyed the mutex. Switched to joinable threads with slot tracking; `stop()` now joins all handler threads.

- **USB transfer buffer freed while in-flight** — After `stop_streaming` timeout, transfer buffers were freed in `close()` even though libusb callbacks might still reference them. Now `close()` checks whether transfers were fully freed before freeing buffers.

- **USB transfers_pending race** — `transfers_pending` was incremented after `libusb_submit_transfer`, creating a window where a callback could fire and decrement before the increment. Moved increment before submit.

- **NULL cancel on partial transfer alloc failure** — If `libusb_alloc_transfer` failed for index > 0, `stop_streaming` would cancel NULL transfer pointers. Now clears all transfer pointers before the allocation loop.

- **FFT spectrum data race** — `spectrum_db[]` and `rssi_db` were written by the USB callback thread and read by the GTK draw thread with no synchronization. Added `pthread_mutex_t output_mutex`.

- **Widget setter data races** — `spectrum_widget` and `waterfall_widget` setter functions (`set_range`, `set_zoom`, `set_pan`, `set_center_freq`, `set_sample_rate`, `set_bandwidth`, `set_demod_bandwidth`) modified draw state without locking `data_mutex`. All setters now lock the mutex.

- **Waterfall draw reads after mutex release** — The waterfall draw function accessed `bandwidth_hz`, `sample_rate`, `demod_bandwidth_hz`, and other fields after releasing the mutex. Now copies all needed fields under the lock.

- **Pending save timeout not cancelled on shutdown** — `save_timeout_id` was not cancelled in `shutdown_app()`, allowing the callback to fire after resources were freed. Added `g_source_remove()`.

- **O_NONBLOCK left set on serial fd** — The CAT serial port was opened with `O_NONBLOCK` (needed to avoid blocking on DCD) but never cleared. `write()` could return EAGAIN, treated as a fatal error. Now clears `O_NONBLOCK` via `fcntl()` after port configuration.

- **EAGAIN not handled in serial write** — The CAT command write loop only retried on EINTR, not EAGAIN. Added EAGAIN to the retry condition.

- **Blocking client read with no timeout** — CAT server client sockets had no `SO_RCVTIMEO`, so a hung client would block its handler thread forever. Added 1-second receive timeout.

- **Blocking IQ header send** — IQ server `send_header()` could block the accept thread on a slow client. Added `SO_SNDTIMEO` (2 seconds).

- **FFT size validation** — `fft_processor_new()` accepted `fft_size < 2`, causing division by zero in window generation. Now returns NULL for invalid sizes.

- **Sample rate correction validation** — USB EEPROM could return garbage for sample rate correction, causing integer overflow. Now validated to ±1 MHz range.

- **CAT frequency set result unchecked** — The final USB control transfer in `usb_device_set_frequency()` was not checked. Now returns error on failure.

- **Frequency parsing with atol()** — `cat_parse_if_response()` used `atol()` which provides no error detection on corrupted serial data. Replaced with `strtol()` + endptr check.

- **GPIO error handling** — `gpiod_line_get_value()` returns -1 on error but was not checked, potentially causing spurious encoder events. Now skips the poll cycle on error.

- **GPIO partial init leak** — If `gpiod_line_request()` failed partway through, already-requested lines were not released. Now properly releases each line on failure.

- **Double parameter toggle on encoder button press** — Each button press on encoder 1 advanced the active parameter by two steps instead of one. Removed the redundant toggle from `rotary_encoder.c`.

- **Zoom level clamping mismatch** — The spectrum and waterfall widgets clamped zoom to a maximum of 8x, but the encoder code in main.c allowed up to 16x. Increased the widget clamp from 8 to 16.

- **Division by zero in frequency labels** — The spectrum widget frequency label code divided by `spectrum_size`, which is zero before the first FFT data arrives. Changed to use the `FFT_SIZE` constant.

- **Integer division precision loss** — `sample_rate / zoom_level` was computed as integer division before assignment to a double. Added explicit `(double)` casts.

- **Overlay text truncation** — The `overlay_mode` buffer was 16 bytes but could receive up to 32 bytes. Increased to 32 bytes.

- **Misleading `(void)length` suppression** — The USB data callback marked `length` as unused but then used it. Removed the incorrect suppression.

### Security

- **TCP CAT server now binds to localhost by default** — The CAT command server previously bound to `INADDR_ANY` with no authentication. Changed default to `INADDR_LOOPBACK`. Use `-l any` for LAN access.

- **Added `-l, --cat-listen` CLI flag** — Allows choosing between `localhost` (default) and `any` (all interfaces).

- **Checked write() return values in CAT server** — Added error checks that close the connection on write failure.

### Thread Safety

- **Made USB `streaming` flag atomic** — Changed to `atomic_int` with proper `atomic_load`/`atomic_store` operations.

### Code Cleanup

- **Removed unused structs from `app_state.h`** — Removed `iq_sample_t`, `spectrum_buffer_t`, and `app_state_t`.

- **Removed unused `waterfall_data` field** — The ring buffer was allocated but never used for rendering.

- **Removed legacy encoder pin defines** — Removed unused backward-compatibility defines from `rotary_encoder.h`.

- **Updated deprecated GTK API** — Replaced `gtk_css_provider_load_from_data()` with `gtk_css_provider_load_from_string()`.

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
