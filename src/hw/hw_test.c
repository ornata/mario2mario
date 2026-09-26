/* Hardware model unit tests: register semantics worked out by hand from
 * the documented N64 register layouts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static uint8_t rom[0x2000];

static void test_mi(HwState *hw) {
  /* Set SP, VI, DP masks (bits 1, 7, 11), then clear VI (bit 6). */
  hw_write32(hw, 0x0430000C, (1u << 1) | (1u << 7) | (1u << 11));
  CHECK(hw->mi[MI_INTR_MASK] == (MI_INTR_SP | MI_INTR_VI | MI_INTR_DP));
  hw_write32(hw, 0x0430000C, 1u << 6);
  CHECK(hw->mi[MI_INTR_MASK] == (MI_INTR_SP | MI_INTR_DP));
  CHECK(hw_read32(hw, 0x04300004) == 0x02020102u);
  CHECK(!hw_irq(hw));
}

static void test_pi(HwState *hw) {
  for (unsigned i = 0; i < sizeof(rom); i++)
    rom[i] = (uint8_t)(i * 7);
  hw->now = 1234;
  hw_write32(hw, 0x04600000, 0x00100000); /* PI_DRAM_ADDR */
  hw_write32(hw, 0x04600004, 0x10001000); /* PI_CART_ADDR: ROM 0x1000 */
  hw_write32(hw, 0x0460000C, 0x000000FF); /* PI_WR_LEN: 256 bytes */
  CHECK(memcmp(hw->rdram + 0x100000, rom + 0x1000, 256) == 0);
  CHECK(hw->rdram[0x100100] == 0);
  CHECK(hw->mi[MI_INTR] & MI_INTR_PI);
  CHECK(hw->dma_count == 1 && hw->dma_log[0].rom_offset == 0x1000 &&
        hw->dma_log[0].ram_addr == 0x100000 && hw->dma_log[0].length == 256 &&
        hw->dma_log[0].icount == 1234);
  CHECK(hw_read32(hw, 0x04600010) == 8); /* not busy, interrupt pending */
  hw_write32(hw, 0x04600010, 2);
  CHECK(!(hw->mi[MI_INTR] & MI_INTR_PI));
  /* Direct cart read: big-endian ROM word. */
  CHECK(hw_read32(hw, 0x10000004) ==
        ((uint32_t)rom[4] << 24 | (uint32_t)rom[5] << 16 |
         (uint32_t)rom[6] << 8 | rom[7]));
}

static void test_sp(HwState *hw) {
  for (unsigned i = 0; i < 16; i++)
    hw->rdram[0x2000 + i] = (uint8_t)(0xA0 + i);
  hw_write32(hw, 0x04040000, 0x1008); /* IMEM + 8 */
  hw_write32(hw, 0x04040004, 0x2000);
  hw_write32(hw, 0x04040008, 0x0000000F); /* 16 bytes (len-1 | 7) */
  CHECK(memcmp(hw->sp_mem + 0x1008, hw->rdram + 0x2000, 16) == 0);
  CHECK(hw_read32(hw, 0x04001008) == 0xA0A1A2A3u);

  /* Semaphore: first read 0 then 1; write releases. */
  CHECK(hw_read32(hw, 0x0404001C) == 0);
  CHECK(hw_read32(hw, 0x0404001C) == 1);
  hw_write32(hw, 0x0404001C, 0);
  CHECK(hw_read32(hw, 0x0404001C) == 0);

  /* HLE task: OSTask at DMEM 0xFC0, gfx type, data at RDRAM 0x3000. */
  for (unsigned i = 0; i < 16; i++)
    hw_put_be32(hw->sp_mem + 0xFC0 + 4 * i, 0);
  hw_put_be32(hw->sp_mem + 0xFC0 + 4 * OSTASK_TYPE, OSTASK_M_GFXTASK);
  hw_put_be32(hw->sp_mem + 0xFC0 + 4 * OSTASK_DATA_PTR, 0x80003000u);
  hw_put_be32(hw->sp_mem + 0xFC0 + 4 * OSTASK_DATA_SIZE, 16);
  hw_put_be32(hw->rdram + 0x3000, 0xDE000000u);
  hw->now = 5000;
  /* set intr-on-break (bit 8), clear broke (2), clear halt (0) */
  hw_write32(hw, 0x04040010, (1u << 8) | (1u << 2) | (1u << 0));
  CHECK(hw->task_count == 1);
  CHECK(hw->task_log[0].data_hash ==
        hw_hash_word(
            hw_hash_word(
                hw_hash_word(hw_hash_word(HW_HASH_INIT, 0xDE000000u), 0), 0),
            0));
  CHECK(!(hw->sp[SP_STATUS] & SPS_HALT));
  hw_run_events(hw, 5000 + HW_SP_TASK_LATENCY - 1);
  CHECK(!(hw->mi[MI_INTR] & MI_INTR_SP));
  hw_run_events(hw, 5000 + HW_SP_TASK_LATENCY);
  CHECK(hw->sp[SP_STATUS] & SPS_HALT);
  CHECK(hw->sp[SP_STATUS] & SPS_SIG2);
  CHECK((hw->mi[MI_INTR] & (MI_INTR_SP | MI_INTR_DP)) ==
        (MI_INTR_SP | MI_INTR_DP));
  hw_write32(hw, 0x04040010, 1u << 3); /* clear SP interrupt */
  CHECK(!(hw->mi[MI_INTR] & MI_INTR_SP));
}

static void test_vi(HwState *hw) {
  hw_run_events(hw, HW_VI_PERIOD);
  CHECK(hw->vi_count == 1 && (hw->mi[MI_INTR] & MI_INTR_VI));
  hw->now = HW_VI_PERIOD + HW_VI_PERIOD / 2;
  CHECK(hw_read32(hw, 0x04400010) == 262); /* 262.5 -> even 262 */
  hw_write32(hw, 0x04400010, 0);
  CHECK(!(hw->mi[MI_INTR] & MI_INTR_VI));
}

static void test_si(HwState *hw) {
  /* Command block: ch0 read buttons (tx1 rx4), ch1 info (no device),
   * end marker; control byte 1. */
  uint8_t cmd[64] = {0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                     0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
  cmd[63] = 0x01;
  memcpy(hw->rdram + 0x4000, cmd, 64);
  hw->pad[0].buttons = 0x9000; /* A + Start */
  hw->pad[0].stick_x = -5;
  hw->pad[0].stick_y = 17;
  hw_write32(hw, 0x04800000, 0x4000);
  hw_write32(hw, 0x04800010, 0x1FC007C0); /* WR64B */
  hw_write32(hw, 0x04800000, 0x4100);
  hw_write32(hw, 0x04800004, 0x1FC007C0); /* RD64B */
  const uint8_t *out = hw->rdram + 0x4100;
  CHECK(out[3] == 0x90 && out[4] == 0x00 && out[5] == 0xFB && out[6] == 17);
  CHECK(out[8] == (0x03 | 0x80)); /* channel 1: no response */
  CHECK(hw->si_reads == 1 && hw->si_writes == 1);
  CHECK(hw_read32(hw, 0x04800018) == (1u << 12));
  hw_write32(hw, 0x04800018, 0);
  CHECK(!(hw->mi[MI_INTR] & MI_INTR_SI));
}

int main(void) {
  HwState *hw = malloc(sizeof(HwState));
  hw_init(hw, rom, sizeof(rom));
  test_mi(hw);
  test_pi(hw);
  test_sp(hw);
  test_vi(hw);
  test_si(hw);
  CHECK(hw->unmapped_accesses == 0);
  hw_free(hw);
  free(hw);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
