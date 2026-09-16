# Maintainer: local
pkgname=steam-tdp-bridge
pkgver=0.1.0
pkgrel=1
pkgdesc="Steam TDP bridge for MSI Claw A8: exposes com.steampowered.SteamOSManager1.TdpLimit1 via a remote interface for steamos-manager"
arch=('x86_64')
url="https://localhost/steam-tdp-bridge"
license=('MIT')
depends=('glib2' 'dbus')
makedepends=('meson' 'ninja' 'gcc')
install="$pkgname.install"
source=()
sha256sums=()

# Build directly from the checkout containing this PKGBUILD (no remote source).
# Use a dedicated build dir so packaging never clashes with ./install.sh's build/.
build() {
  cd "$startdir"
  meson setup _build --prefix=/usr --buildtype=release
  ninja -C _build
}

package() {
  cd "$startdir"
  DESTDIR="$pkgdir" meson install -C _build

  install -Dm644 data/steam-tdp-bridge.service \
    "$pkgdir/usr/lib/systemd/system/steam-tdp-bridge.service"
  install -Dm644 data/steam-tdp-bridge-devicetoml.service \
    "$pkgdir/usr/lib/systemd/system/steam-tdp-bridge-devicetoml.service"
  install -Dm644 data/com.steampowered.TdpBridge.conf \
    "$pkgdir/usr/share/dbus-1/system.d/com.steampowered.TdpBridge.conf"
  install -Dm755 data/steam-tdp-bridge-sleep.sh \
    "$pkgdir/usr/lib/systemd/system-sleep/steam-tdp-bridge"
  install -Dm755 data/steam-set-profile.py \
    "$pkgdir/usr/lib/steam-tdp-bridge/steam-set-profile.py"
  install -Dm644 data/config.ini \
    "$pkgdir/etc/steam-tdp-bridge/config.ini"
  install -Dm644 data/msi-claw-amd.toml \
    "$pkgdir/etc/steam-tdp-bridge/msi-claw-amd.toml"
  install -Dm644 data/steam-tdp-bridge.remotes.toml \
    "$pkgdir/etc/steamos-manager/remotes.d/steam-tdp-bridge.toml"

  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"

  install -d "$pkgdir/var/lib/steam-tdp-bridge"
}
