# Performance and Memory

## What is measured

All figures sum the browser's **whole process tree**: the browser process plus GPU, network/utility and renderer processes. Both browsers are measured from outside by `scripts/benchmark.ps1` with identical code (Windows performance counters via `Win32_PerfFormattedData_PerfProc_Process`).

| Metric | Meaning |
|---|---|
| **Private WS** | Private working set: resident pages no other process shares. This is Task Manager's "Memory" column and the best single "memory footprint" figure |
| WS | Working set, including shared pages (DLLs, shared memory). Summing it across processes double-counts shared pages, so it is reported but not compared |
| **Commit** | Private committed memory ("Private Bytes"): memory the processes have reserved backing for, resident or not |
| Idle CPU | CPU time the tree used during a 10 s window after the settle period, as % of one core |
| Window | Milliseconds from process launch until the first top-level window exists |

Orbit also has a live view at `orbit://diagnostics/`, with the same metrics per process via `GetProcessMemoryInfo`.

## Procedure

```
pwsh ./scripts/build.ps1 -Config Release
pwsh ./scripts/benchmark.ps1                 # Orbit + Edge, synthetic pages
pwsh ./scripts/benchmark.ps1 -Web            # adds 10 real websites (internet)
```

* Every scenario starts from a **fresh, empty profile directory**.
* Pages come from `tools/bench_server.ps1` on `localhost`. Each is a deterministic ~120 KB page: article text, a 200-row table, inline SVG, and a script that adds 300 list items. Pages are static after load (no timers), so idle measurements are not distorted.
* Wait `10 s + tabs/2` (rounded up), then take three samples 2 s apart and report the median.
* Orbit is closed normally (WM_CLOSE). Edge's process tree is terminated.
* Edge runs with `--no-first-run --no-default-browser-check --disable-sync --window-size=1280,820`. Orbit's default window is 1280×820 DIPs.

Scenarios: 1, 5, 10, 25 and 50 tabs. Orbit additionally runs:

* 25 tabs after putting background tabs to sleep (`--orbit-bench-sleep-after=20`)
* a restored 25-tab session (relaunch with no URLs)

## Caveats

* All synthetic pages share one origin (`localhost`). Chromium may then put several tabs in one renderer process, especially once its renderer-process limit is reached (it depends on installed RAM). Real browsing across many sites uses more processes. Use `-Web` for a mixed-site workload.
* Edge includes features Orbit lacks (Safe Browsing, sync and shopping services, extensions infrastructure). The comparison measures the cost of the browsers as shipped, not of the engines.
* Results come from one machine and one run per scenario. Differences under about 5% are within noise.
