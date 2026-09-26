/* Chunker: listing -> basic blocks -> ~N-instruction translation units.
 *
 * Purely structural. Block boundaries come only from the flow flags of
 * decoded words: a block starts at the listing start, at any branch or
 * jump target inside the listing, after a delay slot, and after eret; a
 * block ends with the delay slot of a branch/jump, with eret, or before
 * the next block start. A (branch, delay slot) pair is never split: a
 * branch target that lands on a delay slot is recorded as a `targeted`
 * address inside the block instead. Units are runs of whole blocks. */
#ifndef M2M_TOOLS_CHUNK_H
#define M2M_TOOLS_CHUNK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Instructions of one contiguous listing, structure-of-arrays. */
typedef struct {
  uint32_t n;
  uint32_t base; /* addr[0]; addr[i] == base + 4 * i */
  uint32_t *word;
  uint16_t *op;      /* mips_decode(word[i]) */
  uint8_t *targeted; /* 1 if some branch/jump in the listing targets i */
} Listing;

typedef enum {
  EXIT_FALLTHROUGH, /* next instruction starts another block */
  EXIT_BRANCH,      /* conditional PC-relative branch (incl. bc1*) */
  EXIT_JUMP,        /* j */
  EXIT_JAL,         /* jal */
  EXIT_JR,          /* jr */
  EXIT_JALR,        /* jalr */
  EXIT_ERET,        /* eret (no delay slot) */
  EXIT_END,         /* listing ends without a flow change */
  EXIT_COUNT
} ExitKind;

typedef struct {
  uint32_t first, count; /* instruction index range */
  uint32_t exit_at;      /* index of the flow instruction (if any) */
  uint32_t target;       /* branch/jump destination address */
  uint32_t data_words;   /* words in the block that are not instructions */
  uint8_t exit;          /* ExitKind */
  uint8_t flow;          /* MFLOW_* of the flow instruction */
  uint8_t target_in_range;
  uint8_t delay_slot_cti; /* delay slot itself decodes as a branch/jump */
} Block;

typedef struct {
  uint32_t first_block, block_count;
  uint32_t first, count; /* instruction index range */
} Unit;

typedef struct {
  Block *blocks;
  uint32_t block_count;
  Unit *units;
  uint32_t unit_count;
  uint32_t *unit_of; /* instruction index -> unit index */
} Chunks;

/* Parses listing text (`ADDR  HEXWORD  text` lines as //tools:listing
 * prints them). Each line's text must equal the decoder's rendering of
 * its word, and addresses must be contiguous. On error returns 0 and
 * writes a message to `err`. */
int listing_parse(const char *text, size_t len, Listing *out, char *err,
                  size_t err_cap);
void listing_free(Listing *l);

/* Splits into blocks and units of about `target` instructions. */
void chunk_build(const Listing *l, uint32_t target, Chunks *out);
void chunks_free(Chunks *c);

/* One JSON object per unit, one per line. */
void chunks_write_jsonl(FILE *f, const Listing *l, const Chunks *c);

extern const char *const exit_kind_names[EXIT_COUNT];

#endif
