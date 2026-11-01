// Sampling probe (the Vita's diagnostics builds: M2_SAMPLE_PROBE). The main
// core marks what it is doing in one word; a sampler thread on another core
// reads it, with the i960's IP and the bus address in progress, every ~100 us
// (platform/vita/frontend/sampler.h). A time profile by statistics: no clock
// read on the main core (one costs ~0.7 us on the Vita), only a store at each
// change. Without M2_SAMPLE_PROBE the macros are nothing.
#pragma once

#include <cstdint>

namespace rt {

enum ProbeTag : uint32_t {
    ProbeOther = 0, // outside gen::run (rendering, sound sync, the frontend)
    ProbeI960 = 1,  // gen::run: the recompiled i960 code (and what no other tag covers)
    ProbeSharc = 2, // TgpBoard runs the Model 2B SHARC (need_output, a slice, run to the wait)
    ProbeBus = 3,   // an i960 bus call in M2Board (read_* / write_*), not counting a SHARC run inside
    kProbeTags
};

struct SampleProbe {
    volatile uint32_t tag = ProbeOther;
    volatile uint32_t bus_addr = 0;       // the bus call in progress (ProbeBus)
    const volatile uint32_t *ip = nullptr; // the i960's IP (Cpu::m_IP: the current block's start)
};
inline SampleProbe sample_probe;

#ifdef M2_SAMPLE_PROBE
// Sets the tag for a scope and puts the previous one back at its end.
struct ProbeScope {
    uint32_t prev;
    explicit ProbeScope(uint32_t t) : prev(sample_probe.tag) { sample_probe.tag = t; }
    ~ProbeScope() { sample_probe.tag = prev; }
    ProbeScope(const ProbeScope &) = delete;
    ProbeScope &operator=(const ProbeScope &) = delete;
};
#define M2_PROBE_SCOPE(t) ::rt::ProbeScope m2_probe_scope_(t)
#define M2_PROBE_BUS(a)                                                                                                \
    ::rt::sample_probe.bus_addr = (a);                                                                                 \
    ::rt::ProbeScope m2_probe_scope_(::rt::ProbeBus)
#else
#define M2_PROBE_SCOPE(t) ((void)0)
#define M2_PROBE_BUS(a) ((void)0)
#endif

} // namespace rt
