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
