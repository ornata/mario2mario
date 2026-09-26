#include "src/oracle/boot.h"

#include <string.h>

void boot_pif_hle(CpuState *c, HwState *hw) {

  uint32_t n = hw->rom_size < 0x1000u ? hw->rom_size : 0x1000u;
  memcpy(hw->sp_mem, hw->rom, n);

  hw->ri[RI_MODE] = 0x0E;
  hw->ri[RI_CONFIG] = 0x40;
  hw->ri[RI_SELECT] = 0x14;
  hw->ri[RI_REFRESH] = 0x00063634;
  hw_put_be32(hw->rdram + 0x318, HW_RDRAM_SIZE);

  c->gpr[19] = 0;    /* s3: ROM type */
  c->gpr[20] = 1;    /* s4: TV type NTSC */
  c->gpr[21] = 0;    /* s5: reset type */
  c->gpr[22] = 0x3F; /* s6: CIC-6102 seed */
  c->gpr[23] = 0;    /* s7: version */
  c->gpr[29] = sext32(0xA4001FF0u);
  c->gpr[11] = sext32(BOOT_PC);
  c->gpr[31] = sext32(0xA4001550u);

  c->cop0[CP0_STATUS] = 0x34000000u;
  c->cop0[CP0_CONFIG] = 0x0006E463u;
  c->cop0[CP0_PRID] = 0x00000B22u;
  c->fcr0 = 0x00000A00u;
  c->fcr31 = 0;

  c->pc = BOOT_PC;
  c->next_pc = BOOT_PC + 4;
  c->boundary = 1;
}
