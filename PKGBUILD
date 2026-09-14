# Maintainer: Noel <noel.eliezer@gmail.com>
#
# Builds the current HEAD of this checkout: commit, then `makepkg -si`.
# Installs in one package what kmod/install.sh and wireplumber/install.sh do by
# hand: the evoctl/evotui CLI, the evo_raw DKMS module, udev + modprobe files,
# systemd user units and the EVO 4 PipeWire/WirePlumber drop-ins.
#
# Before the first install, remove a manual setup so files do not clash:
#   sudo kmod/uninstall.sh && wireplumber/uninstall.sh

pkgname=audient-evo-git
_modname=evo_raw
pkgver=0.2.1.r88.eb6fbb0
pkgrel=1
pkgdesc="Linux controller for Audient EVO USB audio interfaces (CLI, TUI, evo_raw DKMS module, PipeWire config)"
arch=('any')
url="https://github.com/anor4k/audient-evo-py"
license=('Unlicense')
depends=('python' 'dkms' 'alsa-utils' 'pipewire' 'wireplumber')
makedepends=('git' 'python-build' 'python-installer' 'python-setuptools' 'python-wheel')
optdepends=('linux-headers: build the module for the stock kernel'
            'python-numpy: audio tests'
            'python-sounddevice: audio tests')
provides=('audient-evo')
conflicts=('audient-evo')
install="$pkgname.install"
source=()
sha256sums=()

pkgver() {
    cd "$startdir"
    local base
    base=$(sed -n 's/^version = "\(.*\)"/\1/p' pyproject.toml)
    printf '%s.r%s.%s' "$base" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

prepare() {
    rm -rf "$srcdir/$pkgname"
    mkdir -p "$srcdir/$pkgname"
    git -C "$startdir" archive HEAD | tar -x -C "$srcdir/$pkgname"
}

build() {
    cd "$srcdir/$pkgname"
    python -m build --wheel --no-isolation
}

package() {
    cd "$srcdir/$pkgname"

    # CLI / TUI
    python -m installer --destdir="$pkgdir" dist/*.whl

    # DKMS module (the alpm dkms hook builds it for every installed kernel)
    local srcdest="$pkgdir/usr/src/$_modname-$pkgver"
    install -Dm644 kmod/evo_raw.c kmod/Makefile -t "$srcdest"
    sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$pkgver\"/" kmod/dkms.conf \
        > "$srcdest/dkms.conf"

    # udev: /dev/evo* access + config autoload, and the rename-wait helper
    install -Dm644 kmod/99-evo4.rules kmod/99-evo8.rules -t "$pkgdir/usr/lib/udev/rules.d"
    install -Dm755 kmod/evo-wait-master "$pkgdir/usr/lib/udev/evo-wait-master"
    sed 's|/usr/local/bin/evo-wait-master|/usr/lib/udev/evo-wait-master|' \
        kmod/70-evo-wait-master.rules \
        > "$pkgdir/usr/lib/udev/rules.d/70-evo-wait-master.rules"
    chmod 644 "$pkgdir/usr/lib/udev/rules.d/70-evo-wait-master.rules"

    # snd-usb-audio quirk (needs kernel >= 6.18 for the string form; see file)
    install -Dm644 kmod/snd-usb-audio-evo.conf -t "$pkgdir/usr/lib/modprobe.d"

    # systemd user units, rewritten to packaged paths
    local unitdir="$pkgdir/usr/lib/systemd/user"
    install -dm755 "$unitdir"
    for dev in evo4 evo8; do
        sed 's|%h/.local/bin/evoctl|/usr/bin/evoctl|' "kmod/$dev-load-config.service" \
            > "$unitdir/$dev-load-config.service"
    done
    sed 's|%h/.local/bin/evo4-setup.sh|/usr/bin/evo4-setup.sh|' \
        wireplumber/evo4/evo4-setup.service > "$unitdir/evo4-setup.service"
    chmod 644 "$unitdir"/*.service
    install -Dm755 wireplumber/evo4/evo4-setup.sh "$pkgdir/usr/bin/evo4-setup.sh"

    # EVO 4 PipeWire loopbacks + WirePlumber device rules (system-wide drop-ins;
    # a copy of the same name under ~/.config overrides these)
    install -Dm644 wireplumber/evo4/evo4-stereo.conf \
        -t "$pkgdir/usr/share/pipewire/pipewire.conf.d"
    install -Dm644 wireplumber/evo4/51-evo4.conf \
        -t "$pkgdir/usr/share/wireplumber/wireplumber.conf.d"

    install -Dm644 LICENSE -t "$pkgdir/usr/share/licenses/$pkgname"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
    install -Dm644 wireplumber/README.md "$pkgdir/usr/share/doc/$pkgname/README-wireplumber.md"
}
