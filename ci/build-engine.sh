set -eu
apk add --no-cache build-base meson samurai pkgconf python3 py3-pip py3-setuptools py3-distlib bison flex linux-headers glib-dev glib-static pcre2-static libffi-dev libffi-static zlib-dev zlib-static curl tar xz git
mkdir -p /work/downloads /work/out
cd /work/downloads
curl -fL --retry 5 --retry-all-errors -C - https://download.qemu.org/qemu-8.2.4.tar.xz -o qemu-8.2.4.tar.xz
printf '%s  %s\n' ecf5537feab92641b99d7482f551f2195d3a5bd34acef9d52bfbff353a607397 qemu-8.2.4.tar.xz | sha256sum -c -
cd /work
tar xf downloads/qemu-8.2.4.tar.xz
cd qemu-8.2.4
for change in /src/ci/senses-patches/*.patch; do patch -p1 < "$change"; done
cp /src/ci/senses-src/* linux-user/
for source in /src/qemu-patches/*; do
    case "$source" in
        */cputlb.c) cp "$source" accel/tcg/cputlb.c ;;
        *) cp "$source" linux-user/ ;;
    esac
done
mkdir build-static
cd build-static
../configure --static --target-list=arm-linux-user --disable-system --disable-tools --disable-docs --disable-guest-agent --disable-werror --without-default-features --extra-cflags=-O2 --extra-ldflags=-Wl,-z,max-page-size=16384
ninja -j2 qemu-arm
cp qemu-arm /work/out/qemu-arm-debug
strip qemu-arm
cp qemu-arm /work/out/libqemu_senses.so
readelf -h /work/out/libqemu_senses.so
if readelf -l /work/out/libqemu_senses.so | grep -q INTERP; then exit 1; fi
/work/out/libqemu_senses.so --version
cd /src
{
    printf 'commit=%s\nqemu=8.2.4\nsenses=0ec68b7a6733e01174f9a768a0c2410a0871fd67\n' "$BUILD_COMMIT"
    sha256sum qemu-patches/* native/sensesbinderd/* ci/senses-src/* ci/senses-patches/*
    sha256sum /work/out/libqemu_senses.so
    apk info -v
} > /work/out/engine-build-manifest.txt
cd /work
 tar -cJf /work/out/qemu-corresponding-source.tar.xz --exclude=qemu-8.2.4/build-static qemu-8.2.4