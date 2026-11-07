#!/usr/bin/env python3
"""generate-catalogs.py - the ONE catalog generator (every provider).

WHAT IT IS
    nevermore's model catalogs are RESOLVED, never hand-written. This
    script resolves every provider's catalog with FULL COVERAGE and
    writes ONE artifact:

      src/catalog_data.h            the generated tables (the compiled-in
                                    FALLBACK: what the app shows until
                                    the RUNTIME resolution lands)

    The tables carry the source document each provider was resolved
    from, so the provenance ships with the rows it belongs to. There is
    no second copy of the catalog in the tree: a record nobody reads
    is a record that drifts.

    Regenerate with `make catalogs` - a release-time step, like the
    release recipe itself. Never run by `make` or `make check`: the
    suite never hits the network, and the shipped binary must not
    depend on a third-party wire document at build time (design §1a).

FULL COVERAGE, OR NOTHING
    A model is in the catalog only when we know its label, its context
    window and its capability claims. A model the provider SERVES but
    that no source covers is NOT offered: an id-only row shows a `-`
    window, no badges and a silent context gauge, and a picker full of
    half-rows is worse than a picker that is honest about what it
    knows. Membership is the served list AND the covered list: a model
    nobody serves is not offered either (a footgun, like OpenRouter's
    ":batch" variants).

THE SOURCES (per provider, the same documents the runtime reads)
    openrouter   GET https://openrouter.ai/api/v1/models?limit=1000
    ollama:cloud GET https://ollama.com/v1/models?verbose=true
    opencode:*   the catalog document (https://models.opencode.ai/api.json
                 - OpenCode's OWN domain; models.dev serves the identical
                 document) intersected with the tier's serving list
                 (GET https://opencode.ai/zen{,/go}/v1/models)

    ollama:local has NO generated catalog: its models are the user's
    own (whatever the daemon has installed), and no source can know
    them. The daemon answers at runtime.

USAGE
    tools/generate-catalogs.py [--out DIR] [--check]

      --out DIR    where to write the tables (default: the repo root)
      --check      resolve everything and compare against the committed
                   tables; exit 1 when they differ (a release gate —
                   it never writes)

    Keys: the OpenCode catalog is public; the ollama cloud's catalog
    answers with or without a key (a key may list more models - set
    OLLAMA_API_KEY to use it).
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

USER_AGENT = "nevermore-catalog-generator"
TIMEOUT = 30


# --------------------------------------------------------------------
# fetch
# --------------------------------------------------------------------


def fetch(url, headers=None):
    """GET a URL -> the bytes. One User-Agent, one timeout."""
    req = urllib.request.Request(
        url, headers={"User-Agent": USER_AGENT, **(headers or {})}
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT) as res:
        return res.read()


def fetch_json(url, headers=None):
    return json.loads(fetch(url, headers).decode("utf-8"))


# --------------------------------------------------------------------
# the row: what a catalog entry becomes
# --------------------------------------------------------------------


def row(
    model_id,
    label,
    context_length,
    vision,
    image_gen,
    tools,
    effort_levels=None,
    effort_default=None,
):
    """One catalog row. The four facts, and nothing else — plus the
    effort vocab when the source names one.

    context_length <= 0 means UNKNOWN: a row without a window has no
    coverage (the caller drops it).
    """
    return {
        "id": model_id,
        "label": label or model_id,
        "vision": 1 if vision else 0,
        "image_gen": 1 if image_gen else 0,
        "context_length": int(context_length),
        "tools": int(tools),
        "effort_levels": effort_levels,
        "effort_default": effort_default,
    }


def effort_join(values):
    """A level list -> the "|"-joined spelling NmModel holds.

    Empty/absent is None (no levels known). The values are the
    source's own spellings, in source order — no canon in between.
    """
    vals = [v for v in (values or []) if isinstance(v, str) and v]
    return "|".join(vals) if vals else None


def has_str(arr, want):
    """Does the list carry the string? (character scan, no regex)"""
    if not isinstance(arr, list):
        return False
    return any(isinstance(x, str) and x == want for x in arr)


# --------------------------------------------------------------------
# the per-provider resolvers
# --------------------------------------------------------------------


def resolve_openrouter():
    """GET /api/v1/models?limit=1000 - the wire carries every fact.

    The ":batch" variants are VETOED (they are served only by the Batch
    API; a sync request naming one 404s - OPENROUTER-API.md §6). The
    rule lives here too, so the generated fallback and the runtime
    resolution agree: a picker must not offer a model that can only
    fail.
    """
    doc = fetch_json("https://openrouter.ai/api/v1/models?limit=1000")
    rows = []
    for entry in doc.get("data") or []:
        model_id = entry.get("id")
        if not model_id:
            continue
        if model_id.endswith(":batch"):
            continue
        arch = entry.get("architecture") or {}
        params = entry.get("supported_parameters")
        if isinstance(params, list):
            # The list is there: its content is the claim (1 = claims
            # tools, -1 = listed WITHOUT tools).
            tools = 1 if has_str(params, "tools") else -1
        else:
            tools = 0  # the catalog says nothing
        # Reasoning effort: the catalog carries support, not levels,
        # so the vocabulary is the provider-level shorthand enum —
        # the same constant the runtime row mapper fills (probed
        # 2026-11-07). Gated on the claim like tools is. The default
        # rides the same gate: reasoning.default_effort (probed:
        # stealth/crouton-alpha "max"), informational only.
        effort = (
            "none|minimal|low|medium|high|xhigh|max"
            if isinstance(params, list)
            and (has_str(params, "reasoning_effort") or has_str(params, "reasoning"))
            else None
        )
        effort_default = None
        if effort is not None:
            default = (entry.get("reasoning") or {}).get("default_effort")
            if isinstance(default, str) and default:
                effort_default = default
        rows.append(
            row(
                model_id,
                entry.get("name"),
                entry.get("context_length") or 0,
                has_str(arch.get("input_modalities"), "image"),
                has_str(arch.get("output_modalities"), "image"),
                tools,
                effort,
                effort_default,
            )
        )
    return rows


def resolve_ollama():
    """GET /v1/models?verbose=true - the whole catalog in one request.

    The verbose half carries slug, display_name, context_window and
    input_modalities - the id, the label, the window and the vision
    claim (OLLAMA-CLOUD-API.md §6.16). The tools claim stays 0 ("the
    catalog says nothing"): the verbose shape's tool list is empty for
    every model, and an empty list is not a claim.
    """
    headers = {}
    key = os.environ.get("OLLAMA_API_KEY")
    if key:
        headers["Authorization"] = "Bearer " + key
    doc = fetch_json("https://ollama.com/v1/models?verbose=true", headers)
    rows = []
    for entry in doc.get("models") or []:
        slug = entry.get("slug")
        if not slug:
            continue
        rows.append(
            row(
                slug,
                entry.get("display_name"),
                entry.get("context_window") or 0,
                has_str(entry.get("input_modalities"), "image"),
                False,
                0,
                effort_join(
                    e.get("effort")
                    for e in (entry.get("supported_reasoning_levels") or [])
                    if isinstance(e, dict)
                ),
                entry.get("default_reasoning_level") or None,
            )
        )
    return rows


CATALOG_URL = "https://models.opencode.ai/api.json"
TIER_BASE = {
    "opencode-go": "https://opencode.ai/zen/go/v1",
    "opencode-zen": "https://opencode.ai/zen/v1",
}
# The catalog files a model under the tier(s) that offer it, and the
# filing MOVES: a model the Go tier serves can be filed under the Zen
# entry (grok-4.6, kimi-k2.6, qwen3.6-plus, glm-5.1, minimax-m2.5 did).
# So the tier's own entry is the FIRST source and the sibling entry is
# the fallback for an id the tier's entry lacks - and the tier's own
# window always wins, because a tier may serve the same model with a
# different one (minimax-m3: 1M on Go, 512k on Zen).
CATALOG_ENTRY = {"opencode-go": "opencode-go", "opencode-zen": "opencode"}


def opencode_effort(entry):
    """The first `type: effort` entry of reasoning_options, joined.

    Toggle-only and budget_tokens-only rows get no knob (a toggle is
    on/off without a level, a budget is a token count). The runtime
    row mapper (provider_opencode.c) applies the same rule, so the
    generated fallback and the live resolution agree.
    """
    for opt in entry.get("reasoning_options") or []:
        if isinstance(opt, dict) and opt.get("type") == "effort":
            return effort_join(opt.get("values"))
    return None


def resolve_opencode(slug):
    """The catalog document (OpenCode's own domain) x the served list.

    The tier's serving list (GET {base}/models) is the MEMBERSHIP: the
    catalog also carries models the tier does not serve, and offering
    those is a footgun. The catalog is the COVERAGE: a served id with
    no row (in the tier's entry nor in its sibling) is not offered.
    """
    catalog = fetch_json(CATALOG_URL)
    base = TIER_BASE[slug]
    served = fetch_json(base + "/models")
    served_ids = {m.get("id") for m in (served.get("data") or []) if m.get("id")}

    own = (catalog.get(CATALOG_ENTRY[slug]) or {}).get("models") or {}
    sibling_slug = "opencode" if slug == "opencode-go" else "opencode-go"
    sibling = (catalog.get(sibling_slug) or {}).get("models") or {}

    rows = []
    for model_id in sorted(served_ids):
        entry = own.get(model_id) or sibling.get(model_id)
        if not entry:
            continue  # no coverage: not offered
        modalities = entry.get("modalities") or {}
        rows.append(
            row(
                model_id,
                entry.get("name"),
                (entry.get("limit") or {}).get("context") or 0,
                has_str(modalities.get("input"), "image"),
                has_str(modalities.get("output"), "image"),
                1 if entry.get("tool_call") else 0,
                opencode_effort(entry),
                None,
            )
        )
    return rows


# --------------------------------------------------------------------
# the artifacts
# --------------------------------------------------------------------


# Where each provider's catalog came from - the document the runtime
# reads too. The tables carry the note, so the provenance ships with
# the rows it belongs to.
SOURCE_NOTE = {
    "openrouter": "GET https://openrouter.ai/api/v1/models?limit=1000",
    "ollama": "GET https://ollama.com/v1/models?verbose=true",
    "opencode-go": "https://models.opencode.ai/api.json (entry opencode-go, sibling opencode) x GET https://opencode.ai/zen/go/v1/models",
    "opencode-zen": "https://models.opencode.ai/api.json (entry opencode, sibling opencode-go) x GET https://opencode.ai/zen/v1/models",
}


def header_text(tables):
    """The generated tables: the compiled-in fallback.

    One table per provider, sorted by id, in the row shape the app
    reads (id, label, vision, image_gen, context_length, tools,
    effort_levels, effort_default).
    """
    out = []
    out.append("/* catalog_data.h - GENERATED model catalogs (every provider).")
    out.append(" *")
    out.append(" * Produced by tools/generate-catalogs.py; regenerate with")
    out.append(" * `make catalogs` (a release-time step). Do not edit by hand.")
    out.append(" *")
    out.append(" * These are the FALLBACK catalogs: what the app shows until the")
    out.append(" * RUNTIME resolution lands (the live fetch). Every row is a model")
    out.append(" * with FULL COVERAGE - a label, a context window and the")
    out.append(" * capability claims. A model nobody covers is not offered.")
    out.append(" */")
    out.append("")
    out.append("#ifndef NM_CATALOG_DATA_H")
    out.append("#define NM_CATALOG_DATA_H")
    out.append("")
    out.append('#include "provider.h"')
    out.append("")
    for slug, rows in tables:
        out.append("/* %s: %d models." % (slug, len(rows)))
        out.append(" * Source: %s" % SOURCE_NOTE[slug])
        out.append(" */")
        out.append("static const NmModel nm_catalog_%s[] = {" % slug.replace("-", "_"))
        for r in rows:
            out.append(
                '    { "%s", "%s", %d, %d, %d, %d, %s, %s },'
                % (
                    r["id"],
                    r["label"],
                    r["vision"],
                    r["image_gen"],
                    r["context_length"],
                    r["tools"],
                    '"%s"' % r["effort_levels"] if r["effort_levels"] else "NULL",
                    '"%s"' % r["effort_default"] if r["effort_default"] else "NULL",
                )
            )
        out.append("    { 0 }")
        out.append("};")
        out.append("")
    out.append("#endif /* NM_CATALOG_DATA_H */")
    out.append("")
    return "\n".join(out)


PROVIDERS = [
    ("openrouter", "openrouter", resolve_openrouter),
    ("ollama", "ollama", resolve_ollama),
    ("opencode-go", "opencode-go", lambda: resolve_opencode("opencode-go")),
    ("opencode-zen", "opencode-zen", lambda: resolve_opencode("opencode-zen")),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    )
    ap.add_argument("--check", action="store_true", help="compare, do not write")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    tables = []
    for slug, _name, resolve in PROVIDERS:
        try:
            rows = resolve()
        except (urllib.error.URLError, urllib.error.HTTPError, ValueError) as e:
            print("generate-catalogs: %s: %s" % (slug, e), file=sys.stderr)
            return 1
        rows.sort(key=lambda r: r["id"])
        covered = sum(1 for r in rows if r["context_length"] > 0)
        print("%-14s %4d models (%d with a window)" % (slug, len(rows), covered))
        tables.append((slug, rows))

    path = os.path.join(out, "src", "catalog_data.h")
    text = header_text(tables)
    current = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    if args.check:
        # A GATE reads the tree; it never writes it.
        if current != text:
            print(
                "generate-catalogs: the committed catalogs are STALE", file=sys.stderr
            )
            return 1
        print("generate-catalogs: the committed catalogs are current")
        return 0
    if current == text:
        print("generate-catalogs: the catalogs are current")
        return 0
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("generate-catalogs: wrote the catalogs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
