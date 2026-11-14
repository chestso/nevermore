#!/usr/bin/env python3
"""Flag a line break inside an inline code span inside a Markdown blockquote.

dprint is configured with `wrapCodeSpans: false`, so it keeps an inline
code span on one line and preserves the blockquote prefix. A hand-edited
span that still crosses a line break is fragile: CommonMark's lazy
continuation hides the problem until something inserts a blank line, a
list or a nested block, and then the text quietly leaves the quote. This
check catches the invariant before a formatter runs.

The invariant: inside a blockquote, every line has an EVEN number of
backticks — no code span is left open at a line end. Fenced code blocks
(``` / ~~~, which are quoted the same way) are skipped.

Usage: check-md-quotes.py FILE...   (exit 1 if anything is flagged)
"""

import sys


def quote_depth(line):
    """The blockquote markers at the start of `line`, and the rest of it."""
    depth, i = 0, 0
    while True:
        while i < len(line) and line[i] in " \t":
            i += 1
        if i < len(line) and line[i] == ">":
            depth += 1
            i += 1
            continue
        return depth, line[i:]


def check(path):
    problems = []
    fence = None
    for number, raw in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
        depth, body = quote_depth(raw)
        if depth == 0:
            fence = None  # a fence cannot survive a step out of the quote
            continue
        marker = body[:3]
        if marker in ("```", "~~~"):
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        # Odd count: one span is still open at the end of a quoted line.
        if body.replace("\\`", "").count("`") % 2:
            problems.append(number)
    return problems


def main(argv):
    if len(argv) < 2:
        print("usage: check-md-quotes.py FILE...", file=sys.stderr)
        return 2
    bad = 0
    for path in argv[1:]:
        for number in check(path):
            print(
                f"{path}:{number}: inline code span crosses a line break "
                f"inside a blockquote; keep the span on one line",
                file=sys.stderr,
            )
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
