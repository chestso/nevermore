AC_PREREQ([2.69])
AC_INIT([nevermore],
        m4_esyscmd_s([cat version 2>/dev/null || echo "0.0.0-unknown"]),
        [https://codeberg.org/thomasc/nevermore/issues])
AC_CONFIG_SRCDIR([src/nevermore.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.14 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

AC_CANONICAL_HOST

dnl Detect target platform
case "$host_os" in
  mingw*|msys*) host_windows=yes ;;
  darwin*)      host_macos=yes ;;
  *)            host_windows=no; host_macos=no ;;
esac
AM_CONDITIONAL([HOST_WINDOWS], [test "x$host_windows" = "xyes"])
AM_CONDITIONAL([HOST_MACOS], [test "x$host_macos" = "xyes"])

dnl Recompute the version at configure time so a rebuild after new commits
dnl picks up the current version without needing to re-run autoreconf. Falls
dnl back to PACKAGE_VERSION (set at autoreconf time via m4_esyscmd_s above)
dnl for tarball builds where build-aux/git-version.sh has nothing to read.
NEVERMORE_VERSION=`"$srcdir/build-aux/git-version.sh" "$srcdir" 2>/dev/null`
AS_IF([test -z "$NEVERMORE_VERSION"],
      [NEVERMORE_VERSION="$PACKAGE_VERSION"])
AC_SUBST([NEVERMORE_VERSION])
AC_MSG_NOTICE([nevermore version: $NEVERMORE_VERSION])

dnl Derive the automake VERSION (and thus the dist tarball name and the
dnl PACKAGE_VERSION/VERSION macros in config.h) from the version recomputed
dnl above. Without this, `make dist` after a tag still names the tarball
dnl with the autoreconf-time version.
PACKAGE_VERSION="$NEVERMORE_VERSION"
VERSION="$NEVERMORE_VERSION"

# Checks for programs
user_cflags="$CFLAGS"
AC_PROG_CC
AC_PROG_RANLIB
AM_PROG_AR

# Build mode selection (--enable-release > --enable-debug > default ASan)
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [unsanitized debug build (-O0 -g3, no ASan)])],
  [enable_debug=$enableval],
  [enable_debug=no])

AC_ARG_ENABLE([release],
  [AS_HELP_STRING([--enable-release],
    [optimized release build (-O3 -DNDEBUG)])],
  [enable_release=$enableval],
  [enable_release=no])

if test "x$enable_release" = "xyes"; then
  CFLAGS="-O3 -DNDEBUG"
  AC_MSG_NOTICE([Release mode: CFLAGS set to -O3 -DNDEBUG])
elif test "x$enable_debug" = "xyes"; then
  CFLAGS="-O0 -g3"
  AC_MSG_NOTICE([Debug mode: CFLAGS set to -O0 -g3])
elif test "x$user_cflags" = "x"; then
  dnl ASan/UBSan is not available on Windows/MinGW. Fall back to
  dnl unsanitized debug when building natively on Windows (MinGW).
  if test "x$host_windows" = "xyes"; then
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common"
    AC_MSG_NOTICE([Debug mode (no ASan on Windows): CFLAGS=$CFLAGS])
  else
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    LDFLAGS="-fsanitize=address,undefined $LDFLAGS"
    AC_MSG_NOTICE([ASan+UBSan mode (default): CFLAGS=$CFLAGS])
  fi
fi

# Check for pkg-config
PKG_PROG_PKG_CONFIG

# boba is required (TUI: textinput, viewport, runtime event loop)
PKG_CHECK_MODULES([BOBA], [boba],
    [have_boba=yes],
    [AC_MSG_ERROR([boba TUI library is required. Build and install the boba sibling project first (make boba from the chestso monorepo root), or: apt install libboba-dev])])
AC_DEFINE([HAVE_BOBA], [1], [boba TUI library is available])
BOBA_VERSION_STRING=$($PKG_CONFIG --modversion boba)
AC_DEFINE_UNQUOTED([BOBA_VERSION], ["$BOBA_VERSION_STRING"], [boba library version])

dnl ------------------------------------------------------------------
dnl TLS backend selection (no libcurl, ever).
dnl
dnl   Windows: Schannel          (Win32 API, always present, -lsecur32)
dnl   macOS:   Secure Transport  (Security.framework, always present)
dnl   Linux/other: optional, auto-detected at configure time
dnl                (--with-tls=mbedtls|openssl|none)
dnl
dnl Plain HTTP (localhost Ollama / llama.cpp servers) works with TLS=none
dnl on every platform, so the zero-dependency core always builds.
dnl ------------------------------------------------------------------
AC_ARG_WITH([tls],
  [AS_HELP_STRING([--with-tls=BACKEND],
    [TLS backend for HTTPS: mbedtls, openssl, none (Windows/macOS use the OS backend and ignore this)])],
  [with_tls=$withval],
  [with_tls=auto])

tls_backend=none
if test "x$host_windows" = "xyes"; then
  tls_backend=schannel
  AC_DEFINE([NM_TLS_SCHANNEL], [1], [TLS backend: Windows Schannel])
  LIBS="-lsecur32 -lcrypt32 -lws2_32 $LIBS"
  AC_MSG_NOTICE([TLS backend: Windows Schannel])
elif test "x$host_macos" = "xyes"; then
  tls_backend=sectransport
  AC_DEFINE([NM_TLS_SECTRANSPORT], [1], [TLS backend: macOS Secure Transport])
  LIBS="-framework Security -framework CoreFoundation $LIBS"
  AC_MSG_NOTICE([TLS backend: macOS Secure Transport])
else
  if test "x$with_tls" = "xauto" || test "x$with_tls" = "xmbedtls"; then
    PKG_CHECK_MODULES([MBEDTLS], [mbedtls >= 3.0],
      [tls_backend=mbedtls
       AC_DEFINE([NM_TLS_MBEDTLS], [1], [TLS backend: mbedTLS])
       AC_MSG_NOTICE([TLS backend: mbedTLS])],
      [AS_IF([test "x$with_tls" = "xmbedtls"],
         [AC_MSG_ERROR([--with-tls=mbedtls requested but mbedTLS not found])],
         [AC_MSG_NOTICE([mbedTLS not found, trying OpenSSL])])])
  fi
  if test "x$tls_backend" = "xnone" && \
     (test "x$with_tls" = "xauto" || test "x$with_tls" = "xopenssl"); then
    PKG_CHECK_MODULES([OPENSSL], [openssl],
      [tls_backend=openssl
       AC_DEFINE([NM_TLS_OPENSSL], [1], [TLS backend: OpenSSL])
       AC_MSG_NOTICE([TLS backend: OpenSSL])],
      [AS_IF([test "x$with_tls" = "xopenssl"],
         [AC_MSG_ERROR([--with-tls=openssl requested but OpenSSL not found])])])
  fi
  if test "x$tls_backend" = "xnone"; then
    if test "x$with_tls" = "xnone"; then
      AC_MSG_NOTICE([TLS disabled (--with-tls=none): HTTPS providers unavailable, plain HTTP only])
    else
      AC_MSG_WARN([no TLS backend found — HTTPS providers unavailable, plain HTTP only (localhost Ollama/llama.cpp). Install libmbedtls-dev or libssl-dev to enable cloud providers.])
    fi
  fi
fi
AM_CONDITIONAL([TLS_SCHANNEL], [test "x$tls_backend" = "xschannel"])
AM_CONDITIONAL([TLS_SECTRANSPORT], [test "x$tls_backend" = "xsectransport"])
AM_CONDITIONAL([TLS_MBEDTLS], [test "x$tls_backend" = "xmbedtls"])
AM_CONDITIONAL([TLS_OPENSSL], [test "x$tls_backend" = "xopenssl"])
AM_CONDITIONAL([TLS_NONE], [test "x$tls_backend" = "xnone"])

# Check for Windows resource compiler (windres) — embeds icon/version metadata
# into the .exe on MinGW/MSYS2 builds.
if test "x$host_windows" = "xyes"; then
  AC_CHECK_TOOL([WINDRES], [windres], [no])
  if test "x$WINDRES" = "xno"; then
    AC_MSG_WARN([windres not found — .exe will not have an embedded icon])
  fi
fi
AM_CONDITIONAL([HAVE_WINDRES], [test "x$host_windows" = "xyes" && test "x$WINDRES" != "xno"])

# Record the C compiler version for --version output
BUILD_CC_VERSION=$($CC --version 2>/dev/null | head -1 || echo "$CC")
AC_DEFINE_UNQUOTED([BUILD_CC], ["$BUILD_CC_VERSION"], [C compiler version string])

AC_TYPE_SIZE_T

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    cli/Makefile
    tests/Makefile
])
AC_OUTPUT

echo "
nevermore $NEVERMORE_VERSION
  compiler:    $CC
  TLS backend: $tls_backend
  boba:        $BOBA_VERSION_STRING
"
