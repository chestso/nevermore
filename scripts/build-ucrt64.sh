#!/bin/bash
# build-ucrt64.sh - build nevermore natively on Windows (MSYS2 UCRT64).
#
# Applies the same MSYS2 workarounds as the other build-ucrt64.sh scripts
# (sh shadow, ACLOCAL_PATH sanitization). Can be launched from any shell;
# it re-execs into a real MSYS2 UCRT64 shell via msys2_shell.cmd.
#
# Prerequisites (must be installed via pacman before running):
#   - mingw-w64-ucrt-x86_64-... (none — Schannel is in the OS)
#   -
#   - boba (must be installed to $MINGW_PREFIX — required)
#
# Usage:
#   ./scripts/build-ucrt64.sh            # autogen + configure + make + check
#   ./scripts/build-ucrt64.sh --install  # build, then make install
#   ./scripts/build-ucrt64.sh --no-check  # build without running tests
#
# Extra args are forwarded to configure, e.g.:
#   ./scripts/build-ucrt64.sh --enable-release

set -eu

cd "$(dirname "$0")/.."

DO_INSTALL=0
SKIP_CHECK=0
CONFIGURE_ARGS=()
for arg in "$@"; do
	case "$arg" in
	--install) DO_INSTALL=1 ;;
	--no-check) SKIP_CHECK=1 ;;
	*) CONFIGURE_ARGS+=("$arg") ;;
	esac
done

if [ -z "${MSYSTEM:-}" ] || [ "${MSYSTEM:-}" != "UCRT64" ]; then
	PACMAN_PATH="$(command -v pacman 2>/dev/null || true)"
	if [ -z "$PACMAN_PATH" ]; then
		echo "ERROR: pacman not found on PATH. Install MSYS2 and add its" >&2
		echo "       /usr/bin to PATH, or run this from an MSYS2 UCRT64 shell." >&2
		exit 1
	fi
	PACMAN_DIR="$(cd "$(dirname "$PACMAN_PATH")" && pwd)"
	MSYS2_ROOT="$(cd "$PACMAN_DIR/../.." && pwd)"
	MSYS2_SHELL="$MSYS2_ROOT/msys2_shell.cmd"
	if [ ! -f "$MSYS2_SHELL" ]; then
		echo "ERROR: msys2_shell.cmd not found at $MSYS2_SHELL" >&2
		exit 1
	fi
	REPO_WIN="$(cygpath -w "$(pwd)" 2>/dev/null || echo "$(pwd)")"
	exec cmd.exe //c "$MSYS2_SHELL" -ucrt64 -defterm -no-start -here \
		-c "cd '$REPO_WIN' && ./scripts/build-ucrt64.sh $*"
fi

echo "==> MSYS2 UCRT64 shell (MSYSTEM=$MSYSTEM)"

FIXSH_DIR="$(mktemp -d /tmp/nevermore-fixsh.XXXXXX)"
cp /usr/bin/bash "$FIXSH_DIR/sh"
export PATH="$FIXSH_DIR:$PATH"
trap 'rm -rf "$FIXSH_DIR"' EXIT
echo "==> Applied sh workaround (shadowed /usr/bin/sh with bash copy)"

if [ -n "${ACLOCAL_PATH:-}" ]; then
	sane=""
	IFS=';' read -ra _ac_elems <<<"$ACLOCAL_PATH"
	for _el in "${_ac_elems[@]}"; do
		[ -z "$_el" ] && continue
		if _posix="$(cygpath -u "$_el" 2>/dev/null)"; then
			sane="${sane:+$sane:}$_posix"
		fi
	done
	if [ -n "$sane" ]; then
		export ACLOCAL_PATH="$sane"
		echo "==> Sanitized ACLOCAL_PATH -> $ACLOCAL_PATH"
	else
		unset ACLOCAL_PATH
		echo "==> Unset unusable ACLOCAL_PATH (aclocal defaults apply)"
	fi
fi

rm -f config.status config.log config.h stamp-h1

echo "==> ./autogen.sh"
./autogen.sh

echo "==> configure"
rm -rf build
mkdir build
(cd build && sh ../configure --prefix="$MINGW_PREFIX" "${CONFIGURE_ARGS[@]}")

echo "==> make -j$(nproc)"
make -C build -j"$(nproc)"

if [ "$SKIP_CHECK" -eq 1 ]; then
	echo "==> make check (skipped)"
else
	echo "==> make check"
	make -C build check
fi

if [ "$DO_INSTALL" -eq 1 ]; then
	echo "==> make install"
	make -C build install
fi

echo "==> Build complete"
