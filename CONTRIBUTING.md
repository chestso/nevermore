# Contributing to nevermore

How to build, test, and land changes. The build recipe and user-facing
setup are in [README.md](README.md)'s Build section; the module design
and the reasoning behind it are in [ARCHITECTURE.md](ARCHITECTURE.md).
This file is the contributor's checklist: the gates, the test
conventions, and the traps that are not obvious from any single file.

## Make targets

- `make check` — the full test suite, **canned loopback servers only:
  no test ever hits a real API**. The gate for every commit.
- `make check-wire` — the wire tests (dummy localhost servers, one
  scripted round per connection).
- `make format` — clang-format (src/tests C), shfmt (scripts), black
  (Python), prettier (Markdown). **Run before every commit** — a clean
  tree is part of the gate, not a periodic cleanup; re-run `make check`
  afterwards if formatting touched anything.
- `make bear` — `compile_commands.json` for clangd.

From a build dir: `make -C build format` (the source-tree Makefile
needs config.status; the target lives in the top-level `Makefile.am`).

## Dependency traps (beyond README's Build section)

- **boba, stale install — both ways**: boba must be re-`make install`ed
  after any boba edit, or nevermore links yesterday's symbols; a new
  boba API that nevermore uses must be in the installed lib before
  nevermore's build.
- **boba, the pin and the push order**: all three workflow files
  (linux, macOS, Windows-MSYS2) carry the same boba ref — bump them
  together, and push the boba commit BEFORE the nevermore commit that
  bumps the pin, or CI's checkout of that ref fails. A pin left behind
  a consumed API fails differently (missing boba symbols at link, not a
  checkout error) — both symptoms mean "check the pin".
- **boba, the version pair lies after a bump**: `nevermore --version`
  prints the boba version baked at nevermore's own configure time, and
  boba's `.pc` is written at its own configure time — so a freshly
  built pair still reports the old versions until both trees are
  re-configured. Trust a rebuilt binary, not the banner.
- **stb** (image transcode) is gitignored, not committed:
  `scripts/fetch-stb.sh` pins the upstream commit and `configure`
  fetches a missing header — a first configure (and CI) needs network.

## Standards (apply to every change)

- **Warnings: fix the smell, accept the noise.** A warning that names a
  real hole is fixed at its root — a composing buffer sized for the
  worst case its type can produce, never for the values seen in
  practice. A genuine false positive and an OS-native API's deprecation
  notice (Secure Transport, by design) are noise we accept: no pragma,
  no blanket attribute, no reshaping a helper into a form the analyzer
  loses track of — a suppression outlives the reason it was added.
  Nothing passes `-Werror`; the optimizer-only warnings show up in the
  local `-O3` release build, which is where they are read.
- **TDD gate**: tests exist before/with the code; a green `make check`
  plus a format-clean tree is the gate for every commit.
- **Delete dead code.** Anything unreferenced by the shipped code is
  deleted, and that includes code referenced only by tests: a test hook
  is not a consumer. If a test needs a seam, test the public behavior
  that uses it, or move the seam into the test. Pre-alpha means no
  compat burden — deleting is free.
- **Never point at a file that does not ship** — not in a commit
  message, not in the documentation, not in a code comment: a path that
  is not in the commit dangles for every other checkout. State the rule
  as the rule, and leave the pointer in whatever local note owns it.

## Writing tests

`tests/test_helpers.h` (RUN_TEST/TEST_SUMMARY/ASSERT__): ASSERT__ use a
bare `return;` — usable only inside functions returning void; check
setup logic in `main` with plain if/return 1. Skip code 77 marks a
missing prerequisite.

- **Never hit real APIs in `make check`** — canned SSE servers on
  loopback, one scripted round per connection.
- **Stall loops must drain-recv**: a closed peer is READABLE (EOF) — a
  select-only loop hot-spins and hangs the process. Poll with a timeout
  - `recv` → `n <= 0` = peer gone.
- **Poll the collector, not the socket** (accept/send race): loop until
  the app's collector has bytes (`nm_chat_app_tail_len`) or state
  changes, bounded — a head-only read is legitimate PENDING.
- **Poll a file's CONTENT, not its existence** (`> file` truncates
  before the write): wait until it PARSES (`wait_for_pid_file`) — the
  signal that means "the data is here" is the data, not a proxy.
- **Bound drive loops by WALL CLOCK, not iteration count** when waiting
  on time: a ready-fd pass costs microseconds, so 400 iterations can
  finish in ~20 ms and exit before a 250 ms budget elapses. An
  iteration bound is safe only when each pass guarantees a fixed sleep.
- **Never test a REMOVAL**: no absent-symbol assertions — a negative
  test pins a deletion the diff already states and outlives its reason.
  Test what the code DOES.
- NUL-terminate before `ASSERT_STR_EQ` (Wine's stack garbage differs
  from glibc's).
- **Pin the cwd** for agent-building tests (`test_chdir_to_scratch`):
  agent construction reads an `AGENTS.md` from the working directory
  (the product's discovery feature), so a test run inside a source tree
  that carries one picks it up — its ~20 KiB shifts the captured wire
  request, and a checkout without it behaves differently.
- **Pin the provider catalog offline** (`test_pin_offline_catalog` +
  `TEST_OFFLINE_CATALOG_PIN_CHECK`): a catalog lookup with no endpoint
  override resolves the provider's DEFAULT base — a real request whose
  fetched list REPLACES the static catalog the test asserts against. An
  explicit `base_url` wins whatever the gate says.
- **Pin env-derived terminal hints** (`pin_color_env`, `harness_new` /
  `harness_new_rgb`): truecolor is read when the probe starts (the
  harness's first flush), so the test process's own COLORTERM would
  otherwise decide the frame bytes. Same family as the catalog pin: a
  capability the test does not control is a coin flip.
- **A test never sleeps a DEADLINE out — it moves the clock.** Pick by
  what is being waited FOR:
  - a deadline → virtual time: `src/clock.c` is a TU so a test links
    `tests/fake_clock.c` instead; drive at boba's reported cadence and
    advance the fake by that cadence. No test hook in production — the
    fake IS the definition.
  - a child's own I/O → real readiness (`drive_virtual`,
    `source_ready`); only a QUIET source advances the clock
    (`want_output`).
  - a paced canned server → rendezvous (`server_paced_init` /
    `server_ack`), not a guessed sleep; a server that must stay open
    while the client reads drain-recvs the peer.
  - the CHILD's cadence is the property (run_command's deadline-reset)
    → keep real time and shrink the budget knob; a frozen clock spins
    the blocking `nm_connect` walk, so test_wire links the real clock
    for it.
  - `nm_chat_app_tick` is NOT a drive: it steps only when a deadline is
    due NOW; `nm_chat_app_step` drives unconditionally. Ticking a
    deadlineless walk leaves the turn STREAMING silently (a
    Windows-only red). A drive that can end inside a step must flush
    before reading output.
- **The watchdog cap is a HANG DETECTOR** (`NM_TEST_TIMEOUT`, 60 s):
  MSYS2 slowness is per-test environment overhead, not the waits — keep
  the margin ≥ 5x the worst legitimate runtime.
- **Canned servers must DRAIN the request** (an unread request + close
  = RST, which on macOS/BSD can discard the client's unread reply)
  **and async-gap assertions must MAKE the gap**: a reply already in
  the socket makes the fetch report READY, never FETCHING — hold it
  ~200 ms. An ASan client against an uninstrumented server thread
  inverts the race.
- **Timing margins generous in BOTH directions**: an inactivity budget
  sits BELOW the traffic it must outlive and ~10x ABOVE the cadence.
  Restore store keys BEFORE asserting — a failed assert leaks the value
  into later tests.
- **Accept loops bounded**: unbounded `accept()` blocks `pthread_join`.
- **The listen backlog is a platform trap**: Linux rounds it up,
  macOS/BSD silently DROP a second SYN while one is unaccepted, Windows
  refuses — listen with backlog 8. A client-closed connection is
  delivered from the backlog on Linux (EOF accept) but dropped on
  macOS/BSD: skip 0-byte connections without consuming a script round.
- **Windows blackholes loopback connects** (WinNAT ports: SYN dropped,
  no RST, ~20 s), and `bind`-then-close is not a guaranteed refusal —
  use `127.0.0.1:1` for "nothing is listening".
- `select()`: pass NULL for unset directions, never an empty-but-nonnull
  fd_set (Windows).
- **PENDING is part of the contract**: continue on OK/PENDING, break
  only on a final verdict.
- **Catalog caches are process-global** (a failed fetch poisons them):
  fetch tests run before fallback tests.
- **A bounded fixture table fails SILENTLY** when full (a NULL path):
  size for the whole binary, `ASSERT_NOT_NULL(path)` at the make site.
- **Hand-counted string lengths are a bug**: pass `strlen()` /
  `sizeof`, never literals.
- **`make -j` does NOT rebuild test binaries** — `make -C build/tests
test_x`, or `make check`. Stale-binary tell: a FAIL line number
  pointing at old text.
- **automake harness wiring**: suffix-less test binaries go through
  `LOG_COMPILER`, NOT `TEST_LOG_COMPILER` (`.test` scripts only) —
  wiring the latter does nothing.

## Local pre-flight (before pushing)

CI runs three workflows (linux, macOS, Windows-MSYS2). Catch failures
locally — minutes here vs 4-minute round-trips.

- **Linux** is the dev box: a clean-tree `make check` is the gate; build
  from scratch when touching configure/Makefile.am (stale build dirs
  mask missing sources). **The macOS job builds only `tests/`** (`make
-j -C tests check-programs` from a FRESH dir — src is never built
  there), which is why tests/Makefile.am carries the recursive rule for
  the generated `nevermore_version.h`. Reproduce that step after
  touching anything a test pulls out of src's build dir — zig-macos
  compiles `src/*.c` and misses test-binary link failures.
- **macOS**: `scripts/zig-macos-check.sh` (compile-only, every
  `src/*.c` for aarch64-macos). Its hand-kept `-I` list must MIRROR
  configure's `nevermore_CPPFLAGS` — a new include dir in configure.ac
  belongs in the script too.
- **Windows**: `scripts/w32-wine-check.sh` (MinGW cross-compile, run
  under Wine, compile + version-grep `src/nevermore.rc`). Wine is not
  the MSYS2 UCRT64 runtime — pre-flight, not guarantee. boba-linking
  binaries SKIP there (no MinGW boba locally); MSYS2 CI builds boba and
  covers them.
- Run the scripts **from the repo ROOT** (0 results from a build dir);
  never zig cc `-fsyntax-only` (`error: FileNotFound`) — use `-c -o
/tmp/x.o`.
- **One pass, read the logs**: evidence lands in
  `build/tests-wine/<name>.build.log` + `<name>.log` and
  `/tmp/nmm_<base>.build.log` — counts and failed TUs are read from
  disk, never bought with a second run; re-run only after a tree change.
- **boba's release `build/` and `build-asan/` are two installs**:
  nevermore links whichever is INSTALLED — put the asan lib in for
  nevermore's ASan suite, restore the release lib after. boba's real
  gate is `build-asan/` (a release tree compiles `assert()` out, so its
  `make check` runs zero assertions).
- **`make distcheck` needs a FRESH `version` file**: rerun
  `./autogen.sh` after committing, or the autoreconf-time tarball name
  disagrees with the configure-time one and distcleancheck fails on the
  leftover nested tarball (remove with `chmod -R u+w`). Repo-wide
  distcleancheck reds are real (`EXTRA_DIST`/`CLEANFILES`); run
  distcheck before a phase-closing push.

**When CI fails anyway**: the mac/windows workflows upload
`build/tests/*.log` as artifacts (test-suite.log has the assertion/ASan
detail the console omits) —
`gh api repos/chestso/nevermore/actions/artifacts/<id>/zip`. `gh run
list` / `gh run watch <id> --exit-status` / `gh run view <id>
--log-failed`.

## Cross-platform / CI lessons

- **Never hand-edit Makefile.in / trust a stale build/**: CI checkouts
  are fresh. After `.am` changes: `autoreconf -i` in the source tree
  AND re-run `../configure` in build/.
- **automake silently drops `../src/$(VAR)` from `_SOURCES`** when the
  prefix is glued to a variable (no object, no error): the variable
  itself must carry the full path AND the reference must be bare
  (`$(TEST_TLS_SOURCE)` last).
- **Build JSON with `nm_json_set` + `nm_json_dump`, never raw snprintf**
  when a value can hold `\` or `"` (Windows paths!): unescaped
  backslashes → "bad escape" → the whole object is rejected. Keys are
  escaped at dump time too. Windows fixture paths use forward slashes.
- **json.c arena**: the root NmJson node lives in the arena's last
  chunk; `nm_json_free` must not write through the Arena afterwards
  (`a->head = NULL` was a use-after-free on every free — glibc tolerates
  it, macOS ASan aborts).
- **Windows (MinGW/MSYS2)**:
  - `winsock2.h` FIRST (before windows.h); `closesocket`; no SIGPIPE;
    WSAStartup before any socket call — raw test servers too.
  - `FindFirstFileW`/`FindNextFileW` are UTF-16
    (MultiByteToWideChar); `_mkdir`; `_getpid`; no `sh -c` — run_command
    uses `cmd.exe /d /c`.
  - CreateProcessW: never quote the whole command line; pass cmd flags
    as separate argv tokens.
  - No `/tmp` — tests use `C:/Users/Public/...`.
  - **`/nonexistent` is not a portable refusal** (an MSYS2 process can
    create it): block a directory by putting a regular FILE where it
    must go — assert the shape, not the permission model.
  - **A `src/` header shadowing a platform header breaks `<...>`
    includes** (`src/process.h` ate MinGW's `<process.h>`): rename to a
    different WORD (`proc.*`, the module's own vocabulary), never by
    include order and never with a prefix — see `ARCHITECTURE.md`
    §Naming (a file name carries no `nm_`; the Windows token is `w32`,
    never `win`). Check: `git grep -n '#include <'` against
    `ls src/*.h`.
  - **The version resource** (`src/nevermore.rc` + `icons/nevermore.ico`)
    is compiled by src/Makefile.am's windres rule: the .rc includes the
    build-generated `nevermore_version.h` (the single version source)
    and derives VERSIONINFO's quad from it — never hand-write it.
    `tests/test_version.c` pins the fields; the wine pre-flight
    compiles the .rc and greps the version out.
  - MSYS2 CI runs ASan; glibc tolerates some of what it rejects —
    "works on Linux" is not clean.
  - **The MSYS2 prefix stays POSIX (`$HOME/.local`)**:
    `PKG_CONFIG_PATH` splits on `:`, so a `C:/…` entry reads as two
    bogus dirs and configure reports boba missing. `actions/cache`
    runs on the HOST: its `path:` must name the host-side mapping while
    msys2 steps keep the POSIX prefix.
  - **Compile in the build step, run in the run step**: `config.status
--recheck` never descends into `tests/`, so in-step test builds are
    serial minutes. Cache boba keyed on the pinned ref, and pin that
    ref in ONE `env:` block per file so checkout and cache key cannot
    drift.
- **macOS**: Secure Transport deprecation warnings are expected (by
  design) — don't "fix" by switching frameworks mid-phase.
- **Wine ≠ CI** (still catches ~90% in seconds), and a pre-flight that
  reuses its output dir LIES (a failed compile leaves the previous
  binary "passing") — delete the target first.
- **Never pipe a compiler into an early-exiting reader** (`| head -5`):
  SIGPIPE kills the compiler before the link, so a "BUILD FAIL" with a
  warning-only log is this trap. The pre-flight scripts redirect to a
  `.build.log` and filter the file.
- **The mbedTLS floor is the DISTRO package (2.28), not 3.0** — Ubuntu
  24.04 ships 2.28.8, so a 3.0 gate makes auto-detect fall through to
  OpenSSL. `linux.yml` is a 2-job matrix (openssl/mbedtls, explicit
  `--with-tls=`, `fail-fast: false`); test_tls's loopback server is
  backend-neutral, so the mbedTLS job tests the handshake. **3.x is a
  three-way split**: `mbedtls.pc` hides `mbedx509`/`mbedcrypto` in
  `Requires.private`, so configure must name all three (`mbedtls >=
2.28 mbedx509 mbedcrypto`) or the link dies "DSO missing from command
  line". **RNG wiring**: `mbedtls_ssl_conf_rng` needs a seeded
  `mbedtls_ctr_drbg_context` passed third, not NULL — the first
  handshake dereferences it.
- **clang's UBSan checks function-pointer calls; gcc's does not**: a
  callback cast to the wrong function type is UB that gcc+UBSan, ASan
  and every platform tolerate; clang 19's `-fsanitize=function` catches
  it. Fix the type, never the cast — every `(NmXxxFn)` cast is this bug
  pending.

## C gotchas

- **C hex escapes are greedy**: `"\x18ftyp"` = `\x18f` + `typ` (out of
  range, wraps, eats a character) — split the literal, and use
  `sizeof(lit)-1` for lengths (magics can hold NULs).
- **`snprintf` truncates silently** (and raises `-Wformat-truncation`):
  for token buffers, `memcpy` + explicit NUL and skip what does not fit
  — a truncated key or secret is never useful.
- **va_list into a variadic function is UB** — route through a
  `vsnprintf` helper.
- **A closed stdin reports POLLHUP, not POLLIN** — a stdin wait loop
  must check `POLLIN|POLLHUP` or it polls forever.
- **`nm_json_at` is ARRAY-only** — on an OBJECT use `nm_json_key` +
  `nm_json_get`.
- **`nm_source_items()` sets `*n = 0`/NULL before the first fetch** —
  `fetch_begin` first. Borrowed boba payloads (a tab-completion prefix,
  a popup's `selected_text`, textinput text) must be copied before the
  next call.
- **boba `history_add` prepends** (newest-first array); the file
  round-trip is identity (oldest-first file order).
- **netrc grammar: `default` is the one valueless key** (it opens a
  machine-less entry); every other key eats exactly one token —
  treating `default` like the others swallows the next key and loses
  the whole entry.
- **Test-data chunk sizes**: verify with `printf '...\n\n' | wc -c`
  (format-string form — `%s` leaves `\n` literal).
- **A stale source file in the build dir shadows the VPATH source** (a
  0-byte `build/tests/foo.c` → "undefined reference to `main`"): `ls
build/tests/*.c`, delete the junk, rebuild.
- **Saved artifacts open in BINARY mode** (`"wb"`): the Windows CRT
  would CRLF-translate and byte-diverge from Linux. (`history.c` and
  `wire_recorder.c` stay text mode on purpose: the loader strips a
  trailing `\r`; the recorder is line-oriented NDJSON.)

## Manual smoke without a provider

The test suite never needs a network; these are for the manual/visual
pass:

- **Fake SSE server**: `-p ollama:local` +
  `tools/fake-ollama/fake_ollama.py` (stdlib; `--scenario
table-fence`). Bind BOTH `127.0.0.1` and `::1` (`--both`) —
  `localhost` can resolve to `::1` first. Drive the TUI with tmux
  `send-keys` + `capture-pane` (recipe in the tool's README).
- **`tools/wire-replay`** replays a recorded dump byte-exact (matched
  on method + path + exact body, so a dump only matches the revision
  and session that produced it; a mismatch is 503 + a byte-offset diff
  — the diagnostic).
- **Image tiers: portty's script system** — `portty -S FILE -- sleep 5`
  runs one command per frame; `emit` feeds the VT engine directly,
  `dump-sixel` reads the image state back, `screendump`/`renderdump`
  write PNGs. Escape payloads `\xNN` per byte — the parser has no `\"`
  case, so `\"` leaks a stray `\` into the data.
- **Let the app SETTLE first** (~2 s before the first keystroke): the
  terminal-profile round trip is still in flight, so
  `nm_image_supported` says unresolved and the attach declines — not a
  bug.
- **Portty's `sendln` is CRLF — the wrong driver for a raw-mode TUI**
  (the CR submits, the LF lands in the NEXT input as a real newline).
  Use `send` + a lone `\r`, or tmux `send-keys "text" Enter`.

## Submitting changes

`make format` + a green `make check` is the gate for every commit; run
`make distcheck` before a phase-closing push.
