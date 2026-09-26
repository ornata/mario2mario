# Token ledger

Updated at every phase boundary and every translation batch. Counts are
exact when the harness or API reports them; otherwise they are estimates
and the basis is stated. Translation tokens (phase 4+ emission) are the
experiment's headline number; everything else is scaffolding.

| date | phase | mechanism | model | tokens (exact/estimated + basis) |
|------|-------|-----------|-------|----------------------------------|
| 2026-09-26 | 0: plan + bootstrap | interactive | claude-fable-5 (orchestrator) + claude-opus-5-5 (worker) | TBD (estimated) |
| 2026-09-26 | 0–1: bootstrap + mechanical tooling (z64, decoder/listing, re-encoder, chunker) | subagent | claude-opus-5-5 | ~3.1M estimated: ~50K output + ~3.0M input (≈55 tool turns × ≈55K average context, mostly prompt-cache reads); basis: turn count × context-size snapshots, no harness count available |
| 2026-09-26 | 2: hw model + oracle interpreter + PIF HLE/IPL3 boot + trace + milestone tests | subagent | claude-opus-5-5 | ~6.1M estimated: ~110K output + ~6.0M input (≈35 tool turns × ≈170K average context, mostly prompt-cache reads); basis: turn count × context-size snapshots, no harness count available |
| 2026-09-26 | 3: Fast3D HLE + GL backend + input/.rec + //oracle:play + recordings/goldens | subagent | claude-opus-5-5 | ~16M estimated: ~130K output + ~16M input (≈75 tool turns × ≈210K average context, mostly prompt-cache reads; ~45 frame images read for route authoring); basis: turn count × context-size snapshots, no harness count available |
| 2026-09-26 | 4a (partial, stopped by classifier block): oracle state split, exec-words capture, codemap, native runtime scaffold, prompt draft | subagent | claude-opus-5-5 | ~6M estimated (orchestration): ≈30 turns × ≈200K context, mostly cache reads; no worker spend |
| 2026-09-26 | 4a pilot — **translation workers** (100 units, 19,598 MIPS instructions) | harness subagents (Agent tool) | claude-opus-5-5 | **6,141,228 exact** (subagent_tokens over 43 completed calls, per-unit in gen/worker_ledger.tsv) + ~1.3M estimated for 7 calls killed by a rate limit (15 units they wrote; ≈180K per 3-unit call, the measured average) |
| 2026-09-26 | 4a pilot — orchestration (renderer, runtime fixes, validation, lockstep, stub harness, //native:play) | subagent | claude-opus-5-5 | ~9M estimated: ≈120 turns × ≈75K average context (mostly cache reads); basis: turn count × context snapshots |
| 2026-09-26 | 4b batching trial + wave 1 start (wind-down) — **translation workers** | harness subagents | claude-opus-5-5 | **1,753,143 exact** (6-, 10-, 15-unit trial calls) + ~0.6M estimated for 2 trial calls killed by a rate limit (18 units written) + wave-1 calls still running at checkpoint (tokens pending, see gen/worker_ledger.tsv) |
| 2026-09-26 | 4b orchestration to checkpoint | subagent | claude-opus-5-5 | ~2M estimated: ≈45 turns × ≈45K average context |
