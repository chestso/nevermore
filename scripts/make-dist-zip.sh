#!/bin/sh
# make-dist-zip.sh - assemble a portable ZIP of nevermore.
#
# A portable ZIP is a folder the user drops anywhere and runs with no
# installer, no MSYS2, no Homebrew:
#
#   nevermore-<ver>-windows-<arch>/      nevermore-<ver>-macos-<arch>/
#     nevermore.exe                        nevermore
#     <the non-system DLLs it imports>     README.md
#     README.md                            COPYING
#     COPYING                              README-PORTABLE.txt
#     README-PORTABLE.txt
#
# Windows: the .exe links boba as a STATIC archive and uses the OS TLS
# (Schannel), so the only DLLs it can need beyond Windows' own are the
# MinGW runtime ones. They are read out of the binary's import table and
# bundled beside it - Windows looks in the executable's own directory
# first, so the folder is self-contained wherever it goes.
#
# macOS: same story in reverse - static boba, Secure Transport, every
# dependency a system one. A non-system path here means a link-time
# regression, so the script says so loudly instead of shipping a ZIP that
# dies on someone else's machine.
#
# GNU/Linux is deliberately NOT packaged: `make dist` (the source tarball)
# is the Linux artifact.
#
# Usage (invoked from the top-level Makefile):
#   make dist-portable                    # pack the host platform
#   make dist-portable DIST_TARGET=windows  # pack a cross-built MinGW tree
#
# Or directly:
#   scripts/make-dist-zip.sh [BUILD_DIR] [OUTPUT_DIR] [TARGET]
#
# BUILD_DIR defaults to "build", OUTPUT_DIR to BUILD_DIR, TARGET to the
# host platform. The result is nevermore-<version>-<target>-<arch>.zip.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$SRC_DIR/build}"
OUTPUT_DIR="${2:-$BUILD_DIR}"
TARGET="${3:-}"

VERSION="$("$SRC_DIR/build-aux/git-version.sh" "$SRC_DIR" 2>/dev/null || echo "")"
[ -n "$VERSION" ] || {
	echo "ERROR: cannot determine the version ($SRC_DIR is neither a git checkout nor a tarball with a version file)" >&2
	exit 1
}

# --- Target platform --------------------------------------------------------
# From the argument (the cross-packing case) or the host. The binary's own
# format is checked below, so a wrong target cannot silently pack the wrong
# shape: it fails with "no <file> - build it first".
if [ -z "$TARGET" ]; then
	case "$(uname -s)" in
	Darwin) TARGET=macos ;;
	MINGW* | MSYS*) TARGET=windows ;;
	*) TARGET=linux ;;
	esac
fi

BIN_DIR="$BUILD_DIR/src"
case "$TARGET" in
macos)
	NAME=nevermore
	EXT=""
	;;
windows)
	NAME=nevermore
	EXT=".exe"
	;;
linux)
	echo "ERROR: GNU/Linux is not packaged - build from the source tarball (make dist)." >&2
	exit 1
	;;
*)
	echo "ERROR: unknown target '$TARGET' (macos or windows)" >&2
	exit 1
	;;
esac

# libtool wraps a program in .libs/ when the link goes through it; this
# build links a plain static archive, so the direct path is the usual one.
REAL_BIN="$BIN_DIR/$NAME$EXT"
if [ ! -f "$REAL_BIN" ] && [ -f "$BIN_DIR/.libs/$NAME$EXT" ]; then
	REAL_BIN="$BIN_DIR/.libs/$NAME$EXT"
fi
[ -f "$REAL_BIN" ] || {
	echo "ERROR: no $NAME$EXT under $BIN_DIR - build it first (make -C src)." >&2
	exit 1
}

# Architecture: what the binary IS, falling back to the host (every CI
# build here is native, so the two agree).
detect_arch() {
	if command -v file >/dev/null 2>&1; then
		case "$(file -b "$REAL_BIN" 2>/dev/null || true)" in
		*arm64* | *aarch64*)
			[ "$TARGET" = macos ] && echo arm64 || echo aarch64
			return 0
			;;
		*x86-64* | *x86_64*)
			echo x86_64
			return 0
			;;
		esac
	fi
	case "$(uname -m)" in
	arm64 | aarch64) [ "$TARGET" = macos ] && echo arm64 || echo aarch64 ;;
	*) uname -m ;;
	esac
}
ARCH="$(detect_arch)"

PKG_NAME="nevermore-${VERSION}-${TARGET}-${ARCH}"
STAGE_DIR="$OUTPUT_DIR/$PKG_NAME"
ZIP_FILE="$OUTPUT_DIR/${PKG_NAME}.zip"

echo "==> Packaging nevermore $VERSION ($TARGET $ARCH)"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
cp "$REAL_BIN" "$STAGE_DIR/$NAME$EXT"
chmod 755 "$STAGE_DIR/$NAME$EXT"

# --- Docs -------------------------------------------------------------------
for doc in README.md COPYING; do
	if [ -f "$SRC_DIR/$doc" ]; then
		cp "$SRC_DIR/$doc" "$STAGE_DIR/$doc"
	fi
done

# --- Windows: bundle the runtime DLLs the .exe actually imports ------------
# The import table is read with objdump (any binutils reads a PE, native or
# cross), walked recursively, and every import that is not something Windows
# itself provides is resolved from the toolchain's bin dir and copied next to
# the exe: boba is a static archive and TLS is Schannel, so what is left is
# the MinGW runtime, and only when the toolchain links it dynamically -
# Fedora's mingw64 gcc does, and the .exe then names libwinpthread-1.dll
# (boba's archive calls pthread_once, the mutexes and thread keys), while
# MSYS2's UCRT64 gcc links the same references in statically and the ZIP
# carries nothing but the exe. The walk is the contract either way: what
# ships is what the import table says, never a guess.
#
# The filter is a case-insensitive PREFIX match, so CRYPT32.dll and
# cryptbase.dll share one word. Everything it names ships with Windows, and
# bundling it would be worse than useless: a DLL next to the exe is found
# before the OS one, so the copy in the ZIP would outlive every security
# patch the user's machine has. api-ms-win / ext-ms-win are the API sets -
# forwarders by name, the largest family of them.
if [ "$TARGET" = windows ]; then
	SYSTEM_DLLS='KERNEL32|kernelbase|NTDLL|msvcrt|ucrtbase|vcruntime|msvcp|api-ms-win|ext-ms-win|ADVAPI32|USER32|GDI32|gdi32full|win32u|SHELL32|SHLWAPI|SHCORE|ole32|COMDLG32|WS2_32|wsock32|mswsock|Secur32|SSPICLI|schannel|CRYPT32|BCRYPT|NCRYPT|cryptbase|dpapi|WINHTTP|iphlpapi|DNSAPI|wkscli|netutils|wldap32|SAMLIB|USERENV|AUTHZ|IMM32|VERSION|WINMM|MMDevApi|powrprof|PSAPI|SETUPAPI|CFGMG32|DEVOBJ|gpapi|apphelp|sechost|RPCRT4|WINSPOOL|USP10|OLEAUT32|combase|clbcat|propsys|dwmapi|dwrite|D3D|DXGI|SensApi'

	command -v objdump >/dev/null 2>&1 || {
		echo "ERROR: objdump not found - cannot read the .exe import table." >&2
		exit 1
	}

	imports() {
		objdump -p "$1" 2>/dev/null |
			awk 'toupper($1) == "DLL" && toupper($2) == "NAME:" { print $3 }' |
			sort -u
	}

	# Where a named DLL lives. $BIN_DIR first (an MSYS2 build tree keeps
	# nothing there, a cross-checkout may), then the toolchain's own bin.
	find_dll() {
		for dir in "$BIN_DIR" ${MINGW_PREFIX:+"$MINGW_PREFIX/bin"} \
			/ucrt64/bin /mingw64/bin /clang64/bin /clangarm64/bin \
			/usr/x86_64-w64-mingw32/sys-root/mingw/bin \
			/usr/i686-w64-mingw32/sys-root/mingw/bin \
			/usr/aarch64-w64-mingw32/sys-root/mingw/bin; do
			if [ -f "$dir/$1" ]; then
				printf '%s\n' "$dir/$1"
				return 0
			fi
		done
		return 1
	}

	echo "==> Bundling runtime DLLs"
	queue="$(mktemp "${TMPDIR:-/tmp}/nm-dist-queue.XXXXXX")"
	seen="$(mktemp "${TMPDIR:-/tmp}/nm-dist-seen.XXXXXX")"
	imports "$REAL_BIN" >"$queue"

	while [ -s "$queue" ]; do
		name="$(head -1 "$queue")"
		tail -n +2 "$queue" >"$queue.tmp" && mv "$queue.tmp" "$queue"
		[ -n "$name" ] || continue
		if echo "$name" | grep -iqE "^($SYSTEM_DLLS)"; then
			continue
		fi
		if grep -qxF "$name" "$seen" 2>/dev/null; then
			continue
		fi
		echo "$name" >>"$seen"
		path="$(find_dll "$name")" || {
			echo "ERROR: $name is imported but was not found next to the toolchain's bin dirs." >&2
			echo "  the ZIP would not run on a machine without MSYS2 - refusing to ship it." >&2
			exit 1
		}
		cp "$path" "$STAGE_DIR/"
		echo "  bundled: $name ($(basename "$(dirname "$path")"))"
		imports "$path" >>"$queue"
	done

	rm -f "$queue" "$seen" "$queue.tmp"
fi

# --- macOS: prove the binary needs nothing we are not shipping -------------
if [ "$TARGET" = macos ]; then
	echo "==> Checking the binary links only system libraries"
	# LC_LOAD_*_DYLIB commands, not `otool -L` columns: a library path with
	# a space in it survives the awk-free read.
	deps="$(otool -l "$REAL_BIN" |
		awk '$1 == "cmd" { want = ($2 ~ /^LC_(LOAD|LOAD_WEAK|REEXPORT|LOAD_UPWARD|LAZY_LOAD)_DYLIB$/) }
		     want && $1 == "name" { sub(/^[[:space:]]*name /, ""); sub(/ \(offset [0-9]+\)$/, ""); print; want = 0 }')"
	bad=""
	while IFS= read -r dep; do
		[ -n "$dep" ] || continue
		case "$dep" in
		/usr/lib/* | /System/Library/* | /usr/lib/swift/*) ;;
		*) bad="$bad
    $dep" ;;
		esac
	done <<EOF
$deps
EOF
	if [ -n "$bad" ]; then
		echo "ERROR: the macOS binary links a non-system library:$bad" >&2
		echo "  nevermore's only dependency is boba, which links as a static" >&2
		echo "  archive, and its TLS is the OS's. Something changed: bundle the" >&2
		echo "  library here (see the Windows branch) or undo the change." >&2
		exit 1
	fi

	# Copying does not disturb the linker's ad-hoc signature, but verify the
	# bytes that go into the ZIP rather than assume it.
	if command -v codesign >/dev/null 2>&1; then
		if ! codesign --verify --strict "$STAGE_DIR/$NAME" 2>/dev/null; then
			codesign --force --sign - "$STAGE_DIR/$NAME"
		fi
		codesign --verify --strict "$STAGE_DIR/$NAME"
	fi
	echo "  system libraries only"
fi

# --- README ----------------------------------------------------------------
echo "==> Writing README-PORTABLE.txt"
if [ "$TARGET" = windows ]; then
	cat >"$STAGE_DIR/README-PORTABLE.txt" <<'README'
nevermore (Windows portable)
============================

A self-contained build of nevermore: no MSYS2, no installer, nothing to
configure. Extract this folder anywhere and run it from any terminal.

  nevermore.exe --version
  nevermore.exe                      interactive chat
  nevermore.exe ask "explain this repo"

Add the folder to PATH (or make a shortcut to nevermore.exe) and `nevermore`
works from PowerShell, cmd, or Windows Terminal.

Keys: set HYPER_API_KEY / OLLAMA_API_KEY / OPENAI_API_KEY /
OPENROUTER_API_KEY / OPENCODE_API_KEY, or put one
`machine <name> password <secret>` line per provider in %USERPROFILE%\.authinfo.
`ollama:local` - the default - needs no key at all.

Files nevermore writes:
  %USERPROFILE%\.config\nevermore\config    your settings, if you make any
  %LOCALAPPDATA%\nevermore\                 the chat's own settings, history
To uninstall: delete this folder, and those two if you want them gone.

The .exe needs only what Windows itself ships: the TLS stack (Schannel), the
socket stack (Winsock), the UCRT and the Win32 files it calls. If a MinGW
runtime DLL sits beside it (libwinpthread-1.dll), that is the one library the
build could not resolve from Windows - Windows looks in the executable's own
folder before anywhere else, so leave it where it is. A build whose toolchain
links that runtime in statically ships without it.
README
else
	cat >"$STAGE_DIR/README-PORTABLE.txt" <<'README'
nevermore (macOS portable)
==========================

A self-contained build of nevermore: no Homebrew, no installer. Extract this
folder anywhere and run it from any terminal.

  ./nevermore --version
  ./nevermore                        interactive chat
  ./nevermore ask "explain this repo"

To put it on PATH:

  install -m 755 nevermore ~/.local/bin/       # or /usr/local/bin

If Terminal refuses to run a freshly downloaded copy, clear the quarantine
attribute and try again:

  xattr -d com.apple.quarantine nevermore

Keys: set HYPER_API_KEY / OLLAMA_API_KEY / OPENAI_API_KEY /
OPENROUTER_API_KEY / OPENCODE_API_KEY, or put one
`machine <name> password <secret>` line per provider in ~/.authinfo.
`ollama:local` - the default - needs no key at all.

Files nevermore writes:
  ~/.config/nevermore/config            your settings, if you make any
  ~/.local/state/nevermore/             the chat's own settings, history
To uninstall: delete the binary and those two, if you want them gone.

This binary links nothing outside /usr/lib and /System/Library: boba is
statically linked and TLS is the OS's Secure Transport.
README
fi

# --- ZIP -------------------------------------------------------------------
echo "==> Creating ZIP: $(basename "$ZIP_FILE")"
rm -f "$ZIP_FILE"
if command -v zip >/dev/null 2>&1; then
	(cd "$OUTPUT_DIR" && zip -q -r -9 "$(basename "$ZIP_FILE")" "$PKG_NAME")
elif command -v 7z >/dev/null 2>&1 && 7z >/dev/null 2>&1; then
	# MSYS2's /usr/bin/7z is a wrapper script; use it only when it runs.
	(cd "$OUTPUT_DIR" && 7z a -tzip -mx=9 "$(basename "$ZIP_FILE")" "$PKG_NAME" >/dev/null)
elif [ -x /usr/lib/p7zip/7z ] || [ -x /usr/lib/p7zip/7z.exe ]; then
	p7z=/usr/lib/p7zip/7z
	[ -x "$p7z" ] || p7z=/usr/lib/p7zip/7z.exe
	(cd "$OUTPUT_DIR" && "$p7z" a -tzip -mx=9 "$(cygpath -w "$(basename "$ZIP_FILE")" 2>/dev/null || echo "$(basename "$ZIP_FILE")")" "$PKG_NAME" >/dev/null)
else
	echo "ERROR: neither zip nor 7z found - cannot create the ZIP" >&2
	echo "  (the staged folder is left at $STAGE_DIR)" >&2
	exit 1
fi

rm -rf "$STAGE_DIR"

echo "==> Done: $ZIP_FILE"
echo "    Size: $(du -h "$ZIP_FILE" | cut -f1)"
