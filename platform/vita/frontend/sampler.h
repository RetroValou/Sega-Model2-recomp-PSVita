// Statistical time profile of the main core (diagnostics builds, M2_SAMPLE_PROBE;
// src/runtime/board/sample_probe.h). A thread on the geometry core wakes every
// ~100 us and reads what the main core marks: outside gen::run, the i960's code,
// a bus call (and its address), the SHARC; with the i960's IP (its current
// block). perf.log then gets, per window, the share of each and the i960 blocks
// that take the time (not the instructions: a block that misses the caches
// stands out here and not in the block counts). The main core's cost: a store
// at each change of tag, none per instruction; the sampler's: a few us of the
// geometry core per wake.
#pragma once

#include "runtime/board/sample_probe.h"

#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace vita {

class Sampler {
public:
    static constexpr unsigned kPeriodUs = 100;
    static constexpr uint32_t kIpSlots = 0x200000 / 4; // the program ROM (2 MB), a slot per instruction
    // Where a bus call goes (the M2Board map).
    enum Region { RegWorkRam, RegMainRam, RegProgramRom, RegDataRom, RegCopro, RegTexture, RegOther, kRegions };

    // Starts the thread on `core` (0-2; otherwise any). False if it cannot.
    bool start(int core) {
        if (thread_ >= 0) return true;
        ip_.assign(kIpSlots, 0);
        ip_prev_.assign(kIpSlots, 0);
        const int mask = core >= 0 && core <= 2 ? SCE_KERNEL_CPU_MASK_USER_0 << core : SCE_KERNEL_CPU_MASK_USER_ALL;
        thread_ = sceKernelCreateThread("daytona_sampler", reinterpret_cast<SceKernelThreadEntry>(&entry), 64,
                                        16 * 1024, 0, mask, nullptr);
        if (thread_ < 0) { thread_ = -1; return false; }
        Sampler *self = this;
        if (sceKernelStartThread(thread_, sizeof(self), &self) < 0) {
            sceKernelDeleteThread(thread_);
            thread_ = -1;
            return false;
        }
        return true;
    }

    // The window since the previous call: frames emulated, its length in ms.
    size_t format(char *out, size_t size, double frames, double window_ms) {
        Writer w{out, size};
        if (thread_ < 0) return 0;
        const uint64_t samples = samples_.load(std::memory_order_relaxed);
        const uint64_t n = samples - prev_samples_;
        prev_samples_ = samples;
        uint64_t tag[rt::kProbeTags], reg[kRegions];
        for (int i = 0; i < rt::kProbeTags; ++i) {
            const uint64_t t = tags_[i].load(std::memory_order_relaxed);
            tag[i] = t - prev_tags_[i];
            prev_tags_[i] = t;
        }
        for (int i = 0; i < kRegions; ++i) {
            const uint64_t t = regions_[i].load(std::memory_order_relaxed);
            reg[i] = t - prev_regions_[i];
            prev_regions_[i] = t;
        }
        if (!n || frames <= 0) return w.size;
        const double ms_per_sample = window_ms / double(n) / frames; // ms a frame one sample stands for
        w.add("---- main core by sampling (every ~%u us from the geometry core: %.0f samples, %.1f a ms) ----\n",
              kPeriodUs, double(n), double(n) / window_ms);
        static const char *const kTag[] = {"outside gen::run (render, sync, frontend)", "i960 code (gen::run)",
                                           "SHARC runs (need_output, slices)", "i960 bus calls (M2Board)"};
        for (int i = 0; i < rt::kProbeTags; ++i)
            w.add("  %-44s %5.1f%%  %6.2f ms/frame\n", kTag[i], 100.0 * double(tag[i]) / double(n),
                  double(tag[i]) * ms_per_sample);
        static const char *const kReg[] = {"work RAM", "main RAM", "program ROM", "data ROM", "copro ports/buffer RAM",
                                           "texture/luma RAM", "other devices"};
        w.add("  bus calls by region (ms/frame):");
        for (int i = 0; i < kRegions; ++i) w.add(" %s %.2f", kReg[i], double(reg[i]) * ms_per_sample);
        w.add("\n");

        // The i960 blocks with the most samples (code + their bus calls).
        std::vector<std::pair<uint32_t, uint32_t>> hot; // samples, slot
        for (uint32_t s = 0; s < kIpSlots; ++s) {
            const uint32_t c = std::atomic_ref<uint32_t>(ip_[s]).load(std::memory_order_relaxed);
            if (c != ip_prev_[s]) {
                hot.emplace_back(c - ip_prev_[s], s);
                ip_prev_[s] = c;
            }
        }
        const size_t top = std::min<size_t>(20, hot.size());
        std::partial_sort(hot.begin(), hot.begin() + long(top), hot.end(), std::greater<>());
        w.add("  i960 blocks by time (code and bus calls; block start, ms/frame, share of i960 code+bus):\n");
        const double i960_samples = double(tag[rt::ProbeI960] + tag[rt::ProbeBus]);
        for (size_t i = 0; i < top; ++i)
            w.add("    %08x %6.3f ms %5.1f%%\n", hot[i].second * 4u, double(hot[i].first) * ms_per_sample,
                  i960_samples > 0 ? 100.0 * double(hot[i].first) / i960_samples : 0.0);
        return w.size;
    }

private:
    struct Writer {
        char *out;
        size_t cap, size = 0;
        void add(const char *fmt, ...) {
            if (size >= cap) return;
            va_list ap;
            va_start(ap, fmt);
            const int n = std::vsnprintf(out + size, cap - size, fmt, ap);
            va_end(ap);
            if (n > 0) size = std::min(cap, size + size_t(n));
        }
    };

    static Region region(uint32_t a) {
        if (a - 0x00500000u < 0x100000u) return RegWorkRam;
        if (a - 0x00200000u < 0x40000u) return RegMainRam;
        if (a < 0x00200000u) return RegProgramRom;
        if (a - 0x02000000u < 0x2000000u || a - 0x06000000u < 0x1000000u) return RegDataRom;
        if (a - 0x00800000u < 0x200000u) return RegCopro; // geometrizer/copro ports, buffer RAM, 0x0098xxxx
        if (a - 0x11000000u < 0x800000u) return RegTexture;
        return RegOther;
    }

    static int entry(SceSize, void *argp) {
        Sampler *self = *static_cast<Sampler **>(argp);
        self->loop();
        return sceKernelExitDeleteThread(0);
    }
    void loop() {
        for (;;) {
            sceKernelDelayThread(kPeriodUs);
            const uint32_t tag = rt::sample_probe.tag;
            samples_.fetch_add(1, std::memory_order_relaxed);
            if (tag >= rt::kProbeTags) continue;
            tags_[tag].fetch_add(1, std::memory_order_relaxed);
            if (tag == rt::ProbeI960 || tag == rt::ProbeBus) {
                // Read only while the main core is in gen::run: the game (and its Cpu) is alive.
                if (const volatile uint32_t *ip = rt::sample_probe.ip) {
                    const uint32_t a = *ip;
                    if (a < 0x200000u) std::atomic_ref<uint32_t>(ip_[a >> 2]).fetch_add(1, std::memory_order_relaxed);
                }
                if (tag == rt::ProbeBus) regions_[region(rt::sample_probe.bus_addr)].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    SceUID thread_ = -1;
    std::atomic<uint64_t> samples_{0};
    std::atomic<uint64_t> tags_[rt::kProbeTags]{};
    std::atomic<uint64_t> regions_[kRegions]{};
    std::vector<uint32_t> ip_; // written by the sampler only (atomic_ref), read by format()
    uint64_t prev_samples_ = 0, prev_tags_[rt::kProbeTags]{}, prev_regions_[kRegions]{};
    std::vector<uint32_t> ip_prev_;
};

} // namespace vita
