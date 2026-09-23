---
name: tracy-stats
description: >
  Read CPU performance statistics directly out of a Tracy profiler capture
  (.tracy file) via the bundled csvexport CLI — no GUI, no screenshots. Use
  when analyzing a Tracy capture, finding hot/expensive functions, listing the
  top zones by self-time or total time, filtering to specific zones, comparing
  two captures (before/after a change), or inspecting when events fire over
  time. Works for any Tracy-instrumented application.
---

# tracy-stats — read Tracy captures from the CLI

Tracy ships a headless `csvexport` tool that dumps the same per-zone statistics
shown in the profiler's **Statistics** tab to CSV. This skill turns a `.tracy`
capture into machine-readable numbers you can sort, filter, and diff — no GUI,
no screenshots.

## Configuration (filled in on first use)

```
CSVEXPORT_PATH: <UNCONFIGURED>
CAPTURE_PATH:   <UNCONFIGURED>
```

**First-run setup — do this before any analysis:**
If `CSVEXPORT_PATH` above is still `<UNCONFIGURED>`, do NOT guess a path. Ask
the user:

> "What's the full path to your Tracy `csvexport` tool? It ships with the
> Tracy profiler distribution (`csvexport.exe` on Windows; `csvexport` or
> `tracy-csvexport` on Linux/macOS). **It must be the same Tracy version that
> produced your `.tracy` captures** — the trace format is version-locked. If
> you also have the headless `capture` tool handy, share that path too."

When they reply, use the **Edit** tool to replace `<UNCONFIGURED>` on the
relevant line(s) in *this* `SKILL.md` (this skill's directory is provided to
you when the skill loads) with the path(s) they gave. Then continue. Future
runs will read the saved path and won't re-ask.

Throughout the recipes below, `$CSV` means the configured `CSVEXPORT_PATH`.

## What csvexport produces

CSV columns (aggregate mode):
`name, src_file, src_line, total_ns, total_perc, counts, mean_ns, min_ns, max_ns, std_ns`

Flags:
```
-e, --self        Report SELF time (excludes children). Matches the
                  "Self only" mode in the Statistics tab. Use this by default
                  for "what function is actually burning CPU".
-f, --filter ARG  Only zones whose name contains ARG (substring match).
-c, --case        Make -f case sensitive.
-s, --sep ARG     CSV separator (default ",").
-u, --unwrap      Emit one row PER zone event (with timestamps) instead of
                  aggregating — use to see WHEN something happens over time.
```

## Recipes (run via the Bash tool; awk does the math)

**Top N zones by self-time** — the go-to "where is the CPU going":
```bash
CSV="<CSVEXPORT_PATH>"          # from Configuration above
TRACE="<path/to/capture.tracy>" # ask the user which capture
"$CSV" -e "$TRACE" > /tmp/t.csv
echo "TOTAL_ms | counts | mean_ns | name"
tail -n +2 /tmp/t.csv | sort -t, -k4 -nr | head -25 \
  | awk -F, '{printf "%9.1f | %9s | %8s | %s\n", $4/1000000, $6, $7, $1}'
```

**Filter to a subsystem:**
```bash
"$CSV" -e -f <substring> "$TRACE"
```

**Before/after diff** — export both captures with `-e`, join on the `name`
column, compare `total_ns` and `counts`, and report the zones that moved most
with concrete numbers (e.g. "submitWork 3.3s -> 1.1s").

**When does a zone fire?** (load-time vs steady-state) — use `-u` for per-event
rows with timestamps, then bucket by time to see whether an expensive zone is
front-loaded (startup/loading) or sprinkled through runtime (stutter).

## Capturing a fresh trace headlessly (optional, needs CAPTURE_PATH)

While the instrumented app is running:
```
<CAPTURE_PATH> -o <output.tracy> -a 127.0.0.1
```
Let it run, then Ctrl-C to stop and flush the file. (The GUI works too; this
just avoids babysitting it.)

## Interpreting a capture (general guidance)

- **`*::synchronize`, `waiting for work`, `*::wait*` are IDLE / BLOCKED time,
  not work.** Large values mean that thread is starved — waiting on another
  thread, another process, the GPU, or a fence. The idle thread is NOT the
  bottleneck; look at what it is waiting *for*.
- **Watch for shader / pipeline compilation zones** (names containing
  `Shader`, `compilePipeline`, `createPipeline`, etc.). Large totals can mean
  on-demand compilation. If `-u` shows them firing during steady-state (not
  just at load), that is render-thread **stutter**, and it is unrelated to
  per-frame work volume — easy to miss from the timeline alone.
- **Self vs total time.** `-e` (self) tells you which leaf function burns CPU;
  without `-e` you get inclusive time (a parent's total includes its children).
  Use self-time to find the actual hot code, inclusive to find heavy subtrees.
- **Whole-trace totals include any startup/loading captured.** A zone with
  `counts == 1` (or a count far below the frame count) is usually one-time
  init, not per-frame cost. Capture only steady-state, or use `-u`, to isolate
  runtime behavior.
- **Per-call cost = `mean_ns`; frequency = `counts`.** A cheap function called
  millions of times can dominate; divide `counts` by the frame/draw count to
  get per-frame or per-draw multiples.

## Output

Lead with the sorted top-zones table (bold the standout metrics). Keep the raw
numbers separate from interpretation. For diffs, give concrete before→after
numbers rather than vague comparatives.
