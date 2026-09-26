/* Chunker structure on a small hand-built listing (no ROM needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools/chunk.h"
#include "src/tools/mips.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define BASE 0x80000000u

static const char *const program[] = {
    /* 0 */ "addiu $sp, $sp, -0x18",
    /* 1 */ "beq $a0, $zero, 0x80000018",
    /* 2 */ "nop",
    /* 3 */ "jal 0x80000100",
    /* 4 */ "nop",
    /* 5 */ "addiu $v0, $zero, 0x1",
    /* 6 */ "addiu $v1, $zero, 0x2",
    /* 7 */ "jr $ra",
    /* 8 */ "addiu $sp, $sp, 0x18",
    /* 9 */ ".word 0x7C000000",
    /* 10 */ "eret",
    /* 11 */ "beq $zero, $zero, 0x80000020",
    /* 12 */ "nop",
};
enum { N = sizeof(program) / sizeof(program[0]) };

/* Renders `program` exactly as //tools:listing would. */
static size_t make_listing(char *buf, size_t cap) {
  size_t len = 0;
  char text[MIPS_TEXT_MAX];
  for (uint32_t i = 0; i < N; i++) {
    uint32_t addr = BASE + 4 * i, w = 0;
    if (!mips_assemble(program[i], addr, &w)) {
      fprintf(stderr, "cannot assemble \"%s\"\n", program[i]);
      exit(1);
    }
    mips_format(w, mips_decode(w), addr, text);
    len += (size_t)snprintf(buf + len, cap - len, "%08X  %08X  %s\n", addr, w,
                            text);
  }
  return len;
}

static void check_block(const Chunks *c, uint32_t bi, uint32_t first,
                        uint32_t count, ExitKind exit) {
  CHECK(bi < c->block_count);
  if (bi >= c->block_count)
    return;
  const Block *b = &c->blocks[bi];
  if (b->first != first || b->count != count || b->exit != exit) {
    fprintf(stderr, "block %u: got [%u,+%u) %s, want [%u,+%u) %s\n", bi,
            b->first, b->count, exit_kind_names[b->exit], first, count,
            exit_kind_names[exit]);
    failures++;
  }
}

int main(void) {
  mips_init();
  char text[4096];
  size_t len = make_listing(text, sizeof(text));

  Listing l;
  char err[256];
  CHECK(listing_parse(text, len, &l, err, sizeof(err)));
  CHECK(l.n == N && l.base == BASE);

  Chunks c;
  chunk_build(&l, 4, &c);
  CHECK(c.block_count == 6);
  check_block(&c, 0, 0, 3, EXIT_BRANCH);
  check_block(&c, 1, 3, 2, EXIT_JAL);
  check_block(&c, 2, 5, 1, EXIT_FALLTHROUGH);
  check_block(&c, 3, 6, 3, EXIT_JR);
  check_block(&c, 4, 9, 2, EXIT_ERET);
  check_block(&c, 5, 11, 2, EXIT_BRANCH);
  CHECK(c.blocks[0].target == 0x80000018 && c.blocks[0].target_in_range);
  CHECK(c.blocks[1].target == 0x80000100 && !c.blocks[1].target_in_range);
  CHECK(c.blocks[4].data_words == 1);
  /* The delay slot at index 8 is a branch target but stays in block 3. */
  CHECK(l.targeted[6] && l.targeted[8] && !l.targeted[5]);

  /* target 4: soft 3, hard 5. */
  CHECK(c.unit_count == 3);
  if (c.unit_count == 3) {
    CHECK(c.units[0].first_block == 0 && c.units[0].block_count == 2);
    CHECK(c.units[1].first_block == 2 && c.units[1].block_count == 2);
    CHECK(c.units[2].first_block == 4 && c.units[2].block_count == 2);
  }

  FILE *f = tmpfile();
  chunks_write_jsonl(f, &l, &c);
  rewind(f);
  char line[2048];
  const char *want[] = {
      "\"entries\":[\"0x80000000\"],\"return_points\":[],"
      "\"external_targets\":[\"0x80000018\",\"0x80000100\"],\"jr_sites\":[]",
      "\"entries\":[\"0x80000014\",\"0x80000018\",\"0x80000020\"],"
      "\"return_points\":[\"0x80000014\"],\"external_targets\":[],"
      "\"jr_sites\":[\"0x8000001C\"]",
      "\"entries\":[\"0x80000024\"],\"return_points\":[],"
      "\"external_targets\":[\"0x80000020\"],\"jr_sites\":[]",
  };
  for (int i = 0; i < 3; i++) {
    if (!fgets(line, sizeof(line), f)) {
      CHECK(!"missing unit line");
      break;
    }
    if (!strstr(line, want[i])) {
      fprintf(stderr, "unit %d: %s  missing: %s\n", i, line, want[i]);
      failures++;
    }
  }
  CHECK(!fgets(line, sizeof(line), f));
  fclose(f);

  /* The listing parser rejects text that disagrees with the decoder. */
  char bad[] = "80000000  00000000  sll $zero, $zero, 0\n";
  Listing lb;
  CHECK(!listing_parse(bad, strlen(bad), &lb, err, sizeof(err)));

  chunks_free(&c);
  listing_free(&l);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
