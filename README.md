# mario2mario

This is an experiment to see how hard it is for an LLM to directly translate a compiled binary from one platform to another, without using specialized tools or knowledge provided by a human.

Typically people would want to use some sort of tooling to statically recompile a binary from host to target.

The question is, is it actually reasonably feasible to just let the LLM handle that nowadays? That would mean

* Low-ish token spend
* ~A weekend "one shot" project
* Little to no intervention by an expert human

also half the text in this repo is shameless claude slop because this is basically a shitpost so like, i dunno man

## What this repository is

This is the source, the full commit history and the results of one run of that experiment.
In that run, an LLM (Claude Opus 5.5) translated Super Mario 64 (USA, N64, MIPS R4300i)
into native AArch64 for Apple Silicon macOS. It worked instruction by instruction, with no
decompiler, emulator or third-party translation tool.
- **Coverage:** every CPU instruction the recorded gameplay route executes was translated:
  107,154 distinct code words in 535 units.
- **Correctness:** the native build runs byte-identical to a MIPS reference interpreter,
  written from scratch in this repo, over 10 billion instructions.
- **Speed:** it plays live at full speed.

Everything else in the repo was also written in it: the hardware model, the oracle
interpreter, the runtime, the graphics front end (display lists → OpenGL) and the
verification suite.

- **[REPORT.md](REPORT.md)**: results, architecture, the boundary on what knowledge was
  allowed, the no-decompilation evidence, divergences found and fixed, the model trial, and
  token accounting.
- **[TOKENS.md](TOKENS.md)**: the token ledger (about 27.6M exact translation-worker tokens;
  about 115M in all).
- **[PROMPT.md](PROMPT.md)**: a self-contained prompt to reproduce the experiment on your own
  ROM.
- **[PROMPT-translate.md](PROMPT-translate.md)**: the frozen worker contract every translated
  unit was written against.
- **[PLAN.md](PLAN.md)**: the original plan.

## Stuff not included:

- any actual translated AArch64 could be considered a ROM derivitave, so it is not included.
- images of game frames
- the ROM

You can use this repo as a reference to recreate the experiment, which isn't too hard with a
modern LLM.

The history keeps its original commits and messages, including the commits that only
touched removed files; those are now empty. The accounting tables the report cites are
included at the tip: `gen/worker_ledger.tsv`, `gen/coverage.tsv`, `gen/provenance.tsv` and
the trial tables. They contain no ROM content.

`//verify:publish_check` checks this. It scans every tracked file and every blob in the
history for runs of ROM words. Seventeen deliberate short excerpts are allowlisted line by
line in `verify/publish_allowlist.tsv`:
- 12 decoder golden-test vectors in `src/tools/decoder_golden_test.c`;
- 5 lines of worked examples in `PROMPT-translate.md`.

## Building and testing

You need macOS on Apple Silicon, [bazelisk](https://github.com/bazelbuild/bazelisk), Homebrew
SDL2, and **your own legally obtained ROM** at `rom/mario64usa.z64`. That path is gitignored.
Alternatively, pass `--repo_env=M2M_ROM=/path/to/rom`, although the two repository-scanning
checks expect the ROM at `rom/` inside this checkout.
The expected SHA1 is `9bef1128717f958171a4afac3ed78ee2bb4e86ce`.

```
bazelisk test //...          # tools, hardware model, oracle, graphics/input, publish + ROM-history checks
bazelisk run //oracle:play   # the game under the reference interpreter, in a window
```

With no translated units, `//native:run` and `//native:play` build and stop at the first
untranslated instruction. The tests that need translations are tagged `manual` and say
why: the lockstep tests, `opcode_stub_test`, `//verify:provenance`, `//verify:coverage` and
`//verify:all`.

## Reproducing the translation

Follow [PROMPT.md](PROMPT.md). It lays out:
- the phases;
- every contract (timebase, checkpoints, register map, memory and TLB, control flow, unit
  format);
- the worker prompt, the validation gates, and the lockstep and repair protocol;
- the model findings and the measured token costs.

Its pipeline captures what your ROM executes over a recorded route and renders
translation units with the in-repo mechanical tools. LLM workers then translate them
against `PROMPT-translate.md`; apply the v2 errata in PROMPT.md first. Once `gen/units/` is
regenerated, `bazelisk test //verify:all` runs the full suite: full-route lockstep,
provenance, coverage, ROM history and opcode stubs.

## License

MIT; see [LICENSE](LICENSE). The license covers the code and documents in this repository only.
It grants nothing regarding Nintendo's ROM, the game, or any output regenerated from the ROM.
