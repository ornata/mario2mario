# mario2mario: Phase 4b report

Direct LLM translation of the Super Mario 64 (US) N64 MIPS code to AArch64 macOS. None of the translation logic is written as code: every translated unit in `gen/units/` was written by an LLM worker, following `PROMPT-translate.md` (frozen at `1e25877`) and seeing only its unit's listing. The oracle is the in-tree R4300i interpreter (`src/oracle`), which shares its hardware model with the native runtime.

## Outcome against the success bar

| | criterion | result |
|-|-----------|--------|
| a | full `gameplay.rec` on `//native:run` byte-identical to the oracle | **yes**. Over 10,000,000,000 instructions, the stride-1000 checkpoint stream (4,118,673 records) and all 6,252 graphics-task hashes are identical |
| b | `//native:play` plays the route live at 60 fields/s | **yes**. The paced full-route replay ran 12,799 fields in 213.30 s (60.0 fields/s) on about 26% of one core (55 s user CPU). All 6,251 task hashes it produced match the oracle |
| c | full-route lockstep regression test | `//src/native:full_route_lockstep_test` (10B instructions, tagged `large`/`long`, size `enormous`) passes in 77 s. `pilot_lockstep_test` (200M) was kept, and both share `lockstep_test.c` |
| d | stub coverage extended | `opcode_stub_test` now covers **129 opcode forms** in 8,256 randomized trials against the oracle, with 0 failing |
| e | ledger + token totals | `gen/worker_ledger.tsv` has one row per unit attempt, with its exact model id. `TOKENS.md` gives exact and estimated totals |
| f | coverage ledger | `gen/work/coverage.sh` → `gen/coverage.tsv`. All **107,154 of 107,154** distinct executed (vaddr, word) pairs of the route are covered by translated units, and every one of them runs under the lockstep in (a) |

Headless native speed is about 1.49 G MIPS instructions/s (`//native:run`, full route in 6.7 s), against about 150 M/s for the oracle.

## Provenance (mixed, stated per unit)

The build links 535 units. Their final provenance comes from `gen/coverage.tsv`, which is joined from the ledger:

| model (exact id the harness sent to the API) | units | notes |
|---|---|---|
| claude-opus-5-5 | 532 | Agent-tool workers, 10 units/call in 4b |
| claude-opus-5-5 + hand | 1 | `80247F08_641534AD`: Opus output with 6 lines hand-fixed after one failed re-spawn (divergence #3). The ledger has `hand=1` |
| claude-sonnet-5 | 2 | `8027FB58_99F85922` and `80379790_25B40F7E`. These are trial units that validated and are lockstep-clean over the full route |

The 20 claude-sonnet-5-5 trial units are **not** in the build. They live in `gen/trial-sonnet55/` with their own ledger. A worker's model id was read from its transcript; the Agent tool's `"sonnet"` resolved to claude-sonnet-5 for every spawn, including one spawned on 2026-09-30.

## Worker trial: three arms

All three arms used the same contract, the same prompt and the same validation gates: assemble; one label and one provenance comment per pc; verbatim header and footer; no forbidden registers or instructions; a retire per group; json pcs; and, added in 4b, the fallthrough target rule and no scratch-register read after a macro. The Sonnet arms ran on the same route units (batches 23–25), which Opus then repaired, so the arms line up per unit. Per-attempt detail for Sonnet 5 is in `gen/trial/sonnet_trial.tsv`, and its failing outputs are kept as evidence in `gen/trial/sonnet_fail/`.

| | claude-opus-5-5 (baseline) | claude-sonnet-5 | claude-sonnet-5-5 |
|---|---|---|---|
| mechanism | Agent tool | Agent tool (`"sonnet"`) | 2 one-off `claude -p` calls (user-approved), side measurement |
| first attempts with output | 545 (517 route + 28 repairs of Sonnet 5 units) | 28 (+2 incomplete, see below) | 20 |
| first-time validation pass | **545/545 (100%)** | **3/28 (10.7%)** | **20/20 (100%)** |
| lockstep-correct after validation | 542/545 (99.4%): 3 units hit divergence #3 | 2/28 (7.1%): 1 unit hit divergence #4 | 20/20 (full-route swap test, byte-identical) |
| UNTRANSLATABLE reports | 2/519 route units, both genuine contract gaps (correct behaviour) | 0 | 0 |
| retries | 3 re-prompts after #3: 2 fixed, 1 repeated the defect and was hand-fixed | 1 retry (80286224): failed again, same error | none needed |
| dominant failure mode | not-taken fallthrough to B+0xC after a delay slot that ends a listing run (3 units, 5 sites) | `add\|sub wN, wzr, #imm` for `addiu` from `$zero` (137 occurrences; the only error in 25 of 26 failures), which the contract's worked example 2 shows done correctly. Also 2 undefined local labels, 1 out-of-range immediate, 1 stored clobbered `w16` after `M2M_FPU_END` (#4), and 1 worker that died on the 64K max-output-token limit | none; 0 `wzr, #imm` occurrences |
| tokens/instruction (exact calls; final-turn context, as `subagent_tokens`) | 292.3 over 87,415 instr (267.2 in the 10-unit batching trial) | 364.6 over 2,138 instr (1 exact call) | 279.3 over 4,151 instr (285.6, 272.7) |

**Decision.** The rule was: the cheapest model that holds near the Opus pass rate takes the bulk, and Opus keeps repairs and quarantine. Sonnet 5 failed that rule clearly (3/28), so the bulk stayed on Opus. Sonnet 5.5 met it on this sample: 20/20 validated, 20/20 lockstep-clean, and a per-instruction token count equal to or below Opus's. It would therefore take any future bulk translation. It came too late to take any bulk here, because every route unit was already translated when the arm ran. The sample of 20 units is small: at 100% on n=20, a 95% one-sided lower bound on the pass rate is about 86%. Per-token cost against usage limits is lower for Sonnet than for Opus, so tokens per instruction alone understates the saving.

## Divergences and stops (chronological)

1. **icount 25,152,136: oracle bug.** After interrupt entry, the oracle did not treat the vector as a block boundary. Fixed in the oracle (`cec7c38`); no translation was at fault.
2. **icount 266,412,957: `M2M_MISSING` stop.** Units split some branch/delay-slot pairs across a unit boundary. 13 units were re-rendered with their slots and re-translated. A runtime rule was added so that a delay-slot row never takes over a word owned by another unit.
3. **Contract gap: a branch into a delay slot** (8031EC30 → 8031EC40). A worker reported it UNTRANSLATABLE, and it was solved structurally by splitting 8031DF2C into three units, with no hand translation.
4. **Divergence #3, icount 2,877,365,148 (full route).** Localized with the new `--checkpoint-from` stride-1 window. Opus unit 80247F08 fell through to B+0xC after a delay slot that ends a listing run, so native landed 4 bytes late and later took a TLB refill that the oracle never takes. A scan found the same defect in 8017D75C and 80246E70; the 80246E70 site is dormant, after an always-taken `beq`. Two re-prompts fixed their units. 80247F08's re-prompt reproduced the defect exactly, so it was hand-fixed (6 lines) and flagged. The validator now rejects this pattern.
5. **Divergence #4, icount 9,483,837,417.** Sonnet 5 unit 8028B068 followed the literal §9 `trunc.w.s` recipe, storing `w16` after `M2M_FPU_END`, which clobbers x16. This is the §9 ambiguity recorded in the ledger at the freeze. Every Opus and Sonnet 5.5 worker routed the value through `s0` or `NS_TMP` instead. Opus re-translated the unit. A scan of all 535 units found no other clobbered-scratch read and no stale-flag read, and the validator now rejects both.

After #4 the full route is byte-identical end to end.

## Contract observations (contract unchanged since the freeze)

- §9's `cvt.*.w/l`/`trunc.*` recipe stores `w16` after `M2M_FPU_END`, which the register table says clobbers x16. Strong workers resolve it; a weaker worker followed it literally and diverged (#4). A contract revision should put the result in `s0` before `M2M_FPU_END`.
- The fallthrough target after a delay slot that ends a `--- gap ---` run is implied by §7.1 (`B+8`) but has no worked example. Three Opus units got it wrong, and one got it wrong twice.
- The `wzr` add-immediate encoding trap is covered only by worked example 2. Sonnet 5 ignored it almost always; Opus and Sonnet 5.5 never did.

## Process notes

- There were six usage-limit cuts in 4b (session and weekly). Each was reconciled from disk and the ledger only. Kept units have `NA` token cells and killed calls have `# KILLED` or `# ESTIMATE` rows, so exact and estimated totals stay separable.
- Two commits (`2612f91`, `e129097`) staged `gen/units` wholesale while trial workers were still writing. Failing Sonnet files became tracked, and the build broke at those commits; `b0a9e1b` fixed it. After that, units were staged by name only.
- One worker call was spawned with an empty placeholder prompt, an orchestrator error. It did nothing and cost 27,467 tokens (exact), recorded as a `# WASTE` row.
- One Sonnet 5 worker ran a Bash `mkdir` despite the Read/Write-only rule.

## Tokens

See `TOKENS.md`. Exact translation-worker totals: 25,616,823 claude-opus-5-5 + 779,443 claude-sonnet-5 + 1,159,529 claude-sonnet-5-5 (trial-only). Killed calls add about 8.9M, estimated from each model's exact tokens-per-instruction rate. Orchestration is estimated separately.

## Reproduce

```
bazelisk build //src/oracle:run //src/native:run
gen/work/regenerate.sh              # oracle reference streams + pipeline inputs (ROM required)
bazelisk test //src/native:full_route_lockstep_test //src/native:opcode_stub_test
gen/work/coverage.sh                # coverage ledger, exit 0 = every executed pair covered
bazel-bin/src/native/play --rom rom/mario64usa.z64 --replay tests/inputs/gameplay.rec --fields 12799
```
