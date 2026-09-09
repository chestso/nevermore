#!/bin/sh
# zig-macos-check.sh - local macOS CI pre-flight via zig cc.
#
# Cross-compiles every translation unit to aarch64-macos Mach-O objects
# (the exact backend CI selects: Secure Transport on darwin), catching
# macOS-only compile errors (Secure Transport API misuse, darwin-only
# headers) without a Mac.
#
# Security/CoreFoundation headers are fetched from Apple's open-source
# releases into ~/.cache/nevermore-zig-macos-sdk on first run; the
# SDK's availability/swift annotations are neutralized by a shim header
# since the full real SDK is not redistributable. Framework symbols are
# checked at compile time only — the real link happens on CI.
#
# Exit: 0 = all units compile for macOS; 1 = compile errors; 77 = no zig.
#
# Usage: scripts/zig-macos-check.sh [builddir]
set -e

builddir=${1:-build}
cd "$(dirname "$0")/.."

cache=$HOME/.cache/nevermore-zig-macos-sdk

command -v zig >/dev/null 2>&1 || {
	echo "zig not found (dnf install zig)"
	exit 77
}
command -v gh >/dev/null 2>&1 || {
	echo "gh not found (sdk staging uses it)"
	exit 77
}

# Stage the Apple-SDK shim once. Idempotent.
if [ ! -f "$cache/.ready" ]; then
	echo "staging macOS SDK shim into $cache ..."
	rm -rf "$cache"
	mkdir -p "$cache/usr/include"
	mkdir -p "$cache/Security.framework/Headers"
	curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/Security/main/OSX/libsecurity_ssl/Security/SecureTransport.h" \
		-o "$cache/Security.framework/Headers/SecureTransport.h"
	curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/Security/main/OSX/libsecurity_ssl/Security/CipherSuite.h" \
		-o "$cache/Security.framework/Headers/CipherSuite.h"
	for p in $(gh api "repos/apple-oss-distributions/Security/git/trees/main?recursive=1" --jq '.tree[].path' | grep -E "^header_symlinks/Security/.*\.h$"); do
		curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/Security/main/$p" \
			-o "$cache/Security.framework/Headers/$(basename "$p")"
	done
	mkdir -p "$cache/CoreFoundation.framework/Headers"
	for p in $(gh api "repos/apple-oss-distributions/CF/git/trees/main?recursive=1" --jq '.tree[].path' | grep -E "\.h$"); do
		curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/CF/main/$p" \
			-o "$cache/CoreFoundation.framework/Headers/$(basename "$p")"
	done
	# cssm chain (SecTrust.h → cssmtype.h → cssmconfig.h → x509defs.h)
	for h in cssmtype.h cssmapi.h cssmerr.h cssmerrors.h cssmconfig.h x509defs.h; do
		curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/Security/main/OSX/libsecurity_cssm/lib/$h" \
			-o "$cache/Security.framework/Headers/$h"
	done
	curl -sL "https://raw.githubusercontent.com/apple-oss-distributions/Security/main/cssm/cssmapple.h" \
		-o "$cache/Security.framework/Headers/cssmapple.h"
	echo done >"$cache/.ready"
fi

# Neutralize the SDK annotations zig's bundled darwin libc does not
# provide (Availability.h does not chain to os/availability.h here).
cat >"$cache/nm-zig-shim.h" <<'SHIM'
#ifndef NM_ZIG_SHIM_H
#define NM_ZIG_SHIM_H
#undef CF_AVAILABLE
#define CF_AVAILABLE(...)
#undef CF_AVAILABLE_MAC
#define CF_AVAILABLE_MAC(...)
#undef CF_AVAILABLE_IOS
#define CF_AVAILABLE_IOS(...)
#undef CF_DEPRECATED
#define CF_DEPRECATED(...)
#undef CF_SWIFT_NAME
#define CF_SWIFT_NAME(x)
#undef API_AVAILABLE
#define API_AVAILABLE(...)
#undef API_DEPRECATED
#define API_DEPRECATED(...)
#undef API_DEPRECATED_WITH_REPLACEMENT
#define API_DEPRECATED_WITH_REPLACEMENT(...)
#undef API_UNAVAILABLE
#define API_UNAVAILABLE(...)
#undef CF_ENUM_DEPRECATED
#define CF_ENUM_DEPRECATED(...)
#undef CF_ENUM_AVAILABLE
#define CF_ENUM_AVAILABLE(...)
#endif
SHIM

# CFAvailability.h ships clang availability attributes; zig's darwin
# libc headers don't chain them. Patch the staged copy once.
cfav="$cache/CoreFoundation.framework/Headers/CFAvailability.h"
if ! grep -q "NM_ZIG_STAGED" "$cfav" 2>/dev/null; then
	python3 - "$cfav" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
# typed-enum fallback (C11-safe): typedef form, not typed anonymous enums
old = """#if (__cplusplus && __cplusplus >= 201103L && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum))) || (!__cplusplus && __has_feature(objc_fixed_enum))
#define CF_ENUM(_type, _name) enum _name : _type _name; enum _name : _type
#if (__cplusplus)
#define CF_OPTIONS(_type, _name) _type _name; enum : _type
#else
#define CF_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
#endif
#else
#define CF_ENUM(_type, _name) _type _name; enum
#define CF_OPTIONS(_type, _name) _type _name; enum
#endif"""
new = """#define CF_ENUM(_type, _name) _type _name; enum
#define CF_OPTIONS(_type, _name) _type _name; enum"""
src = src.replace(old, new)
# strip the attribute-based availability expansions (target OS branches)
src = src.replace("#define CF_AVAILABLE(_mac, _ios) __attribute__((availability(macosx,__NSi_##_mac)))",
                  "#define CF_AVAILABLE(_mac, _ios)")
src = src.replace("#define CF_AVAILABLE_MAC(_mac) __attribute__((availability(macosx,__NSi_##_mac)))",
                  "#define CF_AVAILABLE_MAC(_mac)")
src = src.replace("#define CF_DEPRECATED(_macIntro, _macDep, _iosIntro, _iosDep, ...) __attribute__((availability(macosx,__NSi_##_macIntro __NSd_##_macDep,message=\"\" __VA_ARGS__)))",
                  "#define CF_DEPRECATED(_macIntro, _macDep, _iosIntro, _iosDep, ...)")
src = src.replace("#define CF_DEPRECATED_MAC(_macIntro, _macDep, ...) __attribute__((availability(macosx,__NSi_##_macIntro __NSd_##_macDep,message=\"\" __VA_ARGS__)))",
                  "#define CF_DEPRECATED_MAC(_macIntro, _macDep, ...)")
src = src.replace("#define CF_AVAILABLE(_mac, _ios) __attribute__((availability(macosx,unavailable)))",
                  "#define CF_AVAILABLE(_mac, _ios)")
src += """
/* zig-macos-check shim appended */
#undef CF_ENUM
#define CF_ENUM(_type, _name) _type _name; enum
#undef CF_OPTIONS
#define CF_OPTIONS(_type, _name) _type _name; enum
#undef CF_CLOSED_ENUM
#define CF_CLOSED_ENUM(_type, _name) _type _name; enum
"""
open(path, 'w').write(src)
print("patched", path)
PY
	# SecBase.h one-arg CF_ENUM(OSStatus) + nullability shims
	secbase="$cache/Security.framework/Headers/SecBase.h"
	python3 - "$secbase" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
src = src.replace("#include <sys/cdefs.h>",
"""#include <sys/cdefs.h>
#undef API_UNAVAILABLE
#define API_UNAVAILABLE(...)""", 1)
src = src.replace("CF_ENUM(OSStatus)\n{", "CF_ENUM(OSStatus, __nm_anon1)\n{")
src = src.replace("CF_ENUM(OSStatus) {", "CF_ENUM(OSStatus, __nm_anon2) {")
open(path, 'w').write(src)
PY
	csuite="$cache/Security.framework/Headers/CipherSuite.h"
	python3 - "$csuite" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
src = src.replace("CF_ENUM(SSLCipherSuite)\n{", "CF_ENUM(SSLCipherSuite, __nm_css)\n{")
open(path, 'w').write(src)
PY
	# API_* re-definitions in headers that include zig's Availability.h
	for f in SecCertificate.h SecTrust.h SecItem.h SecPolicy.h SecKey.h CMSEncoder.h; do
		p="$cache/Security.framework/Headers/$f"
		[ -f "$p" ] && python3 - "$p" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
src += """
/* zig-macos-check shim */
#undef API_AVAILABLE
#define API_AVAILABLE(...)
#undef API_DEPRECATED
#define API_DEPRECATED(...)
#undef API_DEPRECATED_WITH_REPLACEMENT
#define API_DEPRECATED_WITH_REPLACEMENT(...)
#undef API_UNAVAILABLE
#define API_UNAVAILABLE(...)
"""
open(path, 'w').write(src)
PY
	done
	echo patched >>"$cache/.ready"
fi

fail=0
for f in src/*.c; do
	case "$f" in
	src/tls_openssl.c | src/tls_schannel.c | src/tls_mbedtls.c)
		echo "SKIP $f (backend not selected on macOS CI)"
		continue
		;;
	src/tools_spawn_win.c | src/os_compat_win.c)
		echo "SKIP $f (windows-only TU)"
		continue
		;;
	src/tls_sectransport.c)
		out=$(zig cc -target aarch64-macos -DNM_TLS_SECTRANSPORT \
			-include "$cache/nm-zig-shim.h" \
			-c -o "/tmp/nmm_$(basename "$f" .c).o" "$f" \
			-I"$builddir" -I"$builddir/src" -Isrc -I. -I"$HOME/.local/include" \
			-isysroot "$cache" -F "$cache" 2>&1 | grep -E "error:" | head -2)
		;;
	*)
		out=$(zig cc -target aarch64-macos \
			-c -o "/tmp/nmm_$(basename "$f" .c).o" "$f" \
			-I"$builddir" -I"$builddir/src" -Isrc -I. -I"$HOME/.local/include" 2>&1 | grep -E "error:" | head -2)
		;;
	esac
	if [ -n "$out" ]; then
		echo "FAIL $f"
		echo "$out"
		fail=1
	else
		echo "OK   $f"
	fi
done

exit $fail
