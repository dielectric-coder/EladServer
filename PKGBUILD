# Maintainer: Michel, VE2EXB <ve2exb@mikelachaine.ca>
pkgname=elad-spectrum
pkgver=1.0.0
pkgrel=1
pkgdesc='GTK4 spectrum analyzer and waterfall display for the Elad FDM-DUO SDR transceiver'
arch=('x86_64' 'aarch64')
license=('GPL')
depends=('gtk4' 'libusb' 'fftw' 'json-glib')
makedepends=('meson' 'ninja')
optdepends=('libgpiod: Rotary encoder support on Raspberry Pi')
source=()

build() {
    cd "$startdir"
    meson setup build --prefix=/usr --buildtype=release
    meson compile -C build
}

package() {
    cd "$startdir"
    meson install -C build --destdir="$pkgdir"
}
