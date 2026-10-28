# Contributing to nevermore

How to build, test and land changes. Build and setup are in
[README.md](README.md); module design and the reasoning behind it are in
[ARCHITECTURE.md](ARCHITECTURE.md). This file is the contributor's checklist:
the gates, the test conventions, and the traps that are not obvious from any
single file.

## Contents

- [The gate](#the-gate)
- [Make targets](#make-targets)
- [Standards](#standards)
- [Writing tests](#writing-tests)
- [Dependencies](#dependencies)
- [Pre-flight before pushing](#pre-flight-before-pushing)
- [Releasing](#releasing)
- [Cross-platform lessons](#cross-platform-lessons)
- [C gotchas](#c-gotchas)
- [Manual smoke without a provider](#manual-smoke-without-a-provider)

## The gate

Before every commit:

1. `make -C build/tests check` is green. It uses canned loopback servers only;
   **no test ever hits a real API.**
2. `make format` leaves nothing to commit that you did not intend. Re-run
   `check` if it touched anything, then read `git diff --stat` and revert churn
   in files you never touched.
3. For a phase-closing push, `make distcheck` too (see Pre-flight).

Commit style is `scope: lowercase summary`, one behavior per commit (see
`git log`).

## Make targets

- `make check`: the full suite. `make check-wire` runs just the wire tests
  (dummy localhost servers, one scripted round per connection).
- `make format`: clang-format (C), shfmt (scripts), black (Python), prettier
  (Markdown). `make format-check` is the dry run plus
  `scripts/check-md-quotes.py`, which catches an inline code span broken across
  lines inside a blockquote (prettier drops the `>` there). Fix the prose rather
  than accept that output. From a build dir use `make -C build format`.
- `make catalogs`: regenerate the model catalogs (`tools/generate-catalogs.py`
  writes `src/catalog_data.h`). A **release-time** step, since the shipped
  binary must not depend on a third-party document and the suite never hits the
  network. Commit the result. `make catalogs-check` compares instead of writing.
- `make bear`: `compile_commands.json` for clangd.
- `make regen-icon`: re-render `icons/nevermore.ico` from `icons/nevermore.svg`
  (`rsvg-convert` per size, then `icotool`). The SVG is the source; commit the
  regenerated `.ico`.
- `make dist-portable`: macOS/Windows only, a self-contained ZIP (binary,
  runtime libraries the OS lacks, docs, `README-PORTABLE.txt`). Run it from a
  build dir configured `--enable-release`, because a sanitized binary is not
  portable. **The dependency must be release-built too:** boba defaults to
  ASan+UBSan on macOS and Linux, and an unsanitized nevermore cannot resolve the
  sanitizer symbols in its archive (`ld: symbol(s) not found`). The macOS job
  builds a second, release boba into its own prefix. Windows needs no second
  build (MinGW has no sanitizer runtime). `DIST_TARGET=windows` packs a tree
  cross-built by the wine pre-flight. Linux is deliberately not packaged;
  `make dist` is its artifact.

`make -j` does **not** rebuild test binaries. Use `make -C build/tests test_x`
or `make check`. A stale-binary tell is a FAIL line number pointing at old text.

## Standards

- **Names.** A file name is the module word, unprefixed (`conf.h`,
  `proc_w32.c`), and our Windows token is `w32`, never `win` (see
  ARCHITECTURE.md, Naming). Guards: `ls src/nm_*` prints nothing, and a new
  header is checked against `ls src/*.h` and the platform's include dir.
- **Warnings: fix the smell, accept the noise.** A warning that names a real
  hole is fixed at its root (a buffer sized for the worst its type can produce,
  never for the values seen). A genuine false positive, or an OS-native
  deprecation notice (Secure Transport, by design), is noise we accept: no
  pragma, no blanket attribute, no reshaping a helper to hide it from the
  analyzer. Nothing passes `-Werror`. Optimizer-only warnings show up in the
  local `-O3` release build. [AGENTS.md](AGENTS.md) has the rules for automated
  agents.
- **Tests first.** Tests exist before or with the code.
- **Delete dead code,** including code referenced only by tests: a test hook is
  not a consumer. If a test needs a seam, test the public behavior that uses it,
  or move the seam into the test. Pre-alpha means deleting is free.
- **Never point at a file that does not ship** (a commit message, doc or
  comment). A path that is not in the commit dangles for every other checkout.
  State the rule as the rule.

## Writing tests

`tests/test_helpers.h` provides `RUN_TEST` / `TEST_SUMMARY` / `ASSERT__`.
`ASSERT__` uses a bare `return;`, so it works only in void functions; check
setup in `main` with plain `if`/`return 1`. Exit code 77 marks a missing
prerequisite (skip).

### Isolation: pin everything the machine could vary

- **Discovery surfaces.** Agent-building tests call `test_chdir_to_scratch` and
  `nm_skills_set_global_dir` at a path that is never created. Agent construction
  reads `AGENTS.md` from the cwd and the user skills directory, and a source
  tree or dev box that has either adds a `<skills>` menu (~20 KiB) to the
  captured request. A new discovery surface owes the same pin.
- **Catalog.** `test_pin_offline_catalog` + `TEST_OFFLINE_CATALOG_PIN_CHECK`: a
  lookup with no endpoint override hits the provider's default base, and the
  fetched list replaces the static catalog the test asserts against. An explicit
  `base_url` wins whatever the gate says. The OpenCode catalog document works
  the same way: `NEVERMORE_OPENCODE_CATALOG_URL` is an explicit request and
  never gated, while the default URL is gated. A test that wants the document
  points it at its own server; one that wants none leaves it unset.
- **Terminal hints.** `pin_color_env`, `harness_new` / `harness_new_rgb`:
  truecolor is read when the probe starts, so the test process's own `COLORTERM`
  would decide the frame bytes.
- **Scratch dirs outlive runs.** `test_scratch_dir` is keyed by PID and never
  wiped, so a run handed an old PID inherits its files, and a layer that merges
  what it loads (the config shadow) fails exact-content asserts. Under Wine the
  PID repeats. **Remove a fixture before writing it** (as `pin_paths` /
  `pin_cfg_paths` do).
- **Restore store keys before asserting,** because a failed assert leaks the
  value into later tests.
- **Catalog caches are process-global** and a failed fetch poisons them, so
  fetch tests run before fallback tests.

### Servers, sockets and timing

- **Never hit real APIs.** Canned SSE servers on loopback, one scripted round
  per connection. A test binary that takes minutes is a bug: it waits on a round
  the code will never ask for, or on a deadline.
- **Drain the request.** An unread request plus close is an RST, which on
  macOS/BSD can discard the client's unread reply. **Async-gap assertions must
  make the gap:** a reply already in the socket reports READY, not FETCHING, so
  hold it ~200 ms. An ASan client against an uninstrumented server thread
  inverts the race.
- **Stall loops must drain-recv.** A closed peer is readable (EOF), so a
  select-only loop hot-spins. Poll with a timeout plus `recv`, and `n <= 0`
  means the peer is gone.
- **Poll the collector, not the socket** (accept/send race): loop until the
  app's collector has bytes (`nm_chat_app_tail_len`) or its state changes,
  bounded. **Poll a file's content, not its existence** (`> file` truncates
  first): wait until it parses (`wait_for_pid_file`).
- **A test never sleeps a deadline out; it moves the clock.** Pick by what is
  waited for:
  - a deadline: virtual time. `src/clock.c` is its own TU, so link
    `tests/fake_clock.c`, drive at boba's reported cadence and advance the fake
    by it. No test hook in production.
  - a child's own I/O: real readiness (`drive_virtual`, `source_ready`); only a
    quiet source advances the clock (`want_output`).
  - a paced canned server: rendezvous (`server_paced_init` / `server_ack`), not
    a guessed sleep.
  - the child's cadence is the property (`run_command`'s deadline reset): real
    time and a small budget knob. A frozen clock spins the blocking `nm_connect`
    walk, so `test_wire` links the real clock.
  - `nm_chat_app_tick` steps only when a deadline is due now; `nm_chat_app_step`
    drives unconditionally. Ticking a deadlineless walk leaves the turn silently
    STREAMING (a Windows-only red). A drive that can end inside a step must
    flush before reading output.
  - **A drive that steps on every iteration hides a stall.** The tool phase has
    no deadline, so a step is the only thing that moves it, and the real loop
    steps on an fd event or a due deadline. To prove a turn makes progress,
    drive it as the loop does (`harness_drive_events`).
- **Bound drive loops by wall clock,** not iteration count: a ready-fd pass
  costs microseconds, so 400 iterations can end before a 250 ms budget.
- **Timing margins are generous both ways:** an inactivity budget sits below the
  traffic it must outlive and ~10x above its cadence. The watchdog
  (`NM_TEST_TIMEOUT`, 60 s) is a hang detector, so keep it >= 5x the worst
  legitimate runtime (MSYS2 is slow).
- **Accept loops are bounded** (unbounded `accept()` blocks `pthread_join`).
- **Listen backlog is a platform trap:** Linux rounds it up, macOS/BSD drop a
  second SYN while one is unaccepted, Windows refuses. Listen with backlog 8. A
  client-closed connection is delivered from the backlog on Linux (EOF accept)
  but dropped on macOS/BSD, so skip 0-byte connections without consuming a
  script round.
- **Windows blackholes loopback connects** (WinNAT: SYN dropped, no RST, ~20 s),
  and bind-then-close is not a guaranteed refusal. Use `127.0.0.1:1` for
  "nothing is listening".
- **`select()`:** pass NULL for unset directions, never an empty non-NULL
  `fd_set` (Windows).
- **PENDING is part of the contract:** continue on OK/PENDING, break only on a
  final verdict.

### Style

- **Never test a removal.** No absent-symbol assertions: a negative test pins a
  deletion the diff already states. Test what the code does.
- NUL-terminate before `ASSERT_STR_EQ` (Wine's stack garbage differs from
  glibc's).
- **A bounded fixture table fails silently when full** (a NULL path): size it
  for the whole binary and `ASSERT_NOT_NULL(path)` at the make site.
- **Hand-counted string lengths are a bug:** pass `strlen()` / `sizeof`, never
  literals. Verify test chunk sizes with `printf '...\n\n' | wc -c` (the
  format-string form; `%s` leaves `\n` literal).
- **If you add a public struct field,** every positional aggregate initializer
  in `tests/` needs it; the compiler lists them as
  `-Wmissing-field-initializers`.

### Harness wiring

- Suffix-less test binaries go through automake's `LOG_COMPILER`, **not**
  `TEST_LOG_COMPILER` (which is for `.test` scripts and does nothing here).
- **The E2E harness drives the binary:** `tests/e2e_portty/` (see its README)
  runs `src/nevermore` through coffer's `cfr-debug` with `tools/fake-ollama`. It
  is part of `make check` and skips (77) when python3, `cfr-debug` or the binary
  is absent, so a tests-only build skips it. Two traps:
  - **A backgrounded helper must not inherit the harness's stdin:** a job
    reading it can be stopped by SIGTTIN, silently (`kill -0` still says alive).
    Give it `</dev/null`.
  - **A canned python server must skip `socket.getfqdn`:**
    `HTTPServer.server_bind` does a reverse lookup that stalls for tens of
    seconds where there is no rDNS (GitHub's macOS runners). `fake_ollama.py`'s
    `NoFqdnServer` exists for this.

## Dependencies

- **boba, stale install:** re-run `make install` in boba after any boba edit, or
  nevermore links yesterday's symbols. A boba API nevermore uses must be
  installed before nevermore builds. boba's release and ASan builds are two
  installs: nevermore links whichever is **installed**, so install the ASan lib
  for nevermore's ASan suite and restore the release lib after. boba's real gate
  is its ASan tree (a release tree compiles `assert()` out).
- **boba, the pin:** all three workflow files (linux, macOS, Windows-MSYS2)
  carry the same boba ref, so bump them together. Push the boba commit
  **before** the nevermore commit that bumps the pin, or CI's checkout fails. A
  pin left behind a consumed API fails differently (missing symbols at link).
  Both symptoms mean "check the pin". Pin the ref in **one** `env:` block per
  file so checkout and cache key cannot drift.
- **boba, the version is in the compile, not the pin.** boba bakes its version
  at its own configure time from `git describe --tags`, so a pin names a tree
  and nothing else. A pair of freshly built trees also reports old versions
  until both are re-configured (trust a rebuilt binary, not the banner). Hence:
  **no CI cache for a dependency whose version a shipped binary prints** (v0.2.0
  shipped a Windows `.exe` reading `boba 0.3.0.5-7d0101b` beside a macOS ZIP
  reading `boba 0.4.0`, from a stale cache), and both packaged binaries are
  asserted against what the checkout describes as.
- **stb** is gitignored, not committed. `scripts/fetch-stb.sh` pins the upstream
  commit and `configure` fetches a missing header, so a first configure (and CI)
  needs network.
- **The mbedTLS floor is the distro package (2.28), not 3.0** (Ubuntu 24.04
  ships 2.28.8, so a 3.0 gate makes auto-detect fall through to OpenSSL).
  `linux.yml` is a two-job matrix (openssl, mbedtls; explicit `--with-tls=`,
  `fail-fast: false`), and test_tls's loopback server is backend-neutral, so the
  mbedTLS job tests the handshake. mbedTLS 3.x notes: it is a **three-way
  split** (`mbedtls.pc` hides `mbedx509`/`mbedcrypto` in `Requires.private`, so
  configure names all three or the link dies "DSO missing");
  `mbedtls_ssl_conf_rng` needs a seeded `mbedtls_ctr_drbg_context` third, never
  NULL; and `mbedtls_pk_parse_keyfile` gained `f_rng` + `p_rng` arguments, which
  only test_tls's server half notices (guard that call on
  `MBEDTLS_VERSION_NUMBER` and include `<mbedtls/version.h>`, since an `#if` on
  an undefined macro is silently false). Both legs run locally:
  `../configure --with-tls=mbedtls` or `--with-tls=openssl` in a second build
  tree, then `make && make check`.
- **clang's UBSan checks function-pointer calls; gcc's does not.** A callback
  cast to the wrong function type is UB that gcc, ASan and every platform
  tolerate, while clang 19's `-fsanitize=function` catches it. Fix the type,
  never the cast.

## Pre-flight before pushing

CI runs three workflows (linux, macOS, Windows-MSYS2) and a round trip is ~4
minutes, so catch failures locally.

- **Linux** is the dev box. Build from scratch when touching `configure` or
  `Makefile.am` (a stale build dir masks missing sources). **The macOS job
  builds `tests/` and `src/` separately** (`make -j -C tests check-programs`,
  then `make -j -C src`, from a fresh dir), which is why `tests/Makefile.am`
  carries the recursive rule for the generated `nevermore_version.h` and why the
  E2E harness needs src built. Reproduce that after touching anything a test
  pulls from src's build dir.
- **Both cross pre-flights are autotools trees:** `configure` in a dir of their
  own, `make`, `make check` under the runner configure picked. The scripts
  provision the **environment** (zig SDK cache, cross boba,
  `PKG_CONFIG*`/`WINEPATH`) and read logs. **No source list, include path,
  library or test roster lives in either script**; a new TU, test or backend
  reaches both through `Makefile.am` alone. A `scripts/*.sh` line naming
  `src/*.c`, `tests/test_*.c` or a `-I` path is the drift this replaced.
- **macOS:** `scripts/zig-macos-check.sh` uses `build-macos-zig/`
  (`--host=aarch64-apple-darwin`, `CC="zig cc -target aarch64-macos"`, SDK shim
  in `~/.cache/nevermore-zig-macos-sdk`). `make compile-check` compiles the
  app's TUs **and every test program's objects** (~280). It is compile-only by
  design: a macOS link needs a real SDK.
- **Windows:** `scripts/w32-wine-check.sh` uses `build-w32/`
  (`--host=x86_64-w64-mingw32`, a Windows boba cross-built into
  `build-w32/boba-prefix`, `--with-wine` wiring tests through wine, so the
  roster, watchdog and `.log`s are the native harness's). The boba source is
  `$BOBA_SRC`, else `./boba`, else `../boba` (or an existing install in
  `$BOBA_PREFIX`); with none the run exits 77. It names the boba SHA it verified
  and notes when that is not the pin. `--no-zip` skips the second half (a
  `--enable-release` tree in `build-w32-rel/`,
  `make dist-portable DIST_TARGET=windows`, then the extracted `.exe` under wine
  with the MinGW bin dir off `PATH`). Wine is not MSYS2 UCRT64: pre-flight, not
  guarantee (it still catches ~90% in seconds).
- **Run the scripts from the repo root** (0 results from a build dir). **Read
  the logs once**: evidence lands in
  `build-w32/{boba-*,configure,build,check}.log`, `build-w32/tests/*.log` and
  `build-macos-zig/{configure,src-build,tests-build}.log`. Both cross trees are
  wiped at the start of each run, and the harness owns the roster.
- **Cross-tree traps:**
  - **The runner is `--with-wine` / `TEST_RUNNER`, never `PATH`.** autoconf
    leaves `cross_compiling` at `maybe`, so Windows-cross detection is the
    build/host OS pair (`CROSS_WINDOWS` in `configure.ac`), and the runner rides
    `LOG_COMPILER` **behind** the watchdog so the watchdog still times and kills
    (`wine test_x.exe`).
  - **`PKG_CONFIG=pkg-config` in a cross tree.** With `--host`,
    `PKG_PROG_PKG_CONFIG` finds the cross-prefixed tool, and Fedora's
    personality prefixes every `-I`/`-L` with the sysroot, mangling a boba
    installed outside it. `PKG_CONFIG_LIBDIR` (which replaces the search path)
    names the cross prefixes.
  - **`WINEPATH` for the runtime DLL:** a cross gcc that links
    `libwinpthread-1.dll` dynamically leaves every `.exe` dying with wine exit
    53 without it. Export `WINEDEBUG=-all` too, or fixme chatter lands in every
    `.log`.
  - **`compile-check` compiles and never links.** src's depends on
    `$(BUILT_SOURCES)` (automake hangs the generated version header off `all`),
    and tests' gathers object lists by re-entering make with `CCLD=true`,
    because the POSIX-make construction
    (`$(foreach p,$(check_PROGRAMS),$($(p)_OBJECTS))`) is rejected by this
    tree's `-Wall -Werror`.
  - **Under `-j`, zig's diagnostics may lack a trailing newline,** so a log line
    can hold a warning and the next `  CC` banner. Count objects with a loose
    pattern, never `^  CC`. Never zig `cc -fsyntax-only` (`FileNotFound`); use
    `-c -o /tmp/x.o`.
  - **`grep -c` exits 1 on zero matches,** a silent early exit under `set -e` on
    the green path. Read the harness's own summary with `sed`.
  - **Never pipe a compiler into an early-exiting reader** (`| head -5`):
    SIGPIPE kills the compiler before the link. Redirect to a `.build.log` and
    filter the file.
- **`make distcheck` needs a fresh `version` file:** rerun `./autogen.sh` after
  committing, or the autoreconf-time tarball name disagrees with the
  configure-time one and distcleancheck fails on the leftover nested tarball
  (remove with `chmod -R u+w`). distcleancheck reds are real (`EXTRA_DIST` /
  `CLEANFILES`).
- **When CI fails anyway:** the mac and windows workflows upload
  `build/tests/*.log` as artifacts (`test-suite.log` has the assertion and ASan
  detail the console omits):
  `gh api repos/chestso/nevermore/actions/artifacts/<id>/zip`. Also
  `gh run list`, `gh run watch <id> --exit-status`,
  `gh run view <id> --log-failed`.

## Releasing

A tag is the whole trigger.
`git tag -a v0.1.0 -m "v0.1.0" && git push origin v0.1.0` runs `release.yml`
(the only workflow with a `tags:` trigger), which calls the three platform
workflows through `workflow_call` and publishes the macOS ZIP, the Windows ZIP
and the source tarball. It also checks that the tarball's `version` stamp agrees
with the tag. Notes go on afterwards with `gh release edit`, **one line per
paragraph and per bullet** (a release body renders newlines as hard breaks).
AGENTS.md has the rest of the release rules.

- **One tag, one run.** Platform workflows carry no `tags:` trigger; adding one
  silently doubles every release's CI.
- **The tested binary and the shipped one differ.** Tests gate the sanitized
  build, the ZIP carries `--enable-release`, and the macOS ZIP links a second,
  release-built boba. All of it is built in the same job, so a packaging failure
  is red on every push, not first at release time.
- **Green master before the tag.** A red required job skips the release job, and
  no release appears. Move the tag to the fixed commit only after master is
  green.
- **Windows ZIP contents are proven, not asserted:** the verify step extracts
  the ZIP and runs `nevermore.exe --version` with only `System32` on `PATH`.
  macOS makes the same proof through `otool` (every load command under
  `/usr/lib` or `/System/Library`) plus `codesign --verify`.
- **A tracked file the build regenerates makes the tag dirty, and the release is
  refused.** The version string is `git describe` plus `-dirty` when a tracked
  file differs. `autoreconf -fi` overwrites `build-aux/ar-lib` with the host
  automake's copy, which once made the tarball `nevermore-0.1.0-dirty.tar.gz`.
  Generated aux files stay untracked (`.gitignore` covers `ar-lib`, `compile`,
  `depcomp`, `install-sh`, `missing`, `test-driver`, ...).

## Cross-platform lessons

- **Never hand-edit `Makefile.in` or trust a stale `build/`.** CI checkouts are
  fresh. After `.am` changes run `autoreconf -i` in the source tree and re-run
  `../configure` in `build/`. A stale source file in the build dir also shadows
  the VPATH source (a 0-byte `build/tests/foo.c` gives "undefined reference to
  `main`"): `ls build/tests/*.c` and delete junk.
- **automake silently drops `../src/$(VAR)` from `_SOURCES`** when the prefix is
  glued to a variable (no object, no error). The variable must carry the full
  path and the reference must be bare (`$(TEST_TLS_SOURCE)` last).
- **Build JSON with `nm_json_set` + `nm_json_dump`, never raw `snprintf`,** when
  a value can hold `\` or `"` (Windows paths). Unescaped backslashes give "bad
  escape" and the whole object is rejected. Windows fixture paths use forward
  slashes.
- **json.c arena:** the root `NmJson` node lives in the arena's last chunk, so
  `nm_json_free` must not write through the arena afterwards (glibc tolerates
  the use-after-free; macOS ASan aborts).
- **macOS:** Secure Transport deprecation warnings are expected. Don't "fix"
  them by switching frameworks.
- **Wine is not CI, and a pre-flight that reuses its output lies** (a leftover
  tree can hold a binary the sources no longer build). The cross trees are wiped
  per run and the roster is the harness's `TESTS`, never a listing of `.exe`
  files.
- **Windows (MinGW/MSYS2):**
  - `winsock2.h` first (before `windows.h`); `closesocket`; no SIGPIPE;
    `WSAStartup` before any socket call, raw test servers too.
  - `FindFirstFileW` / `FindNextFileW` are UTF-16 (`MultiByteToWideChar`);
    `_mkdir`; `_getpid`; no `sh -c` (`run_command` uses `cmd.exe /d /c`).
  - `CreateProcessW`: never quote the whole command line; pass cmd flags as
    separate argv tokens.
  - No `/tmp`; tests use `C:/Users/Public/...`.
  - **`/nonexistent` is not a portable refusal** (an MSYS2 process can create
    it). Block a directory by putting a regular file where it must go, and
    assert the shape, not the permission model.
  - **A `src/` header shadowing a platform header breaks `<...>` includes**
    (`src/process.h` ate MinGW's `<process.h>`). Rename to a different word,
    never by include order and never with a prefix (ARCHITECTURE.md, Naming).
  - **The version resource** (`src/nevermore.rc` + `icons/nevermore.ico`) is
    compiled by `src/Makefile.am`'s windres rule. The `.rc` includes the
    generated `nevermore_version.h` and derives the VERSIONINFO quad from it;
    never hand-write it. `tests/test_version.c` pins the fields.
  - MSYS2 CI runs ASan, which rejects some things glibc tolerates: "works on
    Linux" is not clean.
  - **The MSYS2 prefix stays POSIX (`$HOME/.local`):** `PKG_CONFIG_PATH` splits
    on `:`, so a `C:/…` entry reads as two bogus dirs and configure reports boba
    missing. `actions/cache` runs on the host, so its `path:` names the
    host-side mapping while msys2 steps keep the POSIX prefix.
  - **Compile in the build step, run in the run step:**
    `config.status --recheck` never descends into `tests/`, so in-step test
    builds are serial minutes.

## C gotchas

- **Hex escapes are greedy:** `"\x18ftyp"` is `\x18f` + `typ`. Split the literal
  and use `sizeof(lit)-1` for lengths (magics can hold NULs).
- **`snprintf` truncates silently** and raises `-Wformat-truncation`. For token
  buffers use `memcpy` + explicit NUL and skip what does not fit; a truncated
  key is never useful.
- **Terminate a capture buffer where it is allocated,** not only after a read. A
  child that prints nothing leaves length 0, and a buffer terminated only once
  bytes arrive hands the report uninitialized heap.
- **`va_list` into a variadic function is UB:** route through a `vsnprintf`
  helper.
- **A closed stdin reports POLLHUP, not POLLIN:** wait on `POLLIN|POLLHUP` or
  poll forever.
- **`nm_json_at` is array-only:** on an object use `nm_json_key` +
  `nm_json_get`.
- **`nm_source_items()` sets `*n = 0` / NULL before the first fetch:** call
  `fetch_begin` first. Borrowed boba payloads (a tab-completion prefix, a
  popup's `selected_text`, textinput text) must be copied before the next call.
- **boba `history_add` prepends** (a newest-first array); the file round-trip is
  identity (oldest-first on disk).
- **netrc grammar:** `default` is the one valueless key (it opens a machine-less
  entry). Every other key eats exactly one token, so treating `default` like
  them swallows the next key and loses the entry.
- **Saved artifacts open in binary mode** (`"wb"`): the Windows CRT would
  CRLF-translate and diverge from Linux. (`history.c` and `wire_recorder.c` stay
  text mode on purpose: the loader strips a trailing `\r`, and the recorder is
  line-oriented NDJSON.)

## Manual smoke without a provider

The suite never needs a network. For a manual or visual pass:

- **Fake SSE server:** `tools/fake-ollama/fake_ollama.py` (stdlib) with
  `-p ollama:local`. `--scenario table-fence` exercises reasoning, table, fence
  and prose in one turn. Bind both `127.0.0.1` and `::1` (`--both`), since
  `localhost` can resolve to `::1` first. Drive the TUI with tmux `send-keys` +
  `capture-pane` (recipe in the tool's README).
- **`tools/wire-replay`** replays a recorded dump byte-exact. It matches on
  method + path + exact body, so a dump only matches the revision and session
  that produced it. A mismatch is a 503 plus a byte-offset diff, which is the
  diagnostic.
- **Image tiers use portty's script system:** `portty -S FILE -- sleep 5` runs
  one command per frame; `emit` feeds the VT engine, `dump-sixel` reads image
  state back, `screendump` / `renderdump` write PNGs. Escape payloads are `\xNN`
  per byte (the parser has no `\"` case; it leaks a stray `\`).
- **Let the app settle first** (~2 s before the first keystroke): the
  terminal-profile round trip is still in flight, so `nm_image_supported` says
  unresolved and the attach declines. That is not a bug.
- **portty's `sendln` is CRLF, the wrong driver for a raw-mode TUI** (the CR
  submits, the LF lands in the next input as a newline). Use `send` + a lone
  `\r`, or tmux `send-keys "text" Enter`.

### The standing matrix

Re-run after any transcript-region change. Bytes go through tmux
`capture-pane -e`, pixels through portty's `screendump`:

- reasoning, table and fence rendering; a blank line between paragraphs; the
  one-line-kind boundaries (prose after a heading or a lone image line);
- a resize mid-stream, multiline paste, Ctrl+C mid-stream then resubmit;
- a `/provider` switch plus the next turn, the history round-trip across
  restarts, 40-column tail wrap;
- the spinner during connect and send against a stalling peer (and the
  inactivity error after it) in all three color tiers: the truecolor pulse in
  portty, the blink under `TERM=xterm-256color` with `COLORTERM` unset, and the
  fixed `●` with `spinner_ramp = off`;
- the connect walk's notice and budget against a black-holed address, the status
  row's elision ladder, the live `⚡` rate;
- the reminder panel's block (bytes and pixels), the file ledger's four cases,
  an image attach and a vision round-trip, and the sixel tier (portty answers
  kitty, so force the tier or emit a pinned DCS through the `-S` script: one
  image, `"source": "sixel"`, the cursor on the image's last row);
- live turns through hyper, openrouter and `opencode:go`.
