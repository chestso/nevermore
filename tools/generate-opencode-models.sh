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

c_hdr="$root/src/opencode_models_data.h"

# emit <models.dev-entry> <json-out> <array-name> <tier-label>
# Writes the JSON (the human-readable regeneration record, shipped in
# the dist) AND appends the C table to $c_hdr. The C table is the
# SHIPPED catalog: the live GET {base}/models carries ids only
# (docs/OPENCODE-API.md §5), so the provider enriches each live id
# from this table by id and falls back to it offline.
emit() {
    entry="$1"; out="$2"; arr="$3"; label="$4"
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
    {
        printf '/* %s: models.dev entry "%s", generated %s.\n' "$arr" "$entry" "$stamp"
        printf ' * One line per model, sorted by id; script emits\n'
        printf ' * clang-format-stable output (.clang-format: ColumnLimit 0,\n'
        printf ' * SpacesInContainerLiterals, Cpp11BracedListStyle off). */\n'
        printf 'static const NmModel %s[] = {\n' "$arr"
        printf '%s\n' "$body" |
            jq -r '.[] | "    { \(.id|@json), \(.label|@json), \(.vision), \(.context_length) },"'
        printf '    { 0 }\n};\n\n'
    } >> "$c_hdr"
    echo "wrote $out ($label): $count models" >&2
    echo "wrote $c_hdr:$arr ($label): $count models" >&2
}

{
    printf '/* opencode_models_data.h - GENERATED OpenCode static catalogs.\n'
    printf ' *\n'
    printf ' * Produced by tools/generate-opencode-models.sh from models.dev\n'
    printf ' * (entries "opencode-go" -> Go, "opencode" -> Zen); the derived\n'
    printf ' * JSON sits beside it in data/. Do not edit by hand: regenerate\n'
    printf ' * and commit. Never fetched at runtime, so the shipped binary\n'
    printf ' * carries no third-party wire dependency (design §1a).\n'
    printf ' *\n'
    printf ' * These are the SHIPPED catalogs. The OpenCode live catalog\n'
    printf ' * (GET {base}/models) is membership only — id + object, no\n'
    printf ' * label/context/modality (docs/OPENCODE-API.md §5) — so a live\n'
    printf ' * fetch keeps these rows metadata-bearing by id lookup\n'
    printf ' * (opencode_meta_find), and the same tables are the offline\n'
    printf ' * fallback. -1 context_length = models.dev had none.\n'
    printf ' */\n\n'
    printf '#ifndef NM_OPENCODE_MODELS_DATA_H\n#define NM_OPENCODE_MODELS_DATA_H\n\n'
    printf '#include "provider.h"\n\n'
} > "$c_hdr"

# NOTE: models.dev entry -> nevermore file (inverted, see header).
emit "opencode-go" "$root/data/nm-opencode-models.json" \
    "opencode_go_models" "Go"
emit "opencode" "$root/data/nm-opencode-zen-models.json" \
    "opencode_zen_models" "Zen"

printf '#endif /* NM_OPENCODE_MODELS_DATA_H */\n' >> "$c_hdr"
