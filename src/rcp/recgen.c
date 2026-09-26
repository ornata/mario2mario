/* recgen: compile a text input script into a .rec stream (input.h).
 *
 *   recgen SCRIPT.txt OUT.rec
 *
 * Script lines: `<polls> [BUTTON ...] [stick X Y]`, one record per poll
 * (the game polls once per frame, 30 per second). '#' starts a comment.
 * Buttons: A B Z START DU DD DL DR L R CU CD CL CR ("-" for none). The
 * stick is in controller units (-128..127, full tilt ~ +-80). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/rcp/input.h"

static const struct {
  const char *name;
  uint16_t bit;
} buttons[] = {
    {"A", BTN_A},   {"B", BTN_B},   {"Z", BTN_Z},   {"START", BTN_START},
    {"DU", BTN_DU}, {"DD", BTN_DD}, {"DL", BTN_DL}, {"DR", BTN_DR},
    {"L", BTN_L},   {"R", BTN_R},   {"CU", BTN_CU}, {"CD", BTN_CD},
    {"CL", BTN_CL}, {"CR", BTN_CR},
};

static const char *resolve(const char *path, char *buf, size_t cap) {
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (path[0] == '/' || !wd || !*wd)
    return path;
  snprintf(buf, cap, "%s/%s", wd, path);
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s SCRIPT.txt OUT.rec\n", argv[0]);
    return 2;
  }
  char b1[4096], b2[4096];
  const char *in_path = resolve(argv[1], b1, sizeof(b1));
  const char *out_path = resolve(argv[2], b2, sizeof(b2));
  FILE *f = fopen(in_path, "r");
  if (!f) {
    perror(in_path);
    return 1;
  }
  Rec rec = {0};
  char line[1024];
  for (int lineno = 1; fgets(line, sizeof(line), f); lineno++) {
    char *hash = strchr(line, '#');
    if (hash)
      *hash = '\0';
    char *tok = strtok(line, " \t\r\n");
    if (!tok)
      continue;
    char *end;
    long polls = strtol(tok, &end, 10);
    if (*end || polls <= 0) {
      fprintf(stderr, "%s:%d: expected a poll count\n", in_path, lineno);
      return 1;
    }
    RecFrame fr = {0, 0, 0};
    while ((tok = strtok(NULL, " \t\r\n"))) {
      if (!strcmp(tok, "-"))
        continue;
      if (!strcmp(tok, "stick")) {
        char *x = strtok(NULL, " \t\r\n"), *y = strtok(NULL, " \t\r\n");
        if (!x || !y) {
          fprintf(stderr, "%s:%d: stick needs X Y\n", in_path, lineno);
          return 1;
        }
        fr.x = (int8_t)atoi(x);
        fr.y = (int8_t)atoi(y);
        continue;
      }
      size_t i = 0;
      while (i < sizeof(buttons) / sizeof(buttons[0]) &&
             strcmp(buttons[i].name, tok))
        i++;
      if (i == sizeof(buttons) / sizeof(buttons[0])) {
        fprintf(stderr, "%s:%d: unknown button %s\n", in_path, lineno, tok);
        return 1;
      }
      fr.buttons |= buttons[i].bit;
    }
    for (long i = 0; i < polls; i++)
      rec_push(&rec, fr);
  }
  fclose(f);
  if (!rec_save(&rec, out_path)) {
    perror(out_path);
    return 1;
  }
  printf("%u records -> %s\n", rec.n, out_path);
  rec_free(&rec);
  return 0;
}
