# mario2mario — LLM Direct Binary Translation: MIPS (N64) → AArch64 (macOS)

## Context

The repo works against Super Mario 64 (USA), verified by SHA1 `9bef1128717f958171a4afac3ed78ee2bb4e86ce` (`rom/mario64usa.z64`, 8MB big-endian z64, entry PC `0x80246000`). The experiment (per README.md): determine how hard it is for an LLM to **directly translate a compiled binary from one platform to another**, without specialized tools or decompilation. The outcome must be a **runnable, playable native AArch64 macOS binary** (the host Mac is Apple Silicon — it is the target), with **machine-checkable evidence that no decompilation was used**, a test suite that makes correctness provable, a demo video, a report (including token spend), and a reproduction prompt.

Locked decisions:
- **Translation mechanism:** direct per-block LLM emission. The LLM reads listings and emits AArch64 itself, batched over many prompts. Scripts do only mechanical bookkeeping (byte→mnemonic decoding, assembling, linking) — zero translation logic in code.
- **Success bar:** playable gameplay — controllable Mario in a level, keyboard input, real window.
- **Runtime scaffolding:** written from scratch in this repo; only generic OS-level libs (SDL2 + OpenGL) for window/input/GL output.
- **Test oracle:** an LLM-written-from-scratch MIPS R4300i reference interpreter. No third-party emulator anywhere.
- **Worker model:** the model doing the work is **Claude Opus 5.5** (`claude-opus-5-5`). All bulk translation workers (subagent/headless `claude -p` runs) pin this model explicitly; implementation sessions should be run with the model set to Opus 5.5 (`/model`). `TOKENS.md` records the model per batch, and REPORT.md + PROMPT.md name it so the result is reproducible on the same model.

### The honest boundary of "no specialized tools or knowledge"
You cannot even parse the ROM without platform knowledge. The defensible line, stated up front and enforced throughout:
- **Allowed:** publicly documented *architecture* knowledge already in the model (MIPS ISA encodings, N64 memory map, RCP register behavior, Fast3D display-list command formats) — cited in the report.
- **Forbidden:** third-party tools (no Ghidra/objdump-for-MIPS/emulators/recompilers/sm64 decomp), game-specific knowledge (no symbol maps, no function databases, no decomp source), and any decompilation (no reconstruction of functions, variables, or high-level control flow — translation stays instruction-for-instruction).
- **Disassembly ≠ decompilation:** a self-written, mechanical bytes→mnemonic decoder is bookkeeping (bijective, no analysis). The provenance verifier (below) proves the output never rose above instruction level.

## Architecture

One repo, one hardware model, two execution engines over it:

```
rom/mario64usa.z64
        │
   src/hw/       N64 hardware model (C, shared): RDRAM, PI/SI/VI/AI/SP/DP
        │        registers & DMA, interrupt lines, deterministic Count timebase
   ┌────┴─────────────────┐
src/oracle/          src/native/
MIPS R4300i          runtime + translated code:
interpreter          gen/*.s (LLM-emitted AArch64) + dispatch,
(the test oracle)    trap-on-untranslated, checkpoint hashing
   └────┬─────────────────┘
   src/rcp/     graphics (Fast3D HLE → OpenGL), input (SDL2 → SI), audio stub
   src/tools/   mechanical only: z64 parser, MIPS decoder→listing, trace logger,
                chunker, provenance verifier, checkpoint differ
   gen/         LLM-emitted .s files + provenance.json (MIPS PC ↔ AArch64 mapping)
```

**Data-oriented design** (AGENTS.md): flat state structs (one `CpuState`, one `HwState`), table dispatch everywhere (opcode → handler table in the oracle; `PC>>2` → flat code-pointer array for indirect jumps in the native runtime), SoA for hot paths, no OOP/no inheritance, arena allocation for DL parsing. Each commit message explains its data-oriented choices. `.clang-format` at repo root; one commit per piece of functionality; clean-break (no compat shims).

### Key technical contracts (designed now; the translation prompt embeds them)

1. **Register map (asm↔asm, the core "no decompilation" evidence):** MIPS GPRs map 1:1 to AArch64 GPRs (`$1–$25` → `x1–x25` style; `$zero` → `xzr` reads / discard writes), with `x26` = RDRAM base, `x27` = `CpuState*` (HI/LO, COP0, spill for the few unmapped GPRs), `x28` = dispatch-table base. MIPS FPRs `f0–f31` → NEON `s/d` registers 1:1. Translated code never uses the C ABI; a single trampoline (save/restore everything) is the only bridge to the runtime.
2. **Memory, endianness & TLB:** N64 is big-endian, host is little-endian. RDRAM is a big-endian byte array shared with the oracle (`HwState.rdram`, `src/hw/hw.h`): 8-bit accesses are plain byte loads/stores at the physical offset; 16/32/64-bit accesses load/store and byte-reverse (`REV16`/`REV`). KSEG0/KSEG1 → mask `0x1FFF_FFFF` + `x26`. `lwl/lwr/swl/swr` get exact per-case sequences (libultra `bcopy` uses them — must be right). **TLB (normative, found in Phase 2):** SM64 does use TLB-mapped addresses — its title-screen head code maps segment 4 (virtual `0x04000000`) with `osMapTLB`. KUSEG/KSSEG/KSEG3 accesses must go through the same 32-entry TLB lookup as the oracle (`translate()` in `src/oracle/cpu.c`): VPN2 match under PageMask, ASID or Global, even/odd page select, V and D checks, and TLB refill (vector `0x8000_0000`), invalid and modify exceptions with BadVAddr/Context/EntryHi updated identically. `tlbwi/tlbwr/tlbr/tlbp` operate on `CpuState.tlb`.
3. **Control flow:** every MIPS instruction address is a label `L_<addr>`. Direct branches → `b`/`b.cond` to labels; delay slots handled by instruction scheduling per a fixed recipe (including branch-likely); `jr/jalr` → indexed load from the flat dispatch array; unmapped entry → trap that logs the PC and exits (feeds the next translation batch).
4. **Interrupts & OS:** no HLE of libultra — the ROM's own exception handler (`0x8000_0180`) and thread scheduler get translated and run natively. Runtime sets a pending flag (VI, SP/DP/PI/SI completion, Compare timer). **Normative sampling rule (`src/hw/timebase.h`):** interrupts are taken only at dynamic block boundaries — right after the delay slot of any branch/jump (taken or not), after a nullified branch-likely, after `eret`, and after exception entry — so translated code checks the pending flag at every block exit, not only back-edges, and calls the vector trampoline. `mtc0/mfc0/eret` translate to `CpuState` field ops per contract.
5. **Determinism (what makes lockstep possible):** normative definitions live in `src/hw/timebase.h` (retired-instruction count, `Count = icount + offset` at one tick per instruction, Count==Compare → IP7, VI every 781,250 retired instructions, DMA/RSP-task completion times, interrupt sampling points) and `src/oracle/checkpoint.h` (checkpoint hash word order, hash function, 24-byte record format, emitted at each block boundary before any interrupt is taken). Both engines implement these identically. Input comes from a recorded script (`tests/inputs/*.rec`) or live keyboard. Same ROM + same input script ⇒ bit-identical execution and identical checkpoint streams in both engines.
6. **Boot:** run the ROM's own IPL3 (it's MIPS code at ROM `0x40`) in both engines — even boot is translated code. Fallback if IPL3 fights us: documented HLE boot (copy 1MB from ROM `0x1000` to RAM, jump to `0x80246000`, CIC register seed), flagged in the report.

## Phases

Each phase ends with tests green and one-or-more commits. `TOKENS.md` ledger updated at every phase boundary (see Reporting).

### Phase 0 — Repo bootstrap
`git init`; **PLAN.md** (this document); `.gitignore` (`rom/`, `*.z64`, `*.n64`, `*.v64`, `bazel-*`); build system is **Bazel via bazelisk**: `.bazelversion`, `MODULE.bazel` (bzlmod, `rules_cc`; SDL2 via system/`new_local_repository`), per-package `BUILD.bazel` files, macOS arm64 config in `.bazelrc`; `.clang-format`. Mechanical pipeline steps (listing generation, chunking, assembling `gen/*.s`) become `genrule`/custom-rule targets so the whole pipeline is `bazelisk build`-driven and cacheable; the ROM is read at runtime/test time via absolute path or `--sandbox_writable_path`-style access, never as a checked-in source.

**ROM must never enter git history (hard requirement):** both copies (`mario64usa.z64` at repo root and `rom/mario64usa.z64`) stay untracked via `.gitignore`; a `pre-commit` hook (installed in Phase 0, checked into `hooks/` with `core.hooksPath`) rejects any staged blob whose SHA1 matches the ROM or whose content looks like a z64/n64/v64 header; never `git add -f` a ROM; the verify suite (`bazelisk test //verify:rom_history`) additionally scans `git rev-list --objects --all` to prove no ROM-hashed blob exists anywhere in history. Translated `gen/` output is derivative of the ROM — kept in the local repo only, not for distribution (one line in report).

### Phase 1 — Mechanical tooling (`src/tools/`)
- z64 parser (header, IPL3 extraction).
- MIPS decoder → listing (`addr  hexword  mnemonic operands`), written from the ISA spec, no libs. **Test:** re-encode every decoded instruction back to bytes; bijectivity over the whole ROM.
- Chunker: splits a PC-region into basic blocks / ~200-instruction translation units with block metadata (entries, exits, jr sites).

### Phase 2 — Hardware model + oracle interpreter (`src/hw/`, `src/oracle/`)
The R4300i interpreter: full integer ISA, FPU (singles/doubles, all `cvt`/rounding modes, `sqrt`), COP0 subset, exceptions/interrupts; hardware model: RDRAM (8MB), PI DMA from ROM, SP/DP/VI/AI/SI registers, SP DMEM/IMEM as memory, interrupt lines, deterministic Count.
- **Tests:** per-opcode randomized unit tests (fixed-seed register/memory fuzz per opcode; asserts against hand-computed semantics for tricky ones: `lwl/lwr`, `div` edge cases, FPU rounding). Boot milestone test: IPL3 → game boots, threads start, first SP graphics task submitted (assert on task pointer + command-buffer hash).
- **Exit criterion:** ROM runs interpreted to the point of continuously submitting display lists and reading controller input. (This de-risks everything downstream and *is* the oracle.)

### Phase 3 — Graphics/input runtime (`src/rcp/`)
Fast3D (SM64's microcode) HLE display-list interpreter → OpenGL via SDL2: matrices, `G_VTX`, `G_TRI1`, textures/TMEM formats (CI/IA/RGBA), combiner approximation, two-cycle basics; VI presents at 30fps. SDL keyboard → SI controller (analog stick emulation via key ramp). Audio: AI stubbed silent (**stretch:** HLE audio synth).
- **Tests:** golden-image tests — run the *oracle* N frames with a recorded input script, hash SP task command buffers, and screenshot-compare rendered frames against committed goldens (tolerance-based).
- **Exit criterion:** SM64 title screen and gameplay are playable *under the oracle interpreter* (slow is fine). Now the only untested variable left is the translation itself.

### Phase 4 — Translation pipeline (the experiment core)
- **`PROMPT-translate.md`:** the fixed translation prompt — embeds the contracts above; input = one chunk's listing; output = AArch64 `.s` with one provenance comment per line (`// 0x802461A4: 8C450000 lw a1,0(v0)`). This file is *both* pipeline component and reproduction artifact.
- **Trace-guided coverage:** oracle runs the input scripts (boot→title→file select→castle→level→gameplay) logging every executed PC and every DMA that lands code in RAM (segment = ROM src + RAM dest). Only reachable code gets translated. New PCs found at runtime (trap log) feed the next batch.
- **Batched emission:** chunks fed through repeated prompts (subagent/headless `claude -p --model claude-opus-5-5` workers, each given `PROMPT-translate.md` + one chunk; sanctioned by the mechanism decision). The orchestrating session spot-audits and hand-translates the nastiest blocks (exception vectors, `lwl/lwr` clusters, FPU-dense math) itself. All outputs land in `gen/`, assembled by clang's integrated assembler.
- **Estimated volume:** reachable code for full gameplay ≈ 200k–400k MIPS instructions ⇒ **~10–20M tokens for emission alone** (≈40–60 tokens/instruction amortized in+out). See Reporting.

### Phase 5 — Verification (the "provable" part)
1. **Per-opcode semantic tests:** for every distinct opcode+form in the reachable set, run the LLM's translation of a representative instruction as an isolated stub against the oracle over fixed-seed randomized states. Catches systematic mapping bugs; kills the shared-misunderstanding risk at the *mapping* level.
2. **Lockstep differential runs:** native binary in checkpoint mode hashes (GPRs, HI/LO, FPRs, COP0, dirty pages) at block boundaries; oracle produces the same stream from the same input script; `src/tools/` differ pinpoints the first divergent block → dump listing + emitted asm → re-translate → regress. This is the core proof loop and the main debugging engine.
3. **Frame-level equivalence:** same input script ⇒ identical sequence of SP-task command-buffer hashes in oracle and native. Proves the native build produces bit-identical frames without pixel comparison.
4. **Coverage ledger:** every executed PC is translated-and-lockstep-verified, or listed as outstanding. Report includes final counts.
5. **Provenance verifier (`src/tools/provenance`):** machine-checks the no-decompilation claim over `gen/`: every MIPS instruction maps to a bounded AArch64 range in source order; branch graph is isomorphic to the MIPS CFG; labels are only address-derived; no code motion across block boundaries. Runs as a Bazel test target in the verify suite.

### Phase 6 — Playable native milestone
Iterate: run native → trap on untranslated PC → translate batch → lockstep-verify → repeat, down the path boot → logos → title → file select → castle grounds → level entry → controllable Mario. Then unlock speed: checkpoint mode off, VI-synced 30fps (native should exceed it trivially). **Definition of done:** play Mario with the keyboard in a level, in a window, for minutes, no lockstep divergence on the recorded scripts.

### Phase 7 — Deliverables
- **Binary:** `bazelisk build //native:mario64` → `bazel-bin/native/mario64` (native arm64 macOS; reads `rom/mario64usa.z64` at runtime for assets/DMA).
- **Video:** `demo/demo.mp4` — screen-recorded (`screencapture -v` / ffmpeg): native binary booting, title, live keyboard gameplay; then `bazelisk test //verify:all` running the lockstep + provenance suites green; brief shot of `gen/*.s` provenance comments beside the MIPS listing.
- **REPORT.md:** what worked / what fought back / what was HLE'd vs translated; boundary statement + evidence for no-decompilation (provenance verifier output, tool inventory — everything authored in-repo, session logs pointer); **token accounting** from `TOKENS.md`.
- **PROMPT.md:** self-contained reproduction prompt, binary-agnostic (parameterized on ROM path + target triple): phases, contracts, `PROMPT-translate.md` inline, verification protocol.

## Reporting & token accounting
`TOKENS.md` ledger, updated at every phase boundary and every translation batch: date, phase, mechanism (interactive session / subagent / headless), exact counts where the harness or API reports them, otherwise estimates with the estimation basis (instructions × tokens-per-instruction; session-size snapshots). Report distinguishes **translation tokens** (the experiment's headline number) from **scaffolding tokens** (tooling/runtime/debugging). **Whole-project estimate: ~15–30M tokens** (10–20M emission + 5–10M scaffolding/debugging/repair) — this exceeds one session; work spans sessions with the ledger carrying continuity.

## Risks
- **FPU exactness** (rounding modes, `cvt`, `sqrt` bit-exactness on NEON): mitigated by per-opcode fuzz tests early (Phase 2/5.1).
- **Endianness edge cases** (`lwl/lwr/swl/swr`): hand-audited sequences + dedicated unit tests.
- **Timing-sensitive waits:** SM64 waits on message queues (event-driven), so defined DMA/interrupt boundaries should suffice; deterministic Count is the linchpin — any drift breaks lockstep, so it's tested first.
- **Fast3D HLE fidelity:** wrong pixels don't break the correctness proof (frame equivalence is command-buffer-level), only the demo's looks; iterate visually.
- **Translation worker error rate:** whole point of the experiment — measured, not hidden; lockstep catches everything, error rates go in the report.
- **Volume/cost:** biggest risk is tokens/time. Milestone order guarantees partial results are still demonstrable (oracle-playable at Phase 3; title-screen-native before full gameplay).

## Verification (end-to-end)
`bazelisk test //...` = decoder bijectivity + opcode fuzz + boot milestone + golden frames. `bazelisk run //tools:lockstep -- tests/inputs/gameplay.rec` = differential proof. `bazelisk test //verify:all` = lockstep + provenance + coverage ledger + ROM-history guard. Final acceptance = the video's content: live gameplay + green `//verify:all`.

## Implementation order
1. Phase 0 commits (PLAN.md, gitignore + pre-commit ROM guard, clang-format, bazelisk scaffolding: `.bazelversion`, `MODULE.bazel`, `.bazelrc`, root `BUILD.bazel`).
2. Phase 1 decoder + bijectivity test over the full ROM.
3. Phase 2 oracle skeleton: CpuState/HwState structs + opcode table + first 50 opcodes + fuzz harness.
