/* opencode_models_data.h - GENERATED OpenCode static catalogs.
 *
 * Produced by tools/generate-opencode-models.sh from models.dev
 * (entries "opencode-go" -> Go, "opencode" -> Zen); the derived
 * JSON sits beside it in data/. Do not edit by hand: regenerate
 * and commit. Never fetched at runtime, so the shipped binary
 * carries no third-party wire dependency (design §1a).
 *
 * These are the SHIPPED catalogs. The OpenCode live catalog
 * (GET {base}/models) is membership only — id + object, no
 * label/context/modality (docs/OPENCODE-API.md §5) — so a live
 * fetch keeps these rows metadata-bearing by id lookup
 * (opencode_meta_find), and the same tables are the offline
 * fallback. -1 context_length = models.dev had none.
 */

#ifndef NM_OPENCODE_MODELS_DATA_H
#define NM_OPENCODE_MODELS_DATA_H

#include "provider.h"

/* opencode_go_models: models.dev entry "opencode-go", generated 2026-09-15.
 * One line per model, sorted by id; script emits
 * clang-format-stable output (.clang-format: ColumnLimit 0,
 * SpacesInContainerLiterals, Cpp11BracedListStyle off). */
static const NmModel opencode_go_models[] = {
    { "deepseek-v4-flash", "DeepSeek V4 Flash", 0, 1000000 },
    { "deepseek-v4-flash-vision-exp", "DeepSeek V4 Flash Vision Exp", 1, 1000000 },
    { "deepseek-v4-pro", "DeepSeek V4 Pro (New)", 0, 1000000 },
    { "deepseek-v4.1-flash", "DeepSeek V4.1 Flash", 1, 1000000 },
    { "glm-5", "GLM-5", 0, 202752 },
    { "glm-5.1", "GLM-5.1", 0, 202752 },
    { "glm-5.2", "GLM-5.2", 0, 1000000 },
    { "glm-5.3", "GLM-5.3", 0, 1000000 },
    { "glm-5.3-flash", "GLM-5.3-Flash", 1, 1000000 },
    { "gpt-5.6-luna", "GPT-5.6 Luna", 1, 1050000 },
    { "grok-4.5", "Grok 4.5", 1, 500000 },
    { "grok-4.6", "Grok 4.6", 1, 500000 },
    { "hy3", "Hy3", 0, 256000 },
    { "hy4-preview", "Hy4 preview", 0, 1024000 },
    { "kimi-k2.5", "Kimi K2.5", 1, 262144 },
    { "kimi-k2.6", "Kimi K2.6", 1, 262144 },
    { "kimi-k2.7-code", "Kimi K2.7 Code", 1, 262144 },
    { "kimi-k3", "Kimi K3", 1, 1048576 },
    { "longcat-2.0", "LongCat-2.0", 0, 1000000 },
    { "mimo-v2-omni", "MiMo V2 Omni", 1, 262144 },
    { "mimo-v2-pro", "MiMo V2 Pro", 0, 1048576 },
    { "mimo-v2.5", "MiMo V2.5", 1, 1000000 },
    { "mimo-v2.5-pro", "MiMo V2.5 Pro", 0, 1048576 },
    { "minimax-m2.5", "MiniMax-M2.5", 0, 204800 },
    { "minimax-m2.7", "MiniMax-M2.7", 0, 204800 },
    { "minimax-m3", "MiniMax-M3", 1, 1000000 },
    { "muse-spark-1.2-contributor", "Muse Spark 1.2 Contributor", 1, 1048576 },
    { "muse-spark-1.3-contributor", "Muse Spark 1.3 Contributor", 1, 1048576 },
    { "omen-alpha", "Omen Alpha", 1, 500000 },
    { "ox-alpha-free", "Ox Alpha Free (Unlimited)", 1, 1000000 },
    { "qwen3.5-plus", "Qwen3.5 Plus", 1, 262144 },
    { "qwen3.6-plus", "Qwen3.6 Plus", 1, 1000000 },
    { "qwen3.7-max", "Qwen3.7 Max", 0, 1000000 },
    { "qwen3.7-plus", "Qwen3.7 Plus", 1, 1000000 },
    { "qwen3.8-flash", "Qwen3.8 Flash", 1, 1000000 },
    { "qwen3.8-max", "Qwen3.8 Max", 1, 1000000 },
    { 0 }
};

/* opencode_zen_models: models.dev entry "opencode", generated 2026-09-15.
 * One line per model, sorted by id; script emits
 * clang-format-stable output (.clang-format: ColumnLimit 0,
 * SpacesInContainerLiterals, Cpp11BracedListStyle off). */
static const NmModel opencode_zen_models[] = {
    { "big-pickle", "Big Pickle", 0, 200000 },
    { "claude-3-5-haiku", "Claude Haiku 3.5", 1, 200000 },
    { "claude-fable-5", "Claude Fable 5", 1, 1000000 },
    { "claude-fable-5-1", "Claude Fable 5.1", 1, 1000000 },
    { "claude-haiku-4-5", "Claude Haiku 4.5", 1, 200000 },
    { "claude-opus-4-1", "Claude Opus 4.1", 1, 200000 },
    { "claude-opus-4-5", "Claude Opus 4.5", 1, 200000 },
    { "claude-opus-4-6", "Claude Opus 4.6", 1, 1000000 },
    { "claude-opus-4-7", "Claude Opus 4.7", 1, 1000000 },
    { "claude-opus-4-8", "Claude Opus 4.8", 1, 1000000 },
    { "claude-opus-5", "Claude Opus 5", 1, 1000000 },
    { "claude-sonnet-4", "Claude Sonnet 4", 1, 1000000 },
    { "claude-sonnet-4-5", "Claude Sonnet 4.5", 1, 1000000 },
    { "claude-sonnet-4-6", "Claude Sonnet 4.6", 1, 1000000 },
    { "claude-sonnet-5", "Claude Sonnet 5", 1, 1000000 },
    { "deepseek-v4-flash", "DeepSeek V4 Flash", 0, 1000000 },
    { "deepseek-v4-flash-free", "DeepSeek V4 Flash Free", 0, 200000 },
    { "deepseek-v4-flash-vision-exp", "DeepSeek V4 Flash Vision Exp", 1, 1000000 },
    { "deepseek-v4-pro", "DeepSeek V4 Pro", 0, 1000000 },
    { "gemini-3-flash", "Gemini 3 Flash", 1, 1048576 },
    { "gemini-3-pro", "Gemini 3 Pro", 1, 1048576 },
    { "gemini-3.1-pro", "Gemini 3.1 Pro Preview", 1, 1048576 },
    { "gemini-3.5-flash", "Gemini 3.5 Flash", 1, 1048576 },
    { "gemini-3.5-flash-lite", "Gemini 3.5 Flash Lite", 1, 1048576 },
    { "gemini-3.6-flash", "Gemini 3.6 Flash", 1, 1048576 },
    { "gemini-3.7-flash", "Gemini 3.7 Flash", 1, 1048576 },
    { "gemini-3.8-flash", "Gemini 3.8 Flash", 1, 1048576 },
    { "glm-4.6", "GLM-4.6", 0, 204800 },
    { "glm-4.7", "GLM-4.7", 0, 204800 },
    { "glm-4.7-free", "GLM-4.7 Free", 0, 204800 },
    { "glm-5", "GLM-5", 0, 204800 },
    { "glm-5-free", "GLM-5 Free", 0, 204800 },
    { "glm-5.1", "GLM-5.1", 0, 204800 },
    { "glm-5.2", "GLM-5.2", 0, 1000000 },
    { "glm-5.3", "GLM-5.3", 0, 1000000 },
    { "glm-5.3-flash", "GLM-5.3-Flash", 1, 1000000 },
    { "gpt-5", "GPT-5", 1, 400000 },
    { "gpt-5-codex", "GPT-5 Codex", 1, 400000 },
    { "gpt-5-nano", "GPT-5 Nano", 1, 400000 },
    { "gpt-5.1", "GPT-5.1", 1, 400000 },
    { "gpt-5.1-codex", "GPT-5.1 Codex", 1, 400000 },
    { "gpt-5.1-codex-max", "GPT-5.1 Codex Max", 1, 400000 },
    { "gpt-5.1-codex-mini", "GPT-5.1 Codex Mini", 1, 400000 },
    { "gpt-5.2", "GPT-5.2", 1, 400000 },
    { "gpt-5.2-codex", "GPT-5.2 Codex", 1, 400000 },
    { "gpt-5.3-codex", "GPT-5.3 Codex", 1, 400000 },
    { "gpt-5.3-codex-spark", "GPT-5.3 Codex Spark", 0, 128000 },
    { "gpt-5.4", "GPT-5.4", 1, 1050000 },
    { "gpt-5.4-mini", "GPT-5.4 Mini", 1, 400000 },
    { "gpt-5.4-nano", "GPT-5.4 Nano", 1, 400000 },
    { "gpt-5.4-pro", "GPT-5.4 Pro", 1, 1050000 },
    { "gpt-5.5", "GPT-5.5", 1, 1050000 },
    { "gpt-5.5-pro", "GPT-5.5 Pro", 1, 1050000 },
    { "gpt-5.6-luna", "GPT-5.6 Luna", 1, 1050000 },
    { "gpt-5.6-sol", "GPT-5.6 Sol (50% Off)", 1, 1050000 },
    { "gpt-5.6-terra", "GPT-5.6 Terra", 1, 1050000 },
    { "gpt-6-astra", "GPT-6 Astra", 1, 1050000 },
    { "grok-4.5", "Grok 4.5", 1, 500000 },
    { "grok-4.6", "Grok 4.6", 1, 500000 },
    { "grok-build-0.1", "Grok Build 0.1", 1, 256000 },
    { "grok-code", "Grok Code Fast 1", 0, 256000 },
    { "hy3-free", "Hy3 Free", 0, 190000 },
    { "hy3-preview-free", "Hy3 preview Free", 0, 256000 },
    { "kimi-k2", "Kimi K2", 0, 262144 },
    { "kimi-k2-thinking", "Kimi K2 Thinking", 0, 262144 },
    { "kimi-k2.5", "Kimi K2.5", 1, 262144 },
    { "kimi-k2.5-free", "Kimi K2.5 Free", 1, 262144 },
    { "kimi-k2.6", "Kimi K2.6", 1, 262144 },
    { "kimi-k2.7-code", "Kimi K2.7 Code", 1, 262144 },
    { "kimi-k3", "Kimi K3", 1, 1048576 },
    { "laguna-s-2.1-free", "Laguna S 2.1 Free", 0, 256000 },
    { "ling-2.6-flash-free", "Ling 2.6 Flash Free", 0, 262100 },
    { "ling-3.0-flash-fin-free", "Ling 3.0 Flash Fin Free", 0, 262144 },
    { "ling-3.0-flash-free", "Ling-3.0-flash Free", 0, 262144 },
    { "ling-3.0-tiny-free", "Ling-3.0-tiny Free", 0, 262144 },
    { "longcat-2.0-free", "LongCat-2.0 Free", 0, 1000000 },
    { "mimo-v2-flash-free", "MiMo V2 Flash Free", 0, 262144 },
    { "mimo-v2-omni-free", "MiMo V2 Omni Free", 1, 262144 },
    { "mimo-v2-pro-free", "MiMo V2 Pro Free", 0, 1048576 },
    { "mimo-v2.5-free", "MiMo V2.5 Free", 1, 200000 },
    { "minimax-m2.1", "MiniMax-M2.1", 0, 204800 },
    { "minimax-m2.1-free", "MiniMax-M2.1 Free", 0, 204800 },
    { "minimax-m2.5", "MiniMax-M2.5", 0, 204800 },
    { "minimax-m2.5-free", "MiniMax-M2.5 Free", 0, 204800 },
    { "minimax-m2.7", "MiniMax-M2.7", 0, 204800 },
    { "minimax-m3", "MiniMax-M3", 1, 512000 },
    { "minimax-m3-free", "MiniMax-M3 Free", 1, 200000 },
    { "muse-spark-1.2", "Muse Spark 1.2", 1, 1048576 },
    { "muse-spark-1.2-contributor-free", "Muse Spark 1.2 Free", 1, 1048576 },
    { "muse-spark-1.3", "Muse Spark 1.3", 1, 1048576 },
    { "muse-spark-1.3-contributor-free", "Muse Spark 1.3 Free", 1, 1048576 },
    { "nemotron-3-super-free", "Nemotron 3 Super Free", 0, 204800 },
    { "nemotron-3-ultra-free", "Nemotron 3 Ultra Free", 0, 1000000 },
    { "nemotron-3.5-lightning-free", "Nemotron 3.5 Lightning Free", 0, 262144 },
    { "north-mini-code-free", "North Mini Code Free", 0, 256000 },
    { "qwen3-coder", "Qwen3 Coder", 0, 262144 },
    { "qwen3.5-plus", "Qwen3.5 Plus", 1, 262144 },
    { "qwen3.6-plus", "Qwen3.6 Plus", 1, 262144 },
    { "qwen3.6-plus-free", "Qwen3.6 Plus Free", 1, 262144 },
    { "ring-2.6-1t-free", "Ring 2.6 1T Free", 0, 262000 },
    { "trinity-large-preview-free", "Trinity Large Preview", 0, 131072 },
    { "x-preview-f-free", "Ox Alpha Free (Unlimited)", 1, 1000000 },
    { 0 }
};

#endif /* NM_OPENCODE_MODELS_DATA_H */
