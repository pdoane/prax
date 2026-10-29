# Performance

## What a measure is

- A measure is the cost of one call of something, in the instructions that the process retired (`harnessCounters`).
  Instruction counts barely change with the machine's load, its clocks, or whether the work ran on a performance or an
  efficiency core, so one run is enough, and a run can be compared with any other. Cycles and time are reported beside
  the instructions, and vary with the machine.
- `p-core` is the share of the instructions retired on performance cores. If the time changed and the instructions did
  not, it explains why.
- If a measure drives a device, `gpu ms` is the time that the device spent executing, beside the CPU's count. It varies
  with the device's clocks.
- Power matters more than wall time. A change that buys time with more instructions, as going wide on threads does,
  needs a case in which someone waits for the work. Going wide is for work that someone waits on, never for background
  work.
- Rows without counts, such as the latencies of the document benchmark's paced load, have only times. They vary with
  the machine, and `--all` lists them.

## Workflow

- `./dev.sh bench ab [name...]` measures a change against a base, and needs nothing recorded beforehand. It builds the
  harnesses of the base and of the working tree, runs both, and prints the measures that changed by more than 1%
  (`--threshold`) and by more than the spread of each side's runs. The base is HEAD if the tree has changes, or HEAD's
  parent otherwise. `--base REF` names any other commit, so it measures one change, a series of commits, or a whole
  branch against the commit it started from (`--base main`, or the merge base). A base that is not a commit,
  such as the tree before a change made on top of uncommitted work, is a directory: `--base-dir DIR` builds the base
  from a worktree or from a copy of the tree taken before the change. Measuring never writes to git: no commits or
  snapshots of the tree go into the repository. Each side runs twice by default (`--runs`). `--all` prints every
  measure. A measure whose runs disagree is listed as unsteady. Its work varies from run to run, as the work of the draw
  benchmark's GPU backends varies with the device's timing, which hides a change smaller than the spread.
- `./dev.sh bench [name...]` runs the benchmarks once on the working tree's harnesses and prints their measures.
  Nothing is recorded: what a change did is what `bench ab` measures, against the commits that it names.
- `./dev.sh bench list` names the benchmarks and what each one measures. `tools/bench/registry.py` holds the table,
  with the harnesses that each benchmark runs and the files that it reads. A harness measures with `harnessMeasure`,
  and a script under `tools/bench/scripts` measures with `tools/bench/measure.js`, which counts in pr_shell.

## What the benchmarks cover

A benchmark is either scripted or an engine benchmark. A scripted benchmark is a script that does its work through the
web platform's APIs, so pr_shell and a browser run the same file. Most have a harness beside them that runs the same
cases through the C API. An engine benchmark measures what only the engine exposes, such as whole frames, recording
and rendering, or the browser example, and no browser runs it.

A change is measured on more than one kind of workload, so that it is not tuned to one document. Layout and its
neighbors are covered from three sides, each cheap enough to run on every change:

- `layout`: documents generated from a fixed seed, made only of what the engine lays out: an article, cards, floats
  beside text, nested lists, deep nesting, many siblings, one long paragraph, flex rows, columns, and wrapping lines of
  text. Each is laid out from scratch, at another width, and after a text, style, insertion and float change. They are
  set in the checkout's Arimo, which the browser page loads, so they are fair to compare with browsers. The benchmark
  measures style and layout, which a geometry read brings up to date, and not painting.
- `page-changes`: the invalidation check's changes (docs/testing.md) over the captured pages, without its checks. Each
  row is one kind of change on one page, with the total cost of the frames after it, `update` (style and layout) and
  `scene` (recording). The `all` rows sum each kind over the pages. Real pages use features that the engine does not
  lay out yet, so no browser runs them. Their worst pages are where costs that no synthetic document has show first.
- `frame` and `long-doc`: one large page of cards and one long specification, each measured through whole frames:
  changes that reach one box, and resizes, which reach every box that depends on the width.

A benchmark whose run grows past a few seconds stops being run on every change. Add cases that each reach a different
path, instead of more of the same, and keep a slow benchmark's case count small if its pages are slow.

## Comparing with other layout engines

`flex` lays out the same trees in `lib/layout`, Yoga and Taffy, linked into one harness (`tools/bench/flex_bench`) from the
releases that the build fetches (`cmake/dependencies.cmake`), and counts each engine as it counts the engine. The first
trees have the shapes of Taffy's own flexbox benchmarks. The others each reach a part of flex layout that Taffy's do
not: columns, rows and columns alternating by depth, wrapping lines, absolutely positioned leaves, leaves sized through
an aspect ratio, percentage heights against stretched and flexed sizes, columns at their automatic minimum with scroll
containers among them, and lines whose items each exceed their maximums in a round of their own, which is the most
rounds that resolving flexible lengths takes. A tree that uses features that Yoga and Taffy do not lay out (`order`, and
leaves of inline content) runs in the engine alone, with no "agrees" count.

In `deep auto`, the boxes' margins add up to more than the viewport, so each box is as wide as its content, and a
resize changes no box's size. It measures caching of resolved sizes. The other deep trees fill the viewport, so that a
resize reaches every box. Every engine sets every property that the trees use, so that no engine's defaults decide
anything. Each tree is measured built, laid out cold, resized, after one leaf changes, and unchanged. By default, Yoga
and Taffy round their results to whole pixels after a layout. The engine does not, so neither of them rounds here.

- `./dev.sh bench flex` prints the engine's instructions beside each other engine's, and each other engine's as a
  multiple of the engine's, as `bench browser` does (above 1, the engine is faster). It also prints the work that each
  engine did: the boxes laid out, reused and measured, as each engine counts them, and, on a cold row, the number of
  boxes that each engine placed where Taffy places them. An engine that places boxes differently is doing different
  work, which is read beside its count, not left out of it.
- Taffy, not the specification, is the reference that "agrees" counts against. If the engine places a box
  differently, `flex_bench --differences TREE` names the first boxes at which each difference starts, and
  `flex_bench --html NODE TREE` prints the subtree at one of them as a probe page, which `./dev.sh probe` lays out in
  the browsers.
- The other engines' rows are recorded with the engine's, so a comparison of runs shows whether their rows held still,
  which shows whether the machine and the harness did.

## Comparing with browsers

- `./dev.sh bench browser [name...]` runs the scripted benchmarks in pr_shell and in every installed browser
  (`--browser` names one). It prints each case's time in each, each browser's time as a multiple of the engine's, and
  their geometric means by kind of case. Above 1x, the engine is faster. Nothing is recorded: browsers change with
  their releases, and the engine's own changes are what `bench ab` counts.
- Every engine is timed the same way: batches of calls that run well past the coarsest clock, and the median of five.
  The page is served with cross-origin isolation, which makes a browser's clock as fine as the browser allows, and the
  report gives each clock. Chrome and Firefox run in a fresh profile without extensions or throttling. A multiple
  marked `?` comes from batches whose times overlap the engine's, so that case does not show which is faster.
- A browser may spread the work over several cores, as Firefox does for a restyle, while the engine uses one. Each case
  gives the number of cores that each engine kept busy while it measured (its processes' time on cores divided by the
  elapsed time), and the summary compares time on cores beside time. A lead in time bought with more cores is not a
  lead in power.
- WebKit, the engine that Safari uses, is the system's WebKit, running in `webkit_host`, a browser of lib/app's Cocoa
  backend that keeps no data, so that no profile or tab of the developer's Safari runs beside it.
- A case measures the same work in every engine only if no engine can skip the work. An engine that caches results by
  their input answers a repeated input from its cache, so a case that means to measure the work gives each call an
  input that no earlier call has seen. For example, `css-parse` gives each parse a sheet with a comment that no other
  sheet has.
- `--serve` serves one benchmark's page and compares whichever browser opens it with the engine. This is for a browser
  that cannot be launched, or for a profiler in a browser's developer tools.

## Reading the numbers

- `(C)` measures run the same cases through the C API without the shell. The gap to the shell's measure is the cost of
  the binding and QuickJS. Know it before choosing where to optimize.
- `bound` measures read the input once. They are the floor that every pass over the source is measured against.
- `frame` rows with a card count in their name run the same change on a page twenty times larger. The cost of a change
  should depend on what it reaches, so a row that grows with the page is a defect. Its counters say what the frame did
  and what it wasted: elements restyled to the same style, and boxes painted again although nothing that they paint
  changed. The invalidation check (docs/testing.md) reports the same counts over real pages and their CSS.
- Instructions and cycles can diverge. Many small added instructions run in parallel with the memory accesses that they
  wait on, so a change can cost 30% in instructions and nothing in cycles, and a larger record can cost cycles with no
  added instructions. The A/B compares instructions. Read the cycles beside them before deciding that a change is a
  cost.
- `./dev.sh bench kept [name...] [--count N]` measures what N runs of each case of the scripted benchmarks keep
  allocated, across two processes. The number must stay flat as N grows. A growing number is a leak.

## Finding where a cost is

- Narrow the cost to one row first. `frame_bench` takes a case name and runs only that case, for as long as a profiler
  watches it. The invalidation harness takes `--element N` or `--match TEXT` to make only one element's changes.
- `./dev.sh profile [--base REF] <harness> [args...]` records the harness of the default build, which keeps its debug
  information, and prints the functions and the innermost inlined lines that took the most samples. With `--base`, it
  also records the same harness of REF, built the same way, and prints both side by side as shares of each side's
  samples. `--under NAME` keeps only the samples under one function, such as `prLayoutTreeLayout` for a layout.
  Samples measure cycles, not instructions: read the functions whose share grew, then the source.
- A cost can come from code that never runs. The arguments set up at the call site of a cold function, such as a
  failure report, make the function that holds the call larger, and a larger function may no longer be inlined into its
  hot callers. So `PR_ASSERT` and `PR_FATAL` pass one pointer to a constant record of the message, file and line. When
  they passed the three as arguments, mutation's node builds ran up to 7% more instructions.
- To confirm what a suspect costs in instructions, take it out in a scratch build and run the one row. The difference
  is its cost, whatever the profile says. Put it back before doing anything else.
