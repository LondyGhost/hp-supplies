#!/bin/bash
# 一键构建: 二进制 + deb + rpm + AppImage + Arch包
set -e
cd "$(dirname "$0")/.."
VERSION=1.2.1
PKGREL=1
PKGNAME=hp-supplies
OUT=out
mkdir -p "$OUT"

echo "=== [1/5] 编译 ==="
make clean || true
make APP_VERSION="$VERSION"

echo "=== [2/5] 构建 deb ==="
STAGE="$OUT/deb-stage/$PKGNAME-$VERSION"
rm -rf "$OUT/deb-stage"; mkdir -p "$STAGE/DEBIAN"
make install DESTDIR="$STAGE" PREFIX=/usr APP_VERSION="$VERSION"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKGNAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Depends: libgtk-3-0, libusb-1.0-0
Maintainer: londy <londy@local>
Description: HP 打印机耗材与用量监控
 通过 USB LEDM 接口查询碳粉/墨水余量、耗材状态与总打印页数。
 适用于带 HP LEDM USB 接口的 HP 打印机, 无需 HPLIP。
 提供 GTK3 图形界面与 --text 命令行模式。
EOF
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
udevadm control --reload >/dev/null 2>&1 || true
udevadm trigger >/dev/null 2>&1 || true
EOF
chmod 755 "$STAGE/DEBIAN/postinst"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT/${PKGNAME}-${VERSION}-amd64.deb"

echo "=== [3/5] 构建 rpm ==="
RPMTOP="$(pwd)/$OUT/rpmbuild"
rm -rf "$RPMTOP"; mkdir -p "$RPMTOP"/{BUILD,SOURCES,SPECS,RPMS,SRPMS}
tar czf "$RPMTOP/SOURCES/${PKGNAME}-${VERSION}.tar.gz" \
    --transform "s,^,${PKGNAME}-${VERSION}/," \
    src data Makefile README.md LICENSE
cp packaging/rpm/${PKGNAME}.spec "$RPMTOP/SPECS/"
mkdir -p "$RPMTOP/db"
# Arch 的 rpm-tools 需要指向用户级数据库, 否则报 cannot open Packages database
rpmbuild --define "_topdir $RPMTOP" --define "_dbpath $RPMTOP/db" \
    -bb --nodeps "$RPMTOP/SPECS/${PKGNAME}.spec"
find "$RPMTOP/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;

echo "=== [4/5] 构建 AppImage ==="
APPDIR="$OUT/AppDir"
rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr"
make install DESTDIR="$APPDIR" PREFIX=/usr APP_VERSION="$VERSION"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/hp-supplies" "$@"
EOF
chmod 755 "$APPDIR/AppRun"
cp data/hp-supplies.desktop "$APPDIR/"
cp data/hp-supplies.svg "$APPDIR/"
# appimagetool 想要根目录 png 图标; 有 rsvg 就转, 没有就跳过
if command -v rsvg-convert >/dev/null 2>&1; then
  rsvg-convert -w 128 -h 128 data/hp-supplies.svg -o "$APPDIR/${PKGNAME}.png"
fi
# 需要宿主系统有 GTK3 (绝大多数桌面发行版都有)
if [ -f ~/appimagetool-x86_64.AppImage ]; then
  chmod +x ~/appimagetool-x86_64.AppImage
  ~/appimagetool-x86_64.AppImage --appimage-extract-and-run "$APPDIR" \
      "$OUT/${PKGNAME}-${VERSION}-x86_64.AppImage"
else
  appimagetool "$APPDIR" "$OUT/${PKGNAME}-${VERSION}-x86_64.AppImage"
fi

echo "=== [5/5] 构建 Arch 包 ==="
if [ "${SKIP_ARCH:-0}" = "1" ]; then
  echo "SKIP_ARCH=1, 跳过 Arch 包（CI 环境无 makepkg）"
else
  ARCHDIR="$OUT/arch"
  rm -rf "$ARCHDIR"; mkdir -p "$ARCHDIR"
  cp "$RPMTOP/SOURCES/${PKGNAME}-${VERSION}.tar.gz" "$ARCHDIR/"
  cp packaging/arch/PKGBUILD packaging/arch/${PKGNAME}.install "$ARCHDIR/"
  ( cd "$ARCHDIR" && makepkg -f )
  cp "$ARCHDIR"/${PKGNAME}-${VERSION}-${PKGREL}-*.pkg.tar.zst "$OUT/" 2>/dev/null \
    || cp "$ARCHDIR"/*.pkg.tar.zst "$OUT/"
fi

echo
echo "=== 产物 ==="
ls -lh "$OUT" | grep -Ev '^d|deb-stage|rpmbuild|AppDir|total|^ls:'
