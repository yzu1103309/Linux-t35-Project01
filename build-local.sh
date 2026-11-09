#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_SRC="$ROOT_DIR/linux-5.15.135"
CONFIG_SRC="$ROOT_DIR/config/kernel.config"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_DIR/.local-build}"
KBUILD_OUTPUT="$BUILD_ROOT/kernel"
ARTIFACT_DIR="$BUILD_ROOT/artifact"
JOBS="${JOBS:-$(nproc)}"
REUSE_BUILD="${REUSE_BUILD:-1}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

for command_name in make nproc fakeroot dpkg-buildpackage dpkg-deb sha256sum zip; do
    need_command "$command_name"
done

[[ -d "$KERNEL_SRC" ]] || die "Kernel source directory not found: $KERNEL_SRC"
[[ -f "$CONFIG_SRC" ]] || die "Kernel config not found: $CONFIG_SRC"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
[[ "$REUSE_BUILD" == 0 || "$REUSE_BUILD" == 1 ]] || die "REUSE_BUILD must be 0 or 1"

printf 'Project:       %s\n' "$ROOT_DIR"
printf 'Kernel source: %s\n' "$KERNEL_SRC"
printf 'Build output:  %s\n' "$BUILD_ROOT"
printf 'Parallel jobs: %s\n' "$JOBS"
printf 'Reuse build:   %s\n' "$REUSE_BUILD"

# These directories are generated exclusively by this script. Preserve the
# kernel output when REUSE_BUILD=1 so Kbuild can perform an incremental build.
if [[ "$REUSE_BUILD" == 0 ]]; then
    rm -rf -- "$KBUILD_OUTPUT"
fi
rm -rf -- "$ARTIFACT_DIR"
mkdir -p -- "$KBUILD_OUTPUT" "$ARTIFACT_DIR"

# bindeb-pkg writes packages next to the output directory. Remove old package
# files so a reused build cannot accidentally collect packages from an older
# kernel release.
find "$BUILD_ROOT" -maxdepth 1 -type f -name '*.deb' -delete

# Linux 5.15 refuses an out-of-tree build when an earlier in-tree build left
# these generated markers in the source tree. Preserve the old .config, then
# remove only generated config/header directories; source files are untouched.
if [[ -f "$KERNEL_SRC/.config" ||
      -d "$KERNEL_SRC/include/config" ||
      -d "$KERNEL_SRC/include/generated" ||
      -d "$KERNEL_SRC/arch/x86/include/generated" ]]; then
    printf 'Cleaning old in-tree kernel build markers from the source tree.\n'
    if [[ -f "$KERNEL_SRC/.config" ]]; then
        cp -- "$KERNEL_SRC/.config" "$BUILD_ROOT/source-config.backup"
        rm -f -- "$KERNEL_SRC/.config"
    fi
    rm -rf -- "$KERNEL_SRC/include/config" \
             "$KERNEL_SRC/include/generated" \
             "$KERNEL_SRC/arch/x86/include/generated"
fi

cp -- "$CONFIG_SRC" "$KBUILD_OUTPUT/.config"

# Keep this in sync with .github/workflows/kernel-build.yml.
"$KERNEL_SRC/scripts/config" --file "$KBUILD_OUTPUT/.config" \
    --set-str LOCALVERSION "" \
    --disable LOCALVERSION_AUTO \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --set-str SYSTEM_REVOCATION_KEYS "" \
    --disable DEBUG_INFO \
    --disable DEBUG_INFO_BTF \
    --disable DEBUG_INFO_BTF_MODULES \
    --disable GDB_SCRIPTS

make -C "$KERNEL_SRC" O="$KBUILD_OUTPUT" olddefconfig

KERNEL_RELEASE="$(make -s -C "$KERNEL_SRC" O="$KBUILD_OUTPUT" LOCALVERSION= kernelrelease)"
printf 'Kernel release: %s\n' "$KERNEL_RELEASE"

if [[ ! "$KERNEL_RELEASE" =~ ^5\.15\.135-t35-v[0-9]+$ ]]; then
    die "Unexpected kernel release: $KERNEL_RELEASE; update linux-5.15.135/Makefile EXTRAVERSION"
fi

export LOCALVERSION=""
make -C "$KERNEL_SRC" O="$KBUILD_OUTPUT" -j"$JOBS" LOCALVERSION= bindeb-pkg

# CLion/IntelliJ can use the kernel's real compile commands for code insight.
# Generate this only after a successful build so it describes the exact
# configuration and generated headers used for the packages.
make -s -C "$KERNEL_SRC" O="$KBUILD_OUTPUT" compile_commands.json

mapfile -t PACKAGES < <(find "$BUILD_ROOT" -maxdepth 2 -type f -name '*.deb' -print | sort)
if (( ${#PACKAGES[@]} == 0 )); then
    die "No .deb packages were produced"
fi

IMAGE_COUNT=0
HEADER_COUNT=0
for package in "${PACKAGES[@]}"; do
    package_name="$(dpkg-deb -f "$package" Package)"
    package_arch="$(dpkg-deb -f "$package" Architecture)"
    case "$package_name" in
        linux-image-*)
            ((IMAGE_COUNT += 1))
            ;;
        linux-headers-*)
            ((HEADER_COUNT += 1))
            ;;
    esac
    [[ "$package_arch" == amd64 ]] || die "Unexpected package architecture: $package"
    cp -- "$package" "$ARTIFACT_DIR/"
done

(( IMAGE_COUNT >= 1 )) || die "No linux-image .deb found"
(( HEADER_COUNT >= 1 )) || die "No linux-headers .deb found"

cp -- "$KBUILD_OUTPUT/.config" "$ARTIFACT_DIR/kernel.config"
printf '%s\n' "$KERNEL_RELEASE" > "$ARTIFACT_DIR/KERNEL_RELEASE"

(
    cd -- "$ARTIFACT_DIR"
    sha256sum -- *.deb kernel.config KERNEL_RELEASE > SHA256SUMS
)

ARTIFACT_ZIP="$BUILD_ROOT/linux-${KERNEL_RELEASE}-deb.zip"
rm -f -- "$ARTIFACT_ZIP"
(
    cd -- "$ARTIFACT_DIR"
    zip -q -j "$ARTIFACT_ZIP" ./*.deb kernel.config KERNEL_RELEASE SHA256SUMS
)

printf '\nBuild complete.\n'
printf 'Artifact directory: %s\n' "$ARTIFACT_DIR"
printf 'Artifact ZIP:      %s\n' "$ARTIFACT_ZIP"
printf '\nInstall in the course VM with:\n'
printf '  ./install-latest-kernel.sh --artifact %q\n' "$ARTIFACT_ZIP"
