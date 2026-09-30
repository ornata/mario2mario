# PROMPT.md: reproduce an LLM-direct binary translation (MIPS R4300i ROM → native AArch64)

This is the self-contained prompt for rerunning the mario2mario experiment on any
big-endian MIPS R4300i ROM (N64 z64). It targets any AArch64 host triple. The reference
run used Super Mario 64 (USA) → `aarch64-apple-darwin` (Apple Silicon macOS); its measured
results are in `REPORT.md` and `TOKENS.md`. Every number below that is labelled
*measured* comes from that run.

## Parameters

| name | meaning | reference run |
|---|---|---|
| `ROM` | path of the ROM image; it must never enter git | `rom/mario64usa.z64` |
| `ROM_SHA1` | SHA1 of the image, checked before anything else | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` |
| `TRIPLE` | host target; translated code is its assembly | `aarch64-apple-darwin` (Mach-O, clang integrated assembler) |
| `ROUTE` | recorded controller input (`.rec`) that defines the footprint to translate | `tests/inputs/gameplay.rec` (10B instructions: boot, title, file select, castle, level, gameplay) |
| `WORKER_MODEL` | model for translation workers (pin the exact id; verify it, see §6) | `claude-opus-5-5` |

The entry PC, the IPL3 location and the boot segment are read from the ROM header by the
in-repo parser. None of them is a parameter.

## 0. The question and the rules

**Question.** How hard is it for an LLM to translate a compiled binary directly from one
ISA to another, with no specialized tools and no decompilation? The result must be a
runnable, playable native binary, backed by machine-checkable evidence.

**Hard rules**

1. **The ROM never enters git history.**
   - `.gitignore` the ROM and `*.z64 *.n64 *.v64`.
   - Install a `pre-commit` hook (`hooks/`, `core.hooksPath`) that rejects any staged blob
     whose SHA1 equals `ROM_SHA1` or that starts with a z64/n64/v64 magic.
   - Never `git add -f` a ROM.
   - `//verify:rom_history` scans the whole object store.
   - Translated output is ROM-derived: it stays local and is never published.
     `//verify:publish_check` scans tracked files outside `gen/ tests/goldens/ rom/ out/`
     for runs of ROM words.
2. **Allowed knowledge:** publicly documented architecture knowledge, namely the MIPS ISA
   encodings, the N64 memory map, RCP register behaviour and the display-list microcode
   command formats.
3. **Forbidden:**
   - third-party tools (no disassemblers, emulators, recompilers or decomp projects);
   - game-specific knowledge (no symbol maps, function databases or decomp source);
   - any decompilation: no reconstruction of functions, variables or high-level control
     flow. Translation stays instruction-for-instruction.
4. **Disassembly is bookkeeping, not decompilation.** An in-repo, mechanical, bijective
   bytes→mnemonic decoder is allowed. It is proven bijective by re-encoding the whole ROM.
5. **No translation logic in code.** Scripts do only mechanical bookkeeping: decoding,
   listing, chunking, rendering worker inputs, assembling, linking and checking. Every
   AArch64 instruction of translated code is written by an LLM worker.
   - The orchestrator may hand-translate a unit only after one failed re-spawn, and must
     flag that unit in the ledger (`hand=1`).
   - Never build an automated loop that invokes model CLIs. In the reference run such a
     driver was blocked; the approved mechanism is harness subagents, plus individually run
     and inspected one-off CLI calls when the user approves them.
6. **Engineering style.** Use C11 and Bazel (bazelisk), with `.clang-format` for C and C++.
   Make one commit per piece of functionality, take a clean-break approach (no
   compatibility shims), and prefer data-oriented design: flat state structs, table
   dispatch, and no object orientation. Each commit message explains its data-oriented
   choices.

## 1. Architecture (one hardware model, two engines)

```
ROM ─ src/tools/  z64 parser, MIPS decoder/listing/re-encoder, chunker, codemap, unitrender
      src/hw/     hardware model shared by both engines: RDRAM (big-endian byte array),
                  PI/SI/VI/AI/SP/DP registers and DMA, interrupt lines, deterministic timebase
      src/oracle/ MIPS R4300i interpreter written from scratch: the test oracle
      src/native/ runtime + LLM-written AArch64 units (gen/units/*.s): dispatch,
                  content-keyed lazy linking, trap-on-untranslated, checkpoint hashing
      src/rcp/    display-list HLE → OpenGL (SDL2 window), keyboard → controller, .rec replay
      verify/     provenance verifier, coverage, ROM-history, publish check, //verify:all
```

## 2. Contracts (normative; design them before translating)

1. **Timebase** (`src/hw/timebase.h`):
   - `icount` = retired instructions; `Count = icount + offset` (one tick per instruction);
     `Count == Compare` → IP7.
   - VI every 781,250 retired instructions, and fixed DMA and RSP-task completion times.
   - Interrupts are sampled only at dynamic block boundaries: after the delay slot of any
     branch or jump (taken or not), after a nullified branch-likely, after `eret`, and after
     exception entry.
     - The oracle must treat the vector itself as a boundary. The reference run's first
       divergence was an oracle bug here.
2. **Checkpoint stream** (`src/oracle/checkpoint.h`):
   - A 24-byte record (icount, pc, hash of the architectural state) at every N-th block
     boundary, written before any interrupt is taken.
   - Both engines write it identically, so the same `ROM` and `ROUTE` give byte-identical
     streams.
3. **Register map:**
   - MIPS GPRs map 1:1 to AArch64 GPRs where possible. The rest spill to fixed state
     offsets. RDRAM base, state pointer and icount each get a dedicated register, and there
     are fixed scratch registers.
   - Translated code never uses the C ABI. One trampoline is the only bridge to runtime
     services.
4. **Memory:**
   - Endianness is handled per access: byte-reverse 16/32/64-bit accesses, with exact
     `lwl/lwr/swl/swr` sequences.
   - KSEG0/1 are masked.
   - KUSEG/KSSEG/KSEG3 go through the same TLB lookup and exceptions as the oracle
     (refill, invalid, modify).
5. **Control flow:**
   - Every MIPS instruction address is a label.
   - Delay slots and branch-likely are handled by fixed recipes.
   - `jr/jalr` dispatch through a flat per-physical-word code-pointer table.
   - Unmapped entries trap and log (the trap log feeds the next batch).
6. **Interrupts and OS:** no HLE of the OS. The ROM's own exception handler and scheduler
   are translated and run natively.
7. **Boot:** seed state with a documented PIF HLE, then run the ROM's own IPL3 as
   translated code.
8. **Units:**
   - A unit is about 200 instructions of contiguous executed code, rendered mechanically
     with a verbatim header and footer.
   - The footer holds one `(pc, word, entry offset | -1 for a delay slot)` row per
     instruction.
   - Units self-register through a data section, so no build step knows their names.
   - The runtime links a unit lazily per contiguous run, keyed by content (word match),
     and unlinks it on stores to code or on DMA.
   - A delay-slot row never takes over a word owned by another unit.

## 3. The translation contract (worker prompt)

**`PROMPT-translate.md`** is the complete worker contract, frozen for the reference run at
commit `1e25877` (4,036 words). It covers:
- input format (§1);
- machine-state and register contract with fixed offsets (§2);
- group structure and retire counting (§3);
- runtime macros, the only runtime interface (§4);
- memory access (§5), arithmetic recipes (§6) and control-flow recipes (§7.1–7.7);
- COP0 (§8) and COP1/FPU (§9);
- forbidden constructs (§10), output (§11) and the UNTRANSLATABLE rule (§12);
- three worked examples (§13).

Copy it verbatim into the new repo, then apply the v2 errata in §9 below before freezing.
Freeze it by commit hash before the first worker runs. Any change after that is a new
contract version, and units written under the old version stay as they are.

This is the worker prompt, verbatim, with one call per 10 units:

```
You are a translation worker in the mario2mario experiment (MIPS R4300i -> AArch64 direct translation by an LLM).

Your only inputs are these files. Read them in full with the Read tool, and read nothing else:
1. <repo>/PROMPT-translate.md -- the translation contract. Follow it exactly.
2. Your units, each a separate translation unit, at <repo>/gen/work/full/<ID>.in.txt for these IDs:
<10 unit ids>

Translate each unit separately and completely, purely from its own listing, instruction by instruction, as the contract specifies, and use the Write tool to write exactly the two output files the contract names for each unit (under <repo>/gen/units/; overwrite any existing file). Rules:
- Use no tools other than Read (for those files only) and Write (for those output files only). No Bash, no search, no web, no other files.
- Copy each unit's HEADER and FOOTER blocks verbatim.
- If anything in a unit is outside the contract, do not guess: write no files for that unit and report its UNTRANSLATABLE line.
- When finished, reply with one line per unit: DONE <id> or the UNTRANSLATABLE line.
```

## 4. Phases and exit criteria

| phase | build | exit criterion (tests green, committed) |
|---|---|---|
| 0 | repo, `.gitignore`, pre-commit ROM guard, Bazel (bzlmod), `.clang-format`, PLAN | `bazelisk build //...` |
| 1 | z64 parser; MIPS decoder → listing (`addr hexword mnemonic operands`); re-encoder; chunker | the decoder is bijective over the whole ROM |
| 2 | hardware model + oracle interpreter (full integer ISA, FPU with all rounding modes, COP0 subset, TLB, exceptions, interrupts; deterministic Count) | per-opcode randomized tests; boot milestone (IPL3 → threads → first graphics task, pointer and hash asserted) |
| 3 | display-list HLE → GL; keyboard → controller; `.rec` record/replay; `//oracle:play` | golden frames and task-hash goldens; the game is playable under the oracle (slow is fine) |
| 4 | native runtime + trampolines + macros; exec-words capture; `codemap` → `unitrender`; worker batches | the pilot units run in lockstep with the oracle; a per-opcode stub harness is in place |
| 5 | verification: stub harness over every opcode form; lockstep; frame (task-hash) equivalence; coverage ledger; provenance verifier | `//verify:all` green |
| 6 | full-route translation to coverage; `//native:play` | full-`ROUTE` lockstep byte-identical; live play at 60 fields/s |
| 7 | `REPORT.md`, `PROMPT.md`, `TOKENS.md`, demo video | deliverables committed |

## 5. Pipeline, validation gates, lockstep protocol

1. **Capture.** Run `oracle/run --replay ROUTE --exec-words` to get every distinct
   (vaddr, word) the route executes, with its first icount. Cache the reference streams
   with `--checkpoints ... --checkpoint-stride 1000 --task-hashes ...`.
2. **Subtract and render.** Subtract the pairs already covered by unit footers, then run
   `codemap` (runs of consecutive words per version) and `unitrender` (units of about
   200 instructions, plus header and footer).
3. **Batch in route order.** Order units by first-execution icount and call them 10 at a
   time. The batching trial measured tokens per instruction at 471.5 for 1 unit per call,
   down to 267.2 at 10 and 263.2 at 15. Run waves of about 8 calls. Commit after every
   wave, staging validated unit files **by name only**, never the directory, while workers
   are running.
4. **Validation gates** (per unit, before it enters the build):
   - it assembles;
   - there is exactly one `L_<pc>:` label and one provenance comment `// <pc>: <word>` per
     listing row;
   - the header is verbatim at the top and the footer verbatim at the end;
   - it uses no forbidden registers or instructions (x18, sp, bl/blr/br/ret, svc,
     directives in code);
   - every group retires once;
   - the json pc list equals the listing;
   - each fallthrough target is the last pc + 4, or + 8 after a branch-likely;
   - no scratch register (x16/x17/x30) is read after any macro before being written;
   - no flags are read after a macro or label without a fresh compare.
5. **Lockstep extension.**
   - Every few waves, run the native build over the full `ROUTE` and prefix-compare the
     stride-1000 stream and the task hashes with the oracle's.
   - On a mismatch, rerun both engines with `--checkpoint-stride 1 --checkpoint-from <last
     matching icount>` over a window of a few thousand instructions. Find the first
     differing record, read that block's unit, and fix it.
   - Scan all units for the same defect class, and add it as a validation gate.
6. **Repair.**
   - Re-prompt a failed unit once, on the strongest model and with the same prompt.
   - If the re-prompt repeats the defect, hand-fix it minimally and flag it.
   - A contract gap (e.g. a branch into a delay slot) is solved structurally, by re-rendering
     or splitting units, not by hand translation.
7. **Coverage.** `gen/work/coverage.sh` requires 0 uncovered executed pairs and an ok
   ledger row for every unit.
8. **Provenance.** `//verify:provenance` checks every unit against its footer, its listing
   and the ROM, and then runs its mutation self-test. It checks:
   - source order;
   - per-instruction ranges of at most 24 instructions;
   - address-derived labels, with local branches confined to their own range;
   - one retire per range, and every pc-carrying macro naming its own pc and delay-slot
     flag;
   - emitted CFG edges equal to the decoded MIPS CFG;
   - provenance words equal to the footer, the listing and a contiguous ROM run.
9. **Ledger.** `gen/worker_ledger.tsv` has one row per unit attempt: unit, instructions,
   worker_tokens, tool_uses, wall_s, attempt, result, hand, model. Record the exact
   harness-reported tokens per call, apportioned by instruction count. Use `NA` for calls
   killed before they reported. Put incidents in `# KILLED`, `# ESTIMATE`, `# DIVERGENCE`,
   `# DECISION` and `# WASTE` rows.

## 6. Worker mechanism and model findings (measured)

- **Mechanism.** Use harness subagents (the Agent tool). Each gets only the worker prompt
  above, so it reads nothing but the contract and its inputs. `subagent_tokens` equals the
  worker's final-turn context size, so use that same quantity when comparing CLI calls.
- **Verify the model.** Read the id the harness actually sent to the API from each
  worker's transcript. Aliases lag releases: `"sonnet"` still resolved to claude-sonnet-5
  after claude-sonnet-5-5 shipped.
- **claude-opus-5-5 (baseline):**
  - 545/545 first attempts validated;
  - 542/545 correct under lockstep;
  - 2 correct UNTRANSLATABLE reports;
  - 292 tokens per instruction.
- **claude-sonnet-5: do not use for bulk.**
  - 3/28 validated.
  - It emits `add wN, wzr, #imm` for `addiu` from `$zero`, which is unencodable, despite
    worked example 2.
  - It also stored a clobbered `w16` after `M2M_FPU_END`, which caused a divergence.
  - One worker died on the 64K output-token limit.
- **claude-sonnet-5-5 (candidate):**
  - 20/20 validated and lockstep-clean;
  - 279 tokens per instruction;
  - the sample is small (a 95% lower bound of about 86%).
  - Trial it on about 30 units first, then apply the rule: the cheapest model near Opus's
    pass rate takes the bulk, and Opus keeps repairs.
- **Usage limits.** Session and weekly limits kill calls mid-wave (six cuts in the reference
  run). After any cut:
  - reconcile from disk and the ledger only;
  - re-validate every file written after the last commit;
  - keep complete, valid units with `NA` tokens;
  - discard partial files (a `.s` without its `.json` counts as partial);
  - re-spawn the rest.

## 7. Token expectations (measured, reference run)

- **Footprint:** 107,154 distinct executed words; 535 units; about 107K instructions,
  including repairs and re-renders.
- **Translation workers:** 27.6M exact, plus about 8.9M estimated for killed calls, which
  is roughly 300–340 tokens per instruction all-in at 10 units per call.
- **Orchestration and scaffolding** (phases 0–7: tooling, oracle, graphics, runtime,
  validation, lockstep debugging): about 72M estimated.
- **Whole experiment:** about 108M tokens. The plan's 15–30M estimate assumed 40–60
  tokens per instruction; with a 4K-word contract re-read per call, the measured
  per-instruction cost is about 5–7× that.

## 8. Deliverables and acceptance

- `//native:play` (window, keyboard) and `//native:run` (headless), both reading `ROM` at
  runtime.
- `bazelisk test //verify:all` green: full-route lockstep, provenance, coverage,
  rom_history and opcode stubs.
- `REPORT.md` (results, boundary statement, no-decompilation evidence, divergences, model
  trial, token accounting), `TOKENS.md` (ledger), `PROMPT.md` (this file) and a demo video
  (recorded with the user's screen-recording permission).

## 9. v2 contract errata (apply before freezing a new contract; v1 stays frozen at `1e25877`)

1. **§9 conversions to integer** (`cvt.w.*`, `cvt.l.*`, `trunc/round/ceil/floor.*`):
   - v1 stores `w16`/`x16` to `S(fd)`/`D(fd)` after `M2M_FPU_END`, but the register table
     says every macro clobbers x16.
   - v2: move the integer result into `s0`/`d0` (`fmov s0, w16` or `fmov d0, x16`)
     **before** `M2M_FPU_END`, and store `s0`/`d0` after it.
   - Every Opus and Sonnet 5.5 worker already did this. A worker that followed v1 literally
     diverged.
2. **§7.1 fallthrough after a gap-terminated delay slot.** Add a worked example of a
   conditional branch whose delay slot is the last listed instruction before
   `--- gap ---`:

   ```
   Lnt_<B>:
       M2M_FALL 0x<B+8>
       M2M_FALLTHROUGH 0x<B+8>        // B+8 is not in the unit
   ```

   State the target as B+8, *not* the next listed address. Three Opus units emitted B+0xC
   here, and one repeated it on re-prompt.
3. **§6 `$zero` as the source of an immediate add or subtract.**
   - State the rule, not just the example: register 31 in an add/sub-immediate encoding
     is `sp`, so `add wN, wzr, #imm` is invalid.
   - Emit `mov w16, #imm` (or `movz/movk`) and then the register form.
   - It was 137 of Sonnet 5's errors.
4. **Gates in the contract text.** List the validator's gates in §11: the fallthrough rule,
   no scratch read after a macro, and no flags read across a macro. Workers can then
   self-check, even though they cannot run the assembler.
