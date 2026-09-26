#include "src/hw/hw.h"

#include <stdlib.h>
#include <string.h>

/* Physical address map as a range table, expanded into hw->region[]. */
static const struct {
  uint16_t first_mb, last_mb;
  uint8_t region;
} region_ranges[] = {
    {0x000, 0x007, HWR_RDRAM}, {0x03F, 0x03F, HWR_RDRAM_REGS},
    {0x040, 0x040, HWR_SP},    {0x041, 0x041, HWR_DP},
    {0x042, 0x042, HWR_DPS},   {0x043, 0x043, HWR_MI},
    {0x044, 0x044, HWR_VI},    {0x045, 0x045, HWR_AI},
    {0x046, 0x046, HWR_PI},    {0x047, 0x047, HWR_RI},
    {0x048, 0x048, HWR_SI},    {0x050, 0x1FB, HWR_CART},
    {0x1FC, 0x1FC, HWR_PIF},
};

#define MI_VERSION_VALUE 0x02020102u
#define CART_ROM_BASE    0x10000000u
#define PIF_RAM_BASE     0x1FC007C0u

static void recompute_next_event(HwState *hw) {
  hw->next_event = hw->next_vi < hw->sp_done_at ? hw->next_vi : hw->sp_done_at;
}

void hw_init(HwState *hw, const uint8_t *rom, uint32_t rom_size) {
  memset(hw, 0, sizeof(*hw));
  for (size_t i = 0; i < sizeof(region_ranges) / sizeof(region_ranges[0]); i++)
    for (unsigned mb = region_ranges[i].first_mb;
         mb <= region_ranges[i].last_mb; mb++)
      hw->region[mb] = region_ranges[i].region;
  hw->rom = rom;
  hw->rom_size = rom_size;
  hw->mi[MI_VERSION] = MI_VERSION_VALUE;
  hw->sp[SP_STATUS] = SPS_HALT;
  hw->pad[0].present = 1;
  hw->next_vi = HW_VI_PERIOD;
  hw->sp_done_at = UINT64_MAX;
  recompute_next_event(hw);
}

void hw_free(HwState *hw) {
  free(hw->dma_log);
  free(hw->task_log);
  hw->dma_log = NULL;
  hw->task_log = NULL;
}

uint64_t hw_hash_be_words(const uint8_t *p, uint32_t len) {
  uint64_t h = HW_HASH_INIT;
  for (uint32_t i = 0; i + 4 <= len; i += 4)
    h = hw_hash_word(h, hw_be32(p + i));
  return h;
}

static void raise_mi(HwState *hw, uint32_t bits) {
  hw->mi[MI_INTR] |= bits;
}
static void clear_mi(HwState *hw, uint32_t bits) {
  hw->mi[MI_INTR] &= ~bits;
}

/* Applies a set/clear bit-pair write: bit 2k clears flag k, bit 2k+1 sets
 * it (MI_INTR_MASK layout). */
static uint32_t apply_pairs(uint32_t cur, uint32_t w, unsigned nflags,
                            unsigned first_bit) {
  for (unsigned k = 0; k < nflags; k++) {
    uint32_t flag = 1u << k;
    if (w & (1u << (first_bit + 2 * k)))
      cur &= ~flag;
    if (w & (1u << (first_bit + 2 * k + 1)))
      cur |= flag;
  }
  return cur;
}

/* --- SP ----------------------------------------------------------------- */

static void sp_dma(HwState *hw, int to_rdram, uint32_t lenreg) {
  uint32_t len = ((lenreg & 0xFFFu) | 7u) + 1u;
  uint32_t count = ((lenreg >> 12) & 0xFFu) + 1u;
  uint32_t skip = (lenreg >> 20) & 0xFF8u;
  uint32_t mem = hw->sp[SP_MEM_ADDR] & 0x1FF8u;
  uint32_t dram = hw->sp[SP_DRAM_ADDR] & 0xFFFFF8u;
  for (uint32_t c = 0; c < count; c++) {
    for (uint32_t i = 0; i < len; i++) {
      uint32_t m = (mem & 0x1000u) | ((mem + i) & 0xFFFu);
      uint32_t d = (dram + i) & (HW_RDRAM_SIZE - 1);
      if (to_rdram)
        hw->rdram[d] = hw->sp_mem[m];
      else
        hw->sp_mem[m] = hw->rdram[d];
    }
    mem = (mem & 0x1000u) | ((mem + len) & 0xFFFu);
    dram += len + skip;
  }
  hw->sp[SP_MEM_ADDR] = mem;
  hw->sp[SP_DRAM_ADDR] = dram;
}

static void *log_grow(void *v, uint32_t *cap, uint32_t n, size_t elem) {
  if (n < *cap)
    return v;
  *cap = *cap ? *cap * 2 : 256;
  return realloc(v, *cap * elem);
}

/* HLE task start: record the OSTask and schedule completion. */
static void sp_start_task(HwState *hw) {
  hw->task_log = log_grow(hw->task_log, &hw->task_cap, hw->task_count,
                          sizeof(HwTaskRecord));
  HwTaskRecord *r = &hw->task_log[hw->task_count++];
  r->icount = hw->now;
  for (unsigned i = 0; i < OSTASK_WORDS; i++)
    r->task[i] = hw_be32(hw->sp_mem + HW_OSTASK_DMEM + 4 * i);
  uint32_t ptr = r->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu;
  uint32_t size = r->task[OSTASK_DATA_SIZE];
  if (ptr >= HW_RDRAM_SIZE)
    size = 0;
  else if (size > HW_RDRAM_SIZE - ptr)
    size = HW_RDRAM_SIZE - ptr;
  r->data_hash = hw_hash_be_words(hw->rdram + ptr, size);
  hw->sp_done_at = hw->now + HW_SP_TASK_LATENCY;
  recompute_next_event(hw);
}

static void sp_finish_task(HwState *hw) {
  hw->sp_done_at = UINT64_MAX;
  hw->sp[SP_STATUS] |= SPS_HALT | SPS_BROKE | SPS_SIG2;
  if (hw->sp[SP_STATUS] & SPS_INTR_BREAK)
    raise_mi(hw, MI_INTR_SP);
  const HwTaskRecord *r = &hw->task_log[hw->task_count - 1];
  if (r->task[OSTASK_TYPE] == OSTASK_M_GFXTASK)
    raise_mi(hw, MI_INTR_DP); /* the display list's final full sync */
}

static void sp_write_status(HwState *hw, uint32_t w) {
  uint32_t s = hw->sp[SP_STATUS];
  int was_halted = s & SPS_HALT;
  if (w & (1u << 0))
    s &= ~SPS_HALT;
  if (w & (1u << 1))
    s |= SPS_HALT;
  if (w & (1u << 2))
    s &= ~SPS_BROKE;
  if (w & (1u << 3))
    clear_mi(hw, MI_INTR_SP);
  if (w & (1u << 4))
    raise_mi(hw, MI_INTR_SP);
  /* bits 5/6 single-step, 7/8 interrupt-on-break, then sig0..sig7 pairs */
  if (w & (1u << 5))
    s &= ~(1u << 5);
  if (w & (1u << 6))
    s |= 1u << 5;
  if (w & (1u << 7))
    s &= ~SPS_INTR_BREAK;
  if (w & (1u << 8))
    s |= SPS_INTR_BREAK;
  for (unsigned k = 0; k < 8; k++) {
    if (w & (1u << (9 + 2 * k)))
      s &= ~(1u << (7 + k));
    if (w & (1u << (10 + 2 * k)))
      s |= 1u << (7 + k);
  }
  hw->sp[SP_STATUS] = s;
  if (was_halted && !(s & SPS_HALT))
    sp_start_task(hw);
}

static uint32_t sp_read(HwState *hw, uint32_t off) {
  if (off < HW_SP_MEM_SIZE)
    return hw_be32(hw->sp_mem + off);
  if (off >= 0x40000 && off < 0x40020) {
    unsigned r = (off - 0x40000) >> 2;
    if (r == SP_SEMAPHORE) {
      uint32_t v = hw->sp[SP_SEMAPHORE];
      hw->sp[SP_SEMAPHORE] = 1;
      return v;
    }
    if (r == SP_DMA_FULL || r == SP_DMA_BUSY)
      return 0;
    return hw->sp[r];
  }
  if (off == 0x80000)
    return hw->sp_pc;
  hw->unmapped_accesses++;
  return 0;
}

static void sp_write(HwState *hw, uint32_t off, uint32_t v) {
  if (off < HW_SP_MEM_SIZE) {
    hw_put_be32(hw->sp_mem + off, v);
    return;
  }
  if (off >= 0x40000 && off < 0x40020) {
    unsigned r = (off - 0x40000) >> 2;
    switch (r) {
    case SP_RD_LEN:
      hw->sp[r] = v;
      sp_dma(hw, 0, v);
      return;
    case SP_WR_LEN:
      hw->sp[r] = v;
      sp_dma(hw, 1, v);
      return;
    case SP_STATUS:
      sp_write_status(hw, v);
      return;
    case SP_SEMAPHORE:
      hw->sp[r] = 0;
      return;
    case SP_DMA_FULL:
    case SP_DMA_BUSY:
      return;
    default:
      hw->sp[r] = v;
      return;
    }
  }
  if (off == 0x80000) {
    hw->sp_pc = v & 0xFFCu;
    return;
  }
  hw->unmapped_accesses++;
}

/* --- PI ----------------------------------------------------------------- */

static void pi_dma_to_rdram(HwState *hw, uint32_t lenreg) {
  uint32_t len = (lenreg & 0x00FFFFFFu) + 1u;
  uint32_t dram = hw->pi[PI_DRAM_ADDR] & 0x00FFFFFEu;
  uint32_t cart = hw->pi[PI_CART_ADDR] & 0x1FFFFFFEu;
  uint32_t rom_off = cart - CART_ROM_BASE;
  int from_rom = cart >= CART_ROM_BASE;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t d = dram + i;
    if (d >= HW_RDRAM_SIZE)
      break;
    uint32_t s = rom_off + i;
    hw->rdram[d] = from_rom && s < hw->rom_size ? hw->rom[s] : 0;
  }
  hw->dma_log =
      log_grow(hw->dma_log, &hw->dma_cap, hw->dma_count, sizeof(HwDmaRecord));
  hw->dma_log[hw->dma_count++] =
      (HwDmaRecord){hw->now, from_rom ? rom_off : cart, dram, len};
  hw->pi[PI_DRAM_ADDR] = (dram + len + 7u) & ~7u;
  hw->pi[PI_CART_ADDR] = (cart + len + 1u) & ~1u;
  raise_mi(hw, MI_INTR_PI);
}

static uint32_t cart_read(HwState *hw, uint32_t paddr) {
  if (paddr >= CART_ROM_BASE) {
    uint32_t off = paddr - CART_ROM_BASE;
    if (off + 4 <= hw->rom_size)
      return hw_be32(hw->rom + off);
  }
  return 0;
}

/* --- SI / PIF joybus ---------------------------------------------------- */

/* Executes one joybus command for `channel`. tx/rx point into PIF RAM;
 * rx_len_byte is the byte holding the rx length (error flags go there). */
static void joybus_command(HwState *hw, unsigned channel, const uint8_t *tx,
                           unsigned txn, uint8_t *rx, unsigned rxn,
                           uint8_t *rx_len_byte) {
  if (txn == 0)
    return;
  uint8_t cmd = tx[0];
  if (channel < 4) {
    const HwPad *p = &hw->pad[channel];
    if (!p->present) {
      *rx_len_byte |= 0x80; /* no response */
      return;
    }
    if ((cmd == 0x00 || cmd == 0xFF) && rxn >= 3) {
      rx[0] = 0x05; /* standard controller */
      rx[1] = 0x00;
      rx[2] = 0x02; /* no pak */
    } else if (cmd == 0x01 && rxn >= 4) {
      rx[0] = (uint8_t)(p->buttons >> 8);
      rx[1] = (uint8_t)p->buttons;
      rx[2] = (uint8_t)p->stick_x;
      rx[3] = (uint8_t)p->stick_y;
    } else {
      *rx_len_byte |= 0x80;
    }
    return;
  }
  if (channel == 4) { /* cartridge EEPROM, 4 Kbit */
    if ((cmd == 0x00 || cmd == 0xFF) && rxn >= 3) {
      rx[0] = 0x00;
      rx[1] = 0x80;
      rx[2] = 0x00;
    } else if (cmd == 0x04 && txn >= 2 && rxn >= 8) {
      memcpy(rx, hw->eeprom + (tx[1] & 0x3Fu) * 8u, 8);
    } else if (cmd == 0x05 && txn >= 10) {
      memcpy(hw->eeprom + (tx[1] & 0x3Fu) * 8u, tx + 2, 8);
      if (rxn >= 1)
        rx[0] = 0x00;
    } else {
      *rx_len_byte |= 0x80;
    }
    return;
  }
  *rx_len_byte |= 0x80;
}

static void pif_process(HwState *hw) {
  uint8_t *ram = hw->pif_ram;
  unsigned i = 0, channel = 0;
  while (i < 63) {
    uint8_t t = ram[i];
    if (t == 0xFE)
      break;
    if (t == 0xFF || t == 0xFD) { /* padding / channel reset marker */
      i++;
      continue;
    }
    if (t == 0x00) { /* skip channel */
      channel++;
      i++;
      continue;
    }
    unsigned txn = t & 0x3Fu;
    if (i + 1 >= 63)
      break;
    unsigned rxn = ram[i + 1] & 0x3Fu;
    if (i + 2 + txn + rxn > 63)
      break;
    joybus_command(hw, channel, ram + i + 2, txn, ram + i + 2 + txn, rxn,
                   ram + i + 1);
    i += 2 + txn + rxn;
    channel++;
  }
}

static void si_dma(HwState *hw, int to_rdram) {
  uint32_t dram = hw->si[SI_DRAM_ADDR] & (HW_RDRAM_SIZE - 1) & ~3u;
  if (to_rdram) {
    pif_process(hw);
    for (unsigned i = 0; i < HW_PIF_RAM_SIZE; i++)
      hw->rdram[(dram + i) & (HW_RDRAM_SIZE - 1)] = hw->pif_ram[i];
    hw->si_reads++;
  } else {
    for (unsigned i = 0; i < HW_PIF_RAM_SIZE; i++)
      hw->pif_ram[i] = hw->rdram[(dram + i) & (HW_RDRAM_SIZE - 1)];
    hw->si_writes++;
  }
  raise_mi(hw, MI_INTR_SI);
}

/* --- Dispatch ----------------------------------------------------------- */

static uint32_t vi_current(const HwState *hw) {
  uint64_t phase = hw->now % HW_VI_PERIOD;
  return (uint32_t)(phase * HW_VI_HALF_LINES / HW_VI_PERIOD) & ~1u;
}

uint32_t hw_read32(HwState *hw, uint32_t paddr) {
  uint32_t off = paddr & 0xFFFFFu;
  unsigned r = off >> 2;
  switch (hw_region(hw, paddr)) {
  case HWR_RDRAM:
    return hw_be32(hw->rdram + (paddr & (HW_RDRAM_SIZE - 1)));
  case HWR_RDRAM_REGS:
    return r < 16 ? hw->rdram_regs[r] : 0;
  case HWR_SP:
    return sp_read(hw, off);
  case HWR_DP:
    return r < DP_REG_COUNT ? hw->dp[r] : 0;
  case HWR_MI:
    return r < MI_REG_COUNT ? hw->mi[r] : 0;
  case HWR_VI:
    if (r == VI_V_CURRENT)
      return vi_current(hw);
    return r < VI_REG_COUNT ? hw->vi[r] : 0;
  case HWR_AI:
    if (r == AI_LEN || r == AI_STATUS)
      return 0; /* audio stub: buffers drain instantly, never full */
    return r < AI_REG_COUNT ? hw->ai[r] : 0;
  case HWR_PI:
    if (r == PI_STATUS)
      return hw->mi[MI_INTR] & MI_INTR_PI ? 1u << 3 : 0; /* never busy */
    return r < PI_REG_COUNT ? hw->pi[r] : 0;
  case HWR_RI:
    return r < RI_REG_COUNT ? hw->ri[r] : 0;
  case HWR_SI:
    if (r == SI_STATUS)
      return hw->mi[MI_INTR] & MI_INTR_SI ? 1u << 12 : 0; /* never busy */
    return r < SI_REG_COUNT ? hw->si[r] : 0;
  case HWR_CART:
    return cart_read(hw, paddr);
  case HWR_PIF:
    if (paddr >= PIF_RAM_BASE)
      return hw_be32(hw->pif_ram + (paddr - PIF_RAM_BASE));
    return 0; /* PIF boot ROM: not modelled (PIF stage is HLE) */
  default:
    hw->unmapped_accesses++;
    return 0;
  }
}

void hw_write32(HwState *hw, uint32_t paddr, uint32_t v) {
  uint32_t off = paddr & 0xFFFFFu;
  unsigned r = off >> 2;
  switch (hw_region(hw, paddr)) {
  case HWR_RDRAM:
    hw_put_be32(hw->rdram + (paddr & (HW_RDRAM_SIZE - 1)), v);
    return;
  case HWR_RDRAM_REGS:
    if (r < 16)
      hw->rdram_regs[r] = v;
    return;
  case HWR_SP:
    sp_write(hw, off, v);
    return;
  case HWR_DP:
    if (r == DPC_STATUS || r >= DP_REG_COUNT)
      return; /* no RDP: status flags stay clear */
    hw->dp[r] = v;
    return;
  case HWR_MI:
    if (r == MI_MODE) {
      hw->mi[MI_MODE] = v & 0x7Fu; /* init length */
      if (v & (1u << 11))
        clear_mi(hw, MI_INTR_DP);
    } else if (r == MI_INTR_MASK) {
      hw->mi[MI_INTR_MASK] = apply_pairs(hw->mi[MI_INTR_MASK], v, 6, 0);
    }
    return;
  case HWR_VI:
    if (r == VI_V_CURRENT)
      clear_mi(hw, MI_INTR_VI);
    else if (r < VI_REG_COUNT)
      hw->vi[r] = v;
    return;
  case HWR_AI:
    if (r == AI_STATUS)
      clear_mi(hw, MI_INTR_AI);
    else if (r < AI_REG_COUNT)
      hw->ai[r] = v;
    if (r == AI_LEN)
      hw->ai_buffers++;
    return;
  case HWR_PI:
    if (r == PI_STATUS) {
      if (v & 2u)
        clear_mi(hw, MI_INTR_PI);
    } else if (r == PI_WR_LEN) {
      hw->pi[r] = v;
      pi_dma_to_rdram(hw, v);
    } else if (r == PI_RD_LEN) {
      hw->pi[r] = v; /* RDRAM -> cart (save memory): not modelled */
      raise_mi(hw, MI_INTR_PI);
    } else if (r < PI_REG_COUNT) {
      hw->pi[r] = v;
    }
    return;
  case HWR_RI:
    if (r < RI_REG_COUNT)
      hw->ri[r] = v;
    return;
  case HWR_SI:
    if (r == SI_STATUS) {
      clear_mi(hw, MI_INTR_SI);
    } else if (r == SI_PIF_ADDR_RD64B) {
      hw->si[r] = v;
      si_dma(hw, 1);
    } else if (r == SI_PIF_ADDR_WR64B) {
      hw->si[r] = v;
      si_dma(hw, 0);
    } else if (r < SI_REG_COUNT) {
      hw->si[r] = v;
    }
    return;
  case HWR_PIF:
    if (paddr >= PIF_RAM_BASE)
      hw_put_be32(hw->pif_ram + (paddr - PIF_RAM_BASE), v);
    return;
  case HWR_CART:
    return; /* ROM is read-only; no SRAM/flash on this cartridge */
  default:
    hw->unmapped_accesses++;
    return;
  }
}

void hw_run_events(HwState *hw, uint64_t now) {
  hw->now = now;
  while (hw->next_event <= now) {
    if (hw->next_vi <= now) {
      raise_mi(hw, MI_INTR_VI);
      hw->vi_count++;
      hw->next_vi += HW_VI_PERIOD;
    }
    if (hw->sp_done_at <= now)
      sp_finish_task(hw);
    recompute_next_event(hw);
  }
}
