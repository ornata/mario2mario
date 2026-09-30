# mario2mario: report

## The question and the answer

**Question** (README.md): how hard is it for an LLM to translate a compiled binary
directly from one platform to another, with no specialized tools and no decompilation?

**Answer (reference run: Super Mario 64 USA, N64 MIPS R4300i → Apple Silicon macOS AArch64).**
It can be done, at the scale of a full game route, and the result can be checked by
machine. LLM workers translated every CPU instruction that the recorded gameplay route
executes, instruction by instruction:
- 107,154 distinct executed code words;
- 535 units;
- 107,172 translated MIPS instructions.

The native binary is byte-identical to an independently written reference interpreter
over 10 billion instructions of gameplay: every checkpoint of the architectural state and
every graphics display list match. It plays the route live in a window at 60 fields/s on
about a quarter of one core.

What it cost:
- about 27.6M exact translation-worker tokens, plus about 8.9M estimated for calls killed
  by usage limits;
- about 115M tokens in all, including scaffolding and orchestration.

What fought back: not the ISA itself, but a handful of recipe details (delay-slot
fallthroughs, scratch registers clobbered by runtime calls, encodability of immediates)
and one oracle timing bug.

**Model sensitivity.**
- claude-opus-5-5 was essentially reliable: 100% of first attempts validated, and 99.4%
  were lockstep-correct.
- claude-sonnet-5 was not: 10.7% validated.
- claude-sonnet-5-5 matched Opus on a small trial: 20/20.

## Architecture in brief

One hardware model (`src/hw`), shared byte-for-byte by two engines:
- `src/oracle`: a MIPS R4300i interpreter written from scratch in this repo, the test
  oracle.
- `src/native`: a runtime plus the LLM-written AArch64 in `gen/units/*.s`. Units link
  lazily and are content-keyed. Indirect jumps go through a flat per-physical-word
  dispatch table, and untranslated code traps.

`src/rcp` provides Fast3D display-list HLE → OpenGL, keyboard → controller, and `.rec`
replay. `src/tools` holds only mechanical bookkeeping: the z64 parser, a bijective
decoder/listing/re-encoder, the chunker, `codemap` and `unitrender`. A deterministic
timebase (`src/hw/timebase.h`) and a shared checkpoint format (`src/oracle/checkpoint.h`)
make the two engines' streams byte-comparable.

**What was translated vs HLE'd.**
- **Translated:** all MIPS CPU code the route executes, including the ROM's own IPL3, the
  OS exception handler at `0x80000180` and the thread scheduler. The TLB-refill vector is never
  executed on this route (the oracle takes 0 TLB exceptions), so it is not translated.
- **HLE'd:**
  - PIF boot state (seeded from the documented CIC/PIF values; IPL3 itself runs
    translated);
  - the RSP: graphics display lists go through the Fast3D HLE, and audio tasks are
    acknowledged;
  - AI output is silent.
- The RSP microcode is a different processor and was out of scope.

## Phase history

- **Phases 0–1: bootstrap and mechanical tooling.**
  - ROM guard: `.gitignore`, a pre-commit hook, and the later `//verify:rom_history`.
  - Bazel via bazelisk; clang-format.
  - A table-driven R4300i decoder, proven bijective by re-encoding the whole ROM; a
    chunker.
- **Phase 2: hardware model and oracle.**
  - A flat `HwState`/`CpuState`: full integer ISA, FPU, COP0, a 32-entry TLB, exceptions
    and interrupts, and a deterministic Count.
  - Boot uses PIF HLE, then the real IPL3.
  - Finding that became normative: SM64 does use TLB-mapped addresses (segment 4 through
    `osMapTLB`), so both engines must implement the same TLB, not a KSEG mask.
- **Phase 3: graphics and input.**
  - Fast3D HLE → OpenGL (SDL2); `.rec` record/replay; `//oracle:play`.
  - Title and gameplay recordings, with task-hash and golden-image tests.
  - The game was playable under the oracle.
- **Phase 4a: runtime and translation pilot.**
  - Built: the native runtime, the macros and trampolines, the executed-word capture, the
    `codemap`/`unitrender` renderers and the per-opcode stub harness.
  - The first worker mechanism, an automated `claude -p` driver script, was blocked by a
    safety classifier. The user chose harness subagents (the Agent tool) instead. No
    automated model loop was built after that.
  - The pilot translated 100 units and ran lockstep over the first 200M instructions.
- **The oracle-bug story (divergence #1, icount 25,152,136).**
  - The first lockstep divergence was not a translation error. The oracle did not treat
    the exception vector as a block boundary after entering an interrupt.
  - The native runtime, which follows the normative sampling rule, was right.
  - The oracle was fixed (`cec7c38`), and the spec wording was tightened.
  - This validated the design: the oracle and the contract are independent artifacts, and
    lockstep finds bugs in either.
- **Contract freeze** (`1e25877`, after the pilot): `PROMPT-translate.md`, about 4K words.
- **Phase 4b: full coverage.**
  - The batching trial chose 10 units per call (267 tokens per instruction, against 472
    at 1 per call).
  - Units were translated in route order, in waves of about 8 calls.
  - Contract gaps were solved structurally: branch/delay-slot pairs split across units were
    re-rendered, and a branch into a delay slot was handled by splitting a unit.
  - There were six usage-limit cuts, each recovered from disk and the ledger.
  - A three-arm model trial ran, and two late divergences were localized and fixed (#3 and
    #4 below).
  - The phase ended at full coverage and a byte-identical 10B-instruction route.
- **Phase 5/7: verification and deliverables.**
  - Provenance verifier, coverage test, ROM-history guard and `//verify:all`.
  - `PROMPT.md` (the reproduction prompt, with v2 contract errata).
  - This report. The demo video is recorded separately, with the user.

## Boundary statement: allowed and forbidden knowledge

- **Allowed** (and used): publicly documented architecture knowledge held by the model.
  That is the MIPS R4300i ISA and its encodings, the N64 memory map, RCP register
  behaviour, and the Fast3D display-list command formats.
- **Forbidden** (and not used):
  - third-party tools: no disassembler, emulator, recompiler or decomp project;
  - game-specific knowledge: no symbol maps, function databases or decomp source;
  - decompilation of any kind.
- **Tool inventory.** Everything that touches the ROM was written in this repo: the z64
  parser, decoder, re-encoder, chunker, codemap, unitrender, stubgen, oracle, runtime,
  rcp and verify scripts. The outside pieces are only generic ones: clang (C compiler and
  integrated assembler), Bazel, SDL2, OpenGL, Python 3 (for scripts) and git.
- **Workers saw only the contract and their unit's listing.** A listing is mechanical
  decoder output: address, word and mnemonic. The Read/Write tool restriction enforced
  this, and the workers' transcripts are in the session logs.
- **ROM derivative.** `gen/` is ROM-derived: it stays local and is never published.
  `//verify:publish_check` reports 17 deliberate short excerpts, in the decoder golden test
  and the contract's worked examples, and nothing else.

## Evidence of no decompilation (`//verify:provenance`)

`bazelisk test //verify:provenance` checks every unit in `gen/units/` against its footer
table, its rendered listing and the ROM, using only the MIPS encoding and the contract's
macro vocabulary. Per-unit counts are in `gen/provenance.tsv`. It checks:
- **source order:** one `L_<pc>` label per footer row, in listing order;
- **bounded ranges:** each MIPS instruction maps to at most 24 AArch64 instructions (the
  largest is 18);
- **address-derived labels:** every label is address-derived, and local branches stay
  inside their instruction's range;
- **no code motion:** each range retires exactly once, and every memory, COP0, COP1, FPU,
  TLB and RAISE macro names its own instruction's pc, with the delay-slot flag correct;
- **CFG isomorphism:** each instruction's emitted edges equal the MIPS CFG decoded
  independently from the word;
- **words:** every provenance word equals the footer, the listing and a contiguous run of
  ROM words.

Output of the final run:

```
provenance: 535 units, 107172 MIPS instructions, 3927 ROM runs; checks: order,
bounded (<= 24), labels, motion, cfg, words; 0 unit(s) failing
```

Every one of the 3,927 runs of consecutive translated words is found contiguously in the
ROM. Its mutation self-test, run in the same target, catches all 10 injected defects:
- the real divergence-#3 output;
- a retargeted branch;
- reordered labels;
- two cases of code motion: swapped instruction bodies, and a memory access hoisted across
  an instruction boundary;
- a non-address label;
- an escaping local branch;
- altered provenance and footer words;
- an oversized range.

`bazelisk test //verify:all` runs the whole suite green: full-route lockstep, provenance,
coverage, rom_history and the opcode stubs.

## Token accounting (all phases; detail in `TOKENS.md`)

| component | tokens | basis |
|---|---|---|
| translation workers, claude-opus-5-5 | 25,616,823 exact + ~7.4M est. | harness `subagent_tokens` per call; killed calls estimated at the exact 292.3 tok/instr |
| translation workers, claude-sonnet-5 (trial) | 779,443 exact + ~1.5M est. | same; killed calls at its exact 364.6 tok/instr |
| translation workers, claude-sonnet-5-5 (trial, side measurement) | 1,159,529 exact | CLI JSON, final-turn context (same quantity) |
| **translation subtotal** | **27,555,795 exact + ~8.9M est.** | |
| phases 0–3 scaffolding (tooling, oracle, graphics) | ~25M est. | turn count × context snapshots |
| phase 4a scaffolding + pilot orchestration | ~15M est. | same |
| phase 4b orchestration (to checkpoint, then to completion) | ~32M est. | same |
| final phase (verifier, PROMPT.md, report) | ~6M est. | same |
| **total** | **~115M** (~109M through Phase 4b) | orchestration mostly prompt-cache reads |

The plan estimated 15–30M, assuming 40–60 tokens per instruction. The measured
per-instruction cost is 5–7× that, because every call re-reads the roughly 4K-word
contract and emits about 5 lines of assembly per MIPS instruction. The coordinator's
interactive planning session (Phase 0) is not included.

## Phase 4b detail

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
