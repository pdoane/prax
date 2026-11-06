# Performance

## What a measure is

- A measure is the cost of one call of something, in the instructions that the process retired (`harnessCounters`).
  Instruction counts barely change with the machine's load, its clocks, or whether the work ran on a performance or an
  efficiency core, so one run is enough, and a run can be compared with any other. Cycles and time are reported beside
  the instructions, and vary with the machine.
- `p-core` is the share of the instructions retired on performance cores. If the time changed and the instructions did
  not, it explains why.
- If a measure drives a device, `gpu ms` is the time that the device spent executing, beside the CPU's count. It varies
  with the device's clocks, which drop while the device idles between submissions. The draw benchmark's `draw` measures
  draw a prepared frame many times in one submission, which keeps the device busy. Their GPU time is the steadier
  measure of what a frame costs the device.
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

Printing is covered by `print`: a long generated document, paginated, painted with its content marked, and written by
lib/draw's writers. The PDF is written with its codec and without, so the difference between the two rows is the
cost of its compression.

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

## Comparing with other terminals

The `vterm` benchmark compares `lib/vterm` with libvterm, which the build fetches as a release. Both engines are
linked into one harness (`tools/bench/vterm_bench`) and receive the same program output. Each engine has an adapter
through which the harness writes output, resizes the screen, and reads text and cells. The workloads are the captures
in `lib/vterm/tests/captures` and generated output: floods of plain text, of SGR colors, and of wide, combining and
emoji characters; a TUI's redraws; long lines; a resize that rewraps a full scrollback; and a read of the cells of
every line in a full scrollback, as a view scrolled back would draw them.

- `B/line` is the memory that an engine's lines hold after a workload, divided by the number of lines in its screen
  and scrollback. For lib/vterm the memory is `prVtermLinesMemorySize`. For libvterm it is what libvterm allocated,
  plus the scrollback that its adapter holds. After the floods, which fill the scrollback, it is the cost of one line
  of scrollback.
- `./dev.sh bench vterm` prints each engine's throughput. For libvterm, it also prints how many of the screen's rows
  show the same text as lib/vterm's after the workload. If the screens differ, the engines did different work, and
  their times do not compare.
- `vterm_libvterm_check`, which the unit tests run, checks that both adapters hold the same text after the same input.
  It covers what the screen's rows do not: the scrollback and its limit, the alternate screen, and resizes.

`./dev.sh bench terminals` compares whole terminal applications end to end on macOS. It runs the Prax terminal app and
whichever of Ghostty, kitty, WezTerm, Alacritty, iTerm2 and Terminal.app are installed. No other command runs it,
because it opens their windows. Each terminal is launched once, at 120x40 cells with 100,000 lines of scrollback. Its
configuration is given on its command line or in a temporary file, never the user's. The terminal runs
`terminal_feed`, which runs one workload at a time when the runner asks over a FIFO. The terminal's startup is
therefore not measured, and the runner reads the counters between runs. The terminals run one after another, because
a window that another window covers may skip drawing.

- The workloads are few and distinct. A scrollback fill of 100,000 full lines measures memory. Floods of a service's
  colored log, of wide characters, combining marks and emoji, and of lines that wrap across many rows measure
  throughput in MiB/s. htop's full-screen frames, 60 per second for 5 seconds with each frame in synchronized output,
  measure the cost of one second of drawing. The captures are left to `vterm`, because played as fast as possible
  they mostly measure switches to the alternate screen.
- Wall time ends when the terminal answers a cursor position report (DSR 6) sent after the workload. A terminal
  answers it when it parses it, so the answer marks the end of parsing. The end of the writes marks only that the pty
  buffered the bytes.
- CPU time, instructions, the cores' energy (`proc_pid_rusage`) and GPU time are summed over the terminal's processes.
  GPU time is the accumulated time of the processes' Metal clients, which the I/O Registry publishes and Activity
  Monitor reads. Each measure runs from the start until the terminal goes idle, so the frames that it draws after
  parsing count. The compositor's work belongs to WindowServer and counts for no terminal.
- Memory is the physical footprint of the terminal's processes. It is read at idle after the launch, after the fill as
  bytes for each line of scrollback over the idle footprint, and as its peak during each workload. An idle footprint
  is the least over two seconds, because a terminal may release memory a while after it goes idle. Each run starts
  from an empty screen and scrollback, but memory that a terminal freed and its allocator kept counts in the next
  run's peak. Memory that a terminal reserved at its launch counts in its idle footprint, not in its lines. The idle
  footprint is broken down by the categories of `footprint(1)`. A window's drawables are IOSurface, the GPU's buffers
  and textures are IOAccelerator, and the system's malloc is Malloc. The engine's heap, which mimalloc tags as the
  application's, is App-Specific Tag 1.
- A run starts once the terminal is idle, not after a fixed wait. Each workload except the fill runs once on a fifth
  of its output to warm up, and then twice. Instruction counts and CPU time barely vary, so each cell shows the median
  of the two runs and their spread. A run stops after 10 seconds, plus the length of a paced workload's frames, and
  shows as `> 10 s`. Each row is printed when its workload finishes, so a stopped run still reports. The comparison
  follows once every terminal has run. Nothing is recorded.
- Terminal.app and iTerm2 are driven by AppleScript, which needs the user's permission (Privacy & Security >
  Automation). Each runs all of its windows in one process, so each is measured only if it is not already running. The
  run launches it, closes the windows that its launch opened, and quits it at the end. Terminal.app uses the font and
  scrollback of its default profile. Ghostty limits its scrollback in bytes, which the run sets to about 100,000
  lines.

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
- `./dev.sh profile --gpu <harness> [args...]` records a harness's work on the GPU with Instruments' Metal System
  Trace. The trace shows each encoder's time on each stage, and the bytes of registers that the compiler spilled for
  each pipeline. A spill costs every thread of a pipeline, whichever path the thread takes. A shader that serves many
  kinds of work, such as the fragment renderer's, should therefore spill nothing.
- To confirm what a suspect costs in instructions, take it out in a scratch build and run the one row. The difference
  is its cost, whatever the profile says. Put it back before doing anything else.
