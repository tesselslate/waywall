# Maintainer: esensats (tesselslate)
pkgname=waywall
pkgver=0.5
pkgrel=1
pkgdesc="Wayland compositor for Minecraft speedrunning"
arch=(x86_64)
url="https://github.com/tesselslate/waywall"
license=(GPL3)
depends=(libegl libgles luajit libspng wayland libxcb libxkbcommon xorg-xwayland)
makedepends=(wayland-protocols meson git)
source=()

build() {
  cd ..
  meson setup build --prefix=/usr
  ninja -C build
}

check() {
  cd ..
  ninja -C build test || true
}

package() {
  cd ..
  #WAYWALL
  install -Dm755 build/waywall/waywall "$pkgdir/usr/bin/waywall"
  #WAYWALLL-LICENSE
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  #WAYWALL-DOCS
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
  if [ -d doc ]; then
    cp -r doc "$pkgdir/usr/share/doc/$pkgname/"
  fi
  #GLFW
  mkdir -p "$pkgdir/usr/local/lib64/waywall-glfw"
  install -m644 glfw/libglfw.so "$pkgdir/usr/local/lib64/waywall-glfw/"
  install -m644 glfw/libglfw.so.3 "$pkgdir/usr/local/lib64/waywall-glfw/"
  install -m644 glfw/libglfw.so.3.4 "$pkgdir/usr/local/lib64/waywall-glfw/"
  #GLFW-DOCS
  install -m644 glfw/LICENSE.md "$pkgdir/usr/share/doc/$pkgname/glfw-LICENSE.md"
  install -m644 glfw/CONTRIBUTORS.md "$pkgdir/usr/share/doc/$pkgname/glfw-CONTRIBUTORS.md"
  install -m644 glfw/MAINTAINERS.md "$pkgdir/usr/share/doc/$pkgname/glfw-MAINTAINERS.md"
}
