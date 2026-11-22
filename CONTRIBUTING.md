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

- `make check`: the full suite (dummy loopback servers, one scripted round
  per request).
- `make format`: clang-format (C), shfmt (scripts), black (Python), dprint
  (Markdown). `make format-check` is the dry run plus
  `scripts/check-md-quotes.py`, which catches an inline code span broken across
  lines inside a blockquote. dprint keeps code spans on one line; the check
  guards the invariant for hand edits. From a build dir use `make -C build format`.
- `make catalogs`: regenerate the model catalogs (`tools/generate-catalogs.py`
  writes `src/catalog_data.h`). A **release-time** step, since the shipped
  binary must not depend on a third-party document and the suite never hits the
  network. Commit the result. `make catalogs-check` compares instead of writing.
- `make bear`: `compile_commands.json` for clangd.
- `make regen-icon`: re-render `icons/nevermore.ico` from `icons/nevermore.svg`.
  The SVG is the source; commit the regenerated `.ico`.
- `make dist-portable`: macOS/Windows only, a self-contained ZIP (binary,
  runtime libraries the OS lacks, docs, `README-PORTABLE.txt`). Run it from a
  build dir configured `--enable-release`, because a sanitized binary is not
  portable. **The dependency must be release-built too:** a release nevermore
  cannot resolve the sanitizer symbols in boba's default ASan+UBSan archive
  (`ld: symbol(s) not found`), so CI installs one release boba per job.
  `DIST_TARGET=windows` packs a tree cross-built by the wine pre-flight. Linux
  is deliberately not packaged; `make dist` is its artifact.

`make -j` does **not** rebuild test binaries. Use `make -C build/tests test_x`
or `make check`. A stale-binary tell is a FAIL line number pointing at old text.

## Standards

- **Names.** A file name is the module word, unprefixed (`conf.h`,
  `proc_w32.c`), and our Windows token is `w32`, never `win` (see
  ARCHITECTURE.md, Naming). Guards: `ls src/nm_*` prints nothing, and a new
  header is checked against `ls src/*.h` and the platform's include dir.
- **Time values are milliseconds, everywhere.** Every time-valued key
  (README's Configuration table owns the set) is a positive decimal in ms, and
  its env spelling ends in `_MS` (`off`, where a key takes it, disables). A key
  documented in another unit is a bug: one shipped a seconds story over the ms
  resolver, so the written value silently meant the other.
- **Warnings: fix the smell, accept the noise.** A warning that names a real
  hole is fixed at its root (a buffer sized for the worst its type can produce,
  never for the values seen). A genuine false positive, or an OS-native
  deprecation notice (Secure Transport, by design), is noise we accept: no
  pragma, no blanket attribute, no reshaping a helper to hide it from the
  analyzer. Nothing passes `-Werror`. Optimizer-only warnings show up in the
  local `-O3` release build.
- **A column is DISPLAY columns, never bytes.** A row that aligns fields pads
  through `columns.h` (`nm_columns_needed` / `nm_columns_pad`), which measures
  with boba's `tui_utf8_display_width`: a printf field width counts bytes, so
  `%-16s` on a cell carrying `·` (two bytes, one column) shifts every row that
  has a tag, and a wide or combining glyph shifts the rest. Text that must be
  clipped is clipped with boba's `tui_text_fit` — the same walk — and padded by
  the columns it actually emitted, so a wide glyph is never split and the
  leftover column becomes padding, never a short row.
- **Tests first.** Tests exist before or with the code.
- **Delete dead code,** including code referenced only by tests: a test hook is
  not a consumer. If a test needs a seam, test the public behavior that uses it,
  or move the seam into the test. Pre-alpha means deleting is free.
- **Never point at a file that does not ship** (a commit message, doc or
  comment). A path that is not in the commit dangles for every other checkout.
  State the rule as the rule.

## Writing tests

`tests/test_helpers.h` owns the harness macros. `ASSERT__` uses a bare
`return;`, so it works only in void functions; check setup in `main` with
plain `if`/`return 1`. Exit code 77 marks a missing prerequisite (skip).

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

- **Never hit real APIs.** Canned servers on loopback, one scripted round per
  request. A test binary that takes minutes is a bug: it waits on a round
  the code will never ask for, or on a deadline.
- **One thread, one poll, no timers: the harness is a reactor, not a
  schedule.** A canned server is a scripted pump — nonblocking accept, drain,
  flush — serviced by the same poll that waits on the client's fd, so every
  byte is an event the test reacts to; `tests/net_server.[ch]` is that pump,
  shared by every test that needs a wire peer (`nm_ts_start`, `nm_ts_pump`,
  `nm_ts_wait`, a script of rounds with `NM_TS_SERVE`/`DROP`/`DIE`/`STALL`
  actions and an `on_request` hook for routing; `NM_TS_HOLD` parks a request's
  socket so a held peer and a served one can share one listener — the app
  tests' "a fetch in flight while the chat runs", with no second thread).
  A `usleep` orchestrates by
  calendar and a worker thread is a second order of interleavings the test
  has to reason about; both are how a suite grows hangs and platform flakes
  (the v0.16.0 tag run died on a `shutdown()`-on-a-listener shape that
  Linux tolerates and BSD/Windows reject). A request-counted script makes
  the timing shapes deterministic — the die case closes the listener in the
  harness's own thread, before the client's connect step, so the refused
  redial is the same on every stack. If a case cannot be written as a pump,
  the case is dropped, not threaded. Three exceptions are DELIBERATE and say
  why in the file: ask mode's job-drain pair in `test_agent` (the client
  blocks inside `nm_agent_turn`, so no test thread is left to pump), and
  `test_tls` / `test_wire` (a blocking client — a handshake in a forked child,
  `nm_read_body` — needs a thread of its own). Each keeps `-lpthread`.
- **A blocking wrapper keeps its own test.** `nm_toolset_execute` is covered
  through `test_tools`' synchronous-pump cases and `nm_openai_chat` through
  `test_openai_client`'s one-shot; a file that migrates to the async seam must
  leave those wrappers tested, not orphaned.
- **The capture store dies with the server.** `nm_ts_request()` returns a
  pointer into the server's own store, which `nm_ts_stop` frees: read or copy
  the evidence BEFORE every stop, never after. The wire recorder's path read
  and the provider's wire read were both this use-after-free; glibc tolerates
  it, macOS ASan aborts.
- **A canned server that serves one request and closes MUST say `Connection:
  close`.** Under a keep-alive pool a reply without the header is an invitation
  to reuse, and the next exchange on that socket races the server's FIN — a
  flake, not a failure. Send canned replies through `test_send_reply` (it
  splices the header in when the reply lacks it); a server that WANTS reuse
  accepts once and answers twice. The suite's immunity to the keepalive flip
  is the pin (`test_net_pin_keepalive_off` + `TEST_KEEPALIVE_PIN_CHECK` in
  every dialing binary's main), not the sweep.
- **Drain the request.** An unread request plus close is an RST, which on
  macOS/BSD can discard the client's unread reply. **Async-gap assertions must
  make the gap:** a reply already in the socket reports READY, not FETCHING, so
  arm it only when the test drives (the pump's `chunk_events`, never a sleep).
  An ASan client against an uninstrumented server thread inverts the race.
- **A pump's `send` is not the peer's readability.** A loopback send is queued,
  not instantly visible to the peer's `recv`: a case that reads immediately
  after `nm_ts_pump` can see `NM_READ_WOULD_BLOCK` with the reply already
  written (deterministic on Linux, a timing accident on Windows). Treating that
  as EOF loses the body. Wait on the interest the transport declared
  (`nm_ts_wait`) and read again; only `0`/`-1` ends the body.
- **Stall loops must drain-recv.** A closed peer is readable (EOF), so a
  select-only loop hot-spins. Poll with a timeout plus `recv`, and `n <= 0`
  means the peer is gone.
- **Poll the collector, not the socket** (accept/send race): loop until the
  app's collector has bytes (`nm_chat_app_tail_len`) or its state changes,
  bounded. **Poll a file's content, not its existence** (`> file` truncates
  first): wait until it parses (`wait_for_pid_file`).
- **A waitable HANDLE's event is one-shot.** A job's loop handle on Windows is
  an AUTO-RESET event: the wait that returned has already consumed it, so
  re-probing the handle reports "not ready" and the wakeup is gone — a
  background job then never drains (a harness that re-probed lost every job
  wakeup on Windows). Dispatch on the slot the wait named, and re-probe only
  sockets, where `select()` is level-triggered.
- **A test never sleeps a deadline out; it moves the clock.** Pick by what is
  waited for:
  - a deadline: virtual time. `src/clock.c` is its own TU, so link
    `tests/fake_clock.c`, drive at boba's reported cadence and advance the fake
    by it. No test hook in production.
  - a child's own I/O: real readiness (`drive_virtual`, `source_ready`); only a
    quiet source advances the clock (`want_output`).
  - a paced canned server: `chunk_events` on the round (one `data:` event per
    pump pass), not a guessed sleep.
  - the child's cadence is the property (`run_command`'s deadline reset): real
    time and a small budget knob. A frozen clock spins the blocking `nm_connect`
    walk, so `test_wire` links the real clock.
  - `nm_chat_app_tick` steps only when a deadline is due now (`next_ms == 0`);
    `nm_chat_app_step` drives unconditionally. Ticking a deadlineless walk
    leaves the turn silently STREAMING (a Windows-only red). A drive that can
    end inside a step must flush before reading output.
  - **A spinner frame is one style period, not one tick call.** The spinner's
    frame clock is anchored to the frame that was actually painted, so a tick
    fired for another timer's deadline (or an early fd/keystroke wake) must not
    advance it. A test that wants N frames advances the fake clock N periods
    first (`nm_test_clock_advance_ms`), and a freshly started animation is due
    immediately (its `tick_ms` is 0 once, then the cadence).
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
  second SYN while one is unaccepted, Windows refuses. Listen with backlog 8,
  bound through `test_bind_loopback` (tests/test_net_helpers.h — the backlog
  is the caller's argument, never a hidden constant). A
  client-closed connection is delivered from the backlog on Linux (EOF accept)
  but dropped on macOS/BSD, so skip 0-byte connections without consuming a
  script round.
- **Windows blackholes loopback connects** (WinNAT: SYN dropped, no RST, ~20 s),
  and bind-then-close is not a guaranteed refusal. Use `127.0.0.1:1` for
  "nothing is listening".
- **A refusal is read from `SO_ERROR`,** never from a second `connect()`:
  macOS 26 answers a re-connect on a refused socket with 0/`EISCONN`
  ("connected"), where the older kernels answered the refusal itself. A probe
  that trusts the re-connect turns a refusal into a live socket, and the first
  write into the dead peer raises `SIGPIPE` — the process dies on it.
- **`SIGPIPE` is every POSIX platform's answer to a write into a closed peer,
  Linux included.** `EPIPE` is what the write RETURNS when the signal is
  ignored or blocked; the two are not alternatives. The transport keeps the
  signal from killing it (`sigpipe.h`: `SO_NOSIGPIPE` on the fd, a
  block/drain/restore guard where that option does not exist), so a test that
  proves it must set `SIG_DFL` **itself** in a forked child: the test binaries
  ignore `SIGPIPE` in `main`, and an ignored disposition survives `fork` — an
  inherited ignore makes the test pass whatever the code does.
  `tests/test_tls.c`'s closed-peer cases are the shape. Note also that a close
  over UNREAD data is an RST, which answers `ECONNRESET` and never raises the
  signal at all: the peer half must drain what it was sent before it closes,
  or the test proves nothing.
- **A send into a connection nobody has accepted is a platform race.**
  The kernel buffers it on Linux and wine, but the accept backlog is not a
  contract: a pump that accepts only after the client's first send can hang
  the client's read forever on a BSD stack (macOS CI hung the recorder's
  round trip exactly there, green twice before it bit). Take the connection
  BEFORE the send — `nm_ts_wait` with no client fd waits on the listener
  alone, the pump accepts, and the step then sends onto a live socket pair.
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
  runs `src/nevermore` through coffer's `coffer` PTY inspector CLI with
  `tools/fake-ollama`. It is part of `make check` and skips (77) when python3,
  `coffer` (or one of the commands the scenario uses) or the binary is absent,
  so a tests-only build skips it. Two traps:
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
- **boba, the CI build is release:** every workflow installs it
  `--enable-release` and never runs its `make check` (that suite is boba's own
  CI). Read a green run as nevermore against a boba whose `assert()`s are
  compiled out; the ASan tree is the dev box's (the wine pre-flight builds the
  same release boba the runners do).
- **boba, the pin:** all three workflow files (linux, macOS, Windows-MSYS2)
  carry the same boba ref, so bump them together. Push the boba commit
  **before** the nevermore commit that bumps the pin, or CI's checkout fails. A
  pin left behind a consumed API fails differently (missing symbols at link).
  Both symptoms mean "check the pin". The ref is a literal `ref:` at each
  checkout (linux.yml's two jobs repeat it; the wine script parses
  windows.yml's), so bump every occurrence together.
- **boba, the version is in the compile, not the pin.** boba bakes its version
  at its own configure time from `git describe`, so a pin names a tree and
  nothing else; a pair of freshly built trees reports old versions until both
  are re-configured (trust a rebuilt binary, not the banner). Hence **no CI
  cache for a dependency whose version a shipped binary prints** (a stale cache
  once shipped a Windows `.exe` reading `boba 0.3.0.5-7d0101b` beside a macOS
  ZIP reading `boba 0.4.0`), and both packaged binaries are asserted against
  what the checkout describes.
- **stb** is gitignored, not committed. `scripts/fetch-stb.sh` pins the upstream
  commit and `configure` fetches a missing header, so a first configure (and CI)
  needs network.
- **The mbedTLS floor is the distro package (2.28; Ubuntu 24.04 ships
  2.28.8), not 3.0** — a 3.0 gate makes auto-detect fall through to OpenSSL.
  `linux.yml` is a two-job matrix (openssl, mbedtls; explicit `--with-tls=`,
  `fail-fast: false`), so both backends are compiled and tested; both legs also
  run locally in a second build tree. mbedTLS 3.x: configure must name all three
  libs (`mbedtls.pc` hides `mbedx509`/`mbedcrypto` in `Requires.private`, so a
  bare `mbedtls` link dies "DSO missing"), and test_tls's server half guards the
  changed RNG/key-parse call signatures on `MBEDTLS_VERSION_NUMBER` (include
  `<mbedtls/version.h>`: an `#if` on an undefined macro is silently false).
- **clang's UBSan checks function-pointer calls; gcc's does not.** A callback
  cast to the wrong function type is UB that gcc, ASan and every platform
  tolerate, while clang's UBSan catches it (its `undefined` group includes
  `function`). Fix the type, never the cast.

## Pre-flight before pushing

CI runs three workflows (linux, macOS, Windows-MSYS2); a round trip costs
minutes (macOS is the longest leg), so catch failures locally.

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
  app's TUs **and every test program's objects**. It is compile-only by
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
  guarantee.
- **Wine's Schannel does real cryptography**, so `test_tls.exe` under wine is
  the Windows success-path proof, not a rehearsal: with a live peer the file's
  `NM_LIVE_TLS=1` probe completes a full handshake (measured 2026-10-17: the
  certificate/policy tail, the record buffers, the token handoff), and the
  loop-owned socket
  cases — the `WSAEventSelect` association, the refused mode flip, a held
  reply, the silent-peer budget — run in the same binary. What wine cannot
  cover is an async connect behind a live association, an SSPI token
  outstanding under a cancel, and a real chain's revocation and policy
  checks; those need hardware.
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
- **When CI fails anyway:** every workflow uploads its test logs on failure
  (`test-logs-*`: `build/tests/*.log`, `test-progress.log`, `core*`;
  `test-suite.log` has the assertion detail the console omits):
  `gh api repos/chestso/nevermore/actions/artifacts/<id>/zip`. Also
  `gh run list`, `gh run watch <id> --exit-status`,
  `gh run view <id> --log-failed`.

## Releasing

A tag is the whole trigger.
`git tag -a v0.1.0 -m "v0.1.0" && git push origin v0.1.0` runs `release.yml`
(the only workflow with a `tags:` trigger), which calls the three platform
workflows through `workflow_call` and publishes the macOS ZIP, the Windows ZIP
and the source tarball. It also checks that every artifact is stamped with the
tag's own version. Notes go on afterwards with `gh release edit`, **one line per
paragraph and per bullet** (a release body renders newlines as hard breaks).
Codeberg mirrors master and tags but has no CI and no release automation, so a
tag there never releases.

- **One tag, one run.** Platform workflows carry no `tags:` trigger; adding one
  silently doubles every release's CI.
- **The tested binary and the shipped one differ.** Tests gate the sanitized
  build; the ZIP is `--enable-release` with a release boba, built in the same
  job, so a packaging failure is red on every push, not first at release time.
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

- **A readiness event is not a level fact.** POSIX's `poll` answers "writable
  now"; WinSock's `FD_WRITE`/`FD_READ` answer _transitions_. The app must never
  leave work a step could do now (a queued request) waiting for the loop —
  `nm_request_queue` drains the connected socket for exactly this reason.
  ARCHITECTURE's queued-request note owns the measured detail.
- **The symptom shape.** A stalled round that a _timeout_ ends
  (`timed out: no response for 10000 ms`) and that a fresh dial clears
  afterwards is a wakeup problem, not a peer problem: the first round of a turn
  dials (its connect completion is a real event) and every later round rides the
  pool. Fastest attribution: `keepalive = off` in the shadow config (each round
  then dials) — if the stalls vanish, look at the pooled path before the
  provider. `NEVERMORE_DEBUG_WIRE=1` says whether the request anchor is the last
  line of the round.
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
- **macOS proves the `SIGPIPE` guard through the plain-socket case only.**
  `tests/test_tls.c`'s closed-peer TLS variant needs that file's in-process TLS
  server, which exists only for the two Linux backends, so the library's own
  record write is proved by construction there, not by the test. The plain fd
  mechanism (`sigpipe.h`) is the same guard the test does run.
- **Wine is not CI, and a pre-flight that reuses its output lies** (a leftover
  tree can hold a binary the sources no longer build). The cross trees are wiped
  per run and the roster is the harness's `TESTS`, never a listing of `.exe`
  files.
- **Windows (MinGW/MSYS2):**
  - `winsock2.h` first (before `windows.h`); `closesocket`; no SIGPIPE;
    `WSAStartup` before any socket call, raw test servers too.
  - **A blocking send into a closed peer parks** (measured under wine): the
    blocking `nm_request_send` with a 1 MiB body into a peer that has closed
    never returns, where POSIX answers `EPIPE`/`ECONNRESET`. Windows has no
    signal to die of, so the transport's closed-peer test is POSIX-only and
    says why in the skip line.
  - `FindFirstFileW` / `FindNextFileW` are UTF-16 (`MultiByteToWideChar`);
    `_mkdir`; `_getpid`; no `sh -c` (`run_command` uses `cmd.exe /d /c`).
  - `CreateProcessW`: never quote the whole command line; pass cmd flags as
    separate argv tokens.
  - **A shell's command is ONE argv element, so it needs its own quotes.**
    `$SHELL` under MSYS2 and Git Bash is `bash.exe`, and `bash -c echo hi`
    hands bash `-c echo` with `hi` as the script name — it printed nothing and
    looked like a hung run. The line stays unquoted as a whole (above); the
    command tail is quoted as one argument whenever the shell is not cmd.exe,
    which parses the rest of its own line.
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
  - **MinGW has no sanitizer runtime,** so the Windows tested build is
    uninstrumented: the memory gate is the Linux and macOS sanitized runs.
    Green on Windows says nothing about a heap error glibc happened to tolerate.
  - **The MSYS2 prefix stays POSIX (`$HOME/.local`):** `PKG_CONFIG_PATH` splits
    on `:`, so a `C:/…` entry reads as two bogus dirs and configure reports boba
    missing.
  - **Compile in the build step, run in the run step:**
    `config.status --recheck` never descends into `tests/`, so in-step test
    builds are serial minutes.
  - **A test that links `fake_clock.c` must advance it on every wait.** A loop
    that waits on a real handle hides a frozen clock (the I/O dispatches); a run
    whose last look is a POLL does not, and hangs for the whole budget.
    `shell_drive` spends each wait on the fake clock for exactly this reason.

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
- **A deadline is an INSTANT, never `now + N` recomputed when asked.** A
  "deadline" that slides with every question never comes due, so the timer that
  reads it never fires: the shell's reap poll answered `now + 100 ms` on every
  ask and a Windows run whose output had ended hung with nothing left in the
  wait set (POSIX hid it — a readable master dispatches one last time). Anchor
  the instant in the state when the poll is armed, re-arm on a look that still
  finds work, clear it on finish.
- **A closed stdin reports POLLHUP, not POLLIN:** wait on `POLLIN|POLLHUP` or
  poll forever.
- **An inherited `SIG_IGN` survives `execve`,** so a child started by a wrapper
  that backgrounds its program (`sh -c 'prog &'`, and every automake test
  through `scripts/test-watchdog.sh`) has SIGINT ignored before its own code
  runs. `proc_posix.c` resets `SIGPIPE` and `SIGINT` to `SIG_DFL` in the child
  for that reason; a signal disposition is inherited state, not a constant.
- **A `^C` written to a PTY master is only a signal while the terminal has
  ISIG on and a foreground group** — an interrupt landing between `fork` and
  the child's `TIOCSCTTY`, or inside a raw-mode program, is silently dropped.
  `kill(-pgid, SIGINT)` reaches the same processes and cannot be lost.
- **A lone Esc is a timing question, and boba owns the clock** (`v0.11.1` or
  newer): the parser parks an `ESC` byte until the next byte decides whether it
  was the Escape key or the start of a sequence, and the runtime flushes a
  parked one as `TUI_KEY_ESCAPE` after `TUI_ESC_TIMEOUT_MS` (50 ms) of quiet.
  Before that release a lone Esc produced no key at all and the NEXT key read as
  Alt+key, so a stale `libboba` still loses both ladders — the shell's stop and
  the round cancel — while every unit test passes, because those drive Esc
  through `tui_msg_key(TUI_KEY_ESCAPE, …)`. `configure` probes
  `tui_runtime_input_timeout_ms`; a green `make check` is not the proof of this
  half, a tmux `!sleep 30` + Esc is.
- **A tick ETA is a deadline, and 0 means due now** (`v0.11.2` or newer): the
  runtime re-asks `get_tick_timeout_ms` after every wake and dispatches
  `on_tick` once the named time has passed, so a timer reports the time LEFT,
  rounded up by `nm_until_ms` (0 only when the deadline has passed). An early
  wake — a key, an fd, the Esc deadline — can neither fire nor postpone it. A
  flat period would be pushed out by every wake; a 0 read as a default
  interval would delay the event and spin the loop in `poll(0)`.
- **`nm_json_at` is array-only:** on an object use `nm_json_key` +
  `nm_json_get`.
- **`sigtimedwait` needs the POSIX.1b declarations:** no `-std=` is set in this
  tree, so the compiler's default `_DEFAULT_SOURCE` exposes it. A future
  `-std=c99` (strict) would make `sigpipe.h`'s Linux guard fail to compile —
  which is at least loud, and better than losing the protection silently.
- **`nm_source_items()` sets `*n = 0` / NULL before the first fetch:** call
  `fetch_begin` first. Borrowed boba payloads (a tab-completion prefix, a
  popup's `selected_text`, textinput text) must be copied before the next call.
- **boba `tui_textinput_history_add` prepends** (index 0 is the newest; the
  persisted file is oldest-first and reads back to the same ring).
- **netrc grammar:** `default` is the one valueless key (it opens a machine-less
  entry). Every other key eats exactly one token, so treating `default` like
  them swallows the next key and loses the entry.
- **Saved artifacts open in binary mode** (`"wb"`): the Windows CRT would
  CRLF-translate and diverge from Linux. (`history.c` and `wire_recorder.c` stay
  text mode on purpose: the loader strips a trailing `\r`, and the recorder is
  line-oriented NDJSON.)

## Manual smoke without a provider

The suite never needs a network. For a manual or visual pass:

- **Fake SSE server:** `tools/fake-ollama/fake_ollama.py` with `-p ollama:local`
  (its README has the tmux recipe; `--scenario table-fence` and `--both` cover
  the two shapes).
- **`tools/wire-replay`** replays a recorded dump byte-exact. It matches on
  method + path + exact body, so a dump only matches the revision and session
  that produced it. A mismatch is a 503 plus a byte-offset diff, which is the
  diagnostic.
- **Image tiers go through portty's script system** (`-S FILE`); escape
  payloads are `\xNN` per byte, and the parser has no `\"` case — it leaks a
  stray `\`.
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
- live turns through openrouter and `opencode:go`.
