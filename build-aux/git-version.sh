#!/bin/sh
# git-version.sh - print the nevermore version string.
#
# Usage: git-version.sh [SRCDIR]
#
# Resolution order:
#   1. git describe --tags --match 'v*' in SRCDIR (v0.1-5-gabc1234 -> 0.1.5-abc1234)
#   2. 0.0.<rev-count>-<short-sha>  if SRCDIR is a git checkout with no tags
#   3. contents of SRCDIR/version   for tarball builds with no .git
#   4. literal "0.0.0-unknown"
#
# A "-dirty" suffix is appended whenever the working tree has uncommitted
# tracked changes (matches the convention used by `git describe --dirty`).
set -e

srcdir=${1:-.}

if git -C "$srcdir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        dirty=
        # update-index refreshes the stat cache so unchanged files don't show as
        # modified; ignore its exit status, only diff-index's matters.
        git -C "$srcdir" update-index --refresh -q >/dev/null 2>&1 || true
        if ! git -C "$srcdir" diff-index --quiet HEAD -- 2>/dev/null; then
                dirty="-dirty"
        fi
        if git -C "$srcdir" describe --tags --match 'v*' HEAD >/dev/null 2>&1; then
                base=$(git -C "$srcdir" describe --tags --match 'v*' HEAD |
                        sed 's/^v//;s/-\([0-9]*\)-g/.\1-/')
                echo "${base}${dirty}"
                exit 0
        fi
        rev=$(git -C "$srcdir" rev-list --count HEAD 2>/dev/null || echo 0)
        sha=$(git -C "$srcdir" rev-parse --short HEAD 2>/dev/null || echo unknown)
        case "$rev" in
        ''|*[!0-9]*) rev=0 ;; # guard: a broken git must not yield garbage
        esac
        case "$sha" in
        ''|unknown) sha=unknown ;; # non-empty guaranteed below either way
        esac
        echo "0.0.${rev}-${sha}${dirty}"
        exit 0
fi

if test -f "$srcdir/version"; then
        # An empty (or whitespace-only) file must not poison AC_INIT:
        # m4_esyscmd_s on empty output yields an empty version argument
        # and configure dies with "should be called with package and
        # version arguments". Fall through to the literal instead.
        v=$(cat "$srcdir/version")
        if test -n "$v"; then
                echo "$v"
                exit 0
        fi
fi

echo "0.0.0-unknown"
