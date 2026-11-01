// Fighting Vipers (fvipers): HLE routines of its SHARC program (rules.md
// "Coprocessor HLE"; seeds/fvipers_sharc_hle.txt lists their entries). Only
// in fast_inaccuracy builds (m2sharcrecomp --fast --hle); exact builds run
// the recompiled program. Written from what each routine computes, observed
// in our analysis; no instruction of the program is copied here.
//
// Included by the generated chunks inside namespace rt::sharc, after
// sharc_handlers.h: the device's members, sharcops.hxx's macros and the
// inline compute helpers (whose flags are MAME's) are in scope.
#pragma once

// The command loop (entry 0x20130; 25 % of the SHARC's instructions in the
// fight, ~1,050 commands a frame). Each pass: wait for the input FIFO
// (FLAG0 clear), raising FLG3 while waiting; read the command word through
// I0/M0; its command number is in three bytes (bits 23-30, 0-7, 8-15) that
// must agree, else the error handler at 0x20148; keep the word at DM 8,
// look the handler up in the table at I2 (M7 = the number, I15 = the
// entry), store R15 at PM 0x21f38 and twice at DM I3 + 0x20, and call the
// handler (I15 + M8), which returns to 0x20147, the end of the endless loop
// around this one. Registers, flags, memory and stacks as the recompiled
// code leaves them; the instruction counts as it runs them.
struct sharc_hle_command_loop;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_command_loop>(uint64_t &done)
{
	if (m_core->flag[0] != 0) return 0; // the input FIFO is empty: the recompiled wait loop polls it

	REG(1) = 0xff;
	m_core->astat &= ~0x00400000u; // FLG3 raised while waiting and lowered (the flag outputs do nothing here)

	const uint32_t word = dm_read32_i(DM_REG_I(0)); // R0 = DM(I0), I0 += M0
	DM_REG_I(0) += DM_REG_M(0);
	UPDATE_CIRCULAR_BUFFER_DM(0);
	REG(0) = word;

	constexpr int kLshift = 0x00;               // SHIFT_OPERATION_IMM's logical shift
	SHIFT_OPERATION_IMM_i(kLshift, -23 & 0xfff, 2, 0); // R2 = number from bits 23-30
	compute_and(2, 2, 1);
	compute_and(3, 0, 1);                          // R3 = number from bits 0-7
	compute_comp(2, 3);
	const bool first_differs = !(m_core->astat & AZ);
	SHIFT_OPERATION_IMM_i(kLshift, -8 & 0xfff, 4, 0); // R4 = number from bits 8-15 (also in the
	compute_and(4, 4, 1);                             // delay slots of the first test's jump)
	if (first_differs) {
		done += 13;
		return 0x20148;
	}
	compute_comp(2, 4);
	if (!(m_core->astat & AZ)) {
		done += 15;
		return 0x20148;
	}

	dm_write32_i(0x8, word);
	DM_REG_M(7) = REG(2);
	PM_REG_I(7) = dm_read32_i(DM_REG_I(2) + DM_REG_M(7)); // I15 = the handler
	pm_write32_i(0x21f38, REG(15));
	pm_write32_i(0x21f38, REG(15));
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));

	PUSH_PC(); // CALL (I15 + M8), not delayed: returns to 0x20147
	m_core->pcstk = 0x20147;
	done += 23;
	return PM_REG_I(7) + PM_REG_M(0);
}

// Shared by the routines below: the input FIFO read the program makes
// through I0/M0 (R = DM(I0), I0 += M0, circular), the output FIFO write
// through I1/M0, and a DAG1 post-modify write DM(I, M) = value.
#define FVIPERS_HLE_IN(reg)                                \
	do {                                                   \
		REG(reg) = dm_read32_i(DM_REG_I(0));               \
		DM_REG_I(0) += DM_REG_M(0);                        \
		UPDATE_CIRCULAR_BUFFER_DM(0);                      \
	} while (0)
#define FVIPERS_HLE_OUT(value)                             \
	do {                                                   \
		dm_write32_i(DM_REG_I(1), (value));                \
		DM_REG_I(1) += DM_REG_M(0);                        \
		UPDATE_CIRCULAR_BUFFER_DM(1);                      \
	} while (0)
#define FVIPERS_HLE_PUT(i, m, value)                       \
	do {                                                   \
		dm_write32_i(DM_REG_I(i), (value));                \
		DM_REG_I(i) += DM_REG_M(m);                        \
		UPDATE_CIRCULAR_BUFFER_DM(i);                      \
	} while (0)

// Command 0x20908 (77 a frame in the fight, 4.9 % of the instructions): a
// packet into buffer RAM. Eight input words (R10, R11, R12, R13, R14, R15,
// then R10, R9 again, each after waiting for the FIFO); I6 = 0x1400000 +
// first word / 4; there a header word 0x05800b0b, then the 12 words at the
// address held in DM 0x3033f (a counter loop, its stacks as the program
// leaves them), a second header 0x00800101, R12, R13 (+ the 15th-30th bits of
// R15 when R11 is not zero, R10 then grows by R15's low half), R14, R15's
// low half; R10 and R9 + R15's low half go to the output FIFO (each after
// waiting for room), R8 = -1, and it returns. Where the program waits (input
// FIFO empty, FLAG0; output, FLAG1) and the wait is real, the routine stops
// there, its state the program's, and the recompiled code polls on.
struct sharc_hle_packet_20908;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_packet_20908>(uint64_t &done)
{
	constexpr int kLshift = 0x00;
	uint32_t ran = 0; // instructions run, the waits included
	// Each input word: its wait, then the read (two instructions).
	const auto in = [&](int reg, uint32_t wait_at) {
		if (m_core->flag[0] != 0) return wait_at;
		FVIPERS_HLE_IN(reg);
		ran += 2;
		return 0u;
	};
	const auto stop = [&](uint32_t at) { done += ran; return at; };
	if (m_core->flag[0] != 0) return 0; // nothing done yet: the recompiled wait

	uint32_t w;
	if ((w = in(10, 0x20908))) return stop(w);
	SHIFT_OPERATION_IMM_i(kLshift, -2 & 0xfff, 0, 10); ++ran; // R0 = R10 >> 2
	if ((w = in(11, 0x2090b))) return stop(w);
	REG(1) = 0x01400000; ++ran;
	if ((w = in(12, 0x2090e))) return stop(w);
	REG(2) = 0x05800b0b; ++ran;
	if ((w = in(13, 0x20911))) return stop(w);
	compute_add(0, 0, 1); ++ran;
	if ((w = in(14, 0x20914))) return stop(w);
	DM_REG_I(6) = REG(0); ++ran;
	if ((w = in(15, 0x20917))) return stop(w);
	FVIPERS_HLE_PUT(6, 1, REG(2));
	DM_REG_I(7) = dm_read32_i(0x3033f);
	ran += 2;

	// LCNTR = 12, DO 0x2091d UNTIL LCE: copy 12 words from I7 to I6.
	m_core->lcntr = 12;
	PUSH_PC();
	PUSH_LOOP();
	m_core->pcstk = 0x2091c;
	m_core->laddr.addr = 0x2091d;
	m_core->laddr.code = 0xf;
	m_core->laddr.loop_type = 2;
	++ran;
	for (int i = 0; i < 12; ++i) {
		REG(0) = dm_read32_i(DM_REG_I(7));
		DM_REG_I(7) += DM_REG_M(1);
		UPDATE_CIRCULAR_BUFFER_DM(7);
		FVIPERS_HLE_PUT(6, 1, REG(0));
	}
	m_core->curlcntr = 0;
	POP_LOOP();
	POP_PC();
	ran += 24;

	if ((w = in(10, 0x2091e))) return stop(w);
	if ((w = in(9, 0x20920))) return stop(w);
	REG(2) = 0x00800101;
	FVIPERS_HLE_PUT(6, 1, REG(2));
	SHIFT_OPERATION_IMM_i(kLshift, -14 & 0xfff, 7, 15); // R7 = R15 >> 14
	FVIPERS_HLE_PUT(6, 1, REG(12));
	REG(6) = 0xffff;
	compute_and(15, 15, 6);
	compute_and(11, 11, 11);
	ran += 8; // 0x20922-0x20929
	if (!(m_core->astat & AZ)) {
		compute_add(13, 13, 7);
		compute_add(10, 10, 15);
		ran += 2;
	}
	FVIPERS_HLE_PUT(6, 1, REG(13)); ++ran;
	if (m_core->flag[1] != 0) return stop(0x2092d);
	FVIPERS_HLE_OUT(REG(10));
	compute_add(9, 9, 15);
	ran += 3;
	if (m_core->flag[1] != 0) return stop(0x20930);
	FVIPERS_HLE_OUT(REG(9));
	FVIPERS_HLE_PUT(6, 1, REG(14));
	// RTS (DB), its two delay slots: R8 = -1, DM(I6, M1) = R15.
	REG(8) = 0xffffffff;
	FVIPERS_HLE_PUT(6, 1, REG(15));
	ran += 6;
	done += ran;
	return POP_PC();
}

// A counter loop "LCNTR = n, DO end UNTIL LCE" run natively: its stack
// effects as the program's (the do-until's pushes, the expiry's pops).
#define FVIPERS_HLE_LOOP_BEGIN(n, top, end, type)          \
	do {                                                   \
		m_core->lcntr = (n);                               \
		PUSH_PC();                                         \
		PUSH_LOOP();                                       \
		m_core->pcstk = (top);                             \
		m_core->laddr.addr = (end);                        \
		m_core->laddr.code = 0xf;                          \
		m_core->laddr.loop_type = (type);                  \
	} while (0)
#define FVIPERS_HLE_LOOP_END()                             \
	do {                                                   \
		m_core->curlcntr = 0;                              \
		POP_LOOP();                                        \
		POP_PC();                                          \
	} while (0)

// The table offset two commands below start with: the first input word's
// low byte picks the base (1: 0x304e0, else 0x30420), the second word is
// added to it. R0, R1 and the flags as the program leaves them. Returns the
// instructions run, or 0 with nothing done when the second word is not
// there yet (only the first word read: *stop_at gets the wait's address).
#define FVIPERS_HLE_TABLE_SLOT(entry, slot, ran)                                   \
	do {                                                                           \
		FVIPERS_HLE_IN(0);                                                         \
		REG(1) = 0xff;                                                             \
		compute_and(0, 0, 1);                                                      \
		REG(1) = 1;                                                                \
		compute_comp(0, 1);                                                        \
		const bool equal_ = (m_core->astat & AZ) != 0;                             \
		REG(0) = equal_ ? 0x304e0 : 0x30420;                                       \
		(ran) = equal_ ? 9 : 8; /* entry .. the second wait */                     \
		if (m_core->flag[0] != 0) { done += (ran); return (entry) + 9; }           \
		FVIPERS_HLE_IN(1);                                                         \
		compute_add(1, 0, 1);                                                      \
		(slot) = REG(1);                                                           \
		(ran) += 3;                                                                \
	} while (0)

// Command 0x20473 (26 a frame): 12 words from the address in DM 0x3033f to
// the table slot (FVIPERS_HLE_TABLE_SLOT), then return.
struct sharc_hle_store_20473;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_store_20473>(uint64_t &done)
{
	if (m_core->flag[0] != 0) return 0;
	uint32_t ran, slot;
	FVIPERS_HLE_TABLE_SLOT(0x20473u, slot, ran);
	DM_REG_I(6) = slot;
	DM_REG_I(7) = dm_read32_i(0x3033f);
	FVIPERS_HLE_LOOP_BEGIN(12, 0x20482, 0x20483, 2);
	for (int i = 0; i < 12; ++i) {
		REG(0) = dm_read32_i(DM_REG_I(7));
		DM_REG_I(7) += DM_REG_M(1);
		UPDATE_CIRCULAR_BUFFER_DM(7);
		FVIPERS_HLE_PUT(6, 1, REG(0));
	}
	FVIPERS_HLE_LOOP_END();
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));
	done += ran + 3 + 24 + 3; // I6 =, I7 =, the do; the loop; two stores and the RTS
	return POP_PC();
}

// Command 0x2049b (48 a frame, 27 of them from the command loop, 21 from
// 0x20acd): 12 words of the table slot (FVIPERS_HLE_TABLE_SLOT) to PM
// 0x21f20 (I8, M9), R15 to PM 0x21f38, I7 = DM 0x3033f, I9 = 0x21f2c, then
// it calls 0x201bc (left to the recompiled code); the second entry, 0x204b1
// where that call returns, copies 12 words from PM I9 to DM I7 and returns.
struct sharc_hle_load_2049b;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_load_2049b>(uint64_t &done)
{
	if (m_core->flag[0] != 0) return 0;
	uint32_t ran, slot;
	FVIPERS_HLE_TABLE_SLOT(0x2049bu, slot, ran);
	DM_REG_I(6) = slot;
	PM_REG_I(0) = 0x21f20;
	FVIPERS_HLE_LOOP_BEGIN(12, 0x204aa, 0x204ab, 2);
	for (int i = 0; i < 12; ++i) {
		REG(0) = dm_read32_i(DM_REG_I(6));
		DM_REG_I(6) += DM_REG_M(1);
		UPDATE_CIRCULAR_BUFFER_DM(6);
		pm_write32_i(PM_REG_I(0), REG(0));
		PM_REG_I(0) += PM_REG_M(1);
		UPDATE_CIRCULAR_BUFFER_PM(0);
	}
	FVIPERS_HLE_LOOP_END();
	pm_write32_i(0x21f38, REG(15));
	pm_write32_i(0x21f38, REG(15));
	DM_REG_I(7) = dm_read32_i(0x3033f);
	PM_REG_I(1) = 0x21f2c;
	PUSH_PC(); // CALL 0x201bc, not delayed: returns to 0x204b1
	m_core->pcstk = 0x204b1;
	done += ran + 3 + 24 + 5; // I6 =, I8 =, the do; the loop; two stores, I7 =, I9 =, the CALL
	return 0x201bc;
}

struct sharc_hle_load_2049b_back;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_load_2049b_back>(uint64_t &done)
{
	FVIPERS_HLE_LOOP_BEGIN(12, 0x204b2, 0x204b3, 2);
	for (int i = 0; i < 12; ++i) {
		REG(0) = pm_read32_i(PM_REG_I(1));
		PM_REG_I(1) += PM_REG_M(1);
		UPDATE_CIRCULAR_BUFFER_PM(1);
		FVIPERS_HLE_PUT(7, 1, REG(0));
	}
	FVIPERS_HLE_LOOP_END();
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));
	dm_write32_i(DM_REG_I(3) + 0x20, REG(15));
	done += 1 + 24 + 3; // the do; the loop; two stores and the RTS
	return POP_PC();
}


// ---- Native arithmetic for the long routines below -------------------------
// Each lambda gives the value MAME's compute helper (compute.hxx) writes, the
// sticky bits it sets (into `stky`, OR'ed into STKY at the end) and, where a
// condition or the final ASTAT needs them, its flags; nothing else of ASTAT
// is touched until the routine writes its final ASTAT. The arithmetic is the
// host's single precision as in those helpers (-ffp-contract=off: no fused
// multiply-add), so each value is the helper's on every host.
#define FVIPERS_HLE_ARITH                                                                       \
	const bool trunc_ = (m_core->mode1 & MODE1_TRUNCATE) != 0;                                  \
	uint32_t stky = 0;                                                                          \
	/* FADD / FSUB: ALU flags into af (CLEAR_ALU_FLAGS, then AZ AN AV AI, AF set) */            \
	[[maybe_unused]] auto fadd_sub = [&](uint32_t a, uint32_t b, bool sub, uint32_t &af) -> uint32_t {         \
		uint32_t r, f = AF;                                                                     \
		if (sub ? IS_FLOAT_NAN_SUB(a, b) : IS_FLOAT_NAN_ADD(a, b)) {                            \
			r = FLOAT_CANONICAL_NAN;                                                            \
			f |= AI;                                                                            \
			stky |= AIS;                                                                        \
		} else {                                                                                \
			const float x = std::bit_cast<float>(FLOAT_FLUSH_DENORMAL(a));                      \
			const float y = std::bit_cast<float>(FLOAT_FLUSH_DENORMAL(b));                      \
			r = std::bit_cast<uint32_t>(sub ? x - y : x + y);                                   \
			if (IS_FLOAT_INFINITY(r)) {                                                         \
				f |= AV;                                                                        \
				stky |= AVS;                                                                    \
				if (trunc_) r = (r & FLOAT_SIGN_MASK) | 0x7f7fffff;                             \
			} else if (IS_FLOAT_DENORMAL_OR_ZERO(r)) {                                          \
				f |= AZ;                                                                        \
				if (r & FLOAT_MANTISSA_MASK) stky |= AUS;                                       \
				r &= FLOAT_SIGN_MASK;                                                           \
			}                                                                                   \
			if (r & FLOAT_SIGN_MASK) f |= AN;                                                   \
		}                                                                                       \
		af = f;                                                                                 \
		return r;                                                                               \
	};                                                                                          \
	/* SHARC_COND_LT after a float ALU operation (AF set) */                                    \
	[[maybe_unused]] auto f_lt = [](uint32_t af) { return (af & AN) && !(af & AZ); };                            \
	/* FMUL: multiplier flags into mf (CLEAR_MULTIPLIER_FLAGS, then MN MV MU MI) */             \
	[[maybe_unused]] auto fmul = [&](uint32_t a, uint32_t b, uint32_t &mf) -> uint32_t {                         \
		uint32_t r, f = 0;                                                                      \
		if (IS_FLOAT_NAN_MUL(a, b)) {                                                           \
			r = FLOAT_CANONICAL_NAN;                                                            \
			f |= MI;                                                                            \
			stky |= MIS;                                                                        \
		} else {                                                                                \
			r = std::bit_cast<uint32_t>(std::bit_cast<float>(a) * std::bit_cast<float>(b));     \
			if (r & FLOAT_SIGN_MASK) f |= MN;                                                   \
			if (IS_FLOAT_INFINITY(r)) f |= MV, stky |= MVS;                                     \
			if (IS_FLOAT_DENORMAL(r)) f |= MU, stky |= MUS;                                     \
		}                                                                                       \
		mf = f;                                                                                 \
		return r;                                                                               \
	};                                                                                          \
	/* FIX (compute_fix); its ALU flags are not needed by the callers */                        \
	[[maybe_unused]] auto fix = [&](uint32_t a) -> uint32_t {                                                    \
		const float v = std::bit_cast<float>(a);                                                \
		const int32_t i = trunc_ ? int32_t(floorf(v)) : int32_t(nearbyintf(v));                 \
		if (IS_FLOAT_DENORMAL(a)) stky |= AUS;                                                  \
		return uint32_t(i);                                                                     \
	};                                                                                          \
	/* Integer add / subtract: only their AOS sticky bit (ALUSAT is off: checked at entry) */  \
	[[maybe_unused]] auto iadd = [&](uint32_t a, uint32_t b) -> uint32_t {                                       \
		const uint32_t r = a + b;                                                               \
		if (~(a ^ b) & (a ^ r) & 0x80000000u) stky |= AOS;                                      \
		return r;                                                                               \
	};                                                                                          \
	[[maybe_unused]] auto isub = [&](uint32_t a, uint32_t b) -> uint32_t {                                       \
		const uint32_t r = a - b;                                                               \
		if ((a ^ b) & (a ^ r) & 0x80000000u) stky |= AOS;                                       \
		return r;                                                                               \
	};                                                                                          \
	/* FABS (FABS): its flags are not needed by the callers */                                  \
	[[maybe_unused]] auto fabs_ = [&](uint32_t a) -> uint32_t {                                                  \
		if (IS_FLOAT_NAN(a)) { stky |= AIS; return FLOAT_CANONICAL_NAN; }                         \
		return IS_FLOAT_DENORMAL_OR_ZERO(a) ? 0u : a & ~FLOAT_SIGN_MASK;                         \
	};                                                                                          \
	/* COMP(Fx, Fy) (compute_fcomp): AZ / AN / AI, and the compare accumulation */              \
	[[maybe_unused]] auto fcomp = [&](uint32_t x, uint32_t y, uint32_t &cacc) -> uint32_t {                      \
		uint32_t f = 0;                                                                         \
		if (IS_FLOAT_NAN(x) || IS_FLOAT_NAN(y)) f = AI, stky |= AIS;                             \
		else if (std::bit_cast<float>(x) == std::bit_cast<float>(y)) f = AZ;                    \
		else if (std::bit_cast<float>(x) < std::bit_cast<float>(y)) f = AN;                     \
		cacc = ((cacc >> 1) & 0x7f000000u) | ((f & (AZ | AN)) ? 0u : 0x80000000u);              \
		return f;                                                                               \
	};                                                                                          \
	/* The compare accumulation (ASTAT bits 24-31) after an integer COMP(a, b) */               \
	[[maybe_unused]] auto cacc_comp = [](uint32_t cacc, uint32_t a, uint32_t b) -> uint32_t {                    \
		const bool zn = a == b || int32_t(a) < int32_t(b);                                     \
		return ((cacc >> 1) & 0x7f000000u) | (zn ? 0u : 0x80000000u);                           \
	}

// The ASTAT bits the routines below compute natively: the ALU's (with AF),
// the multiplier's, the shifter's and the compare accumulation.
#define FVIPERS_HLE_ASTAT_MASK (AZ | AN | AV | AC | AS | AI | AF | MN | MV | MU | MI | SV | SZ | SS | 0xff000000u)

// Command 0x20b8a (0.65 a frame in the fight, 9.9 % of the instructions,
// ~15,400 a call): four input words (two masks at DM I3+0x11 and I3+0x12,
// two values at DM 0x3041a and 0x3041b), 32 words of buffer RAM from
// 0x1403e20 set to -1, then three passes (DM I3+0x10 = 0, 1, 2). Each pass
// clears two 64-word occupancy masks (DM 0x30340 and 0x30380) and walks 32
// entries twice:
//  - first walk: two buffer RAM floats at 0x1403f40 + pass + 3k and 0x60
//    further, ordered (low, high); with the k-th word of the table at
//    0x30600 as a half-width (scaled by DM 0x306f0 where the first mask's
//    bit k is set and DM 0x3041a is not 4), each end maps to a cell
//    (16 x + 32 - 0.5, fixed, & 63) whose mask word gets the pass's bit
//    (1 << k); then each mask becomes the OR of itself and every cell before
//    (0x30340 from below, 0x30380 from above);
//  - second walk: the same from 0x1407f40 with the table at 0x30700, DM
//    0x307f0 and the second mask; an entry inside the grid (0 <= 16 x + 32 <
//    64, both ends) ANDs the two cells' masks into the buffer RAM word
//    0x1403e20 + k, others AND it with 0.
// Then 16 words of buffer RAM from 0x1403d80 are cleared and it returns.
// Registers, memory, buffer RAM accesses (in the program's order), stacks,
// STKY and ASTAT as the program leaves them; the loops' stack effects are
// those of the last one, which overwrites the earlier ones' slots; the
// instruction count as it runs.
struct sharc_hle_grid_20b8a;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_grid_20b8a>(uint64_t &done)
{
	if (m_core->flag[0] != 0) return 0; // the recompiled wait
	// Only where the whole routine fits the run's budget (at most ~17,700
	// instructions), as the recompiled code would run it: a short slice runs
	// it the program's way, so the SHARC is never further ahead of the i960.
	if (done >= m_budget || m_budget - done < 17800) return 0;
	const uint32_t i3 = DM_REG_I(3);
	if (i3 != 0x30300 || DM_REG_M(0) != 0 || DM_REG_M(1) != 1 || DM_REG_M(2) != 2 || DM_REG_L(6) != 0 ||
	    DM_REG_L(7) != 0 || (m_core->mode1 & MODE1_ALUSAT))
		return 0; // not the state this was written for: the recompiled code

	// The four input words, each after its wait; a wait that is real stops here.
	uint64_t n = 0;
	FVIPERS_HLE_IN(0);
	dm_write32_i(i3 + 0x11, REG(0));
	n += 3;
	if (m_core->flag[0] != 0) { done += n; return 0x20b8d; }
	FVIPERS_HLE_IN(1);
	dm_write32_i(i3 + 0x12, REG(1));
	n += 3;
	if (m_core->flag[0] != 0) { done += n; return 0x20b90; }
	FVIPERS_HLE_IN(0);
	dm_write32_i(0x3041a, REG(0));
	n += 3;
	if (m_core->flag[0] != 0) { done += n; return 0x20b93; }
	FVIPERS_HLE_IN(1);
	dm_write32_i(0x3041b, REG(1));
	n += 3;

	FVIPERS_HLE_ARITH;
	uint32_t r0, r1 = REG(1), r2 = REG(2), r3 = REG(3), r4 = REG(4), r5 = REG(5), r6 = REG(6), r7 = REG(7);
	uint32_t r8 = REG(8), r11 = REG(11), r12 = REG(12), r13 = REG(13), r14 = REG(14), r15 = REG(15);
	const uint32_t w41a = dm_read32_i(0x3041a), w41b = dm_read32_i(0x3041b);
	uint32_t v11 = dm_read32_i(i3 + 0x11), v12 = dm_read32_i(i3 + 0x12);
	uint32_t mf = m_core->astat & (MN | MV | MU | MI), sf = m_core->astat & (SV | SZ | SS);
	uint32_t cacc = m_core->astat & 0xff000000u, af;
	uint32_t i6, i7, m6, m7, v3 = 0, v5 = 0, v6 = 0, v8 = 0;

	r0 = 0xffffffff; // 0x20b96-0x20b98 (DM I3+0x10 = 0 below)
	i6 = 0x01403e20;
	for (int k = 0; k < 32; ++k) dm_write32_i(i6++, r0);
	n += 4 + 1 + 64 + 2; // 0x20b96-0x20b99, the do, the loop, two stores

	uint32_t pass = 0;
	for (;;) {
		v5 = 1; // 0x20b9f: the bit of entry k
		for (uint32_t a = 0x30340; a < 0x303c0; ++a) dm_write32_i(a, 0);
		i6 = 0x303c0;
		r1 = pass;
		r0 = iadd(0x01403f40, r1);
		v3 = r0;
		r0 = 0x30600;
		v6 = r0;
		m6 = 0x60;
		n += 4 + 1 + 128 + 7 + 1; // 0x20b9f-0x20ba2, the do, the loop, 0x20ba5-0x20bab, the do
		for (int k = 0; k < 32; ++k) { // 0x20bad-0x20bf7
			i7 = v3;
			r0 = dm_read32_i(i7);
			r4 = i7;
			r1 = dm_read32_i(i7 + m6);
			r3 = 3;
			r4 = iadd(r4, r3);
			v3 = r4;
			r4 = fadd_sub(r1, r0, true, af);
			n += 13;
			if (f_lt(af)) { r15 = r0; r0 = r1; r1 = r15; n += 3; }
			i7 = v6;
			r8 = dm_read32_i(i7);
			i7 += 1;
			r15 = i7;
			v6 = r15;
			n += 8;
			if (r8 != 0) {
				r14 = v11;
				r13 = r14 >> 1;
				v11 = r13;
				sf = (r14 & 1) ? 0 : SZ; // BTST R14 BY 0 (after the LSHIFT's flags)
				n += 7;
				if (r14 & 1) {
					r14 = w41a;
					r15 = isub(4, r14);
					n += 4;
					if (r15 != 0) {
						r14 = dm_read32_i(0x306f0);
						r8 = fmul(r8, r14, mf);
						n += 2;
					}
				}
				r0 = fadd_sub(r0, r8, true, af);
				r1 = fadd_sub(r1, r8, false, af);
				r15 = 0x41800000; // 16.0
				r14 = 0x42000000; // 32.0
				r13 = 0x3f000000; // 0.5
				r8 = fmul(r0, r15, mf);
				r8 = fadd_sub(r8, r14, false, af);
				r8 = fadd_sub(r8, r13, true, af);
				r2 = fix(r8) & 0x3f;
				r3 = v5;
				r4 = 0x30340;
				r11 = iadd(r4, r2);
				i6 = r11;
				r5 = dm_read32_i(i6) | r3;
				dm_write32_i(i6, r5);
				r8 = fmul(r1, r15, mf);
				r8 = fadd_sub(r8, r14, false, af);
				r8 = fadd_sub(r8, r13, true, af);
				r2 = fix(r8) & 0x3f;
				r3 = v5;
				r4 = 0x30380;
				r11 = iadd(r4, r2);
				i6 = r11;
				r5 = dm_read32_i(i6) | r3;
				dm_write32_i(i6, r5);
				n += 35;
			}
			r0 = v5 << 1; // LSHIFT R0 BY 1: SV, SZ when zero
			sf = SV | (r0 == 0 ? SZ : 0);
			v5 = r0;
			n += 3;
		}
		// Each mask word becomes the OR of the cells up to it: 0x30340 upwards
		// (the last word read, not written), 0x30380 downwards.
		m6 = 0xffffffff;
		m7 = 0xfffffffe;
		r0 = 0;
		r1 = dm_read32_i(0x30340);
		for (uint32_t a = 0x30340; a < 0x3037f; ++a) {
			r0 = r1 | r0;
			r1 = dm_read32_i(a + 1);
			dm_write32_i(a, r0);
		}
		n += 5 + 1 + 63 * 5;
		r0 = 0;
		r1 = dm_read32_i(0x303bf);
		for (uint32_t a = 0x303bf; a > 0x30380; --a) {
			r0 = r1 | r0;
			r1 = dm_read32_i(a - 1);
			dm_write32_i(a, r0);
		}
		i7 = 0x3037f;
		n += 4 + 63 * 5;

		r1 = pass;
		r0 = iadd(0x01407f40, r1);
		v3 = r0;
		r2 = 0;
		v8 = 0;
		r0 = 0x30700;
		v6 = r0;
		m6 = 0x60;
		m7 = 3;
		n += 12 + 1;
		for (int k = 0; k < 32; ++k) { // 0x20c19-0x20c6b
			i7 = v3;
			r0 = dm_read32_i(i7);
			r1 = dm_read32_i(i7 + m6);
			r13 = dm_read32_i(i7);
			i7 += m7;
			r13 = i7;
			v3 = r13;
			r4 = fadd_sub(r1, r0, true, af);
			n += 8;
			if (f_lt(af)) { r15 = r0; r0 = r1; r1 = r15; n += 3; }
			i7 = v6;
			r8 = dm_read32_i(i7);
			i7 += 1;
			r15 = i7;
			v6 = r15;
			n += 8;
			if (r8 != 0) {
				r14 = v12;
				r13 = r14 >> 1;
				v12 = r13;
				sf = (r14 & 1) ? 0 : SZ;
				n += 5;
				if (r14 & 1) {
					r14 = w41b;
					r15 = isub(4, r14);
					n += 4;
					if (r15 != 0) {
						r14 = dm_read32_i(0x307f0);
						r8 = fmul(r8, r14, mf);
						n += 2;
					}
				}
				r0 = fadd_sub(r0, r8, true, af);
				r1 = fadd_sub(r1, r8, false, af);
				r15 = 0x41800000; // 16.0
				r14 = 0x42000000; // 32.0
				r13 = 0x3f000000; // 0.5
				r12 = 0x42800000; // 64.0
				r8 = fmul(r0, r15, mf);
				r8 = fadd_sub(r8, r14, false, af);
				n += 9;
				bool out = f_lt(af); // below the grid
				if (!out) {
					r7 = fadd_sub(r8, r12, true, af);
					n += 2;
					out = !f_lt(af); // at or above 64
				}
				if (!out) {
					r8 = fadd_sub(r8, r13, true, af);
					r2 = fix(r8) & 0x3f;
					r3 = 0x3f;
					r4 = 0x30380;
					r11 = iadd(r4, r2);
					i6 = r11;
					r5 = dm_read32_i(i6);
					v5 = r5;
					r8 = fmul(r1, r15, mf);
					r8 = fadd_sub(r8, r14, false, af);
					n += 14;
					out = f_lt(af);
					if (!out) {
						r7 = fadd_sub(r8, r12, true, af);
						n += 2;
						out = !f_lt(af);
					}
					if (!out) {
						r8 = fadd_sub(r8, r13, true, af);
						r2 = fix(r8) & 0x3f;
						r3 = 0x3f;
						r4 = 0x30340;
						r11 = iadd(r4, r2);
						i6 = r11;
						r5 = dm_read32_i(i6);
						r4 = v5 & r5;
						r6 = v8;
						n += 11;
					}
				}
				if (out) { // 0x20c78: two stores, JUMP (DB) 0x20c5d, R4 = 0, R6 = DM(I3+8)
					r4 = 0;
					r6 = v8;
					n += 5;
				}
				r7 = 0x01403e20;
				r15 = iadd(r7, r6);
				i6 = r15;
				r3 = dm_read32_i(i6) & r4;
				dm_write32_i(i6, r3);
				n += 8;
			}
			r6 = iadd(v8, 1);
			v8 = r6;
			n += 7;
		}
		r0 = iadd(pass, 1);
		pass = r0;
		r1 = 3;
		cacc = cacc_comp(cacc, r0, r1);
		n += 6;
		if (r0 == r1) break;
	}

	// 0x20c72: 16 words of buffer RAM from 0x1403d80 cleared, then RTS.
	r0 = 0;
	i6 = 0x01403d80;
	FVIPERS_HLE_LOOP_BEGIN(16, 0x20c75, 0x20c76, 2);
	for (int k = 0; k < 16; ++k) dm_write32_i(i6++, r0);
	FVIPERS_HLE_LOOP_END();
	n += 2 + 1 + 32 + 1;

	REG(0) = r0; REG(1) = r1; REG(2) = r2; REG(3) = r3; REG(4) = r4; REG(5) = r5; REG(6) = r6; REG(7) = r7;
	REG(8) = r8; REG(11) = r11; REG(12) = r12; REG(13) = r13; REG(14) = r14; REG(15) = r15;
	DM_REG_I(6) = i6;
	DM_REG_I(7) = i7;
	DM_REG_M(6) = m6;
	DM_REG_M(7) = m7;
	dm_write32_i(i3 + 0x03, v3);
	dm_write32_i(i3 + 0x05, v5);
	dm_write32_i(i3 + 0x06, v6);
	dm_write32_i(i3 + 0x08, v8);
	dm_write32_i(i3 + 0x10, pass);
	dm_write32_i(i3 + 0x11, v11);
	dm_write32_i(i3 + 0x12, v12);
	dm_write32_i(i3 + 0x20, r15);
	// The last ALU operation is COMP(R0, R1) with R0 = R1: AZ only.
	m_core->astat = (m_core->astat & ~FVIPERS_HLE_ASTAT_MASK) | AZ | mf | sf | cacc;
	m_core->stky |= stky;
	done += n;
	return POP_PC();
}

// Command 0x20982 (1.3 a frame in the fight, 7 % of the instructions, ~5,500
// a call): five input words (DM 0x30801, 0x30800, 0x30802, 0x30804, and
// I3+0x1e whose bit 31 is a flag and the rest picks the side: the entries
// at 0x1407e80 and the table at 0x30700, or at 0x1403e80 and 0x30600 with
// DM 0x30817-0x30826 cleared). DM 0x30805-0x30812 cleared, 0x30813-0x30816
// = 100.0. For each of 32 entries k with a non-zero half-width w (table
// word k), centre (x, y, z) from buffer RAM: DM 0x3080a keeps the largest
// y + w; when y - w < DM 0x30804 the entry is in range: its bit k goes into
// DM I3+3, and into 0x30805-0x30808 by comparing y - w and y + w with 0.05,
// -0.1 and 0.05 above DM 0x30800 and 0x30801; then four corners (x or z, +-
// w, against DM 0x30802) give either the smallest gap (DM 0x30813 + c) or,
// for a corner over the limit, its bit k into 0x30809 and the smallest
// overshoot (DM 0x3080b + c). Then the four overshoots and gaps are folded
// pairwise (0x30809 covering every entry in range with I3+0x1e's bit 31
// clears the overshoots), the 18 words 0x30805-0x30816 go to the output
// FIFO, and four masks (0x30805, 0x30809, 0x30807, 0x30808) are each mapped
// through the table at 0x306a0 by the routine 0x20b57 (run here too) and
// sent. Registers, memory, buffer RAM reads (in the program's order), FIFO
// words, stacks (the last loop's, then the four calls'), STKY, ASTAT and
// the instruction count as the program leaves them.
struct sharc_hle_contact_20982;

template <>
inline uint32_t adsp21062_device::run_hle<sharc_hle_contact_20982>(uint64_t &done)
{
	if (m_core->flag[0] != 0) return 0; // the recompiled wait
	// Only where the whole routine fits the run's budget (at most ~7,500
	// instructions; see 0x20b8a).
	if (done >= m_budget || m_budget - done < 7600) return 0;
	const uint32_t i3 = DM_REG_I(3);
	if (i3 != 0x30300 || DM_REG_M(0) != 0 || DM_REG_M(1) != 1 || DM_REG_M(2) != 2 || DM_REG_L(6) != 0 ||
	    DM_REG_L(7) != 0 || DM_REG_L(1) != 0 || (m_core->mode1 & MODE1_ALUSAT) || m_core->flag[1] != 0)
		return 0; // not the state this was written for (FLAG1, the output FIFO's, never rises here)

	// The five input words, each after its wait; a wait that is real stops here.
	uint64_t n = 0;
	static constexpr uint32_t kTo[4] = {0x30801, 0x30800, 0x30802, 0x30804};
	static constexpr uint32_t kWait[4] = {0x20985, 0x20988, 0x2098b, 0x2098e};
	for (int w = 0; w < 4; ++w) {
		FVIPERS_HLE_IN(0);
		dm_write32_i(kTo[w], REG(0));
		n += 3;
		if (m_core->flag[0] != 0) { done += n; return kWait[w]; }
	}
	FVIPERS_HLE_IN(1);
	dm_write32_i(i3 + 0x1e, REG(1));
	n += 3;

	FVIPERS_HLE_ARITH;
	uint32_t r0 = REG(0), r1 = REG(1), r2 = REG(2), r3, r4 = REG(4), r5 = REG(5), r6 = REG(6), r7 = REG(7);
	uint32_t r8 = REG(8), r9 = REG(9), r10 = REG(10), r11, r12 = REG(12), r13 = REG(13), r14 = REG(14), r15;
	uint32_t i6, i7, m6 = DM_REG_M(6);
	uint32_t sf, cacc = m_core->astat & 0xff000000u, af, f;
	const uint32_t mf = m_core->astat & (MN | MV | MU | MI); // no multiply here
	const auto rd = [&](uint32_t a) { return dm_read32_i(a); };
	const auto wr = [&](uint32_t a, uint32_t v) { dm_write32_i(a, v); };
	// BSET Rx BY Ry (the shifter's flags into sf)
	const auto bset = [&](uint32_t x, uint32_t b) {
		const uint32_t r = b < 32 ? x | (1u << b) : x;
		sf = (b < 32 ? 0 : SV) | (r == 0 ? SZ : 0);
		return r;
	};
	const auto le = [](uint32_t fl) { return (fl & (AZ | AN)) != 0; }; // after a float COMP (AF set)

	// 0x20991: the side
	r1 &= 0x7fffffff; // BCLR R1 BY 31
	sf = r1 == 0 ? SZ : 0;
	r3 = 0x01407e80;
	r15 = 0x30700;
	uint32_t v1f = r15;
	n += 6;
	if (r1 == 0) {
		r3 = 0x01403e80;
		r15 = 0x30600;
		v1f = r15;
		r0 = 0;
		for (uint32_t a = 0x30817; a < 0x30827; ++a) wr(a, r0);
		n += 5 + 32;
	}
	wr(0x30803, r3);
	r0 = 0;
	for (uint32_t a = 0x30805; a < 0x30817; ++a) wr(a, r0);
	r0 = 0x42c80000; // 100.0
	for (uint32_t a = 0x30813; a < 0x30817; ++a) wr(a, r0);
	i6 = 0x30817;
	r11 = 0;
	uint32_t v3 = 0;
	n += 4 + 18 + 3 + 4 + 4;

	do { // 0x209ab: entry r11
		r1 = iadd(r11, r11);
		r1 = iadd(r1, r11);
		r4 = iadd(r3, r1);
		i6 = r4;
		r12 = rd(i6);
		r13 = rd(i6 + 1);
		r14 = rd(i6 + 2);
		i6 = v1f;
		m6 = r11;
		r15 = rd(i6 + m6);
		n += 12;
		if (r15 != 0) {
			r0 = fadd_sub(r13, r15, false, af);
			r4 = rd(0x3080a);
			f = fcomp(r0, r4, cacc);
			n += 4;
			if (!le(f)) { wr(0x3080a, r0); n += 1; }
			r0 = fadd_sub(r13, r15, true, af);
			r4 = rd(0x30804);
			f = fcomp(r4, r0, cacc);
			n += 4;
			if (!le(f)) {
				r0 = bset(v3, r11);
				v3 = r0;
				r2 = 1;
				r4 = 0x3d4ccccc; // 0.05
				r0 = fadd_sub(r13, r15, true, af);
				f = fcomp(r0, r4, cacc);
				n += 10;
				if (le(f)) { r5 = bset(rd(0x30805), r11); wr(0x30805, r5); n += 3; }
				r4 = 0xbdcccccc; // -0.1
				r0 = fadd_sub(r13, r15, false, af);
				f = fcomp(r0, r4, cacc);
				n += 4;
				if (le(f)) { r5 = bset(rd(0x30806), r11); wr(0x30806, r5); n += 3; }
				r6 = rd(0x30800);
				r4 = fadd_sub(0x3d4ccccc, r6, false, af);
				r0 = fadd_sub(r13, r15, true, af);
				f = fcomp(r0, r4, cacc);
				n += 6;
				if (le(f)) { r5 = bset(rd(0x30807), r11); wr(0x30807, r5); n += 3; }
				r6 = rd(0x30801);
				r4 = fadd_sub(0x3d4ccccc, r6, false, af);
				r0 = fadd_sub(r13, r15, true, af);
				f = fcomp(r0, r4, cacc);
				n += 6;
				if (le(f)) { r5 = bset(rd(0x30808), r11); wr(0x30808, r5); n += 3; }
				r10 = 0;
				r5 = rd(0x30802);
				n += 4;
				do { // 0x209ea: corner r10
					r0 = (r10 & 2) ? r14 : r12;
					r7 = fadd_sub(r0, r15, false, af);
					r2 = fadd_sub(r5, r7, true, af);
					n += 7;
					if (r10 & 1) {
						r7 = fadd_sub(r0, r15, true, af);
						r2 = fadd_sub(r5, r7, false, af);
						n += 2;
					}
					r1 = (r10 << 31) ^ r7; // LSHIFT R10 BY 31, XOR R7
					sf = (r1 & 0x80000000u) ? 0 : SZ; // BTST R1 BY 31
					n += 4;
					bool gap = (r1 & 0x80000000u) != 0;
					if (!gap) {
						r4 = fabs_(r7);
						f = fcomp(r5, r4, cacc);
						n += 3;
						gap = !le(f);
					}
					if (!gap) {
						n += 1;
						if (f & AN) { // over the limit
							r0 = bset(rd(0x30809), r11);
							wr(0x30809, r0);
							r4 = fadd_sub(r4, r5, true, af);
							n += 6;
						} else {
							r4 = 0x3a83126e;
							n += 2;
						}
						i6 = 0x3080b;
						m6 = r10;
						r0 = rd(i6 + m6);
						f = fcomp(r4, r0, cacc);
						n += 5;
						if (!(f & AN)) { wr(i6 + m6, r4); n += 2; }
					} else {
						i6 = 0x30813;
						m6 = r10;
						r0 = rd(i6 + m6);
						f = fcomp(r2, r0, cacc);
						n += 5;
						if (f & AN) { wr(i6 + m6, r2); n += 3; }
					}
					r10 = iadd(r10, 1);
					r0 = isub(4, r10);
					n += 4;
				} while (r0 != 0);
			}
		}
		r11 = iadd(r11, 1);
		r0 = isub(0x20, r11);
		n += 4;
	} while (r0 != 0);

	// 0x20a1a: every entry in range over the limit, with the flag: no overshoot
	r0 = rd(0x30809);
	r1 = v3;
	r2 = isub(r1 & r0, r1);
	n += 5;
	if (r2 == 0) {
		r0 = rd(i3 + 0x1e);
		sf = (r0 & 0x80000000u) ? 0 : SZ;
		n += 3;
		if (r0 & 0x80000000u) {
			for (uint32_t a = 0x3080b; a < 0x3080f; ++a) wr(a, r2);
			n += 6;
		}
	}

	// 0x20a28: each overshoot slot from the gaps
	i6 = 0x30813;
	i7 = 0x3080f;
	m6 = 0xffffffff;
	r8 = 0x42c80000; // 100.0
	r9 = rd(0x30802);
	r9 = fadd_sub(r9, r9, false, af);
	r14 = 0x3a83126e;
	r15 = 0;
	r10 = 0;
	r7 = 4;
	n += 10;
	do {
		r3 = rd(i6);
		f = fcomp(r3, r8, cacc);
		n += 3;
		if (!(f & AZ)) {
			r4 = rd(i6 + 1);
			sf = (r10 & 1) ? 0 : SZ;
			n += 3;
			if (r10 & 1) { r4 = rd(i6 + m6); n += 1; }
			r5 = fadd_sub(r9, r4, true, af);
			r13 = fadd_sub(r15, r5, true, af);
			if (af & AZ) r5 = r14;
			wr(i7, r5);
			n += 6;
		}
		r5 = rd(i6++);
		r5 = rd(i7++);
		r10 = iadd(r10, 1);
		r5 = isub(r10, r7);
		n += 5;
	} while (r5 != 0);

	// 0x20a44: each pair keeps its smaller non-negative value
	i6 = 0x3080f;
	n += 2;
	for (int k = 0; k < 2; ++k) {
		r4 = rd(i6);
		r5 = rd(i6 + 1);
		r9 = 0;
		if (r4 & 0x80000000u) r4 = r9;
		sf = (r5 & 0x80000000u) ? 0 : SZ;
		if (r5 & 0x80000000u) r5 = r9;
		f = fcomp(r4, r5, cacc);
		n += 9;
		if (!le(f)) { r4 = r9; n += 1; }
		else { r5 = r9; n += 2; }
		wr(i6, r4);
		wr(i6 + 1, r5);
		r5 = rd(i6);
		i6 += 2;
		n += 5;
	}
	r4 = rd(0x3080f);
	r5 = rd(0x30810);
	r7 = rd(0x30811);
	r8 = rd(0x30812);
	r6 = fadd_sub(r4, r5, false, af);
	r9 = fadd_sub(r7, r8, false, af);
	f = fcomp(r6, r9, cacc);
	n += 8;
	if (f & AN) { r7 = 0; r8 = 0; n += 2; }
	else { r4 = 0; r5 = 0; n += 3; }
	wr(0x3080f, r4);
	wr(0x30810, r5);
	wr(0x30811, r7);
	wr(0x30812, r8);
	const uint32_t last20 = r15; // the last DM(I3+0x20) = R15
	n += 6;

	// 0x20a6a: the 18 words to the output FIFO
	i6 = 0x30805;
	FVIPERS_HLE_LOOP_BEGIN(18, 0x20a6c, 0x20a70, 3);
	for (int k = 0; k < 18; ++k) {
		r0 = rd(i6++);
		FVIPERS_HLE_OUT(r0);
	}
	FVIPERS_HLE_LOOP_END();
	n += 2 + 90;

	// 0x20a71: four masks through 0x20b57, each result sent
	static constexpr uint32_t kMask[4] = {0x30805, 0x30809, 0x30807, 0x30808};
	for (int c = 0; c < 4; ++c) {
		r0 = rd(kMask[c]);
		PUSH_PC(); // CALL 0x20b57, not delayed
		m_core->pcstk = 0x20a73 + 4 * c;
		n += 2;
		i6 = 0x306a0;
		r9 = 0;
		r13 = 0x20;
		n += 4;
		for (r11 = 0; r11 < 32;) {
			sf = ((r0 >> r11) & 1) ? 0 : SZ; // BTST R0 BY R11
			n += 2;
			if ((r0 >> r11) & 1) {
				m6 = r11;
				r12 = rd(i6 + m6);
				r9 = bset(r9, r12);
				n += 3;
			}
			r11 = iadd(r11, 1);
			r15 = isub(r11, r13);
			n += 3;
		}
		n += 1; // RTS
		POP_PC();
		FVIPERS_HLE_OUT(r9);
		n += 2;
	}

	REG(0) = r0; REG(1) = r1; REG(2) = r2; REG(3) = r3; REG(4) = r4; REG(5) = r5; REG(6) = r6; REG(7) = r7;
	REG(8) = r8; REG(9) = r9; REG(10) = r10; REG(11) = r11; REG(12) = r12; REG(13) = r13; REG(14) = r14;
	REG(15) = r15;
	DM_REG_I(6) = i6;
	DM_REG_I(7) = i7;
	DM_REG_M(6) = m6;
	wr(i3 + 0x03, v3);
	wr(i3 + 0x1f, v1f);
	wr(i3 + 0x20, last20);
	// The last ALU operation is 0x20b57's R15 = R11 - R13 with both 32: AZ and AC.
	m_core->astat = (m_core->astat & ~FVIPERS_HLE_ASTAT_MASK) | AZ | AC | mf | sf | cacc;
	m_core->stky |= stky;
	done += n + 1; // the RTS
	return POP_PC();
}
