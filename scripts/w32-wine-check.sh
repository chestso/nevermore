#!/bin/sh
# scripts/w32-wine-check.sh - build + run the Windows binaries under
# local wine before pushing (MSYS2 CI does the real run).
#
# The cross compilation is AUTOTOOLS' work; this script provisions what
# it needs and reads the logs:
#
#   boba       cross-built by its OWN configure --host into a prefix of
#              its own (or taken from $BOBA_PREFIX), so pkg-config
#              answers WINDOWS cflags/libs for it.
#   nevermore  configure --host=x86_64-w64-mingw32, then `make` (the
#              .exe — src/nevermore.rc and its version fields come from
#              src/Makefile.am's own rules) and `make check`: the test
#              roster, the per-test sources, the TLS backend and the
#              wine runner all come from Makefile.am and configure
#              (--with-wine).
#
# Nothing here names a source file, an include dir, a link library or a
# test, so a new TU, a new test or a new TLS backend reaches this
# pre-flight through the build system alone. In particular the four
# boba-linking binaries are not special any more: in a cross tree EVERY
# test binary links boba, exactly as on CI.
#
# The portable-ZIP half (--no-zip skips it) is the Windows end of the
# release recipe, run the same way: a second, --enable-release cross
# tree, `make dist-portable DIST_TARGET=windows`, then the EXTRACTED
# .exe under wine with the MinGW bin dirs OFF the path — the
# self-containment proof AGENTS.md otherwise asks for by hand.
#
# Prereqs: x86_64-w64-mingw32-gcc, wine, and boba for the Windows target
# — a source tree ($BOBA_SRC, else ./boba, else ../boba) to cross-build,
# or a cross install ($BOBA_PREFIX). Configure requires boba for the
# target, so this exits 77 without either: it is ONE build now, not a
# hand-kept per-test list that can quietly skip the boba half.
#
# Logs — read them, never buy a second run:
#   build-w32/boba-{autogen,configure,make,install}.log
#   build-w32/configure.log, build-w32/build.log, build-w32/check.log
#   build-w32/nevermore-res.strings (the version the resource carries)
#   build-w32/tests/*.log + test-suite.log + test-progress.log
#   build-w32-rel/{configure,build,zip}.log           (the ZIP half)
#
# Exit: 0 = every binary built and passed under wine (and, with the ZIP
# half, the packed .exe ran); 1 = a build, a run or the packaging failed;
# 77 = a prereq is missing.
#
# Usage: scripts/w32-wine-check.sh [--no-zip]

set -e

DO_ZIP=1
for arg in "$@"; do
	case "$arg" in
	--no-zip) DO_ZIP=0 ;;
	*)
		echo "usage: scripts/w32-wine-check.sh [--no-zip]" >&2
		exit 2
		;;
	esac
done

cd "$(dirname "$0")/.."
srcdir=$PWD

CC=x86_64-w64-mingw32-gcc
command -v $CC >/dev/null 2>&1 || {
	echo "SKIP: $CC not found (mingw-w64 cross toolchain)"
	exit 77
}
command -v wine >/dev/null 2>&1 || {
	echo "SKIP: wine not found"
	exit 77
}

builddir=$srcdir/build-w32
relbuild=$srcdir/build-w32-rel

# The toolchain's own tree, DERIVED (never a hardcoded path): it is
# pkg-config's search path for the target (so a mingw dependency
# resolves), and the home of the MinGW runtime DLL, which wine needs when
# the cross gcc links libwinpthread dynamically (Fedora's does; MSYS2's
# UCRT64 gcc links it statically and needs none). `-print-sysroot` names
# the tree and each toolchain layout keeps the DLL somewhere under it.
sysroot=$($CC -print-sysroot 2>/dev/null || echo "")
[ -n "$sysroot" ] && [ -d "$sysroot" ] || sysroot=
dll_dir=
for d in "$sysroot/mingw/bin" "$sysroot/bin" "$sysroot/mingw/lib" \
	"$sysroot/lib" /usr/x86_64-w64-mingw32/lib \
	/usr/lib/gcc/x86_64-w64-mingw32/*/; do
	[ -n "$d" ] && [ -f "$d/libwinpthread-1.dll" ] && dll_dir=$d && break
done

# Every TU is compiled by THIS run: a reused tree would report yesterday's
# objects as today's OK.
rm -rf "$builddir"
mkdir -p "$builddir"

# ---------------------------------------------------------------------
# boba for Windows
# ---------------------------------------------------------------------
# configure requires boba for the TARGET, and the native install's
# boba.pc describes a LINUX library — it must not be what pkg-config
# answers with, which is why the configure calls below use
# PKG_CONFIG_LIBDIR (it REPLACES the default search path; PKG_CONFIG_PATH
# would only prepend to it and the host boba could still win).
boba_pc_dir=
if [ -n "${BOBA_PREFIX:-}" ] && [ -f "$BOBA_PREFIX/lib/pkgconfig/boba.pc" ]; then
	boba_pc_dir=$BOBA_PREFIX/lib/pkgconfig
	echo "==> boba: cross install in $BOBA_PREFIX"
else
	BOBA_SRC=${BOBA_SRC:-}
	if [ -z "$BOBA_SRC" ]; then
		for d in ./boba ../boba; do
			[ -f "$d/src/Makefile.am" ] && BOBA_SRC=$d && break
		done
	fi
	if [ -z "$BOBA_SRC" ] || [ ! -f "$BOBA_SRC/src/Makefile.am" ]; then
		echo "SKIP: no boba for the Windows target (set BOBA_SRC to a boba" >&2
		echo "      source tree, or BOBA_PREFIX to a cross install)" >&2
		exit 77
	fi
	boba_src=$(cd "$BOBA_SRC" && pwd)
	boba_prefix=$builddir/boba-prefix
	boba_build=$builddir/boba-build
	mkdir -p "$boba_prefix" "$boba_build"

	# boba's `configure` is autogen.sh's output (gitignored): generate it
	# when this checkout has never been configured.
	if [ ! -x "$boba_src/configure" ]; then
		echo "==> boba: ./autogen.sh (no configure in $boba_src)"
		(cd "$boba_src" && ./autogen.sh) >"$builddir/boba-autogen.log" 2>&1 ||
			echo "WARN: boba autogen.sh failed (see $builddir/boba-autogen.log)"
	fi

	echo "==> boba: cross-building for Windows"
	(cd "$boba_build" &&
		"$boba_src/configure" --host=x86_64-w64-mingw32 \
			--prefix="$boba_prefix") \
		>"$builddir/boba-configure.log" 2>&1 || {
		echo "FAIL: boba configure (see $builddir/boba-configure.log)"
		tail -20 "$builddir/boba-configure.log"
		exit 1
	}
	if ! make -C "$boba_build" -j"$(nproc)" >"$builddir/boba-make.log" 2>&1 ||
		! make -C "$boba_build" install >"$builddir/boba-install.log" 2>&1 ||
		[ ! -f "$boba_prefix/lib/libboba.a" ]; then
		echo "FAIL: boba build/install (see $builddir/boba-make.log,"
		echo "      $builddir/boba-install.log)"
		tail -20 "$builddir/boba-make.log" "$builddir/boba-install.log"
		exit 1
	fi
	boba_pc_dir=$boba_prefix/lib/pkgconfig

	# Which boba this run verified: the CI pin is what a release uses, so
	# say when they differ (an uncommitted boba change is the one case this
	# pre-flight is MOST wanted for).
	bo_sha="$(git -C "$boba_src" rev-parse --short HEAD 2>/dev/null || echo unknown)"
	# The pin is a literal at the boba checkout in windows.yml, and it is a
	# TAG — so resolve it in the tree on disk instead of comparing the
	# string (the tree may not carry the tag at all, which is worth saying
	# rather than assuming). `^{commit}` peels the annotated tag: without it
	# `rev-parse` answers with the tag OBJECT's sha, which never equals HEAD.
	pin_ref="$(sed -n 's/^ *ref: *\(v[^ ]*\)$/\1/p' .github/workflows/windows.yml |
		head -1)"
	pin_short="$(git -C "$boba_src" rev-parse --short "$pin_ref^{commit}" 2>/dev/null ||
		true)"
	echo "PASS: libboba.a (Windows, $BOBA_SRC @ $bo_sha)"
	if [ -z "$pin_ref" ]; then
		echo "NOTE: no boba pin in windows.yml — has its checkout shape changed?"
	elif [ -z "$pin_short" ]; then
		echo "NOTE: the pin $pin_ref is not in $BOBA_SRC (HEAD $bo_sha)"
	elif [ "$bo_sha" != "$pin_short" ]; then
		echo "NOTE: boba HEAD $bo_sha is not the pinned $pin_ref ($pin_short)"
		echo "      (the run verifies the tree on disk, not the pin)"
	fi
fi

pkg_libdir=$boba_pc_dir
[ -n "$sysroot" ] && [ -d "$sysroot/mingw/lib/pkgconfig" ] &&
	pkg_libdir="$pkg_libdir:$sysroot/mingw/lib/pkgconfig"

# ---------------------------------------------------------------------
# nevermore for Windows
# ---------------------------------------------------------------------
# PKG_CONFIG, not PKG_CONFIG_PATH alone: with --host set, autoconf's
# PKG_PROG_PKG_CONFIG finds the CROSS-prefixed tool first
# (x86_64-w64-mingw32-pkg-config), and Fedora's "personality" for it
# prefixes every -I/-L with the toolchain sysroot — which mangles a boba
# installed outside that sysroot (-I/usr/.../sys-root/mingw/tmp/... and
# the build dies on boba/components/textinput.h). The plain tool plus an
# explicit PKG_CONFIG_LIBDIR is what a cross build means here.
echo "==> nevermore: configure (--host=x86_64-w64-mingw32)"
(cd "$builddir" && env -u PKG_CONFIG_PATH PKG_CONFIG=pkg-config \
	PKG_CONFIG_LIBDIR="$pkg_libdir" \
	"$srcdir/configure" --host=x86_64-w64-mingw32) \
	>"$builddir/configure.log" 2>&1 || {
	echo "FAIL: configure (see $builddir/configure.log)"
	tail -20 "$builddir/configure.log"
	exit 1
}
grep -E '^  (compiler|TLS backend|boba|test runner):' "$builddir/configure.log" || true

echo "==> nevermore: make (the app; src/nevermore.rc rides along)"
make -C "$builddir" -j"$(nproc)" >"$builddir/build.log" 2>&1 || {
	echo "FAIL: make (see $builddir/build.log)"
	tail -20 "$builddir/build.log"
	exit 1
}

# The version resource: windres compiled it (src/Makefile.am's own rule,
# run by the make above) from THIS tree's generated header, so the quad
# the .exe carries cannot drift from the version the binary prints — the
# same check MSYS2 CI's test_version pins on the host side.
version=$(sed -n 's/^#define NEVERMORE_VERSION "\(.*\)"$/\1/p' \
	"$builddir/src/nevermore_version.h")
if command -v strings >/dev/null 2>&1 && [ -n "$version" ]; then
	# Into a file, not a pipe: grep -q exits on the first match, and the
	# early close would SIGPIPE strings — plus the dump is evidence.
	strings -el "$builddir/src/nevermore-res.o" >"$builddir/nevermore-res.strings"
	if grep -qF "$version" "$builddir/nevermore-res.strings"; then
		echo "PASS: nevermore-res.o (version $version)"
	else
		echo "FAIL: nevermore-res.o does not carry version $version"
		exit 1
	fi
else
	echo "SKIP: nevermore-res.o version grep (no strings, or no header)"
fi

echo "==> nevermore: build the test binaries"
make -C "$builddir/tests" -j"$(nproc)" check-programs >>"$builddir/build.log" 2>&1 || {
	echo "FAIL: make check-programs (see $builddir/build.log)"
	tail -20 "$builddir/build.log"
	exit 1
}

# WINEDEBUG=-all: wine's fixme chatter would otherwise land in every
# test's .log. WINEPATH: a dynamically linked libwinpthread-1.dll has to
# be findable by the loader. The runner itself is configure's business
# (TEST_RUNNER, wired into tests/Makefile.am's LOG_COMPILER behind the
# per-test watchdog), so the roster, the logs and the timeout are the
# ones the native suite uses.
echo "==> nevermore: make check under wine"
run_wine() {
	if [ -n "$dll_dir" ]; then
		env WINEDEBUG=-all WINEPATH="$dll_dir" "$@"
	else
		env -u WINEPATH WINEDEBUG=-all "$@"
	fi
}

check_failed=
run_wine make -C "$builddir/tests" check >"$builddir/check.log" 2>&1 ||
	check_failed=1
if [ -n "$check_failed" ]; then
	echo "FAIL: make check (see $builddir/check.log, $builddir/tests/*.log)"
	grep -E '^(FAIL|ERROR):' "$builddir/check.log" || true
	exit 1
fi
# The harness's OWN summary, read with sed: `grep -c` exits 1 when it
# counts zero, and under `set -e` that would end the run silently on a
# suite with no skipped tests — the exact shape of the green run.
passed=$(sed -n 's/^# PASS: *//p' "$builddir/check.log")
skipped=$(sed -n 's/^# SKIP: *//p' "$builddir/check.log")
echo "PASS: ${passed:-?} tests under wine, ${skipped:-?} skipped ($builddir/check.log)"

if [ "$DO_ZIP" = 1 ]; then
	echo "==> portable ZIP (release cross tree)"
	rm -rf "$relbuild"
	mkdir -p "$relbuild"
	(cd "$relbuild" && env -u PKG_CONFIG_PATH PKG_CONFIG=pkg-config \
		PKG_CONFIG_LIBDIR="$pkg_libdir" \
		"$srcdir/configure" --host=x86_64-w64-mingw32 --enable-release) \
		>"$relbuild/configure.log" 2>&1 || {
		echo "FAIL: release configure (see $relbuild/configure.log)"
		tail -20 "$relbuild/configure.log"
		exit 1
	}
	if ! make -C "$relbuild" -j"$(nproc)" >"$relbuild/build.log" 2>&1 ||
		! make -C "$relbuild" dist-portable DIST_TARGET=windows \
			>"$relbuild/zip.log" 2>&1; then
		echo "FAIL: release build/pack (see $relbuild/build.log,"
		echo "      $relbuild/zip.log)"
		tail -20 "$relbuild/build.log" "$relbuild/zip.log"
		exit 1
	fi
	zip=$(find "$relbuild" -maxdepth 1 -name 'nevermore-*-windows-*.zip' |
		head -1)
	if [ -z "$zip" ]; then
		echo "FAIL: dist-portable produced no ZIP in $relbuild"
		exit 1
	fi
	echo "PASS: $(basename "$zip")"

	if ! command -v unzip >/dev/null 2>&1; then
		echo "WARN: unzip not found — the packed .exe was not run"
	else
		stage=$(mktemp -d "${TMPDIR:-/tmp}/nm-w32-zip.XXXXXX")
		unzip -q "$zip" -d "$stage"
		exe=$(find "$stage" -name nevermore.exe | head -1)
		zipver=$(basename "$zip" | sed 's/^nevermore-//; s/-windows-.*$//')
		if [ -z "$exe" ]; then
			echo "FAIL: no nevermore.exe in the ZIP"
			exit 1
		fi
		# The self-containment proof: WINEPATH is UNSET and the MinGW bin
		# dir is off PATH, so a DLL the bundle needs has to sit beside the
		# .exe — exactly what make-dist-zip.sh's import walk bundled.
		if out=$(env -u WINEPATH PATH=/usr/bin:/bin WINEDEBUG=-all \
			wine "$exe" --version 2>&1); then
			echo "PASS: extracted nevermore.exe: $out"
			case "$out" in
			*"nevermore $zipver"*) ;;
			*)
				echo "FAIL: the ZIP's .exe does not report $zipver"
				exit 1
				;;
			esac
		else
			echo "FAIL: the extracted nevermore.exe did not run:"
			echo "$out"
			exit 1
		fi
		rm -rf "$stage"
	fi
fi

echo "==> done"
