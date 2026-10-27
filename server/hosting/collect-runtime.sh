#!/bin/sh
# collect-runtime.sh <binary> <qt prefix> <out>: copies the binary, the Qt plugins shadNet loads
# (SQLite, TLS) and every library they need but glibc's own into <out>, for a runtime image with
# no Qt installed (Dockerfile). LD_LIBRARY_PATH=<out>/lib, QT_PLUGIN_PATH=<out>/plugins.
set -eu
bin=$1 qt=$2 out=$3
mkdir -p "$out/lib" "$out/plugins"
cp "$bin" "$out/"
plugins=$qt/lib/qt6/plugins # conda-forge
[ -d "$plugins" ] || plugins=$qt/plugins # aqtinstall
for dir in sqldrivers tls; do
    mkdir -p "$out/plugins/$dir"
    for p in "$plugins/$dir"/libqsqlite.so "$plugins/$dir"/libqopensslbackend.so; do
        if [ -f "$p" ]; then cp "$p" "$out/plugins/$dir/"; fi
    done
done
# Qt opens OpenSSL with dlopen (ldd does not show it): take the prefix's when it has one.
for l in "$qt"/lib/libssl.so.3 "$qt"/lib/libcrypto.so.3; do
    if [ -f "$l" ]; then cp -L "$l" "$out/lib/"; fi
done
find "$out" -type f \( -name '*.so*' -o -perm -u+x \) | while read -r f; do
    LD_LIBRARY_PATH=$qt/lib ldd "$f" 2>/dev/null | awk '/=> \// { print $3 }'
done | sort -u | while read -r lib; do
    case $(basename "$lib") in
    libc.so.* | libm.so.* | libdl.so.* | libpthread.so.* | librt.so.* | libresolv.so.* | ld-linux*) ;;
    *) cp -Ln "$lib" "$out/lib/" ;;
    esac
done
echo "collect-runtime: $(ls "$out/lib" | wc -l) libraries, $(du -sh "$out" | cut -f1)"
