# rmv-report

A command-line report for **Radeon Memory Visualizer** (`.rmv`) traces, with
no GUI, no Qt and no AMD GPU required.

AMD's [Radeon Memory Visualizer](https://github.com/GPUOpen-Tools/radeon_memory_visualizer)
(RMV) is open source, but it ships exactly one executable: the Qt GUI. There is
no way to get numbers out of a trace in a script, over SSH, or on a machine
where the GUI does not build. `rmv-report` links against RMV's own parser and
backend libraries and prints a plain-text summary instead.

It reads both trace formats in circulation:

| Header | Written by |
|---|---|
| `MINI` | Mesa's RADV Vulkan driver on Linux (`MESA_VK_TRACE=rmv`) |
| `AMD_RDF ` | AMD's Radeon Developer Panel (RDF container, zstd-compressed) |

The format decoding is entirely AMD's code. This project only adds the report
on top, plus build glue and notes on how to record a trace with RADV.

## What the report shows

For a number of evenly spaced points in time (default 10):

- per heap (local VRAM, invisible VRAM, system memory): memory requested,
  bound and mapped, allocation count, mean and maximum allocation size,
  resource count
- for local VRAM: a breakdown by usage type (textures, buffers, render
  targets, command buffers, ...)
- **backing**: where the process's memory actually lives (local, invisible,
  system, unbacked)
- the allocation size distribution in power-of-two classes, with the
  buddy-allocator *order* relative to 4 KiB pages
- the most frequent resource sizes, each with its usage types

Excerpt, from the small RADV trace in `examples/` (`rmv-report -n 1 examples/vkcube-radv.rmv`):

```
================ Trace ================
  File                  examples/vkcube-radv.rmv
  Target process ID     34583
  Streams               1
  Segments              3
  Active GPU            0
  Maximum timestamp     921120
  Segment 0: Local (VRAM, CPU-visible)              16.000 GiB
  Segment 1: Invisible (VRAM, not CPU-visible)       0.000 GiB
  Segment 2: System (host memory)                   15.567 GiB

================ Timeline ================
         Heap       req_GiB  bound_GiB mapped_GiB     allocs  mean_MiB   max_MiB resources
--- 100 % of the trace  (t = 921120) ---
         Local        0.011      0.011      0.011         12      0.92      4.12        12
                 by usage:  Render target  0.006  Command buffer  0.004  Depth stencil texture  0.001  Shader pipeline  0.000  Descriptors  0.000
         Invis        0.000      0.000      0.000          0      0.00      0.00         0
         System       0.000      0.000      0.000          3      0.09      0.25         3
         backing Local  0.011  Invisible  0.000  System  0.000  unbacked  0.000 GiB
                 allocations 15,  mean fragmentation 0.5333,  largest resource      4.12 MiB
                 allocation sizes: median      0.02 MiB, p90      2.01 MiB, p99      4.12 MiB, total  0.011 GiB
                   up to     0.004 MiB (order  0):    5 allocations
                   ...
                 most frequent resource sizes:
                         6 x     2056 KiB  =   0.01 GiB   Heap:3 Render target:3
                         3 x       16 KiB  =   0.00 GiB   Command buffer:3
                         ...
```

## Building

Requirements: `git`, `python3`, CMake ≥ 3.16, a C++17 compiler, Ninja or
Make. **No Qt.** Tested on Linux with GCC.

```sh
scripts/fetch-rmv.sh                  # clones RMV v1.15 and its dependencies
cmake -S . -B build -G Ninja
cmake --build build
build/rmv-report --help
```

`scripts/fetch-rmv.sh` clones AMD's repository at a pinned release into
`radeon_memory_visualizer/` and runs AMD's own `build/fetch_dependencies.py`.
**AMD's tree is not modified.** RMV's top-level `CMakeLists.txt` always
configures the Qt frontend and has no switch to turn it off. This project's
`CMakeLists.txt` therefore pulls in only the parser, the backend and their two
dependencies (`libamdrdf`, `system_info_utils`) directly.

The pinned release is the tested one. `scripts/fetch-rmv.sh --latest` fetches
the newest RMV release instead, and `--tag vX.Y` a specific one. Both are
untested: the parts of RMV's parser/backend API that rmv-report uses changed
once in six releases (v1.12), but RMV's build files and dependency versions
change in almost every release, and older releases (v1.14 and earlier) no
longer compile with current GCC because of a missing include in AMD's code.

To use an existing checkout elsewhere: `cmake -S . -B build -DRMV_SOURCE_DIR=/path/to/radeon_memory_visualizer`.

`build/compile_commands.json` is generated for clangd and other language servers.

## Usage

```sh
rmv-report [options] <trace.rmv>

  -n, --points N   number of evenly spaced points in time (default 10, max 1000)
  -h, --help       show help
```

Exit codes: `0` success, `1` the trace could not be read, `2` usage error.

Large traces take a while: each point in time rebuilds a full snapshot.

## Recording a trace on Linux with RADV

RADV can write RMV traces itself; neither AMDVLK nor the Radeon Developer
Panel is needed. (Since AMD driver 25.20 the Radeon Developer Panel cannot
record Vulkan applications on Linux at all.)

### 1. Prepare the ftrace instance (after every reboot)

RADV reads the kernel's memory events from an ftrace instance called
`amd_rmv` but does **not** create it. Without it, the driver only reports
`Can't access the tracing instance directory` and writes nothing.

```sh
sudo scripts/setup-tracing.sh     # creates the instance, fixes permissions
scripts/setup-tracing.sh --check  # verifies it as your own user
```

This loosens permissions on `/sys/kernel/tracing`. Reboot afterwards to return
to the defaults. (Mesa's documentation refers to `scripts/setup.sh` from the
Radeon Developer Tool Suite for the same step. This script does the same
without installing the suite.)

### 2. Run the application

```sh
MESA_VK_TRACE=rmv MESA_VK_TRACE_TRIGGER=/tmp/rmv-trigger ./your-vulkan-app
```

On success RADV prints `radv: Enabled Memory Trace.`

**Steam games** run inside the pressure-vessel container, which needs write
access to the tracing directory. Launch options that work:

```
PRESSURE_VESSEL_DEVEL=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/sys/kernel/tracing:/tmp MESA_VK_TRACE=rmv MESA_VK_TRACE_TRIGGER=/home/<user>/rmv-trigger %command%
```

With Steam, the trigger file was tested in the home directory (which the
container shares), not in `/tmp`. Use that path with `touch` below.

`PRESSURE_VESSEL_DEVEL=1` is the essential part. Without it, pressure-vessel
mounts `/sys` read-only over the shared tracing directory, and the capture fails
with `Read-only file system`. This cannot be fixed from outside a running
container. Add `PROTON_LOG=1` to see RADV's messages in the Proton log.

### 3. Trigger and collect

```sh
touch /tmp/rmv-trigger
```

RADV checks for the file on every presented frame. When it finds it, it
**deletes the file itself** and captures once (`src/vulkan/wsi/wsi_common.c` in
Mesa). The trace is written to `/tmp/<process>_<date>.rmv` and announced with
`RMV capture saved to ...`. Wait for that message before copying the file;
large traces take a while to write. `MESA_VK_TRACE_FRAME=<n>` triggers at a
fixed frame instead.

The trigger file must be **deletable by the application**. If RADV cannot
remove it, it prints `Could not remove trace trigger file, ignoring` and does
not capture at all. That is why the Steam example above puts it in the home
directory.

## Limitations

- An RMV trace records what the **driver** knows. On Linux, buffer moves made
  by the kernel's memory manager (TTM eviction between VRAM and GTT) are not
  part of it: in one measurement RMV reported 49 MiB where the kernel had moved
  0.69 GiB. For that side, use the `amdgpu_bo_move` tracepoint or
  `/proc/<pid>/fdinfo`.
- Only tested on Linux. Windows traces (`AMD_RDF `) are read fine on Linux.
- Built against RMV v1.15. The parser/backend API is internal to RMV and may
  change between releases; `scripts/fetch-rmv.sh` pins the tested version on
  purpose.

## License and credits

`rmv-report` is released under the MIT License, see `LICENSE`.

All trace parsing is done by AMD's Radeon Memory Visualizer
(Copyright © Advanced Micro Devices, Inc., MIT License), which is **fetched
at build time, not redistributed** here. Its dependencies come with their own
licenses; see `radeon_memory_visualizer/NOTICES.txt` after fetching.
