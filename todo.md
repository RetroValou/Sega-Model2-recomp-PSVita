# To do

The next items, most useful first. Each one says what is known and how it
will be checked. Details and measurements are in `HANDOFF.md`; the rules
that apply are in `rules.md` (notably "Coprocessor HLE").

## Fighting Vipers on the Vita (Model 2B)

State on 2026-11-01, fight on the Vita (two different characters, heavier
than the mirror match): 38.6 fps (target 57.5). The main core needs
~25.8 ms a frame: the i960 + SHARC chain ~19.5 ms (SHARC 12.4, i960 code
5.4, bus 1.6), polygon recording 4.6, frame end 0.9. Core 1 49 %, core 2
up to 66 % (this match's voices). The chain alone is over 17.4 ms: no split
of the cores reaches full speed without shortening it (HANDOFF.md, 1 Nov).

1. **SHARC flag liveness and specialised registers: done, no gain** (HANDOFF.md,
   1 Nov). A command finishing in one hand-off: done, -29 % hand-offs.
2. **The SHARC's per-instruction cost is in its chains of loads**, not in
   its instruction count. Only HLE-style code (values in locals across whole
   loops) beat it; a generic route would compile the SHARC program with
   registers allocated over loops. Large; to be weighed against more HLEs.
3. **The SHARC's call/return dispatch.** ~4,800 switch dispatches and
   ~1,700 chunk changes a frame for small float routines (7-80
   instructions). First measure their cost on the Vita (two counters in the
   SHARC profile); then direct returns to known call sites and callees
   laid out with their callers (exact).
4. **More HLE routines, only with a measured gain** (6 of 10 used: the
   command loop, 0x20908, 0x20473, 0x2049b (two entries), 0x20b8a, 0x20982
   (0x20b57 runs inside it)). What is left is flat: 0x20e29 3.75 %, then
   routines under 2.3 % (PC profile with the HLEs).
5. **Vertex writing on core 1 (on hold by the owner).** The plan: core 1
   builds the batches and vertices of a list right after its parse, from
   the colour tables of its frame; core 0 maps texture keys to GL textures
   and issues the draw list; a second vertex arena; Model 2B (and 2C) only,
   an option. Estimated -4 ms on core 0, core 1 to ~78 % at 46 fps.
6. **Time per HLE routine in perf.log** and the per-caller split of the
   hand-off probe (need_output against slices), if needed to choose.

## Model 2B pipeline (every 2B game)

7. **Check that the SHARC command library is shared between 2B games.**
   Needs a second Model 2B ROM set (Virtual-On, Sonic the Fighters, Last
   Bronx, Virtua Striker…): compare their SHARC programs and the command
   loop. If they match, put a hash of the routine's opcodes in the HLE
   seeds, so that an entry is only used where the code is the one it was
   written for, and reuse the routines.
8. **Commit a SHARC disassembler tool.** HLE work used MAME's
   `sharc_dasm.cpp` (BSD-3) built locally with a small shim. A
   `tools/sharcdis` with its `THIRD_PARTY.md` record would make the
   pipeline reproducible.
9. **`scripts/profile`-style helper for the SHARC.** One command to produce
   `seeds/<set>_sharc_layout.txt` (today: `recompile.py --sharc-profile`,
   then `M2_SHARC_DUMP=... m2run ... --inputs scripts/inputs/<set>_fight.txt`).
10. **build_vita.py's summary line.** It says "SHARC --fast" without the
   layout or the number of HLE routines; m2sharcrecomp's own line has them.

## Model 2 / 2A (the TGP)

11. **TGP flags and block-local registers in m2tgprecomp.** Never applied:
   the TGP's flags are computed for every operation though rarely read, and
   its registers are fields that may alias its data RAM (every store forces
   reloads). Estimated 1-2 ms a frame in Daytona's race (HANDOFF.md,
   not measured); exact, checked against MAME.

## Measurement practice

12. PC timings are noisy when another program uses the cores: use
    `taskset -c N` and `perf stat -e instructions` (as in HANDOFF), or
    measure on the Vita.
13. Profiled Vita builds (`--sharc-profile`) are slower than the others (each
    instruction counts itself): compare like with like.
