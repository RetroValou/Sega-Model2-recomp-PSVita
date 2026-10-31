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
