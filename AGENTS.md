# Editing PTSan

These instructions apply to PTSan work in this LLVM fork, including the compiler
pass, compiler-rt runtime, and associated tests. They capture standing editing
preferences; feature-specific plans and temporary limitations belong with the
relevant work.

PTSan targets security-relevant bugs. It is not a general undefined-behavior
detector: C or C++ undefined behavior alone is not a reason to require detection.

## Consistency comes first

- Read analogous implementations and their callers before choosing an approach.
  Use existing infrastructure and follow local coding practices, including where
  responsibilities live. Review the whole delta for consistency.
- Before adding a new mechanism or fallback, check whether existing machinery
  should already handle the case. Investigate missed optimizations and violated
  invariants at their source rather than adding another path around them.
- Keep PTSan-specific complications inside PTSan where possible and minimize
  changes to shared LLVM infrastructure. Follow LLVM conventions for build and
  tooling integration as well as for code.
- Do not introduce a different approach for one member of an existing family
  merely because it seems better in isolation. If there is a concrete technical
  reason to depart from the pattern, explain that reason to the user.
- The current implementation is a starting point, not a fixed design. Suggest
  changes to existing infrastructure when they would simplify the requested
  work. Explain the proposed change and its benefit rather than silently
  expanding the task into a refactor.
- Prefer direct code with obvious control flow. Use explicit loops and shared
  cursors when they make a traversal easier to follow. Hoist invariants; avoid
  redundant state, speculative helpers, and unnecessary special cases.
- Comments should explain non-obvious reasons and invariants. Preserve the user's
  comments and edits, and avoid unrelated reformatting.

## Phase responsibilities and shared infrastructure

The compiler pass lives in `llvm/lib/Transforms/Instrumentation/PTSan/`.

- Keep discovery, optimization, and lowering separate. Discovery records the
  operations to handle; lowering translates them into their implementation.
  Both should be as simple and mechanical as possible. Analysis, inference, and
  simplification belong in the optimization passes. Avoid discovery-order
  dependencies caused by doing that work too early.
- Choose existing patterns by semantic role, including which layer owns a
  transformation. Use shared construction and recognition infrastructure; avoid
  duplicating behavior that it already provides.
- Represent new dataflow through ordinary IR values and operands. Do not add
  parallel metadata bookkeeping for information already available in the IR.
  Existing metadata mechanisms have specific purposes; reuse them only when
  those purposes actually match the task.

## Reason from LLVM IR

- Work from the IR the pass actually sees. Use concrete IR examples and inspect
  optimized compiler output when reasoning about source-language behavior.
  O0 is unsupported; evaluate concerns at a supported optimization level.
- Check LLVM APIs and existing helpers before reimplementing behavior. Verify
  actual type and operand constraints instead of assuming them from C syntax.
- GEP `inbounds` and nowrap flags express optimizer assumptions, not runtime
  validation. They do not establish PTSan's required proof that an access or
  pointer computation is safe. Consult the existing bounds/polarity analysis
  and tests when changing GEP handling.
- Prefer one correct path over special cases that only appear simpler locally.
  Check the actual constraints before deciding whether a special case is needed,
  and comment on non-obvious reasons for the chosen approach.

## Optimization and complexity

- Ordinary optimizations must preserve the selected security mode's detection
  guarantees and valid-program behavior: no new false negatives or false
  positives. Deliberately changing protection is a separate, explicitly
  requested mode or experiment.
- Before implementing an optimization, state what the remaining instrumentation
  already guarantees and exactly what the compiler still has to prove. For
  pointer and bounds reasoning, say which pointer each offset is relative to.
  Put that explanation in the code when it makes the implementation easier to
  read.
- Before handing back a diff, challenge each non-obvious guard or special case.
  What concrete behavior would go wrong without it? Which guarantee would that
  violate? Follow the case through the existing machinery. If the guard exists
  to satisfy a helper's precondition, check whether the algorithm actually
  needs that helper. Raise unresolved assumptions before encoding them in a
  regression test.
- Substantial optimization complexity needs demonstrated practical benefit.
  Establish the opportunity with diagnostics or a small experiment before
  building elaborate machinery, then measure the benefit on relevant workloads.
  This is not a benchmarking requirement for every small cleanup.
- Rare supported cases can use simple correct slow paths. A C or C++ reproducer
  establishes that a case must not fail silently; it does not establish that the
  case needs a fast path. Keep the common path simple and cheap.
- Validation and debug machinery should not drive production complexity.
  Validation performance is a lower priority; do not complicate production
  paths merely to make validation easier or faster.

## Collaboration and review

- Keep `clang/docs/PointerTagSanitizer.rst` focused on the first-time user's
  quickstart. Put flag inventories and detailed option behavior in
  `clang/docs/PointerTagSanitizerOptions.rst`.
- Treat questions as questions. Investigate and explain why the implementation
  works before changing it; do not assume a challenged choice must be wrong.
- Explain concerns with small, concrete examples. Distinguish reproduced behavior
  from hypotheses, and LLVM-IR-only cases from source-language reproducers.
- Distinguish implementation bugs from model limitations, and compatibility
  failures from security weaknesses. Whether a program runs correctly under
  PTSan is a different question from what security concerns remain when it does.
- When a decision is needed, ask the user a concrete question rather than telling
  them to specify or document something. Treat a session implementation outline
  as working context unless the user requests a polished design document.
- Keep changes small and independently reviewable. When committing, match the
  surrounding history's message style and wrapping. Push only with explicit
  authorization to push now; permission to develop, commit, or prepare a release
  does not imply permission to push.

## Verification

- Follow nearby tests in `compiler-rt/test/ptsan/`; pass-level IR tests live in
  `IRTests/`. Runtime implementation lives in `compiler-rt/lib/ptsan/`.
- Investigate with the smallest useful experiment before expensive rebuilds or
  repeated benchmark runs. Prefer reduced cases and small test inputs; verify
  new diagnostics on a small case before rebuilding large applications. Still
  complete the final validation warranted by the change.
- Test the affected behavior and phase boundaries. Exercise ID16 and ID32 when
  changing shared code, and relevant scalar/vector cases when changing types or
  provenance. Run the relevant build and focused tests; report what was checked.
- Lasting regression tests must exercise the real implementation and fail when
  it regresses. A standalone model or copied implementation can help an
  investigation, but belongs in untracked `ManualTests`, not the regression suite.
- Prefer FileCheck captures over adding custom instruction names solely to make
  tests convenient. Check semantics rather than incidental formatting.
- IR tests can check transformations, but an IR-only example does not demonstrate
  a bug in compiled C or C++. Without a C or C++ reproducer at the relevant
  optimization level, treat the concern as hypothetical: bring it up and label
  any IR-only regression or XFAIL clearly, but do not complicate the
  implementation solely to address it.
