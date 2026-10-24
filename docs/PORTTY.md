# nevermore × portty — Terminal Feature Matrix

nevermore is a first-class portty citizen, and must degrade gracefully
in any terminal (boba's runtime handles capability detection).

| Feature                 | Carrier                | portty/coffer support               | nevermore use                                                                                                                     | Fallback                  |
| ----------------------- | ---------------------- | ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------- |
| Kitty keyboard protocol | escape sequences       | coffer kitty keys via boba          | full-fidelity prompt editing, unambiguous ESC (the interrupt key)                                                                 | legacy sequences          |
| Image attach (vision)   | sixel / iTerm2 / kitty | coffer sixel + OSC 1337 + kitty APC | attach screenshots to vision models; the tier table picks kitty > iTerm2 > sixel (portty answers all three, so its lane is kitty) | marker / file path only   |
| Hyperlinks/styling      | SGR via boba styles    | coffer SGR pipeline                 | chat styling, tool-call blocks                                                                                                    | plain SGR                 |
| Soft-wrap + reflow      | terminal-native        | coffer reflow (per-line, on resize) | committed transcript rows are emitted unwrapped; portty wraps and reflows them                                                    | terminal soft-wrap (same) |
| Mouse (future)          | SGR mouse              | coffer                              | model/catalog pickers                                                                                                             | numbered lists            |

## Dropped by decision (not deferred — do not re-add)

Both were user calls, and the tracked tree no longer promises either:

- **OSC 52 clipboard** — dropped 2026-10-11.
- **OSC 5555 Lottie spinner** — dropped 2026-10-12. boba carries no
  OSC 5555 carrier at all, so the drop was the docs and the three
  comments in the spinner module and its test that promised the tier.
  What ships is the frame model (`nm_spinner_tick`): a dot that pulses
  on a truecolor terminal, blinks on a 16-color one, and is held still
  with `spinner_ramp = off`.

## E2E testing

`tests/e2e_portty/` (Phase 7) drives the built binary through coffer's
`cfr-debug` PTY inspector with scripted `wait-for`/`assert-contains`
assertions, against `tools/fake-ollama` on an ephemeral port — the
binary's startup, a turn end to end, the transcript's bytes, and its
exit status, all through a real terminal. Skipped with exit 77 when
`cfr-debug` (a coffer tool), `python3` or the built binary is absent
(the automake skip convention). See that directory's README for the
skip matrix and the scenario rules.
