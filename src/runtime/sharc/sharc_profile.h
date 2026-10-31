// SHARC profiling (Model 2B's copro): where the recompiled SHARC program's
// time goes. Built only with M2_SHARC_PROFILE (scripts/recompile.py
// --sharc-profile, CMake -DM2_SHARC_PROFILE=ON; scripts/build_vita.py
// --sharc-profile, written to perf.log; m2sharcprof for the per-address
// counts alone). Without it the runtime and the generated code are unchanged.
//
// What it measures:
//   - by caller (TgpBoard): run to the wait (vblank, the TGP's sync), need
//     the next output word (the i960 reading the output FIFO: run_generated
//     in 16-instruction calls), a 256-instruction slice (an i960 buffer RAM
//     read): calls, run_generated calls, instructions, time (one clock read
//     pair a caller's call, the clock's cost measured at start);
//   - run_generated: calls by size, chunk calls, dispatches (a chunk's
//     switch), plain run ends, pinned branches taken, delayed branches taken
//     after their slots, counter loops looping back, full steps, the
//     interpreter's steps (plain_ready), exits (waiting, budget);
//   - memory: the data space by region (internal 32-bit, the 16-bit view,
//     IOP registers, the board), program memory data, the board by kind
//     (input FIFO, empty input FIFO, output FIFO, buffer RAM, data ROM);
//   - executions per instruction of the program (M2SHARC_COUNT in the
//     generated code: a load, add and store an instruction), so the window's
//     hottest instructions and handlers, and the share of plain, pinned and
//     full instructions;
//   - with SharcProfile::dump_path, every executed instruction of the
//     window (address, executions a frame, kind, handler) in a file, and
//     every control transfer that is not a fall-through (SharcEdges: from,
//     to, goto / dispatch / chunk change / entry, a frame).
// Counters never change what the SHARC computes; they cost time (the counts
// above all), so the report's times are a profiling build's.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef M2_SHARC_PROFILE

#include <vector>

namespace rt::sharc {

enum SharcCaller : uint8_t { SharcToWait, SharcNeedOutput, SharcSlice, kSharcCallers };

// run_generated's calls by instructions run: 0, 1-16, 17-64, 65-256, 257-4096, more.
constexpr int kSharcSizeBuckets = 6;

struct SharcProfile {
    uint64_t (*clock)() = nullptr; // host ticks (set by the frontend); null: no timing
    uint64_t ticks_per_second = 1000000;
    // Every instruction executed in a report's window, by address, written to this file at each report
    // (rewritten each time), to group the hot addresses into routines; null: none. m2run: M2_SHARC_DUMP=FILE;
    // the Vita: ux0:data/<set>/sharc_hot.txt.
    const char *dump_path = nullptr;
    // TgpBoard, by caller.
    uint64_t calls[kSharcCallers]{}, runs[kSharcCallers]{}, instructions[kSharcCallers]{}, ticks[kSharcCallers]{};
    // run_generated (sharc_gen.cpp) and the chunks.
    uint64_t gen_calls = 0, chunk_calls = 0, dispatches = 0, run_ends = 0, taken = 0, slot_taken = 0, loop_backs = 0;
    uint64_t full_steps = 0, fallback_entries = 0, fallback_steps = 0, exit_wait = 0, exit_budget = 0;
    uint64_t hle_runs = 0, hle_declined = 0; // HLE routines (run_hle): run, or 0 returned (the recompiled code ran)
    uint64_t sizes[kSharcSizeBuckets]{};
    // The data space (data_space::read_dword / write_dword) and program memory data.
    uint64_t dm_r = 0, dm_w = 0, short_r = 0, short_w = 0, iop_r = 0, iop_w = 0, ext_r = 0, ext_w = 0;
    uint64_t pm_r = 0, pm_w = 0, pm_w_code = 0; // pm_w_code: into the recompiled code's range (read twice: the rewrite check)
    // The board (SharcCopro).
    uint64_t fifo_in = 0, fifo_in_empty = 0, fifo_out = 0, buffer_r = 0, buffer_w = 0, rom_r = 0, other = 0;
};
extern SharcProfile sharc_profile;

// The program's control transfers that are not a fall-through, by (from, to, kind): the
// layout's data (which instructions should share a chunk). From and to are indexes in the
// program (address - 0x20000); kind: a direct goto inside a chunk, a dispatch (the chunk's
// switch) that stays in the chunk, a dispatch after a chunk change (run_generated's switch),
// a run_generated call's first dispatch (from kSharcFromEntry). An open-addressing table, so
// the Vita can keep it too (a full table counts the rest in `lost`).
enum SharcEdgeKind : uint8_t { SharcEdgeGoto, SharcEdgeDispatch, SharcEdgeChunk, SharcEdgeEntry, kSharcEdgeKinds };
constexpr uint32_t kSharcFromEntry = 0xfffff;
struct SharcEdges {
    static constexpr uint32_t kSize = 1u << 15;
    uint64_t key[kSize]{}; // (kind << 40 | from << 20 | to) + 1; 0: empty
    uint32_t count[kSize]{};
    uint64_t lost = 0;
    void add(uint32_t from, uint32_t to, SharcEdgeKind kind) {
        const uint64_t k = ((uint64_t(kind) << 40) | (uint64_t(from & 0xfffff) << 20) | (to & 0xfffff)) + 1;
        uint32_t i = uint32_t((k * 0x9e3779b97f4a7c15ull) >> 49); // 15 bits
        for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSize - 1)) {
            if (key[i] == k) { ++count[i]; return; }
            if (!key[i]) { key[i] = k; count[i] = 1; return; }
        }
        ++lost;
    }
};
extern SharcEdges sharc_edges;

// Routines' inclusive cost: at each call (an instruction named *_call that pushed the PC stack) the
// target and the instruction count are pushed here; at each rts that popped it, the instructions
// between call and return are added to the target's entry. The HLE candidates (scripts/sharc_hle.py).
struct SharcRoutines {
    static constexpr uint32_t kSize = 1u << 12, kDepth = 32;
    uint32_t target[kSize]{}; // address + 1; 0: empty
    uint64_t calls[kSize]{}, instructions[kSize]{};
    uint32_t stack_target[kDepth]{};
    uint64_t stack_start[kDepth]{};
    uint32_t depth = 0;
    uint64_t lost = 0; // a full table, a return without its call, a stack deeper than kDepth
    void call(uint32_t to, uint64_t at) {
        if (depth < kDepth) stack_target[depth] = to, stack_start[depth] = at;
        ++depth;
    }
    void ret(uint64_t at) {
        if (!depth) { ++lost; return; }
        if (--depth >= kDepth) { ++lost; return; }
        const uint32_t to = stack_target[depth];
        uint32_t i = (to * 0x9e3779b1u) >> 20;
        for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSize - 1)) {
            if (target[i] == to + 1 || !target[i]) {
                target[i] = to + 1;
                ++calls[i];
                instructions[i] += at - stack_start[depth];
                return;
            }
        }
        ++lost;
    }
};
extern SharcRoutines sharc_routines;

// m2sharcrecomp: what each instruction of the program is (the generated
// sharc_gen.cpp defines them; sharc_nogen.cpp none).
enum SharcKind : uint8_t { SharcPlain, SharcPinned, SharcFull };
extern const uint32_t sharc_program_crc32; // the recompiled program's (0 without one): the dump's header, --layout's check
extern const char *const sharc_gen_mode; // how m2sharcrecomp made it: "exact", "--fast", "full steps" (the report's first line)
extern const uint32_t sharc_gen_count;
extern const char *const sharc_gen_names[];
extern const uint8_t sharc_gen_kinds[];

// The report of one window: start() at its beginning, format() at its end.
// counts: the board's per-instruction executions (adsp21062_device::set_profile).
class SharcProfileReport {
public:
    void start(uint64_t sharc_instructions, const uint32_t *counts);
    // frames: emulated frames in the window; cpu_mhz: for cycles (0: none).
    size_t format(char *out, size_t size, uint64_t frames, uint64_t sharc_instructions, const uint32_t *counts, int cpu_mhz);

private:
    SharcProfile at_{};
    uint64_t instructions_ = 0;
    std::vector<uint32_t> counts_;
    std::vector<uint32_t> edge_counts_; // sharc_edges.count at start()
    std::vector<uint64_t> routine_calls_, routine_instructions_; // sharc_routines at start()
    double clock_ns_ = -1; // one clock read, measured once
};

} // namespace rt::sharc

#define M2SHARC_EV(field) (++::rt::sharc::sharc_profile.field)
// A run_generated call's size, by instructions run.
#define M2SHARC_SIZE(n) (++::rt::sharc::sharc_profile.sizes[(n) == 0 ? 0 : (n) <= 16 ? 1 : (n) <= 64 ? 2 : (n) <= 256 ? 3 : (n) <= 4096 ? 4 : 5])

#else

#define M2SHARC_EV(field) ((void)0)
#define M2SHARC_SIZE(n) ((void)0)

#endif // M2_SHARC_PROFILE
