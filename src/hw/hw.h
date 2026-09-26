/* N64 hardware model shared by the oracle interpreter and (later) the
 * native runtime: one flat HwState holding every memory and register
 * file as a plain array. Memories are kept in native N64 (big-endian)
 * byte order. Devices react to 32-bit physical register accesses.
 *
 * Timing follows src/hw/timebase.h. RSP tasks are HLE'd: the task is
 * recorded (type, pointers, display-list hash) and completes after a
 * fixed latency; no microcode runs. RDRAM configuration registers are
 * inert storage (IPL3 takes its warm-boot path, see oracle/boot.c). */
#ifndef M2M_HW_HW_H
#define M2M_HW_HW_H

#include <stddef.h>
#include <stdint.h>

#include "src/hw/timebase.h"

#define HW_RDRAM_SIZE   0x800000u /* 8 MB */
#define HW_SP_MEM_SIZE  0x2000u   /* DMEM 0x0000..0x0FFF, IMEM 0x1000.. */
#define HW_PIF_RAM_SIZE 64u
#define HW_EEPROM_SIZE  512u /* 4 Kbit */

/* Physical regions, one byte per 1 MB of the 512 MB physical space. */
typedef enum {
  HWR_UNMAPPED = 0,
  HWR_RDRAM,
  HWR_RDRAM_REGS,
  HWR_SP,
  HWR_DP,
  HWR_DPS,
  HWR_MI,
  HWR_VI,
  HWR_AI,
  HWR_PI,
  HWR_RI,
  HWR_SI,
  HWR_CART, /* 0x05000000..0x1FBFFFFF: ROM at 0x10000000 */
  HWR_PIF,
} HwRegion;

/* Register indices (offset / 4) within each block. */
enum {
  SP_MEM_ADDR,
  SP_DRAM_ADDR,
  SP_RD_LEN,
  SP_WR_LEN,
  SP_STATUS,
  SP_DMA_FULL,
  SP_DMA_BUSY,
  SP_SEMAPHORE,
  SP_REG_COUNT
};
enum {
  DPC_START,
  DPC_END,
  DPC_CURRENT,
  DPC_STATUS,
  DPC_CLOCK,
  DPC_BUFBUSY,
  DPC_PIPEBUSY,
  DPC_TMEM,
  DP_REG_COUNT
};
enum { MI_MODE, MI_VERSION, MI_INTR, MI_INTR_MASK, MI_REG_COUNT };
enum {
  VI_STATUS,
  VI_ORIGIN,
  VI_WIDTH,
  VI_V_INTR,
  VI_V_CURRENT,
  VI_BURST,
  VI_V_SYNC,
  VI_H_SYNC,
  VI_LEAP,
  VI_H_START,
  VI_V_START,
  VI_V_BURST,
  VI_X_SCALE,
  VI_Y_SCALE,
  VI_REG_COUNT
};
enum {
  AI_DRAM_ADDR,
  AI_LEN,
  AI_CONTROL,
  AI_STATUS,
  AI_DACRATE,
  AI_BITRATE,
  AI_REG_COUNT
};
enum {
  PI_DRAM_ADDR,
  PI_CART_ADDR,
  PI_RD_LEN,
  PI_WR_LEN,
  PI_STATUS,
  PI_BSD_DOM1_LAT,
  PI_BSD_DOM1_PWD,
  PI_BSD_DOM1_PGS,
  PI_BSD_DOM1_RLS,
  PI_BSD_DOM2_LAT,
  PI_BSD_DOM2_PWD,
  PI_BSD_DOM2_PGS,
  PI_BSD_DOM2_RLS,
  PI_REG_COUNT
};
enum {
  RI_MODE,
  RI_CONFIG,
  RI_CURRENT_LOAD,
  RI_SELECT,
  RI_REFRESH,
  RI_LATENCY,
  RI_RERROR,
  RI_WERROR,
  RI_REG_COUNT
};
enum {
  SI_DRAM_ADDR,
  SI_PIF_ADDR_RD64B,
  SI_R2,
  SI_R3,
  SI_PIF_ADDR_WR64B,
  SI_R5,
  SI_STATUS,
  SI_REG_COUNT
};

/* MI_INTR / MI_INTR_MASK bits. */
enum {
  MI_INTR_SP = 1u << 0,
  MI_INTR_SI = 1u << 1,
  MI_INTR_AI = 1u << 2,
  MI_INTR_VI = 1u << 3,
  MI_INTR_PI = 1u << 4,
  MI_INTR_DP = 1u << 5,
};

/* SP_STATUS read bits. */
enum {
  SPS_HALT = 1u << 0,
  SPS_BROKE = 1u << 1,
  SPS_INTR_BREAK = 1u << 6,
  SPS_SIG0 = 1u << 7, /* libultra: yield request */
  SPS_SIG1 = 1u << 8, /* libultra: yielded */
  SPS_SIG2 = 1u << 9, /* libultra: task done */
};

/* OSTask as libultra places it at DMEM 0xFC0 (16 big-endian words). */
#define HW_OSTASK_DMEM 0xFC0u
enum {
  OSTASK_TYPE,
  OSTASK_FLAGS,
  OSTASK_UCODE_BOOT,
  OSTASK_UCODE_BOOT_SIZE,
  OSTASK_UCODE,
  OSTASK_UCODE_SIZE,
  OSTASK_UCODE_DATA,
  OSTASK_UCODE_DATA_SIZE,
  OSTASK_DRAM_STACK,
  OSTASK_DRAM_STACK_SIZE,
  OSTASK_OUTPUT_BUFF,
  OSTASK_OUTPUT_BUFF_SIZE,
  OSTASK_DATA_PTR,
  OSTASK_DATA_SIZE,
  OSTASK_YIELD_DATA_PTR,
  OSTASK_YIELD_DATA_SIZE,
  OSTASK_WORDS
};
enum { OSTASK_M_GFXTASK = 1, OSTASK_M_AUDTASK = 2 };

/* Controller state the SI/PIF joybus model reports for a port. */
typedef struct {
  uint16_t buttons;
  int8_t stick_x, stick_y;
  uint8_t present;
} HwPad;

/* One PI DMA (cart -> RDRAM), for the trace. */
typedef struct {
  uint64_t icount;
  uint32_t rom_offset, ram_addr, length;
} HwDmaRecord;

/* One RSP task start. */
typedef struct {
  uint64_t icount;
  uint32_t task[OSTASK_WORDS]; /* OSTask words as read from DMEM */
  uint64_t data_hash;          /* hash of RDRAM[data_ptr, +data_size) */
  uint64_t dl_hash; /* full display-list stream hash, set by the RCP HLE
                       task hook for graphics tasks (0 if none) */
} HwTaskRecord;

typedef struct HwState HwState;

/* Optional runtime hooks (NULL when unused):
 * task_hook runs when an RSP task starts, after it is logged; the
 * graphics HLE walks the display list here and may set r->dl_hash.
 * pad_hook runs before the PIF answers a controller-read command for
 * `port`, so the runtime can fill hw->pad[port] (live input or a
 * recorded .rec stream). */
typedef void (*HwTaskHook)(void *user, HwState *hw, HwTaskRecord *r);
typedef void (*HwPadHook)(void *user, HwState *hw, unsigned port);

struct HwState {
  uint8_t rdram[HW_RDRAM_SIZE];
  uint8_t sp_mem[HW_SP_MEM_SIZE];
  uint8_t pif_ram[HW_PIF_RAM_SIZE];
  uint8_t eeprom[HW_EEPROM_SIZE];
  uint8_t region[512]; /* HwRegion per 1 MB of physical space */

  uint32_t sp[SP_REG_COUNT];
  uint32_t sp_pc;
  uint32_t dp[DP_REG_COUNT];
  uint32_t mi[MI_REG_COUNT];
  uint32_t vi[VI_REG_COUNT];
  uint32_t ai[AI_REG_COUNT];
  uint32_t pi[PI_REG_COUNT];
  uint32_t ri[RI_REG_COUNT];
  uint32_t si[SI_REG_COUNT];
  uint32_t rdram_regs[16];

  const uint8_t *rom; /* big-endian z64 image */
  uint32_t rom_size;

  HwPad pad[4];

  /* Time (retired instructions) and scheduled events. */
  uint64_t now;        /* icount, refreshed by the CPU before device access */
  uint64_t next_event; /* min of the event times below */
  uint64_t next_vi;
  uint64_t sp_done_at; /* UINT64_MAX when no task is running */

  /* Logs and counters (always on; cheap). */
  HwDmaRecord *dma_log;
  uint32_t dma_count, dma_cap;
  HwTaskRecord *task_log;
  uint32_t task_count, task_cap;
  uint64_t vi_count, si_reads, si_writes, ai_buffers, unmapped_accesses;

  HwTaskHook task_hook;
  HwPadHook pad_hook;
  void *hook_user;
};

/* Allocates nothing large inside: `hw` itself holds RDRAM (allocate it on
 * the heap). `rom` must stay mapped for the lifetime of `hw`. */
void hw_init(HwState *hw, const uint8_t *rom, uint32_t rom_size);
void hw_free(HwState *hw);

/* 32-bit physical access for anything outside RDRAM (RDRAM is accessed
 * directly by the engines). `paddr` must be word-aligned. */
uint32_t hw_read32(HwState *hw, uint32_t paddr);
void hw_write32(HwState *hw, uint32_t paddr, uint32_t value);

/* True if MI raises the CPU's IP2 line. */
static inline int hw_irq(const HwState *hw) {
  return (hw->mi[MI_INTR] & hw->mi[MI_INTR_MASK]) != 0;
}

/* Processes all events due at or before `now`. Call when
 * now >= hw->next_event. */
void hw_run_events(HwState *hw, uint64_t now);

/* Physical region of `paddr`. */
static inline HwRegion hw_region(const HwState *hw, uint32_t paddr) {
  return (HwRegion)hw->region[(paddr >> 20) & 0x1FF];
}

static inline uint32_t hw_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         (uint32_t)p[3];
}

static inline void hw_put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* Word-wise FNV-1a variant used for all state hashes (checkpoints and
 * task data): for each 64-bit word w, h = (h ^ w) * 0x100000001B3,
 * then h ^= h >> 32. Start value HW_HASH_INIT. */
#define HW_HASH_INIT 0xCBF29CE484222325ull
static inline uint64_t hw_hash_word(uint64_t h, uint64_t w) {
  h = (h ^ w) * 0x100000001B3ull;
  return h ^ (h >> 32);
}

/* Hash of a big-endian byte range, taken as 32-bit words (length rounded
 * down to a multiple of 4). */
uint64_t hw_hash_be_words(const uint8_t *p, uint32_t len);

#endif
