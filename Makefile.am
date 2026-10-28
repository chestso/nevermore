SUBDIRS = src tests

ACLOCAL_AMFLAGS = -I m4

dist_doc_DATA = README.md

EXTRA_DIST = \
    autogen.sh \
    build-aux/git-version.sh \
    version \
    COPYING \
    README.md \
    ARCHITECTURE.md \
    CONTRIBUTING.md \
    .clang-format \
    scripts \
    tools \
    docs \
    icons \
    third_party/stb/stb_image.h \
    third_party/stb/stb_image_write.h

# dist hygiene: tools/ ships wholesale, so strip Python bytecode caches
# (regenerated whenever the tools run in-tree; never part of the dist).
dist-hook:
	find $(distdir) -name '__pycache__' -type d -prune -exec rm -rf {} +

# Format target — clang-format on C, shfmt on shell, black on Python,
# prettier on Markdown.
.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
        echo "Formatting C with clang-format..."; \
        find $(top_srcdir)/src $(top_srcdir)/tests \
            \( -name '*.c' -o -name '*.h' \) -print0 \
            | xargs -0 clang-format -i; \
    else \
        echo "clang-format not found - skipping C formatting." >&2; \
    fi
	@if command -v shfmt >/dev/null 2>&1; then \
        echo "Formatting shell scripts with shfmt..."; \
        find $(top_srcdir)/scripts \
            -name '*.sh' -print0 2>/dev/null \
            | xargs -0 -r shfmt -w; \
    else \
        echo "shfmt not found - skipping shell script formatting." >&2; \
    fi
	@if command -v black >/dev/null 2>&1; then \
        echo "Formatting Python with black..."; \
        find $(top_srcdir) -name '*.py' \
            -not -path '$(top_srcdir)/build/*' \
            -not -path '$(top_srcdir)/build-asan/*' -print0 \
            | xargs -0 -r black --quiet; \
    else \
        echo "black not found - skipping Python formatting." >&2; \
    fi
	@if command -v prettier >/dev/null 2>&1; then \
        echo "Formatting Markdown with prettier..."; \
        find $(top_srcdir) -maxdepth 2 -name '*.md' -print0 \
            | xargs -0 -r prettier --write; \
    else \
        echo "prettier not found - skipping Markdown formatting." >&2; \
    fi

# Format CHECK — the same four formatters in dry-run mode, so drift is a
# red line instead of a silent rewrite of files the change never touched,
# plus the one construct prettier cannot round-trip: an inline code span
# that crosses a line break inside a blockquote (check-md-quotes.py; the
# `>` prefix is dropped, and lazy continuation hides it until a later
# edit lets the text fall out of the quote). `make format` stays what you
# run before a commit; this is what a gate runs.
.PHONY: format-check
format-check:
	@status=0; \
    if command -v clang-format >/dev/null 2>&1; then \
        echo "Checking C with clang-format..."; \
        find $(top_srcdir)/src $(top_srcdir)/tests \
            \( -name '*.c' -o -name '*.h' \) -print0 \
            | xargs -0 clang-format --dry-run --Werror || status=1; \
    else \
        echo "clang-format not found - skipping C check." >&2; \
    fi; \
    if command -v shfmt >/dev/null 2>&1; then \
        echo "Checking shell scripts with shfmt..."; \
        find $(top_srcdir)/scripts \
            -name '*.sh' -print0 2>/dev/null \
            | xargs -0 -r shfmt -d || status=1; \
    else \
        echo "shfmt not found - skipping shell check." >&2; \
    fi; \
    if command -v black >/dev/null 2>&1; then \
        echo "Checking Python with black..."; \
        find $(top_srcdir) -name '*.py' \
            -not -path '$(top_srcdir)/build/*' \
            -not -path '$(top_srcdir)/build-asan/*' -print0 \
            | xargs -0 -r black --check --diff || status=1; \
    else \
        echo "black not found - skipping Python check." >&2; \
    fi; \
    if command -v prettier >/dev/null 2>&1; then \
        echo "Checking Markdown with prettier..."; \
        find $(top_srcdir) -maxdepth 2 -name '*.md' -print0 \
            | xargs -0 -r prettier --check || status=1; \
    else \
        echo "prettier not found - skipping Markdown check." >&2; \
    fi; \
    echo "Checking quoted inline code spans..."; \
    python3 "$(top_srcdir)/scripts/check-md-quotes.py" $$( \
        find $(top_srcdir) -maxdepth 2 -name '*.md' -print0 \
            | xargs -0 -r printf '%s\n') || status=1; \
    exit $$status

# The catalogs — ONE generator for every provider (tools/generate-catalogs.py).
# It resolves each provider's catalog with FULL COVERAGE and writes the
# generated tables (src/catalog_data.h) that the app falls back to until
# the RUNTIME resolution lands. A RELEASE-time step, never a build-time
# one: the shipped binary must not depend on a third-party wire document,
# and the suite never hits the network.
#
# `--check` compares instead of writing: the release gate that says the
# committed catalogs are current.
.PHONY: catalogs
catalogs:
	@python3 "$(top_srcdir)/tools/generate-catalogs.py" --out "$(top_srcdir)"

.PHONY: catalogs-check
catalogs-check:
	@python3 "$(top_srcdir)/tools/generate-catalogs.py" --out "$(top_srcdir)" --check

# Portable ZIP — a self-contained folder for macOS or Windows: the binary,
# the runtime libraries it needs that the OS does not ship, the docs, and a
# README-PORTABLE.txt. GNU/Linux is deliberately not packaged: the source
# tarball (`make dist`) is the Linux artifact, and CI builds it in a job of
# its own.
#
# Run it from a build dir configured --enable-release: the default build is
# the sanitizer build, and a sanitizer binary is not a portable binary.
# DIST_TARGET=windows|macos packs a cross-built tree from another host.
.PHONY: dist-portable
dist-portable:
	@sh "$(top_srcdir)/scripts/make-dist-zip.sh" "$(abs_builddir)" "$(abs_builddir)" "$(DIST_TARGET)"

# Bear target — produce compile_commands.json for clangd.
.PHONY: bear
bear:
	@command -v bear >/dev/null 2>&1 || { \
        echo "bear not installed - install via your package manager." >&2; \
        exit 1; \
    }
	$(MAKE) clean
	bear --output $(top_srcdir)/compile_commands.json -- $(MAKE)

# Windows icon — the committed icons/nevermore.ico is a six-size
# multi-resolution icon (256/128/64/48/32/16, 32bpp) rendered from the
# source SVG by a 2-pass recipe: rsvg-convert renders each size
# individually, icotool combines them. The SVG is the source of truth;
# after changing it run `make regen-icon` and commit the result. The
# rasterized PNGs are intermediates and stay in the build tree.
.PHONY: regen-icon
regen-icon:
	@command -v rsvg-convert >/dev/null 2>&1 || { \
        echo "rsvg-convert not installed (librsvg) - needed to render the SVG." >&2; \
        exit 1; \
    }
	@command -v icotool >/dev/null 2>&1 || { \
        echo "icotool not installed (icoutils) - needed to combine the PNGs." >&2; \
        exit 1; \
    }
	@set -e; \
    for size in 16 32 48 64 128 256; do \
        rsvg-convert -w $$size -h $$size -f png \
            $(top_srcdir)/icons/nevermore.svg \
            -o nevermore-$$size.png; \
    done; \
    icotool -c -o $(top_srcdir)/icons/nevermore.ico \
        nevermore-16.png nevermore-32.png nevermore-48.png \
        nevermore-64.png nevermore-128.png nevermore-256.png; \
    rm -f nevermore-16.png nevermore-32.png nevermore-48.png \
        nevermore-64.png nevermore-128.png nevermore-256.png
	@echo "  GEN      icons/nevermore.ico"
