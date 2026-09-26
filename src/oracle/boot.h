/* Boot: HLE of the PIF boot ROM stage only.
 *
 * The PIF ROM (in the console, not the cartridge) is not in the ROM image
 * and its job before IPL3 is fixed, so it is replaced by its documented
 * end state. Everything after that is the ROM's own IPL3 running as real
 * interpreted code from SP DMEM at 0xA4000040:
 *
 * - ROM[0x000..0x1000] (header + IPL3) is copied to SP DMEM.
 * - CPU: PC = 0xA4000040; GPRs IPL3 reads before writing, per the
 *   CIC-NUS-6102 boot convention: s3 = 0 (ROM type: cartridge),
 *   s4 = 1 (TV type: NTSC), s5 = 0 (cold reset), s6 = 0x3F (6102 seed),
 *   s7 = 0 (PIF version), sp = 0xA4001FF0, t3 = 0xA4000040,
 *   ra = 0xA4001550. COP0 Status = 0x34000000 (CU1|CU0|FR),
 *   Config = 0x0006E463, PRId = 0x00000B22; FCR0 = 0x00000A00.
 * - RI is left as a completed RDRAM initialisation (RI_MODE 0x0E,
 *   RI_CONFIG 0x40, RI_SELECT 0x14, RI_REFRESH 0x00063634). IPL3 tests
 *   RI_SELECT and, when non-zero, skips RDRAM module probing (its warm
 *   boot path), so the RDRAM register protocol need not be modelled.
 * - Because that path skips RDRAM sizing, osMemSize (RDRAM 0x318) is
 *   seeded with the modelled size (8 MB), as a cold boot would have
 *   left it. */
#ifndef M2M_ORACLE_BOOT_H
#define M2M_ORACLE_BOOT_H

#include "src/oracle/cpu.h"

#define BOOT_PC 0xA4000040u

/* Leaves `c` and `hw` in the post-PIF state. Shared by both engines. */
void boot_pif_hle(CpuState *c, HwState *hw);

#endif
