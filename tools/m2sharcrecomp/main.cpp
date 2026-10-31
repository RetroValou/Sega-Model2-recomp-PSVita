// m2sharcrecomp: recompile the SHARC program a Model 2B game uploads (Fighting
// Vipers) to C++.
//
//   m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops SHARCOPS_TABLE.inc] [--sharcops SHARCOPS.hxx]
//                 [--compute COMPUTE.hxx] [--full-steps]
//
// SHARC_PROGRAM.bin is m2import's cut of the upload: 16-bit little-endian words,
// three per 48-bit instruction (DMA channel 6's 16/48 packing, MSWF clear: the
// first word is the low 16 bits), loaded from 0x20000 one instruction an
// address (the IOP set-up every Model 2B game's boot writes; the generated code
// compares program memory with it on its first run). Every instruction of the
// upload is recompiled, executed or not (some are data): no reachability
// analysis to get wrong. Each one runs MAME's interpreter step as
// src/runtime/sharc's step() does (step_pre, step_fetch, step_loop_end,
// the instruction's handler, step_after). The handlers are upstream's, made
// templates on the opcode: the output carries a copy of every sharcop_* of
// sharcops.hxx (and COMPUTE, on its compute field) with m_core->opcode replaced
// by the template parameter, so each instruction's decoding folds at compile
// time; the copy is made at each run, from the file as it is (the code is
// MAME's word for word: only that substitution and COMPUTE(x) calls becoming
// COMPUTE_t<x>()). A handler is named from MAME's opcode table
// (sharcops_table.inc, as build_opcode_table does). The loop-end
// check only where a do-until can end a loop (laddr is only ever a do-until's
// target), and control falls through to the next instruction when the
// pipeline's next address is it, else through a dispatch on it. A one-nop
// do-until loop that loops again (the SHARC polling its flags) returns
// "waiting" to the caller. The output includes the lifted core (it is the
// core's other build): link it instead of the `sharc` library.

#include <algorithm>

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <bit>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

struct Op {
    uint16_t mask, bits;
    std::string name;
};

// MAME's table, in its order; the first match wins (build_opcode_table).
std::vector<Op> read_ops(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string text{std::istreambuf_iterator<char>(f), {}};
    const std::regex entry(R"(\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*&adsp21062_device::(\w+)\s*\})");
    std::vector<Op> ops;
    for (std::sregex_iterator i(text.begin(), text.end(), entry), end; i != end; ++i)
        ops.push_back({uint16_t(std::stoul((*i)[1], nullptr, 16)), uint16_t(std::stoul((*i)[2], nullptr, 16)), (*i)[3]});
    if (ops.empty()) throw std::runtime_error(path + ": no opcode table entries");
    return ops;
}

// The text from `open` (a '{') to its matching '}', skipping comments and
// string and character literals.
size_t match_brace(const std::string &t, size_t open) {
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') i = t.find('\n', i);
        else if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') i = t.find("*/", i + 2) + 1;
        else if (c == '"' || c == '\'') {
            for (++i; i < t.size() && t[i] != c; ++i)
                if (t[i] == '\\') ++i;
        } else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
        if (i == std::string::npos) break;
    }
    throw std::runtime_error("unbalanced braces in sharcops.hxx");
}

// Every identifier `word` not preceded by '.', "->" or an identifier character.
std::string replace_word(const std::string &t, const std::string &word, const std::string &with, bool member_too) {
    std::string out;
    size_t i = 0;
    for (size_t at; (at = t.find(word, i)) != std::string::npos; i = at + word.size()) {
        const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const bool before_ok = at == 0 || !ident(t[at - 1]);
        const bool after_ok = at + word.size() >= t.size() || !ident(t[at + word.size()]);
        const bool member = at >= 1 && (t[at - 1] == '.' || (at >= 2 && t[at - 2] == '-' && t[at - 1] == '>'));
        out += t.substr(i, at - i);
        out += before_ok && after_ok && (member_too || !member) ? with : word;
    }
    return out + t.substr(i);
}

// COMPUTE(x) -> COMPUTE_t<uint32_t(x)>().
std::string templated_compute_calls(const std::string &t) {
    std::string out;
    size_t i = 0;
    for (size_t at; (at = t.find("COMPUTE(", i)) != std::string::npos;) {
        const bool word = at == 0 || !(std::isalnum(static_cast<unsigned char>(t[at - 1])) || t[at - 1] == '_');
        if (!word) { out += t.substr(i, at + 8 - i); i = at + 8; continue; }
        int depth = 0;
        size_t close = at + 7;
        for (; close < t.size(); ++close) {
            if (t[close] == '(') ++depth;
            else if (t[close] == ')' && --depth == 0) break;
        }
        out += t.substr(i, at - i) + "COMPUTE_t<uint32_t(" + t.substr(at + 8, close - at - 8) + ")>()";
        i = close + 1;
    }
    return out + t.substr(i);
}

// sharcops.hxx's sharcop_* and COMPUTE, as templates on the opcode.
std::string templated_handlers(const std::string &path, size_t &count) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\n(\[\[noreturn\]\] )?void adsp21062_device::(sharcop_\w+|COMPUTE)\(([^)]*)\)\s*\n\{)");
    std::ostringstream o;
    count = 0;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const std::string name = (*i)[2];
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        const size_t close = match_brace(t, open);
        std::string body = t.substr(open, close - open + 1);
        if (name == "COMPUTE") {
            if ((*i)[3] != "uint32_t opcode") throw std::runtime_error("COMPUTE's signature changed upstream");
            o << "template <uint32_t OPCODE>\nSHARC_INLINE void adsp21062_device::COMPUTE_t()\n" << replace_word(body, "opcode", "OPCODE", false) << "\n\n";
        } else {
            if (!(*i)[3].str().empty()) throw std::runtime_error(name + " takes arguments upstream");
            body = templated_compute_calls(replace_word(body, "m_core->opcode", "OPCODE", true));
            for (const char *f : {"GET_UREG", "SET_UREG", "SHIFT_OPERATION_IMM", "dm_read32", "dm_write32", "pm_read32", "pm_write32"})
                body = replace_word(body, f, std::string(f) + "_i", false);
            // Forced inline (SHARC_INLINE): the Vita's GCC otherwise calls most of
            // them (a call an instruction, the registers saved and m_core reloaded).
            o << "template <uint64_t OPCODE>\n" << ((*i)[1].matched ? (*i)[1].str() : std::string("SHARC_INLINE ")) << "void adsp21062_device::"
              << name << "_t()\n" << body << "\n\n";
        }
        ++count;
    }
    if (count < 40) throw std::runtime_error(path + ": only " + std::to_string(count) + " handlers found");
    return o.str();
}

// sharcops.hxx's handler bodies, by name.
std::map<std::string, std::string> handler_bodies(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\nvoid adsp21062_device::(sharcop_\w+)\(\)\s*\n\{)");
    std::map<std::string, std::string> bodies;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        bodies[i->str(1)] = t.substr(open, match_brace(t, open) - open + 1);
    }
    return bodies;
}

// The handlers of sharcops.hxx that can delay a branch (CHANGE_PC_DELAYED):
// each does it only when the opcode's J bit (26) is set.
std::set<std::string> delaying_handlers(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\nvoid adsp21062_device::(sharcop_\w+)\(\)\s*\n\{)");
    std::set<std::string> names;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        const std::string body = t.substr(open, match_brace(t, open) - open + 1);
        if (body.find("CHANGE_PC_DELAYED") == std::string::npos) continue;
        if (body.find("op_get_jump_j(m_core->opcode)") == std::string::npos)
            throw std::runtime_error(std::string(i->str(1)) + " delays a branch without the J bit upstream");
        names.insert(i->str(1));
    }
    if (names.size() < 8) throw std::runtime_error(path + ": only " + std::to_string(names.size()) + " delayed branch handlers found");
    return names;
}

std::string read_text(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

// Every #define of a file, with its continuation lines.
std::string defines(const std::string &path) {
    std::istringstream in(read_text(path));
    std::string line, out;
    bool cont = false;
    while (std::getline(in, line)) {
        if (cont || line.rfind("#define", 0) == 0) {
            out += line + "\n";
            cont = !line.empty() && line.back() == '\\';
        }
    }
    return out;
}

// sharcops.hxx's inline helpers (the PC, loop and status stacks, the
// conditions), word for word.
std::string inline_helpers(const std::string &path) {
    const std::string t = read_text(path);
    const std::regex def(R"(\ninline [^\n]*adsp21062_device::\w+\([^)]*\)\s*\n\{)");
    std::string out;
    size_t count = 0;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t start = size_t(i->position(0)) + 1, open = size_t(i->position(0) + i->length(0) - 1);
        out += "SHARC_INLINE " + t.substr(start + 7, match_brace(t, open) - start - 6) + "\n\n"; // "inline " forced
        ++count;
    }
    if (count < 9) throw std::runtime_error(path + ": fewer inline helpers than expected");
    return out;
}

// GET_UREG, SET_UREG and SHIFT_OPERATION_IMM, word for word, as inline
// GET_UREG_i, SET_UREG_i, SHIFT_OPERATION_IMM_i: called by the templated
// handlers with constant arguments, their switches fold.
std::string inline_accessors(const std::string &path) {
    const std::string t = read_text(path);
    std::string out;
    for (const char *name : {"GET_UREG", "SET_UREG", "SHIFT_OPERATION_IMM"}) {
        const std::regex def(std::string(R"(\n(uint32_t|void) adsp21062_device::)") + name + R"(\(([^)]*)\)\s*\n\{)");
        std::smatch m;
        if (!std::regex_search(t, m, def)) throw std::runtime_error(path + ": no " + name);
        const size_t open = size_t(m.position(0) + m.length(0) - 1);
        const std::string body = t.substr(open, match_brace(t, open) - open + 1);
        out += "SHARC_INLINE " + m[1].str() + " adsp21062_device::" + name + "_i(" + m[2].str() + ")\n" + body + "\n\n";
    }
    return out;
}

// compute.hxx without its #include lines and its two lookup tables.
std::string compute_inline(const std::string &path) {
    std::string t = read_text(path);
    t = std::regex_replace(t, std::regex(R"(\n#include [^\n]*)"), "\n");
    const std::regex table(R"(const uint32_t adsp21062_device::\w+\[128\] =\s*\{[^}]*\};)");
    if (std::distance(std::sregex_iterator(t.begin(), t.end(), table), std::sregex_iterator()) != 2)
        throw std::runtime_error(path + ": not the two lookup tables expected");
    t = std::regex_replace(t, table, "");
    // Its member functions forced inline (sharc.h declares them inline).
    return std::regex_replace(t, std::regex(R"(\n([A-Za-z][^\n]*?adsp21062_device::\w+\([^\n]*\))[ \t]*\n\{)"), "\nSHARC_INLINE $1\n{");
}

std::string handler(const std::vector<Op> &ops, uint64_t opcode) {
    const uint16_t op = uint16_t(((opcode >> 39) & 0x1ff) << 7);
    for (const Op &o : ops)
        if ((o.mask & op) == o.bits) return o.name;
    return "sharcop_unimplemented";
}

int32_t sext24(uint64_t v) { return int32_t(uint32_t(v & 0xffffff) << 8) >> 8; }

std::string hx(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%" PRIx64, v);
    return b;
}

} // namespace

int main(int argc, char **argv) {
    std::string ops_path = "src/runtime/sharc/sharcops_table.inc", sharcops_path = "src/runtime/sharc/sharcops.hxx",
                compute_path = "src/runtime/sharc/compute.hxx";
    bool full_steps = false; // every instruction the interpreter's full step (no plain runs)
    // --fast (build_vita.py --fast-inaccuracy): less bookkeeping around the same handlers. Pinned branches write
    // only the pipeline registers their handler and the taken test read (pc, daddr; nfaddr when delayed) and no
    // ASTAT history (kept before the ends of loops on an ASTAT condition); a counter loop looping back to its top
    // in the chunk writes the pipeline only when it leaves; a branch taken forward inside the chunk skips the
    // budget test (backward edges and every exit keep it). What the SHARC computes is unchanged; the run can
    // overshoot need_output's word by a few instructions, and a jump into the three instructions before such a
    // loop end can see another ASTAT history than the interpreter's.
    bool fast = false;
    std::string cfg_path; // --cfg FILE: each instruction's handler, fall-through and constant target (the layout's input)
    // --layout PROFILE: the chunks cut by affinity instead of by address (a profiled build's dump: executions per
    // instruction and the transfers between them; seeds/<set>_sharc_layout.txt). Exact: where the code sits only.
    std::string layout_path;
    uint32_t layout_cap = 384; // --layout-cap: most instructions a chunk with --layout
    // --hle FILE (with --fast only; seeds/<set>_sharc_hle.txt): a game's HLE routines (rules.md "Coprocessor
    // HLE"). Lines: "include PATH" (the header defining them, from src/), "ADDRESS NAME": at that entry the
    // chunk calls run_hle<sharc_hle_NAME>(done) and goes on where it returns, or runs the recompiled code (0).
    std::string hle_path;
    // Instructions a chunk (a function and a file). fvipers' fight, PC profile, chunk
    // calls a frame / dispatches per 1000 instructions: 256 4,116 / 109, 512 4,035 /
    // 107, 1024 2,300 / 76, 2048 1,279 / 58 (its hot code calls far away); yet the
    // fight check on the PC 314-318 M instructions/s with 256, 272-277 with 1024,
    // 269-273 with 2048 (bigger functions, worse code), and the Vita's compiler
    // (handlers forced inline) takes 133 s for a 1024 chunk, 183 s and 800 MB for 2048.
    uint32_t chunk = 256;
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops SHARCOPS_TABLE.inc] [--fast] [--cfg FILE] [--layout PROFILE [--layout-cap N]] [--hle FILE]\n");
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--sharcops") && i + 1 < argc) sharcops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--compute") && i + 1 < argc) compute_path = argv[++i];
        else if (!std::strcmp(argv[i], "--full-steps")) full_steps = true;
        else if (!std::strcmp(argv[i], "--fast")) fast = true;
        else if (!std::strcmp(argv[i], "--cfg") && i + 1 < argc) cfg_path = argv[++i];
        else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
        else if (!std::strcmp(argv[i], "--layout-cap") && i + 1 < argc) layout_cap = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--hle") && i + 1 < argc) hle_path = argv[++i];
        else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else {
            std::fprintf(stderr, "m2sharcrecomp: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!chunk || (chunk & (chunk - 1))) { // the dispatch divides by it: a shift (no divider on the Vita)
        std::fprintf(stderr, "m2sharcrecomp: --chunk must be a power of two\n");
        return 2;
    }
    try {
        std::ifstream f(argv[1], std::ios::binary);
        const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(f), {}};
        if (raw.empty() || raw.size() % 6) throw std::runtime_error(std::string(argv[1]) + ": not a SHARC program (16-bit words, 3 an instruction)");
        const std::vector<Op> ops = read_ops(ops_path);
        constexpr uint32_t base = 0x20000;
        const uint32_t n = uint32_t(raw.size() / 6);
        std::vector<uint64_t> prog(n);
        for (uint32_t k = 0; k < n; ++k)
            for (int w = 0; w < 3; ++w)
                prog[k] |= uint64_t(raw[k * 6 + w * 2] | raw[k * 6 + w * 2 + 1] << 8) << (16 * w);

        // Where loops can end (a do-until's target; laddr has no other source),
        // those of loops on a condition (the ASTAT history), the wait loops.
        std::set<uint32_t> loop_ends, cond_loop_ends, waits;
        std::map<uint32_t, std::set<uint32_t>> loop_tops; // by loop end: where its do-untils loop back to
        // Loop ends on a condition that does not read ASTAT (DO_CONDITION_CODE: FLAG0-3 and their NOT, BM, NOT BM,
        // LCE, FOREVER): step_loop_end's ASTAT juggling (the history from three instructions back, then astat_old)
        // leaves ASTAT as it was and the test does not see it, so they run plain, as counter loop ends, with the
        // test inline. An end shared with a counter loop or another condition stays a full step.
        std::map<uint32_t, bool> flag_cond; // by loop end: every do-until ending there tests such a condition
        const auto astat_free = [](unsigned cond) { return (cond >= 0x09 && cond <= 0x0c) || cond == 0x0e || cond == 0x0f ||
                                                           (cond >= 0x19 && cond <= 0x1c) || cond == 0x1e || cond == 0x1f; };
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (h.rfind("sharcop_do_until", 0) != 0) continue;
            const uint32_t end = uint32_t(base + k + sext24(prog[k]));
            loop_ends.insert(end);
            loop_tops[end].insert(base + k + 1);
            if (h == "sharcop_do_until") cond_loop_ends.insert(end); // loop_type 0; the counter ones 1-3
            const bool free = h == "sharcop_do_until" && astat_free(unsigned(prog[k] >> 33) & 0x1f); // op_get_cond
            const auto it = flag_cond.find(end);
            flag_cond[end] = free && (it == flag_cond.end() || it->second);
            if (end == base + k + 1 && k + 1 < n && handler(ops, prog[k + 1]) == "sharcop_nop") waits.insert(end);
        }
        for (const auto &[end, free] : flag_cond) if (free) cond_loop_ends.erase(end);
        std::set<uint32_t> flag_loop_ends; // the plain ones on a condition (ends of counter loops: loop_ends alone)
        for (const auto &[end, free] : flag_cond) if (free) flag_loop_ends.insert(end);

        size_t handlers = 0;
        const std::string templates = templated_handlers(sharcops_path, handlers);
        // The program in chunks (--chunk, 256 instructions), one function each
        // and one file each, compiled in parallel (the whole program in one
        // function, before the forced inlining: 9 min to optimise on the PC; in
        // one file: over 20 min with the Vita's compiler). Control falls through within a chunk and goes back
        // to run_generated's dispatch between chunks. OUT.cpp holds the core
        // (sharc.cpp, compiled once) and the dispatch; sharc_handlers.h, next
        // to it, what every chunk needs inline: sharcops.hxx's macros and inline
        // helpers, compute.hxx without its two lookup tables (the only
        // definitions it cannot repeat) and the templated handlers.
        const std::string out_path = argv[2];
        const std::string dir = out_path.find('/') == std::string::npos ? "." : out_path.substr(0, out_path.rfind('/'));
        const auto write_file = [](const std::string &path, const std::string &text) {
            std::ofstream f(path);
            if (!(f << text)) throw std::runtime_error("cannot write " + path);
        };
        // Plain instructions: their handlers touch neither the pipeline nor the
        // stacks, loops, interrupts or system registers (sharcops.hxx: outside
        // control flow, the pipeline registers are only read in error messages).
        // They run without the interpreter's per-instruction bookkeeping: the
        // pipeline is written once when a run of them ends, as the last one
        // leaves it (each arrival is linear: daddr, faddr, nfaddr consecutive),
        // and their count is kept in a register. Every other instruction runs
        // the full step, and so do what the opcodes say needs it: the two delay
        // slots of a delayed branch (J bit) and the ends of loops on a condition.
        // The three before those stay plain but shift the ASTAT history
        // (step_loop_end reads it). The ends of counter loops stay plain with
        // step_loop_end's counter part inline: a run ends where one loops back. What no opcode tells
        // (a MODE1 write's latency, from a register write or a status stack pop;
        // a pipeline not linear) is checked where a plain instruction is entered
        // after a full step (plain_ready): the interpreter's steps run until it
        // is gone. The step_pre checks a plain run skips (a delayed IOP write,
        // DMA cycles, interrupts, idling) only change when a full step runs, or
        // with a DMA running: run_generated refuses that. So step_pre runs
        // only after full steps (and at run_generated's entry), not where a run
        // ends, a pinned branch is taken or a counter loop loops back: those
        // count (done) and test the budget. m_instructions is set on exit.
        const std::set<std::string> plain_names = {
            "sharcop_compute", "sharcop_compute_ureg_dmpm_premod", "sharcop_compute_ureg_dmpm_postmod",
            "sharcop_compute_dm_to_dreg_immmod", "sharcop_compute_dreg_to_dm_immmod", "sharcop_compute_pm_to_dreg_immmod",
            "sharcop_compute_dreg_to_pm_immmod", "sharcop_compute_ureg_to_ureg", "sharcop_compute_dreg_dm_dreg_pm",
            "sharcop_imm_shift", "sharcop_imm_shift_dreg_dmpm", "sharcop_compute_modify", "sharcop_dm_to_ureg_direct",
            "sharcop_dm_to_ureg_indirect", "sharcop_pm_to_ureg_direct", "sharcop_pm_to_ureg_indirect",
            "sharcop_ureg_to_dm_direct", "sharcop_ureg_to_dm_indirect", "sharcop_ureg_to_pm_direct",
            "sharcop_ureg_to_pm_indirect", "sharcop_imm_to_dmpm", "sharcop_imm_to_ureg", "sharcop_modify", "sharcop_nop"};
        // sysreg_bitop on USTAT1, USTAT2, ASTAT or STKY (sreg 0, 1, 0xc, 0xe): SET_UREG only stores them (ASTAT's
        // FLG outputs call m_flag_out_cb, empty here; STKY keeps its stack bits); MODE1 (latency), MODE2, IRPTL and
        // IMASK (interrupts) stay full steps.
        const auto plain_bitop = [&](const std::string &h, uint64_t op) {
            if (h != "sharcop_sysreg_bitop") return false;
            const unsigned sreg = unsigned(op >> 32) & 0xf, bop = unsigned(op >> 37) & 0x7;
            return (sreg == 0x0 || sreg == 0x1 || sreg == 0xc || sreg == 0xe) && (bop <= 2 || bop == 4 || bop == 5);
        };
        const auto writes_sysreg = [&](const std::string &h, uint64_t op) {
            unsigned ureg = 0;
            if (h == "sharcop_compute_ureg_dmpm_premod" || h == "sharcop_compute_ureg_dmpm_postmod" ||
                h == "sharcop_compute_ureg_to_ureg") ureg = unsigned(op >> 23) & 0xff;
            else if (h == "sharcop_imm_to_ureg" || h == "sharcop_dm_to_ureg_direct" || h == "sharcop_dm_to_ureg_indirect" ||
                     h == "sharcop_pm_to_ureg_direct" || h == "sharcop_pm_to_ureg_indirect") ureg = unsigned(op >> 32) & 0xff;
            else return false;
            return (ureg >> 4) == 6 || (ureg >> 4) == 7;
        };
        // Branches, calls, returns and do-untils run inside plain runs too
        // ("pinned", as m2tgprecomp's branches): their handler, MAME's, on the
        // pipeline as the fetch of a linear arrival leaves it (constants), then
        // a direct goto to a constant target where the branch is taken (the
        // count, budget and step_pre first, as at a run's end), the dispatch
        // for a computed one. A delayed branch's two delay slots run in the
        // run (plain, same chunk, no loop end) and the jump follows the second
        // (`pending`: the target CHANGE_PC_DELAYED left in nfaddr). Not pinned:
        // a jump clearing an interrupt (CI bit 24: the status stack, MODE1's
        // latency, interrupt_active), rti, anything else: full steps.
        const std::set<std::string> delaying = delaying_handlers(sharcops_path);
        const std::set<std::string> pinned_names = {
            "sharcop_direct_jump", "sharcop_relative_jump", "sharcop_indirect_jump", "sharcop_relative_jump_compute",
            "sharcop_direct_call", "sharcop_relative_call", "sharcop_indirect_call", "sharcop_relative_call_compute",
            "sharcop_rts", "sharcop_do_until", "sharcop_do_until_counter_imm", "sharcop_do_until_counter_ureg"};
        {
            const auto bodies = handler_bodies(sharcops_path);
            for (const std::string &name : pinned_names) {
                const auto it = bodies.find(name);
                if (it == bodies.end()) throw std::runtime_error(name + " not found upstream");
                const std::string &b = it->second;
                for (const char *w : {"SET_UREG", "idle", "imask", "irq_pending", "systemreg_latency", "dma_", "iop"})
                    if (b.find(w) != std::string::npos) throw std::runtime_error(name + " touches " + w + " upstream: not pinned");
                if (b.find("interrupt_active") != std::string::npos && b.find("if (ci)") == std::string::npos)
                    throw std::runtime_error(name + " changes interrupt_active without the CI bit upstream");
            }
        }
        const auto pinned_op = [&](const std::string &h, uint64_t op) {
            if (!pinned_names.count(h)) return false;
            return h.find("_jump") == std::string::npos || !((op >> 24) & 1); // CI
        };
        const auto has_j = [&](const std::string &h, uint64_t op) { return delaying.count(h) && ((op >> 26) & 1); };
        // A branch's constant target (direct, relative), or 0.
        const auto target_of = [&](const std::string &h, uint32_t a, uint64_t op) -> uint32_t {
            if (h == "sharcop_direct_jump" || h == "sharcop_direct_call") return uint32_t(op & 0xffffff);
            if (h == "sharcop_relative_jump" || h == "sharcop_relative_call") return uint32_t(a + sext24(op));
            return 0;
        };
        std::vector<bool> full(n, false), pinned(n, false), astat_shift(n, false);
        std::vector<uint8_t> why(n, 0); // 1 itself, 2 a delay slot, 4 a loop end (the profile's statistics)
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (pinned_op(h, prog[k]) && !loop_ends.count(base + k)) pinned[k] = true;
            else if ((!plain_names.count(h) && !plain_bitop(h, prog[k])) || writes_sysreg(h, prog[k])) full[k] = true, why[k] |= 1;
            if (cond_loop_ends.count(base + k)) full[k] = true, why[k] |= 4;
            if (cond_loop_ends.count(base + k))
                for (uint32_t j = k >= 3 ? k - 3 : 0; j < k; ++j) astat_shift[j] = true;
            if (flag_loop_ends.count(base + k)) astat_shift[k] = true; // as the full step's fetch did
        }
        // The chunks: which instructions each holds, in the order it emits them. By address (--chunk
        // instructions each) or, with --layout, by affinity: units (ranges whose instructions reach the
        // next only by falling through: never split, but at the cap) merged along their heaviest
        // transfers in the profile while the chunk stays within --layout-cap (Pettis-Hansen); executed
        // groups first-fit into chunks, the rest by address. Fall-through flow, not in the profile, is an
        // instruction's executions minus the transfers into it. scripts/sharc_layout.py is the same model.
        const auto falls = [&](uint32_t k) { // can k's execution go on at k + 1 (not counting delay slots)?
            const std::string h = handler(ops, prog[k]);
            const bool jump = h.find("_jump") != std::string::npos || h == "sharcop_rts";
            const bool call = h.find("_call") != std::string::npos;
            const bool always = (unsigned(prog[k] >> 33) & 0x1f) == 0x1f; // op_get_cond: TRUE
            return !(call || h == "sharcop_rti" || (jump && always));
        };
        const auto reaches = [&](uint32_t k) { // k -> k + 1 without a transfer
            if (k + 1 >= n) return false;
            if (!falls(k) && !has_j(handler(ops, prog[k]), prog[k])) return false;
            return !(k >= 2 && !falls(k - 2) && has_j(handler(ops, prog[k - 2]), prog[k - 2])); // k: a never-falling branch's second slot
        };
        std::vector<std::vector<uint32_t>> chunk_list;
        bool by_layout = false;
        if (!layout_path.empty()) {
            std::ifstream pf(layout_path);
            if (!pf) throw std::runtime_error("cannot read " + layout_path);
            std::vector<double> count(n, 0.0);
            struct Xfer { uint32_t from, to; double n; };
            std::vector<Xfer> xfers;
            std::vector<double> into(n, 0.0);
            std::string line;
            uint32_t crc = 0;
            bool has_crc = false;
            while (std::getline(pf, line)) {
                if (line.empty()) continue;
                if (line[0] == '#') {
                    const size_t at = line.find("program crc32 ");
                    if (at != std::string::npos) crc = uint32_t(std::strtoul(line.c_str() + at + 14, nullptr, 16)), has_crc = true;
                    continue;
                }
                std::istringstream ls(line);
                if (line[0] == '>') {
                    std::string mark, from, to, kind;
                    double e = 0;
                    ls >> mark >> from >> to >> kind >> e;
                    const uint32_t t = uint32_t(std::strtoul(to.c_str(), nullptr, 16)) - base;
                    if (t >= n) continue;
                    into[t] += e;
                    if (from != "entry") {
                        const uint32_t fr = uint32_t(std::strtoul(from.c_str(), nullptr, 16)) - base;
                        if (fr < n) xfers.push_back({fr, t, e});
                    }
                } else {
                    std::string addr;
                    double e = 0;
                    ls >> addr >> e;
                    const uint32_t k = uint32_t(std::strtoul(addr.c_str(), nullptr, 16)) - base;
                    if (k < n) count[k] = e;
                }
            }
            const uint32_t our_crc = crc32(raw.data(), raw.size());
            if (!has_crc || crc != our_crc) {
                std::fprintf(stderr, "m2sharcrecomp: %s is %s, not this program's (crc32 %08x): chunks by address\n", layout_path.c_str(),
                             has_crc ? "another program's profile" : "a profile without the program's crc32", our_crc);
            } else {
                by_layout = true;
                for (uint32_t k = 1; k < n; ++k)
                    if (reaches(k - 1) && count[k] - into[k] > 0.5) xfers.push_back({k - 1, k, count[k] - into[k]});
                // Units, split at the cap.
                std::vector<uint32_t> unit_of(n);
                std::vector<std::vector<uint32_t>> units;
                for (uint32_t k = 0; k < n; ++k) {
                    if (k == 0 || !reaches(k - 1) || units.back().size() >= layout_cap) units.emplace_back();
                    units.back().push_back(k);
                    unit_of[k] = uint32_t(units.size() - 1);
                }
                std::map<std::pair<uint32_t, uint32_t>, double> weight;
                for (const Xfer &x : xfers) {
                    const uint32_t a = unit_of[x.from], b = unit_of[x.to];
                    if (a != b) weight[{std::min(a, b), std::max(a, b)}] += x.n;
                }
                std::vector<std::pair<double, std::pair<uint32_t, uint32_t>>> order;
                for (const auto &[ab, w] : weight) order.push_back({w, ab});
                std::sort(order.begin(), order.end(), [](const auto &x, const auto &y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
                std::vector<uint32_t> parent(units.size()), size(units.size());
                for (uint32_t u = 0; u < units.size(); ++u) parent[u] = u, size[u] = uint32_t(units[u].size());
                const auto find = [&](uint32_t u) {
                    while (parent[u] != u) u = parent[u] = parent[parent[u]];
                    return u;
                };
                for (const auto &[w, ab] : order) {
                    const uint32_t ga = find(ab.first), gb = find(ab.second);
                    if (ga == gb || size[ga] + size[gb] > layout_cap) continue;
                    parent[gb] = ga;
                    size[ga] += size[gb];
                }
                std::map<uint32_t, std::vector<uint32_t>> groups; // representative -> units, in address order
                for (uint32_t u = 0; u < units.size(); ++u) groups[find(u)].push_back(u);
                std::vector<std::pair<double, std::vector<uint32_t>>> hot; // executions, units
                std::vector<uint32_t> cold;
                for (const auto &[g, us] : groups) {
                    double e = 0;
                    for (uint32_t u : us) for (uint32_t k : units[u]) e += count[k];
                    if (e > 0) hot.push_back({e, us});
                    else cold.insert(cold.end(), us.begin(), us.end());
                }
                std::stable_sort(hot.begin(), hot.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
                std::vector<uint32_t> fill;
                for (const auto &[e, us] : hot) { // first fit
                    uint32_t sz = 0;
                    for (uint32_t u : us) sz += uint32_t(units[u].size());
                    size_t c = 0;
                    while (c < chunk_list.size() && fill[c] + sz > layout_cap) ++c;
                    if (c == chunk_list.size()) chunk_list.emplace_back(), fill.push_back(0);
                    for (uint32_t u : us) chunk_list[c].insert(chunk_list[c].end(), units[u].begin(), units[u].end());
                    fill[c] += sz;
                }
                std::sort(cold.begin(), cold.end());
                bool fresh = true;
                for (uint32_t u : cold) { // by address, after the executed code
                    if (fresh || chunk_list.back().size() + units[u].size() > chunk) chunk_list.emplace_back(), fresh = false;
                    chunk_list.back().insert(chunk_list.back().end(), units[u].begin(), units[u].end());
                }
            }
        }
        if (!by_layout)
            for (uint32_t k = 0; k < n; ++k) {
                if (k % chunk == 0) chunk_list.emplace_back();
                chunk_list.back().push_back(k);
            }
        const uint32_t chunks = uint32_t(chunk_list.size());
        if (chunks > 0xffff) throw std::runtime_error("too many chunks");
        std::vector<uint32_t> chunk_of(n);
        std::vector<int64_t> next_text(n, -1); // the instruction its chunk emits right after k
        for (uint32_t c = 0; c < chunks; ++c)
            for (size_t i = 0; i < chunk_list[c].size(); ++i) {
                chunk_of[chunk_list[c][i]] = c;
                if (i + 1 < chunk_list[c].size()) next_text[chunk_list[c][i]] = chunk_list[c][i + 1];
            }
        const auto text_next = [&](uint32_t k) { return next_text[k] == int64_t(k) + 1; }; // k + 1 follows k in the chunk's text
        const auto in_chunk = [&](uint32_t target, uint32_t c) { return target >= base && target - base < n && chunk_of[target - base] == c; };

        // Delay slots, to a fixed point (a branch that cannot keep its slots in
        // the run becomes a full step, and its slots too).
        for (bool changed = true; changed;) {
            changed = false;
            for (uint32_t k = 0; k < n; ++k) {
                const std::string h = handler(ops, prog[k]);
                if (!has_j(h, prog[k])) continue;
                const bool slots_ok = pinned[k] && !full[k] && k + 2 < n && text_next(k) && text_next(k + 1) &&
                                      !full[k + 1] && !full[k + 2] && !pinned[k + 1] && !pinned[k + 2] &&
                                      !loop_ends.count(base + k + 1) && !loop_ends.count(base + k + 2);
                if (slots_ok) continue;
                for (uint32_t j = k; j <= k + 2 && j < n; ++j) {
                    if (j > k) why[j] |= 2;
                    if (!full[j]) full[j] = true, changed = true;
                    if (j == k && !(why[k] & 1)) why[k] |= 1;
                }
            }
        }
        for (uint32_t k = 0; k < n; ++k) if (full[k]) pinned[k] = false;

        // The HLE routines' entries (--hle): a plain instruction, no loop end, no delay slot.
        std::map<uint32_t, std::string> hle; // index -> routine name
        std::string hle_include;
        if (!hle_path.empty()) {
            if (!fast) throw std::runtime_error("--hle needs --fast (HLE routines are for fast_inaccuracy builds only)");
            std::ifstream hf(hle_path);
            if (!hf) throw std::runtime_error("cannot read " + hle_path);
            std::string line;
            while (std::getline(hf, line)) {
                std::istringstream ls(line);
                std::string first, second;
                if (!(ls >> first) || first[0] == '#') continue;
                ls >> second;
                if (first == "include") { hle_include = second; continue; }
                const uint32_t k = uint32_t(std::strtoul(first.c_str(), nullptr, 16)) - base;
                if (k >= n || second.empty()) throw std::runtime_error(hle_path + ": bad line: " + line);
                const bool slot = (k >= 1 && pinned[k - 1] && has_j(handler(ops, prog[k - 1]), prog[k - 1])) ||
                                  (k >= 2 && pinned[k - 2] && has_j(handler(ops, prog[k - 2]), prog[k - 2]));
                if (full[k] || pinned[k] || loop_ends.count(base + k) || slot)
                    throw std::runtime_error(hle_path + ": " + first + " is not a plain instruction outside loop ends and delay slots");
                hle[k] = second;
            }
            if (!hle.empty() && hle_include.empty()) throw std::runtime_error(hle_path + ": no include line");
        }
        if (full_steps) full.assign(n, true), pinned.assign(n, false);

        if (const char *prof = std::getenv("M2SHARC_PROFILE_FILE")) { // the dynamic share, from m2sharcprof's counts
            std::ifstream pf(prof);
            uint32_t addr = 0;
            uint64_t count = 0, all = 0, by[8] = {}, shifts = 0, pins = 0;
            while (pf >> std::hex >> addr >> std::dec >> count) {
                const uint32_t k = addr - base;
                all += count;
                if (k < n) by[full[k] ? why[k] : 0] += count, shifts += !full[k] && astat_shift[k] ? count : 0, pins += pinned[k] ? count : 0;
            }
            const auto pc = [&](uint64_t v) { return all ? 100.0 * double(v) / double(all) : 0.0; };
            std::printf("m2sharcrecomp: steps, weighted by the profile: %.1f %% in plain runs (%.1f %% pinned branches, %.1f %% shifting the ASTAT "
                        "history); full: %.1f %% as control flow/sysreg writes, %.1f %% only as delay slots, %.1f %% only as loop ends, "
                        "%.1f %% for several reasons\n",
                        pc(by[0]), pc(pins), pc(shifts), pc(by[1]), pc(by[2]), pc(by[4]), pc(by[3] + by[5] + by[6] + by[7]));
        }
        uint32_t plain_count = 0;
        for (uint32_t k = 0; k < n; ++k) plain_count += !full[k];

        if (!cfg_path.empty()) {
            // Per instruction: address, handler, f (falls through to the next) or n (never: an
            // unconditional jump or return, a call (its return comes back through the rts), rti), j
            // when delayed (the two slots run first), its constant target or -, and the kind (p, b, F).
            std::ostringstream c;
            c << "# m2sharcrecomp --cfg: program crc32 " << hx(crc32(raw.data(), raw.size())) << ", " << n
              << " instructions; address handler fall delayed target kind\n";
            for (uint32_t k = 0; k < n; ++k) {
                const std::string h = handler(ops, prog[k]);
                const bool jump = h.find("_jump") != std::string::npos || h == "sharcop_rts";
                const bool call = h.find("_call") != std::string::npos;
                const bool always = (unsigned(prog[k] >> 33) & 0x1f) == 0x1f; // op_get_cond: TRUE
                const bool falls = !(call || h == "sharcop_rti" || (jump && always));
                const uint32_t t = target_of(h, base + k, prog[k]);
                char line[160];
                std::snprintf(line, sizeof line, "%05x %s %c %c %s %c\n", unsigned(base + k), h.substr(8).c_str(), falls ? 'f' : 'n',
                              has_j(h, prog[k]) ? 'j' : '-', t ? hx(t).substr(2).c_str() : "-", full[k] ? 'F' : pinned[k] ? 'b' : 'p');
                c << line;
            }
            write_file(cfg_path, c.str());
        }

        for (const auto &e : std::filesystem::directory_iterator(dir))
            // (sharc_compact_* and sharc_mixed_*: an older generator's other versions)
            if (e.path().filename().string().rfind("sharc_chunk_", 0) == 0 || e.path().filename().string().rfind("sharc_compact_", 0) == 0 ||
                e.path().filename().string().rfind("sharc_mixed_", 0) == 0)
                std::filesystem::remove(e.path());
        write_file(dir + "/sharc_inline.h",
                   "// Generated by m2sharcrecomp: sharcops.hxx's macros and inline helpers, for\n"
                   "// sharc_handlers.h. From MAME's sources (BSD-3).\n"
                   "#pragma once\n\n// sharcops.hxx's macros\n" + defines(sharcops_path) +
                   "\n// sharcops.hxx's inline helpers\n" + inline_helpers(sharcops_path));
        write_file(dir + "/sharc_handlers.h",
                   "// Generated by m2sharcrecomp: what the chunks of the recompiled SHARC\n"
                   "// program need inline. Derived from MAME's sources (BSD-3) and the game: never commit.\n"
                   "#pragma once\n\n#include \"sharc_inline.h\"\n"
                   "\n// GET_UREG, SET_UREG, SHIFT_OPERATION_IMM, inline\n" + inline_accessors(sharcops_path) +
                   "\n// compute.hxx without its lookup tables\n" + compute_inline(compute_path) +
                   "\n// sharcops.hxx's handlers, templates on the opcode\n" + templates);
        // The chunks: MAME's handlers as templates on each opcode, the step
        // pieces inline (a shared-handler version, a few bytes an instruction,
        // ran 26 % slower on the Vita: its code cache was not the limit).
        for (uint32_t c = 0; c < chunks; ++c) {
            const std::vector<uint32_t> &list = chunk_list[c];
            std::ostringstream o;
            o << "// Generated by m2sharcrecomp: the recompiled SHARC program, chunk " << c << ".\n"
                 "// Derived from the game: never commit.\n"
                 "#include \"runtime/sharc/sharc.h\"\n\n"
                 "#include <algorithm>\n#include <bit>\n#include <cmath>\n#include <cstring>\n#include <iterator>\n#include <limits>\n\n"
                 "namespace rt::sharc {\n\n"
                 "#include \"runtime/sharc/sharcinternal.ipp\"\n"
                 "#include \"runtime/sharc/sharc_step.h\"\n"
                 "#include \"sharc_handlers.h\"\n"
              << (by_layout ? "\nextern const uint16_t sharc_chunk_of[]; // sharc_gen.cpp: each instruction's chunk (--layout)\n" : "")
              << [&] { // the game's HLE routines (--hle), where this chunk has an entry
                     for (uint32_t k : list) if (hle.count(k)) return "#include \"" + hle_include + "\"\n";
                     return std::string();
                 }()
              << "\n// 0 when control leaves the chunk (step_pre done), 1 when the SHARC waits,\n"
                 "// 2 when the budget ran out, 3 when the next instruction needs the\n"
                 "// interpreter's step (plain_ready).\n"
                 "template <>\nint adsp21062_device::run_chunk<" << c << ">(uint64_t &done)\n{\n"
                 "\tuint32_t plain = 0; // plain instructions run since the last count\n"
                 "\tuint32_t pending = 0; // a pinned delayed branch's target, taken after its second delay slot\n"
                 "\tfor (;;) {\n"
                 "\t\tif (!plain_ready()) return 3;\n"
                 "\t\tM2SHARC_EV(dispatches);\n"
                 "\t\tM2SHARC_DISPATCH(" << (by_layout ? "m_core->daddr - " + hx(base) + "u < " + std::to_string(n) + "u && sharc_chunk_of[m_core->daddr - " + hx(base) + "u] == " + std::to_string(c)
                                                    : "m_core->daddr - " + hx(base + list.front()) + "u < " + std::to_string(list.size()) + "u") << ");\n"
                 "\t\tpending = 0;\n"
                 "\t\tswitch (m_core->daddr) {\n";
            for (uint32_t k : list) o << "\t\tcase " << hx(base + k) << ": goto L_" << std::hex << base + k << std::dec << ";\n";
            o << "\t\tdefault: return 0;\n"
                 "\t\t}\n";
            for (uint32_t k : list) {
                const uint32_t a = base + k;
                const std::string call = handler(ops, prog[k]) + "_t<" + hx(prog[k]) + "ull>();";
                o << "\tL_" << std::hex << a << std::dec << ":\n"
                  << "\t\tM2SHARC_COUNT(" << k << ");\n";
                if (hle.count(k)) // the game's HLE routine; 0: the recompiled code runs
                    o << "\t\tdone += plain; plain = 0;\n"
                         "\t\tif (const uint32_t hle_to = run_hle<sharc_hle_" << hle.at(k) << ">(done)) {\n"
                         "\t\t\tM2SHARC_EV(hle_runs);\n"
                         "\t\t\tCHANGE_PC(hle_to);\n"
                         "\t\t\tif (done >= m_budget) return 2;\n"
                         "\t\t\tcontinue;\n"
                         "\t\t}\n"
                         "\t\tM2SHARC_EV(hle_declined);\n";
                if (!full[k]) {
                    const std::string h = handler(ops, prog[k]);
                    if (pinned[k] && fast) // what the handler (pc, daddr for the delay slots) and the taken test read
                        o << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << ";"
                          << (has_j(h, prog[k]) ? " m_core->nfaddr = " + hx(a + 3) + ";" : std::string()) << "\n";
                    else if (pinned[k]) // the pipeline as the fetch leaves it (a linear arrival: plain_ready), the full step's ASTAT history
                        o << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << "; m_core->faddr = " << hx(a + 2)
                          << "; m_core->nfaddr = " << hx(a + 3) << ";\n";
                    if (astat_shift[k] || (pinned[k] && !fast))
                        o << "\t\tm_core->astat_old_old_old = m_core->astat_old_old; m_core->astat_old_old = m_core->astat_old; "
                             "m_core->astat_old = m_core->astat;\n";
                    const bool loop_end = loop_ends.count(a) != 0;
                    if (loop_end && flag_loop_ends.count(a)) // step_loop_end, a condition without ASTAT (flag_cond)
                        o << "\t\t{\n"
                             "\t\tbool back = false;\n"
                             "\t\tif (!(m_core->stky & LSEM) && m_core->laddr.addr == " << hx(a) << ") {\n"
                             "\t\t\tif (DO_CONDITION_CODE(m_core->laddr.code)) { POP_LOOP(); POP_PC(); }\n"
                             "\t\t\telse back = true;\n"
                             "\t\t}\n";
                    else if (loop_end) // step_loop_end, counter loops (cond_loop_ends are full); the pipeline as CHANGE_PC leaves it
                        o << "\t\t{\n"
                             "\t\tbool back = false;\n"
                             "\t\tif (!(m_core->stky & LSEM) && m_core->laddr.addr == " << hx(a) << ") {\n"
                             "\t\t\tif (--m_core->curlcntr == 0) { POP_LOOP(); POP_PC(); }\n"
                             "\t\t\telse back = true;\n"
                             "\t\t}\n";
                    const bool callret = h.find("_call") != std::string::npos || h == "sharcop_rts"; // the routines' probe
                    const std::string to = has_j(h, prog[k]) ? "m_core->nfaddr" : "m_core->daddr";
                    if (callret) o << "\t\tM2SHARC_STACK_PRE();\n";
                    o << "\t\t" << call << "\n"
                      << "\t\t++plain;\n";
                    if (callret) o << "\t\tM2SHARC_STACK_POST(" << to << ", m_prof_base + done + plain);\n";
                    if (loop_end) {
                        const auto &tops = loop_tops.at(a);
                        const bool goto_top = tops.size() == 1 && in_chunk(*tops.begin(), c);
                        const std::string pipe = "m_core->pc = top; m_core->daddr = top; m_core->faddr = top + 1; m_core->nfaddr = top + 2;";
                        o << "\t\tif (back) {\n"
                             "\t\t\tconst uint32_t top = TOP_PC();\n"
                             "\t\t\tM2SHARC_EV(loop_backs);\n";
                        if (fast && goto_top && !full[*tops.begin() - base]) { // the pipeline only where the loop leaves the chunk (a full step's fetch reads it)
                            o << "\t\t\tdone += plain; plain = 0;\n";
                            if (waits.count(a)) o << "\t\t\tif (top == " << hx(a) << ") { " << pipe << " return 1; }\n";
                            o << "\t\t\tif (top == " << hx(*tops.begin()) << " && done < m_budget) { M2SHARC_GOTO(" << k << ", top); goto L_"
                              << std::hex << *tops.begin() << std::dec << "; }\n"
                              << "\t\t\t" << pipe << "\n"
                                 "\t\t\tif (done >= m_budget) return 2;\n"
                                 "\t\t\tcontinue;\n"
                                 "\t\t}\n"
                                 "\t\t}\n";
                        } else {
                            o << "\t\t\t" << pipe << "\n"
                                 "\t\t\tdone += plain; plain = 0;\n";
                            if (waits.count(a)) // a one-nop wait loop looping back: the SHARC polls its flags
                                o << "\t\t\tif (top == " << hx(a) << ") return 1;\n";
                            o << "\t\t\tif (done >= m_budget) return 2;\n";
                            if (goto_top)
                                o << "\t\t\tif (m_core->daddr == " << hx(*tops.begin()) << ") { M2SHARC_GOTO(" << k << ", m_core->daddr); goto L_"
                                  << std::hex << *tops.begin() << std::dec << "; }\n";
                            o << "\t\t\tcontinue;\n"
                                 "\t\t}\n"
                                 "\t\t}\n";
                        }
                    }
                    // Where a pinned branch is taken: the count, budget and step_pre as at a
                    // run's end (CHANGE_PC wrote the pipeline), then the target.
                    const auto taken = [&](uint32_t target, const char *event) {
                        const bool here = in_chunk(target, c);
                        const std::string go = here ? "\t\t\tif (m_core->daddr == " + hx(target) + ") { M2SHARC_GOTO(" + std::to_string(k) +
                                                              ", m_core->daddr); goto L_" + [&] {
                            char b[16];
                            std::snprintf(b, sizeof b, "%x", target);
                            return std::string(b);
                        }() + "; }\n" : std::string();
                        o << "\t\t\tM2SHARC_EV(" << event << ");\n"
                             "\t\t\tdone += plain; plain = 0;\n";
                        if (fast && here && target > a) // forward: no budget test (backward edges keep it)
                            o << go << "\t\t\tif (done >= m_budget) return 2;\n";
                        else
                            o << "\t\t\tif (done >= m_budget) return 2;\n" << go;
                        o << "\t\t\tcontinue;\n"
                             "\t\t}\n";
                    };
                    if (pinned[k] && has_j(h, prog[k]))
                        o << "\t\tif (m_core->nfaddr != " << hx(a + 3) << ") pending = m_core->nfaddr;\n";
                    else if (pinned[k] && h.rfind("sharcop_do_until", 0) != 0) {
                        o << "\t\tif (m_core->daddr != " << hx(a + 1) << ") {\n";
                        taken(target_of(h, a, prog[k]), "taken");
                    }
                    if (k >= 2 && pinned[k - 2] && has_j(handler(ops, prog[k - 2]), prog[k - 2])) {
                        // The second delay slot: the pipeline as the fetch of the target finds it.
                        o << "\t\tif (pending) {\n"
                             "\t\t\tconst uint32_t t = pending;\n"
                             "\t\t\tpending = 0;\n"
                             "\t\t\tm_core->pc = " << hx(a) << "; m_core->daddr = t; m_core->faddr = t + 1; m_core->nfaddr = t + 2;\n";
                        taken(target_of(handler(ops, prog[k - 2]), a - 2, prog[k - 2]), "slot_taken");
                    }
                    if (text_next(k) && !full[k + 1]) continue; // the run goes on
                    // The run ends: the pipeline as this instruction leaves it, its count.
                    o << "\t\tM2SHARC_EV(run_ends);\n"
                      << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << "; m_core->faddr = " << hx(a + 2)
                      << "; m_core->nfaddr = " << hx(a + 3) << ";\n"
                      << "\t\tdone += plain; plain = 0;\n"
                      << "\t\tif (done >= m_budget) return 2;\n";
                } else {
                    o << "\t\tM2SHARC_EV(full_steps);\n"
                      << "\t\tstep_fetch_gen(" << hx(prog[k]) << "ull);\n";
                    if (loop_ends.count(a)) o << "\t\tstep_loop_end();\n";
                    const std::string hf = handler(ops, prog[k]);
                    const bool callret = hf.find("_call") != std::string::npos || hf == "sharcop_rts";
                    if (callret) o << "\t\tM2SHARC_STACK_PRE();\n";
                    o << "\t\t" << call << "\n";
                    if (callret)
                        o << "\t\tM2SHARC_STACK_POST(" << (has_j(hf, prog[k]) ? "m_core->nfaddr" : "m_core->daddr") << ", m_prof_base + done + 1);\n";
                    o << "\t\tstep_after();\n";
                    if (waits.count(a)) o << "\t\tif (m_core->daddr == " << hx(a) << ") { ++done; return 1; } // polling its flags\n";
                    o << "\t\t++done;\n\t\tif (done >= m_budget) return 2;\n\t\tif (!step_pre()) return 1;\n";
                }
                if (k + 1 < n && chunk_of[k + 1] == c) {
                    // From a full step into a plain instruction: only in a state it can run in.
                    const bool guard = full[k] && !full[k + 1];
                    o << "\t\tif (m_core->daddr == " << hx(a + 1) << (guard ? " && plain_ready()" : "") << ") goto L_" << std::hex << a + 1
                      << std::dec << ";\n";
                }
                o << "\t\tcontinue;\n";
            }
            o << "\t}\n}\n\n} // namespace rt::sharc\n";
            char name[40];
            std::snprintf(name, sizeof name, "/sharc_chunk_%03u.cpp", c);
            write_file(dir + name, o.str());
        }
        std::ostringstream o;
        o << "// Generated by m2sharcrecomp from the SHARC program the i960 uploads (Model 2B):\n"
             "// the core and the dispatch over the chunks (sharc_chunk_*).\n"
             "// Derived from the game: never commit.\n"
             "#include \"runtime/sharc/sharc.cpp\"\n\n"
             "namespace rt::sharc {\n\n"
             "const uint32_t sharc_program_crc32 = "
          << hx(crc32(raw.data(), raw.size())) << "u;\nconst uint32_t sharc_program_instructions = " << n
          << ";\n\nnamespace {\nconstexpr uint64_t kProgram[" << n << "] = {\n";
        for (uint32_t k = 0; k < n; ++k) o << "    " << hx(prog[k]) << "ull,\n";
        o << "};\n} // namespace\n\n";
        if (by_layout) { // each instruction's chunk (--layout): run_generated's switch and the profile's dispatch probe
            o << "extern const uint16_t sharc_chunk_of[];\nconst uint16_t sharc_chunk_of[" << n << "] = {";
            for (uint32_t k = 0; k < n; ++k) o << (k % 32 ? "" : "\n    ") << chunk_of[k] << ",";
            o << "\n};\n\n";
        }
        // The profile's view of the program (sharc_profile.h): each instruction's handler and kind.
        o << "#ifdef M2_SHARC_PROFILE\nconst char *const sharc_gen_mode = \"" << (full_steps ? "full steps" : fast ? (hle.empty() ? "--fast" : "--fast + HLE") : "exact")
          << "\";\nconst uint32_t sharc_gen_count = " << n << ";\nconst char *const sharc_gen_names[" << n << "] = {\n";
        for (uint32_t k = 0; k < n; ++k) o << "    \"" << handler(ops, prog[k]).substr(8) << "\",\n"; // without "sharcop_"
        o << "};\nconst uint8_t sharc_gen_kinds[" << n << "] = {";
        for (uint32_t k = 0; k < n; ++k) o << (k % 32 ? "" : "\n    ") << (full[k] ? "SharcFull" : pinned[k] ? "SharcPinned" : "SharcPlain") << ",";
        o << "\n};\n"
             "// A run_generated exit: its size, why (waiting or the budget).\n"
             "#define M2SHARC_EXIT(w) do { m_instructions = start + done; M2SHARC_SIZE(done); ++((w) ? sharc_profile.exit_wait : sharc_profile.exit_budget); return (w); } while (0)\n"
             "#else\n#define M2SHARC_EXIT(w) do { m_instructions = start + done; return (w); } while (0)\n#endif\n\n";
        for (uint32_t c = 0; c < chunks; ++c)
            o << "template <> int adsp21062_device::run_chunk<" << c << ">(uint64_t &done);\n";
        o << "\nbool adsp21062_device::run_generated(uint64_t budget)\n{\n"
             "\tif (m_halted) return true;\n"
             "\tif (!m_gen_checked) {\n"
             "\t\tfor (uint32_t i = 0; i < " << n << "; ++i)\n"
             "\t\t\tif (m_program.read_qword(" << hx(base) << " + i) != kProgram[i])\n"
             "\t\t\t\tthrow SharcFatal(emu_fatalerror::fmt(\"SHARC program memory at %05x is not the recompiled program\", " << hx(base) << " + i));\n"
             "\t\tm_gen_checked = true;\n"
             "\t\tm_gen_end = " << hx(base + n) << ";\n"
             "\t}\n"
             "\t// done: every instruction this call runs; m_instructions is start + done on exit\n"
             "\t// (plain runs count only in done; full steps' step_after also counts, overwritten).\n"
             "\tconst uint64_t start = m_instructions;\n"
             "\tuint64_t done = 0;\n"
             "\tm_budget = budget; // end_run() lowers it\n"
             "\tM2SHARC_EV(gen_calls);\n"
             "\tM2SHARC_ENTRY(start);\n"
             "\t// Plain runs skip step_pre and the DMA cycle: not while a DMA or a delayed IOP write is pending.\n"
             "\tif ((m_core->dma_status & 0xc0) || m_iop_delay > 0)\n"
             "\t\tthrow SharcFatal(\"SHARC: a DMA or delayed IOP write while the recompiled program runs\");\n"
             "\tif (!step_pre()) M2SHARC_EXIT(true);\n"
             "\tfor (;;) {\n"
             "\t\tint r;\n"
             "\t\tM2SHARC_EV(chunk_calls);\n"
             << (by_layout ? "\t\tif (m_core->daddr - " + hx(base) + "u >= " + std::to_string(n) + "u)\n"
                             "\t\t\tthrow SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
                             "\t\tswitch (sharc_chunk_of[m_core->daddr - " + hx(base) + "u]) {\n"
                           : "\t\tswitch ((m_core->daddr - " + hx(base) + ") / " + std::to_string(chunk) + "u) {\n");
        for (uint32_t c = 0; c < chunks; ++c) o << "\t\tcase " << c << ": r = run_chunk<" << c << ">(done); break;\n";
        o << "\t\tdefault: throw SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
             "\t\t}\n"
             "\t\tif (r == 1) M2SHARC_EXIT(true);\n"
             "\t\tif (r == 2) M2SHARC_EXIT(false);\n"
             "\t\tif (r == 3) { // a delay slot entered from elsewhere, a MODE1 write's latency: the interpreter's steps\n"
             "\t\t\tM2SHARC_EV(fallback_entries);\n"
             "\t\t\tdo {\n"
             "\t\t\t\tM2SHARC_EV(fallback_steps);\n"
             "\t\t\t\tstep_body();\n"
             "\t\t\t\tif (++done >= m_budget) M2SHARC_EXIT(false);\n"
             "\t\t\t\tif (!step_pre()) M2SHARC_EXIT(true);\n"
             "\t\t\t} while (!plain_ready());\n"
             "\t\t}\n"
             "\t\tif (m_core->daddr - " << hx(base) << " >= " << n << "u)\n"
             "\t\t\tthrow SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
             "\t}\n}\n\n} // namespace rt::sharc\n";
        write_file(out_path, o.str());
        std::printf("m2sharcrecomp: %u instructions (%u plain), %zu loop ends, %zu wait loops, %zu handlers templated -> %s and %u chunks%s%s%s\n", n,
                    plain_count, loop_ends.size(), waits.size(), handlers, argv[2], chunks, by_layout ? " by affinity (--layout)" : "",
                    fast ? " (--fast)" : "", hle.empty() ? "" : (", " + std::to_string(hle.size()) + " HLE routine(s)").c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2sharcrecomp: %s\n", e.what());
        return 2;
    }
}
