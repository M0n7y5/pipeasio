#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "Usage: $0 BUILD_DIRECTORY OUTPUT.AppImage [WORK_DIRECTORY]" >&2
    exit 2
fi
build=$(realpath "$1")
output=$(realpath -m "$2")
work=$(realpath -m "${3:-build-manager-appimage}")
[[ ! -e "$work" ]] || { echo "Work directory must not exist: $work" >&2; exit 1; }
mkdir -p "$work/AppDir" "$(dirname "$output")"
app="$work/AppDir"
DESTDIR="$app" cmake --install "$build" --prefix /usr --component Manager
[[ -f "$app/usr/share/pipeasio/manager/pipeasio-check.exe" ]] || {
    echo 'A prebuilt pipeasio-check.exe is required. Configure PIPEASIO_CHECK_EXECUTABLE.' >&2
    exit 1
}
[[ -x "$app/usr/bin/pipeasio-manage" && -x "$app/usr/bin/pipeasio-settings" ]] || {
    echo 'Build both the native manager and settings panel before packaging.' >&2
    exit 1
}

lib="$app/usr/lib"
mkdir -p "$lib"
pending=("$app/usr/bin/pipeasio-manage" "$app/usr/bin/pipeasio-settings")
copy_runtime() {
    local source=$1 destination=$2
    mkdir -p "$(dirname "$destination")"
    cp -L "$source" "$destination"
    pending+=("$destination")
}
pw_dump=$(command -v pw-dump)
copy_runtime "$pw_dump" "$app/usr/bin/pw-dump"
qtpaths=$(command -v qtpaths6 || true)
if [[ -z "$qtpaths" ]]; then
    qtpaths="$(pkg-config --variable=bindir Qt6Core)/qtpaths"
fi
[[ -x "$qtpaths" ]] || { echo 'The Qt6 SDK qtpaths executable is required.' >&2; exit 1; }
plugins=$("$qtpaths" --query QT_INSTALL_PLUGINS)
for name in platforms/libqxcb.so platforms/libqoffscreen.so \
            imageformats/libqsvg.so iconengines/libqsvgicon.so tls/libqopensslbackend.so \
            platformthemes/libqxdgdesktopportal.so; do
    copy_runtime "$plugins/$name" "$app/usr/plugins/$name"
done
for source in "$plugins"/platforms/libqwayland*.so \
              "$plugins"/xcbglintegrations/*.so \
              "$plugins"/wayland-decoration-client/*.so \
              "$plugins"/wayland-graphics-integration-client/*.so \
              "$plugins"/wayland-shell-integration/*.so; do
    copy_runtime "$source" "$app/usr/plugins/${source#"$plugins/"}"
done
# Qt's OpenSSL TLS backend can load these at runtime rather than link them.
for name in ssl crypto; do
    source=$(realpath "$(pkg-config --variable=libdir openssl)/lib$name.so")
    copy_runtime "$source" "$lib/$(basename "$source")"
done
spa=$(pkg-config --variable=plugindir libspa-0.2)
modules=$(pkg-config --variable=moduledir libpipewire-0.3)
for name in support audioconvert videoconvert; do
    for source in "$spa/$name"/*.so; do
        copy_runtime "$source" "$lib/spa-0.2/$name/$(basename "$source")"
    done
done
for name in protocol-native client-node client-device adapter metadata session-manager rt; do
    copy_runtime "$modules/libpipewire-module-$name.so" "$lib/pipewire-0.3/libpipewire-module-$name.so"
done
mkdir -p "$app/usr/share/pipewire"
cp "$(pkg-config --variable=prefix libpipewire-0.3)/share/pipewire/client.conf" "$app/usr/share/pipewire/"

# The host loader and glibc must remain one matching set.
declare -A copied=()
for ((index=0; index<${#pending[@]}; index++)); do
    binary=${pending[index]}
    dependencies=$(LC_ALL=C ldd "$binary")
    if [[ "$dependencies" == *'not found'* ]]; then
        printf 'Unresolved dependency of %s:\n%s\n' "$binary" "$dependencies" >&2
        exit 1
    fi
    while IFS= read -r line; do
        [[ "$line" =~ \=\>[[:space:]]+(/[^[:space:]]+) ]] || continue
        source=${BASH_REMATCH[1]}
        name=$(basename "$source")
        case "$name" in
            ld-linux*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libanl.so.*|libnss_*) continue ;;
        esac
        [[ -z ${copied[$name]+set} ]] || continue
        copied[$name]=1
        copy_runtime "$source" "$lib/$name"
    done <<< "$dependencies"
done

cp "$app/usr/share/applications/pipeasio-settings.desktop" "$app/"
cp "$app/usr/share/icons/hicolor/scalable/apps/pipeasio.svg" "$app/"
ln -s pipeasio.svg "$app/.DirIcon"
printf '[Paths]\nPrefix=..\nPlugins=plugins\n' > "$app/usr/bin/qt.conf"
cat > "$app/AppRun" <<'APP_RUN'
#!/usr/bin/env bash
set -euo pipefail
app=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for key in LD_LIBRARY_PATH QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QT_QPA_PLATFORMTHEME \
           QT_STYLE_OVERRIDE QML2_IMPORT_PATH SPA_PLUGIN_DIR PIPEWIRE_MODULE_DIR PIPEWIRE_CONFIG_DIR; do
    unset "PIPEASIO_ORIGINAL_$key" "PIPEASIO_ORIGINAL_SET_$key"
    if [[ -v $key ]]; then
        export "PIPEASIO_ORIGINAL_$key=${!key}" "PIPEASIO_ORIGINAL_SET_$key=1"
    fi
done
export APPDIR="$app"
export PATH="$app/usr/bin${PATH:+:$PATH}"
export XDG_DATA_DIRS="$app/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export LD_LIBRARY_PATH="$app/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$app/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$app/usr/plugins/platforms"
export QT_QPA_PLATFORMTHEME="${QT_QPA_PLATFORMTHEME:-xdgdesktopportal}"
export QT_STYLE_OVERRIDE="${QT_STYLE_OVERRIDE:-Fusion}"
unset QML2_IMPORT_PATH
export SPA_PLUGIN_DIR="$app/usr/lib/spa-0.2"
export PIPEWIRE_MODULE_DIR="$app/usr/lib/pipewire-0.3"
export PIPEWIRE_CONFIG_DIR="$app/usr/share/pipewire"
exec "$app/usr/bin/pipeasio-settings" "$@"
APP_RUN
chmod +x "$app/AppRun"

tool="$work/appimagetool.AppImage"
curl --fail --location --proto '=https' --tlsv1.2 \
    https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage \
    --output "$tool"
printf '%s  %s\n' 46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1 "$tool" | sha256sum --check
chmod +x "$tool"
offset=$("$tool" --appimage-offset)
dd if="$tool" of="$work/runtime" bs=1 count="$offset" status=none
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --runtime-file "$work/runtime" "$app" "$output"
(cd "$(dirname "$output")" && sha256sum "$(basename "$output")") > "$output.sha256"
