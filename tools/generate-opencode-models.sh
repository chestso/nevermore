#!/bin/sh
# generate-opencode-models.sh - one-off, OFFLINE data-file generator
#
# Produces nevermore's OpenCode static fallback catalogs from
# models.dev. NEVER run by `make` or `make check`: the shipped binary
# must not depend on a third-party wire document (design §1a); the
# generated data/*.json files are committed instead.
#
# Usage:  tools/generate-opencode-models.sh [path/to/api.json]
#         (defaults to fetching https://models.dev/api.json)
#
# THE MAPPING IS INVERTED, and that is the easy mistake (design §5):
#   models.dev "opencode"    == nevermore "opencode:zen" (Zen tier)
#   models.dev "opencode-go" == nevermore "opencode:go"  (Go tier)
# The script asserts both entries exist; a missing entry is an error,
# not a silent empty file.
#
# Output shape matches data/nm-openai-models.json: a JSON array of
# {id, label, vision, context_length}, preceded by '#' provenance
# lines (the design's "header comment records source entry + date";
# the file is documentation/regeneration source, never parsed at
# runtime — the shipped catalog is the static array in the provider).

set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:-}"
tmp=""
if [ -z "$src" ]; then
    tmp="$(mktemp)"
    src="$tmp"
    echo "fetching https://models.dev/api.json ..." >&2
    curl -fsSL https://models.dev/api.json -o "$src"
fi
trap 'test -n "$tmp" && rm -f "$tmp"' EXIT

command -v jq >/dev/null 2>&1 || {
    echo "jq is required" >&2
    exit 1
}
command -v curl >/dev/null 2>&1 || true

stamp="$(date -u +%Y-%m-%d)"

# emit <models.dev-entry> <nevermore-file> <tier-label>
emit() {
    entry="$1"; out="$2"; label="$3"
    jq -e --arg e "$entry" '.[$e].models | length > 0' "$src" >/dev/null || {
        echo "models.dev entry '$entry' missing or empty" >&2
        exit 1
    }
    body="$(
        jq --arg e "$entry" '
          [ .[$e].models | to_entries[]
            | {
                id: .key,
                label: (.value.name // .key),
                vision: (if ((.value.modalities.input // []) | index("image"))
                         then 1 else 0 end),
                context_length: (.value.limit.context // -1)
              }
          ] | sort_by(.id)' "$src"
    )"
    count="$(printf '%s\n' "$body" | jq 'length')"
    {
        printf '# Generated from models.dev api.json entry "%s" on %s by\n' \
            "$entry" "$stamp"
        printf '# tools/generate-opencode-models.sh. Do not edit by hand;\n'
        printf '# regenerate and commit. Never fetched at runtime.\n'
        printf '# \n'
        printf '%s\n' "$body"
    } > "$out"
    echo "wrote $out ($label): $count models" >&2
}

# NOTE: models.dev entry -> nevermore file (inverted, see header).
emit "opencode-go" "$root/data/nm-opencode-models.json" "Go"
emit "opencode" "$root/data/nm-opencode-zen-models.json" "Zen"
