/* Actual write/rollback loop extracted by verify_cheat_writes.py; memory I/O is injected. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static void cleanup(void *p) { free(*(void **)p); }
#define CR_AUTOFREE __attribute__((cleanup(cleanup)))
#define ROUND_PG_DOWN(a) ((a) & ~(uintptr_t)0x3fff)
#define ROUND_PG_UP(a) (((a) + 0x3fff) & ~(uintptr_t)0x3fff)
typedef struct { intptr_t addr; uint8_t *old_bytes; size_t len; } applied_patch_t;
static uint8_t memory[3];
static int fault, writes, reads, allocations, order[16];
static void cr_log(const char *level, const char *tag, const char *fmt, ...) {
  (void)level; (void)tag; (void)fmt;
}
static int write_process_memory_forced(int pid, intptr_t addr, const void *data, size_t len) {
  (void)pid; assert(addr >= 0 && addr < 3 && len == 1 && writes < 16);
  order[writes++] = (int)addr;
  memcpy(memory + addr, data, len);
  if (writes == 2 && (fault == 1 || fault == 2)) {
    memory[addr] = 0xbb;
    if (fault == 2) return -1; /* partial modification followed by write failure */
  }
  return 0;
}
static int read_process_memory(int pid, intptr_t addr, void *out, size_t len) {
  (void)pid; reads++;
  if (fault == 3 && reads == 2) return -1;
  memcpy(out, memory + addr, len); return 0;
}
static void *test_malloc(size_t len) {
  if (++allocations == 2 && fault == 4) return NULL;
  return malloc(len);
}
#define malloc test_malloc
static void fmt_hex16(const uint8_t *data, size_t len, char *out, size_t cap) {
  (void)data; (void)len; snprintf(out, cap, "test");
}
static int kernel_mprotect(int pid, intptr_t addr, size_t len, int prot) {
  (void)pid; (void)addr; (void)len; (void)prot; return 0;
}
static void exercise(int injected_fault, int hook_codecave, int effective_on) {
  uint8_t old[] = {0x11, 0x22, 0x33}, desired[] = {0xaa, 0xaa, 0xaa};
  memcpy(memory, old, sizeof(memory));
  fault = injected_fault; writes = reads = allocations = 0;
  applied_patch_t applied[] = {{0, old, 1}, {1, old + 1, 1}, {2, old + 2, 1}};
  uint8_t *wi_data[] = {desired, desired + 1, desired + 2};
  int wi_is_cave[] = {0, 1, 0}, written_order[64] = {0};
  int rc = 0, write_attempt_n = 0, pre_n = 3, pid = 1, mod_index = 0;
  int restore_rx = 0, restore_orig_prot = 0;
  const char *title_id = "test";
  char err[256] = {0}; size_t err_size = sizeof(err);
#include "cheat_write_loop.inc"
  if (fault) {
    assert(rc != 0 && err[0]);
    assert(write_attempt_n == 2 && writes == 4);
    assert(!memcmp(memory, old, sizeof(memory)));
    assert(order[2] == order[1] && order[3] == order[0]);
  } else {
    assert(rc == 0 && write_attempt_n == 3 && writes == 3);
    assert(!memcmp(memory, desired, sizeof(memory)));
    if (hook_codecave && effective_on) assert(order[0] == 1 && order[1] == 0 && order[2] == 2);
    else if (hook_codecave) assert(order[0] == 0 && order[1] == 2 && order[2] == 1);
    else assert(order[0] == 0 && order[1] == 1 && order[2] == 2);
  }
}
int main(void) {
  for (int caves = 0; caves < 2; caves++)
    for (int on = 0; on < 2; on++)
      for (int mode = 0; mode < 5; mode++) exercise(mode, caves, on);
  puts("PASS: 20 write cases: success, mismatch, partial write failure, unreadable verification, allocation failure; enable/disable ordering and reverse rollback");
}
