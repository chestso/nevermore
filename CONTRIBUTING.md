# Contributing to nevermore

How to build, test, and land changes. The build recipe and user-facing
setup are in [README.md](README.md)'s Build section; the module design
and the reasoning behind it are in [ARCHITECTURE.md](ARCHITECTURE.md).
This file is the contributor's checklist: the gates, the test
conventions, and the traps that are not obvious from any single file.

## Make targets

- `make check` — the full test suite, **canned loopback servers only:
  no test ever hits a real API**. The gate for every commit.
- `make catalogs` — **regenerate the model catalogs** (one generator
  for every provider: `tools/generate-catalogs.py`). It resolves each
  provider's catalog with FULL COVERAGE (a model with no known
  label/window/capabilities is not offered) and writes the generated
  tables (`src/catalog_data.h`) the app falls back to until the
  runtime resolution lands. **A release-time step**, not a build-time
  one: the shipped binary must not depend on a third-party wire
  document, and the suite never hits the network. Run it before a
  release (the catalogs are the fallback a fresh install starts from)
  and commit the result. `make catalogs-check` compares instead of
  writing — the gate that says the committed catalogs are current.
- `make check-wire` — the wire tests (dummy localhost servers, one
  scripted round per connection).
- `make format` — clang-format (src/tests C), shfmt (scripts), black
  (Python), prettier (Markdown). **Run before every commit** — a clean
  tree is part of the gate, not a periodic cleanup; re-run `make check`
  afterwards if formatting touched anything.
- `make format-check` — the same four formatters in dry-run mode, plus
  `scripts/check-md-quotes.py` for the one construct prettier cannot
  round-trip: an inline code span broken across lines inside a
  blockquote, where prettier drops the `>` (CommonMark's lazy
  continuation then hides the loss until a later edit lets the text
  fall out of the quote). A gate, not a cleanup.
- `make bear` — `compile_commands.json` for clangd.
- `make regen-icon` — re-render `icons/nevermore.ico` (the six sizes,
  32bpp) from `icons/nevermore.svg`: `rsvg-convert` per size, then
  `icotool` to combine, the same recipe portty documents. The SVG is
  the source of truth and the committed `.ico` is the artifact —
  `src/nevermore.rc` compiles it, so after changing the SVG run this
  and commit the result. The per-size PNGs stay in the build tree.
- `make dist-portable` — macOS/Windows only: a self-contained ZIP (the
  binary, the runtime libraries its OS does not ship, the docs, a
  README-PORTABLE.txt). Run it from a build dir configured
  `--enable-release` — the default build is the sanitizer build, and a
  binary that needs a sanitizer runtime is not portable. **The
  dependency has to be release-built too**: boba's default on macOS and
  Linux is ASan+UBSan, and an unsanitized nevermore cannot resolve the
  sanitizer calls its static archive still holds — the link dies with
  `ld: symbol(s) not found`. The macOS job builds a second boba,
  `--enable-release`, into a prefix of its own and links the ZIP
  against that. Windows needs no second build: MinGW has no sanitizer
  runtime, so boba's default there is already unsanitized.
  `DIST_TARGET` packs a tree built by another host
  (`DIST_TARGET=windows` over the wine pre-flight's cross build).
  GNU/Linux is deliberately not packaged: `make dist` is the Linux
  artifact — a Linux binary links a TLS library the OS may not have
  (OpenSSL here), which a ZIP would then have to carry.

From a build dir: `make -C build format` (the source-tree Makefile
needs config.status; the target lives in the top-level `Makefile.am`).

## Releasing

A tag is the whole trigger. `git tag -a v0.1.0 -m "v0.1.0" && git push
origin v0.1.0` runs `release.yml` — the only workflow with a `tags:`
trigger — which calls the three platform workflows through
`workflow_call` and publishes what they built: the macOS ZIP, the
Windows ZIP, the source tarball. The release job also checks that the
tarball's own `version` stamp agrees with the tag, the trap the first
coffer release hit. Notes go on afterwards with `gh release edit` — write
them **one line per paragraph and per bullet**: a release body renders
newlines as HARD breaks, so pre-wrapped prose comes out broken after
every line.

- **One tag, one run**: the platform workflows carry no `tags:` trigger;
  a tag push must not double-run them through both the push and the
  call. Adding a `tags:` trigger back to a platform workflow silently
  doubles every release's CI.
- **The tested binary and the shipped one are different builds** — the
  tests gate the sanitized build, the ZIP carries `--enable-release`,
  and the macOS ZIP links a second, release-built boba (a sanitized
  libboba.a leaves `__ubsan_*` undefined at that link). All of it is
  made in the same job, so a packaging failure is red on every push,
  not first at release time.
- **Green master before the tag**: a red required job skips the release
  job and no release appears at all; move the tag to the fixed commit
  only after master runs green.
- **Windows ZIP contents are not asserted**, they are PROVEN: the verify
  step extracts the ZIP and runs `nevermore.exe --version` with
  `System32` as the only `PATH` — if anything else had to come from
  MSYS2, that run fails. The macOS step makes the same proof through
  `otool` (every load command must be `/usr/lib` or `/System/Library`)
  plus `codesign --verify`.
- **A tracked file the build regenerates makes the tag dirty, and the
  release is refused**: the version string is `git describe` plus a
  `-dirty` when a tracked file differs, and the release job accepts
  only names and `version` files that match the tag. The first `v0.1.0`
  run died exactly there — `build-aux/ar-lib` was tracked, `autoreconf
-fi` (autogen.sh) overwrites it with the HOST automake's copy, and the
  runner's is older than the one the tree carried, so the tarball came
  out `nevermore-0.1.0-dirty.tar.gz`. A generated aux file stays
  untracked (`.gitignore` covers `ar-lib` with `compile`, `depcomp`,
  `install-sh`, `missing`, `test-driver`, …), which is what every
  sibling repo does.

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
- **boba, the version is not in the pin — it is in the compile**: boba
  bakes its version string in at its OWN configure time, from the tags
  `git describe --tags` sees in that checkout, so a pin names the tree
  and nothing else — the same commit describes as `v0.3.0-5-g…` before
  a tag lands on it and as `v0.4.0` after. The Windows job used to skip
  that compile on a cache hit keyed on the pin; when boba tagged the
  commit the pin already named, the ref did not change, the key did not
  move, and the run restored an install compiled before the tag existed
  — v0.2.0 shipped a Windows `.exe` reading `boba 0.3.0.5-7d0101b`
  beside a macOS ZIP from the same run reading `boba 0.4.0`. So: **no
  CI cache for a dependency whose version a shipped binary prints**, and
  **both packaged binaries are asserted against what the checkout
  describes as** (the artifact's own `(boba …)` line — a restored
  install, a moved tag or a dirty dependency tree fails the build
  instead of shipping). macOS and GNU/Linux never had the trap: they
  compile every run (6–15 s).
- **stb** (image transcode) is gitignored, not committed:
  `scripts/fetch-stb.sh` pins the upstream commit and `configure`
  fetches a missing header — a first configure (and CI) needs network.

## Standards (apply to every change)

- **Names**: a file name is the module word, unprefixed (`conf.h`,
  `proc_w32.c`), and our Windows token is `w32`, never `win`
  (`ARCHITECTURE.md` §Naming spells both out). The guards:
  `ls src/nm_*` prints nothing, and a new header is checked against
  `ls src/*.h` and the platform's include dir before it is added.
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
- **A pid-keyed scratch dir outlives the run** (`test_scratch_dir`,
  the PID in the name, never wiped): a run handed a PID an earlier run
  used inherits that run's FILES, and a layer that MERGES what it loads
  — the config shadow is the one that bit — turns an exact-content
  assert into a failure on a file this test never wrote (under Wine,
  where the PID repeats, two runs a day apart land in the SAME
  directory). Remove a fixture before writing it, the way `pin_paths` /
  `pin_cfg_paths` remove both config files, and give every new fixture
  the same treatment.
- **Bound drive loops by WALL CLOCK, not iteration count** when waiting
  on time: a ready-fd pass costs microseconds, so 400 iterations can
  finish in ~20 ms and exit before a 250 ms budget elapses. An
  iteration bound is safe only when each pass guarantees a fixed sleep.
- **Never test a REMOVAL**: no absent-symbol assertions — a negative
  test pins a deletion the diff already states and outlives its reason.
  Test what the code DOES.
- NUL-terminate before `ASSERT_STR_EQ` (Wine's stack garbage differs
  from glibc's).
- **Pin every discovery surface** for agent-building tests
  (`test_chdir_to_scratch` + `nm_skills_set_global_dir` at a path that
  is never created): agent construction reads an `AGENTS.md` from the
  working directory AND the user-tier skills directory, both product
  discovery features. A source tree that carries one (or a dev box with
  a real `~/.agents/skills/`) adds a `<skills>` menu / ~20 KiB to the
  captured wire request, and a checkout without them behaves
  differently. A third discovery surface owes the same pin.
- **Pin the provider catalog offline** (`test_pin_offline_catalog` +
  `TEST_OFFLINE_CATALOG_PIN_CHECK`): a catalog lookup with no endpoint
  override resolves the provider's DEFAULT base — a real request whose
  fetched list REPLACES the static catalog the test asserts against. An
  explicit `base_url` wins whatever the gate says. The same rule
  governs the OpenCode tiers' catalog DOCUMENT
  (`NEVERMORE_OPENCODE_CATALOG_URL`): the override is an explicit
  request and is never gated, the DEFAULT url is live-catalog traffic
  and the gate governs it — so a test that wants the document points
  it at its own server, and a test that wants no document at all
  leaves the override unset.
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
  - **A drive that steps on EVERY iteration hides a stall**: the tool
    phase has no deadline, so a step is the only thing that can move
    it — and the real loop steps on an fd event or a due deadline,
    never on the passage of time. When the property is that a turn
    MAKES PROGRESS, drive it the way the loop does (wait on the app's
    wait target → the ready branch; the tick on a timeout):
    `harness_drive_events`.
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
- **The E2E harness drives the BINARY**: `tests/e2e_portty/` (see its
  README) runs the built `src/nevermore` through coffer's `cfr-debug`
  with `tools/fake-ollama`. It is part of `make check` and SKIPs (77)
  when python3, cfr-debug or the built binary is absent — so a
  tests-only build (`make -C tests check-programs`) skips it, and CI
  builds coffer (linux, macOS) or skips (Windows). Two traps that cost
  a CI round trip each:
  - **A helper process a test backgrounds must not inherit the
    harness's stdin**: a job that reads it can be STOPPED by SIGTTIN —
    silent, and `kill -0` still says "alive", so it looks like a slow
    start. Give it `</dev/null` (the fake-ollama launch does).
  - **A canned python server must skip `socket.getfqdn`**:
    `http.server.HTTPServer.server_bind` does a reverse lookup that
    stalls for tens of seconds where the resolver has no rDNS answer
    (GitHub's macOS runners). `fake_ollama.py`'s `NoFqdnServer` exists
    for exactly that.

## Local pre-flight (before pushing)

CI runs three workflows (linux, macOS, Windows-MSYS2). Catch failures
locally — minutes here vs 4-minute round-trips.

- **Linux** is the dev box: a clean-tree `make check` is the gate; build
  from scratch when touching configure/Makefile.am (stale build dirs
  mask missing sources). **The macOS job builds `tests/` and `src/`**
  separately (`make -j -C tests check-programs` + `make -j -C src`
  from a FRESH dir), which is why tests/Makefile.am carries the
  recursive rule for the generated `nevermore_version.h` and why the
  E2E harness needs src built. Reproduce that step after touching
  anything a test pulls out of src's build dir.
- **Both pre-flights are autotools trees now**: `configure` in a dir of
  their own, `make` to compile, `make check` to run under the runner
  configure picked. The scripts provision the ENVIRONMENT (the zig SDK
  cache, the cross boba, `PKG_CONFIG*`/`WINEPATH`) and read the logs —
  **no source list, include path, link library or test roster lives in
  either script**, so a new TU, a new test or a new backend reaches both
  through `Makefile.am` alone. That is the property to keep: a
  `scripts/*.sh` line naming `src/*.c`, `tests/test_*.c` or a `-I` path
  is the drift this replaced (the old macOS check needed its `-I` list
  kept in sync with `nevermore_CPPFLAGS` by hand, and the old Windows
  script's roster was three tests stale).
- **macOS**: `scripts/zig-macos-check.sh` — the cross tree in
  `build-macos-zig/` (`--host=aarch64-apple-darwin`, `CC="zig cc
-target aarch64-macos"`, SDK shim staged into
  `~/.cache/nevermore-zig-macos-sdk`). `make compile-check` compiles the
  app's TUs **and every test program's objects** (~280: each test
  binary's own copies of the src TUs included) — the compile half of
  what the macOS job does first. It is compile-only BY DESIGN: a macOS
  link needs a real SDK, and zig's bundled darwin libc ships no
  `Security.framework` stub while the staged headers are not a framework.
- **Windows**: `scripts/w32-wine-check.sh` — the cross tree in
  `build-w32/` (`configure --host=x86_64-w64-mingw32`, a Windows boba
  cross-built into `build-w32/boba-prefix`, `--with-wine` wiring the
  test binaries through wine, so the roster, the per-test watchdog and
  the `.log`s are the native harness's). Wine is not the MSYS2 UCRT64
  runtime — pre-flight, not guarantee. The boba source tree is
  `$BOBA_SRC`, else `./boba`, else `../boba` (an existing cross install
  in `$BOBA_PREFIX` also works); with neither the run exits 77 —
  configure requires boba for the target, so it is ONE build now, not a
  per-test list that could skip. The run names the boba SHA it verified
  and notes when that is not the pinned one, so a green pre-flight is
  never mistaken for the pinned pair. `--no-zip` skips its second half:
  a `--enable-release` tree in `build-w32-rel/`, `make dist-portable
DIST_TARGET=windows`, then the EXTRACTED `.exe` run under wine with
  the MinGW bin dir off `PATH` — the self-containment proof otherwise
  done by hand at release time.
- Traps in the cross trees, each of which cost a run to find:
  - **The runner is `--with-wine`/`TEST_RUNNER`, never `PATH`**:
    autoconf leaves `cross_compiling` at `maybe` here (nothing ever
    settles it), so the Windows-cross detection is the build/host OS
    pair (`CROSS_WINDOWS` in configure.ac), and the runner rides
    `LOG_COMPILER` BEHIND the watchdog — the watchdog stays the process
    that times and kills the run (`wine test_x.exe`).
  - **`PKG_CONFIG=pkg-config` in a cross tree**: with `--host`,
    `PKG_PROG_PKG_CONFIG` finds the cross-prefixed tool first, and
    Fedora's personality for it prefixes every `-I`/`-L` with the
    toolchain sysroot, which mangles a boba installed outside that
    sysroot (`-I/usr/…/sys-root/mingw/tmp/…`) — the build then dies on
    `boba/components/textinput.h`. `PKG_CONFIG_LIBDIR` (which REPLACES
    the search path, unlike `PKG_CONFIG_PATH`) names the cross prefixes.
  - **`WINEPATH` for the runtime DLL**: a cross gcc that links
    `libwinpthread-1.dll` dynamically (Fedora's does) leaves every
    `.exe` dying with wine exit 53 (DLL not found) without it; export
    `WINEDEBUG=-all` too, or wine's fixme chatter lands in every test's
    `.log`.
  - **`compile-check` compiles and never links**: src's depends on
    `$(BUILT_SOURCES)` as well (automake hangs the generated version
    header off `all`, which that target does not go through); tests'
    gathers the per-program object lists by re-entering make with
    `CCLD=true` (a no-op linker), because the POSIX-make construction
    that would do it directly — `$(foreach p,$(check_PROGRAMS),
$($(p)_OBJECTS))` — is rejected by this tree's `-Wall -Werror`.
  - **Under `-j`, zig's diagnostics do not always end in a newline**, so
    one log line can hold a warning AND the next `  CC` banner: count
    compiled objects with a loose pattern, never `^  CC`.
  - **`grep -c` exits 1 when it counts ZERO** — inside a `set -e`
    script that is a silent early exit on the GREEN path (a suite with
    no skipped tests). Read the harness's own summary with `sed`.
- Run the scripts **from the repo ROOT** (0 results from a build dir);
  never zig cc `-fsyntax-only` (`error: FileNotFound`) — use `-c -o
/tmp/x.o`.
- **One pass, read the logs**: evidence lands in
  `build-w32/{boba-*,configure,build,check}.log` + `build-w32/tests/*.log`
  and `build-macos-zig/{configure,src-build,tests-build}.log` — counts
  and failed TUs are read from disk, never bought with a second run;
  re-run only after a tree change. Both cross trees are WIPED at the
  start of a run (every TU compiled by that run), and the harness owns
  the roster, so the old "delete the target first" guard is gone with
  the hand-kept lists.
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
  reuses its output artifacts lies (a tree left over from an earlier
  checkout can hold a binary the current sources no longer build): the
  cross trees are wiped per run, and the roster a run executes is the
  harness's `TESTS`, never a listing of whatever `.exe` files are lying
  around.
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
  handshake dereferences it. **3.x also moved `mbedtls_pk_parse_keyfile`'s
  RNG into the signature** (2.28's 3-arg call is 5-arg there, `f_rng` +
  `p_rng`), which only the test file notices: its TLS _server_ half parses
  the committed key, so guard that one call on `MBEDTLS_VERSION_NUMBER`
  (include `<mbedtls/version.h>` — an `#if` on an undefined macro is
  silently false, which would compile the 2.28 spelling on 3.x) and pass
  the drbg the same clause already seeded. **Both legs are runnable
  locally**: with `mbedtls-devel` installed, `configure`'s `auto` prefers
  mbedTLS, so a second build tree is the explicit spelling
  (`../configure --with-tls=mbedtls`, then `make && make check` — the
  suite builds its own test programs) and the OpenSSL leg needs
  `--with-tls=openssl`.
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
- **Terminate a capture buffer where it is ALLOCATED, not only after a
  read**: a child that prints nothing leaves the length at 0, so a buffer
  that is only terminated once bytes arrive hands the report
  uninitialized heap — the command's "output" is then whatever the
  allocator returned (the async capture path had this; the sync one
  terminated its buffer).
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

### The standing matrix (re-run after any transcript-region change)

Bytes through tmux `capture-pane -e`, pixels through portty's
`screendump`, against the vehicles above — one pass over the paths the
suite cannot see:

- reasoning / table / fence rendering, a blank line between paragraphs,
  and the one-line-kind boundaries (prose after a heading or a lone
  image line);
- a resize mid-stream, multiline paste, Ctrl+C mid-stream then
  resubmit;
- a `/provider` switch plus the next turn, the history round-trip
  across restarts, 40-column tail wrap;
- the spinner during connect and send against a stalling peer (and the
  inactivity error that follows), in all three color tiers: the
  truecolor pulse in portty, the blink under `TERM=xterm-256color` with
  `COLORTERM` unset, and the fixed `●` with `spinner_ramp = off`;
- the connect walk's notice and budget against a black-holed address,
  the status row's elision ladder, the live `⚡` rate;
- the reminder panel's block (bytes AND pixels), the file ledger's four
  cases, an image attach and a vision round-trip, and the sixel tier
  (portty answers kitty, so force the tier or emit a pinned DCS through
  portty's `-S` script: one image, `"source": "sixel"`, the cursor on
  the image's last row);
- live turns through hyper, openrouter and `opencode:go`.

## Submitting changes

`make format` + a green `make check` is the gate for every commit; run
`make distcheck` before a phase-closing push.
