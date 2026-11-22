# Documents at the limits

`suites/prax-limits` holds documents whose content multiplies or runs long, which `./dev.sh bench limits` draws and
prints (`tools/bench/limits_bench.cpp`). The engine bounds what multiplies, and what runs long must cost in
proportion to what it holds, so a document's row grows sharply when a bound is lost or a cost stops growing linearly.
The documents are not committed: `tools/bench/gen_limits.py` makes them, and its docstrings say what each holds. The
bench writes them into the build that it runs, and `python3 -m tools.bench.gen_limits <directory>` writes them
anywhere.

| kind | documents |
| --- | --- |
| `svg` | use elements that each use the level below ten times, six levels down to a rectangle: a million instances, which the DOM's bound on use instances keeps to a hundred thousand |
| `lottie` | compositions that each hold ten precomps of the one before, six levels down to a solid: a million layers, which the player bounds |
| `html` | a table of a thousand rows with collapsed borders, and a flex column and a grid of a thousand items, each printed on forty pages; and chains of flex containers 256 levels deep, of each kind whose layout has gone quadratic before |

A benchmark's inputs are part of what it measures, so a change to the generator is a change to the rows: a comparison
across the change reports that its workload changed (tools/bench/perf.py).
