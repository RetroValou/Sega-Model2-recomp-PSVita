# To do

The next items, most useful first. Each one says what is known and how it
will be checked. Details and measurements are in `HANDOFF.md`; the rules
that apply are in `rules.md` (notably "Coprocessor HLE").

## Fighting Vipers on the Vita (Model 2B)

State on 2026-10-31, fight on the Vita: ~40 fps (target 57.5). The main
core needs ~24 ms per frame against 17.4 ms: the SHARC ~12.2 ms, the rest of
the vblank handler (i960) ~5.5 ms, polygon recording ~4.5 ms. Core 1
(geometry) is at 52 %, core 2 (sound) at 45 %.

1. **Measure whether the SHARC can run on another core.** Before writing
   anything: in a fight, how long does the i960 work between pushing a
   command and reading its result (TgpBoard::need_output)? If it reads
   right after each command, running in parallel gains little. If it
   pushes batches, the SHARC on core 1 or 2 (fast_inaccuracy only) is the
   only lever close to x2. It needs a thread-safe FIFO between the i960 and
   the SHARC, and a timing that differs from MAME's (accepted in
   fast_inaccuracy).
2. **Move or speed up polygon recording (4.5 ms on core 0).** Vertex
   writing is 2.6 ms of it. Core 1 has room (52 %). See the Vita renderer
   in `platform/vita/` and its HANDOFF.
3. **Other HLE candidates, only where the gain is shown.** The three copy
   HLEs gained only ~1 % on the Vita: straight copy loops were already
   cheap in the recompiled code. The candidates left, from the profile's
   routine table (`M2_SHARC_DUMP` on a profiled build):
   - `0x20b8a` (9.9 % of the instructions, a 64-cell occupancy grid with
     float compares) and `0x20982` (7 %, collision tests). Both are long
     and branchy, so they only pay if done with host floats and without
     the flags nobody reads.
   - `0x20c7d` and `0x20e29` (5.2 %).
   - The float leaf routines (`0x201bc`, `0x2014c`, …). An exact HLE costs
     what the recompiled code costs; a faster one would drop flags (not
     identical to MAME, checked by eye).
   Stay within ~5 routines per game (rules.md). Check each with
   `m2sharccheck_gen` on a `--fast --hle` build, then a Vita perf.log.
4. **Measure the command loop's HLE on its own on the Vita.** Its gain was
   only measured mixed with the layout and with the profile counters
   removed. To measure it alone: build with and without its line in
   `seeds/fvipers_sharc_hle.txt`, same scene.
5. **Time per HLE routine in perf.log.** Today the profile only counts
   runs. Add a clock probe per routine to see where the remaining 12 ms of
   SHARC goes, without the per-instruction counters that slow the
   profiled build.

## Model 2B pipeline (every 2B game)

6. **Check that the SHARC command library is shared between 2B games.**
   Needs a second Model 2B ROM set (Virtual-On, Sonic the Fighters, Last
   Bronx, Virtua Striker…): compare their SHARC programs and the command
   loop. If they match, put a hash of the routine's opcodes in the HLE
   seeds, so that an entry is only used where the code is the one it was
   written for, and reuse the routines.
7. **Commit a SHARC disassembler tool.** HLE work used MAME's
   `sharc_dasm.cpp` (BSD-3) built locally with a small shim. A
   `tools/sharcdis` with its `THIRD_PARTY.md` record would make the
   pipeline reproducible.
8. **`scripts/profile`-style helper for the SHARC.** One command to produce
   `seeds/<set>_sharc_layout.txt` (today: `recompile.py --sharc-profile`,
   then `M2_SHARC_DUMP=... m2run ... --inputs scripts/inputs/<set>_fight.txt`).
9. **build_vita.py's summary line.** It says "SHARC --fast" without the
   layout or the number of HLE routines; m2sharcrecomp's own line has them.

## Measurement practice

10. PC timings are noisy when another program uses the cores: use
    `taskset -c N` and `perf stat -e instructions` (as in HANDOFF), or
    measure on the Vita.
11. Profiled Vita builds (`--sharc-profile`) are slower than the others (each
    instruction counts itself): compare like with like.
