# nevermore × portty — Terminal Feature Matrix

nevermore is a first-class portty citizen, and must degrade gracefully
in any terminal (boba's runtime handles capability detection).

| Feature                 | Carrier                | portty/coffer support               | nevermore use                                                                                                                     | Fallback                  |
| ----------------------- | ---------------------- | ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------- |
| Kitty keyboard protocol | escape sequences       | coffer kitty keys via boba          | full-fidelity prompt editing, unambiguous ESC                                                                                     | legacy sequences          |
| Image attach (vision)   | sixel / iTerm2 / kitty | coffer sixel + OSC 1337 + kitty APC | attach screenshots to vision models; the tier table picks kitty > iTerm2 > sixel (portty answers all three, so its lane is kitty) | marker / file path only   |
| Hyperlinks/styling      | SGR via boba styles    | coffer SGR pipeline                 | chat styling, tool-call blocks                                                                                                    | plain SGR                 |
| Soft-wrap + reflow      | terminal-native        | coffer reflow (per-line, on resize) | committed transcript rows are emitted unwrapped; portty wraps and reflows them                                                    | terminal soft-wrap (same) |
| Mouse (future)          | SGR mouse              | coffer                              | model/catalog pickers                                                                                                             | numbered lists            |

## E2E testing

`tests/e2e_portty/` (phase 6) drives the built binary through coffer's
`cfr-debug` PTY inspector with scripted `assert-contains` assertions —
same harness style as coffer's CI. Skipped with exit 77 when cfr-debug
is not installed (automake skip convention).
