# Token ledger

Updated at every phase boundary and every translation batch. Counts are
exact when the harness or API reports them; otherwise they are estimates
and the basis is stated. Translation tokens (phase 4+ emission) are the
experiment's headline number; everything else is scaffolding.

| date | phase | mechanism | model | tokens (exact/estimated + basis) |
|------|-------|-----------|-------|----------------------------------|
| 2026-09-26 | 0: plan + bootstrap | interactive | claude-fable-5 (orchestrator) + claude-opus-5-5 (worker) | TBD (estimated) |
| 2026-09-26 | 0–1: bootstrap + mechanical tooling (z64, decoder/listing, re-encoder, chunker) | subagent | claude-opus-5-5 | ~3.1M estimated: ~50K output + ~3.0M input (≈55 tool turns × ≈55K average context, mostly prompt-cache reads); basis: turn count × context-size snapshots, no harness count available |
