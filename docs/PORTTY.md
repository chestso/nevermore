# nevermore × portty — Terminal Feature Matrix

nevermore is a first-class portty citizen, and must degrade gracefully
in any terminal (boba's runtime handles capability detection).

| Feature                 | Carrier             | portty/coffer support                       | nevermore use                                 | Fallback                     |
| ----------------------- | ------------------- | ------------------------------------------- | --------------------------------------------- | ---------------------------- |
| Kitty keyboard protocol | escape sequences    | coffer kitty keys via boba                  | full-fidelity prompt editing, unambiguous ESC | legacy sequences             |
| Clipboard copy/paste    | OSC 52              | portty osc52 support                        | yank assistant output, paste long prompts     | terminal-native paste        |
| Thinking spinner        | OSC 5555 (Lottie)   | coffer Lottie protocol; ConPTY-safe carrier | streaming/"thinking" indicator                | charset frames, braille dots |
| Image attach (vision)   | sixel               | coffer sixel                                | attach screenshots to vision models           | file path only               |
| Hyperlinks/styling      | SGR via boba styles | coffer SGR pipeline                         | chat styling, tool-call blocks                | plain SGR                    |
| Mouse (future)          | SGR mouse           | coffer                                      | model/catalog pickers                         | numbered lists               |

## E2E testing

`tests/e2e_portty/` (phase 6) drives the built binary through coffer's
`cfr-debug` PTY inspector with scripted `assert-contains` assertions —
same harness style as coffer's CI. Skipped with exit 77 when cfr-debug
is not installed (automake skip convention).
