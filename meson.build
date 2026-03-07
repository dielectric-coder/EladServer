project('elad-spectrum', 'c',
  version: '1.0.0',
  default_options: ['c_std=c11', 'warning_level=2']
)

gtk4_dep = dependency('gtk4')
libusb_dep = dependency('libusb-1.0')
fftw3_dep = dependency('fftw3')
threads_dep = dependency('threads')
math_dep = meson.get_compiler('c').find_library('m', required: true)
gpiod_dep = dependency('libgpiod', required: false)
json_glib_dep = dependency('json-glib-1.0')

src_files = [
  'src/main.c',
  'src/usb_device.c',
  'src/fft_processor.c',
  'src/spectrum_widget.c',
  'src/waterfall_widget.c',
  'src/cat_control.c',
  'src/cat_server.c',
  'src/settings.c',
  'src/bandplan.c',
]

# Rotary encoder support (optional, requires libgpiod)
if gpiod_dep.found()
  src_files += 'src/rotary_encoder.c'
  add_project_arguments('-DHAVE_GPIOD', language: 'c')
endif

deps = [gtk4_dep, libusb_dep, fftw3_dep, threads_dep, math_dep, json_glib_dep]
if gpiod_dep.found()
  deps += gpiod_dep
endif

executable('elad-spectrum',
  src_files,
  dependencies: deps,
  install: true
)

# Install band plan files
install_data(
  'resources/bands-r1.json',
  'resources/bands-r2.json',
  'resources/bands-r3.json',
  install_dir: get_option('datadir') / 'elad-spectrum'
)
