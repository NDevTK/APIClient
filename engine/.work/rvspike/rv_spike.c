// rv_spike.c — DE-RISK: performant IN-EMULATOR per-instruction instrumentation
// (taint + control-flow forcing + memcpy snapshot/fork) on a real, Linux-capable
// full-system CPU emulator (mini-rv32ima, RV32IMA) that WE compile to wasm with
// our own emsdk. Mirrors the unicorn forced-exec spike, but the instrumentation
// is C-level inside the emulator (no JS-per-instruction round-trip) and the
// snapshot/fork is a memcpy of the flat CPU state + RAM — the #5/#7/#10 primitive.
//
// Guest RV32I program — a flag/login-gated endpoint analog (input is OPAQUE):
//   0x00 lui  x5, 0x80001       ; x5 = 0x80001000 (input address)
//   0x04 lw   x6, 0(x5)         ; x6 = [input]  -> TAINT SOURCE
//   0x08 addi x7, x0, 1
//   0x0C bne  x6, x7, +12       ; gate: if input != 1 skip the syscall  (TAINTED branch)
//   0x10 lui  x8, 0x11000       ; x8 = 0x11000000 (MMIO endpoint)
//   0x14 sw   x8, 0(x8)         ; GATED "syscall": MMIO store -> endpoint fires
//   0x18 ecall                  ; halt
// Concrete input = 0 -> branch taken -> gated store SKIPPED (sniffer view: nothing).
// Forcing the not-taken arm -> gated store RUNS -> endpoint surfaced (learned-not-live).
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <emscripten.h>

static long     g_instr_count = 0;   // proves the per-instruction in-emulator hook fired
static int      g_endpoint_fired = 0;
static uint32_t g_endpoint_addr = 0;

#define MINI_RV32_RAM_SIZE  0x2000u   // 8KB: code @0..0x1B, input @0x1000
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, trap)  { g_instr_count++; }   // genuine per-instruction in-emulator hook
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) \
    { if ((addy) == 0x11000000u) { g_endpoint_fired = 1; g_endpoint_addr = (addy); } }
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval)  { rval = 0; }
#include "mini-rv32ima.h"

#define RAM_BASE  0x80000000u
#define END_PC    (RAM_BASE + 0x18u)   // ecall
#define INPUT_ADDR 0x80001000u
#define INPUT_OFS  0x1000u

static const uint32_t guest[] = {
  0x800012B7u, // lui  x5, 0x80001
  0x0002A303u, // lw   x6, 0(x5)
  0x00100393u, // addi x7, x0, 1
  0x00731663u, // bne  x6, x7, +12
  0x11000437u, // lui  x8, 0x11000
  0x00842023u, // sw   x8, 0(x8)
  0x00000073u, // ecall
};

static uint8_t image[MINI_RV32_RAM_SIZE];
static char logbuf[8192]; static int logn = 0;
static void LG(const char* fmt, ...) {
  va_list a; va_start(a, fmt);
  if (logn < (int)sizeof(logbuf)-1) logn += vsnprintf(logbuf+logn, sizeof(logbuf)-logn, fmt, a);
  va_end(a);
}

static void load_guest(void) {
  memset(image, 0, sizeof(image));
  memcpy(image, guest, sizeof(guest));
  *(uint32_t*)(image + INPUT_OFS) = 0u;   // concrete opaque input = 0 (gate closed)
}

static int      op_(uint32_t ir)  { return ir & 0x7f; }
static int      rd_(uint32_t ir)  { return (ir >> 7)  & 0x1f; }
static int      rs1_(uint32_t ir) { return (ir >> 15) & 0x1f; }
static int      rs2_(uint32_t ir) { return (ir >> 20) & 0x1f; }
static int32_t  bimm_(uint32_t ir) {  // B-type immediate
  int32_t imm = (int32_t)(((ir>>31)<<12) | (((ir>>25)&0x3f)<<5) | (((ir>>8)&0xf)<<1) | (((ir>>7)&1)<<11));
  if (imm & 0x1000) imm |= (int32_t)0xffffe000;
  return imm;
}

struct Res { int concrete, forced, tainted; };

static struct Res run(int taint_on, int force_on) {
  struct MiniRV32IMAState st; memset(&st, 0, sizeof(st));
  st.pc = RAM_BASE; st.extraflags = 3;   // machine mode
  load_guest();

  uint8_t taintReg[32]; memset(taintReg, 0, sizeof(taintReg));
  int taintInput = taint_on ? 1 : 0;
  int concrete_ep = 0, forced_ep = 0, tainted_branches = 0;

  struct MiniRV32IMAState snap_st; static uint8_t snap_img[MINI_RV32_RAM_SIZE];
  int have_fork = 0; uint32_t fork_taken = 0, fork_fall = 0;

  // ---- concrete pass: in-emulator step, taint propagation, tainted-branch detect ----
  for (int n = 0; n < 1000 && st.pc != END_PC; n++) {
    uint32_t pc = st.pc;
    uint32_t ir = *(uint32_t*)(image + (pc - RAM_BASE));
    int o = op_(ir);
    if (o == 0x63) {  // BRANCH
      int r1 = rs1_(ir), r2 = rs2_(ir);
      int tc = taintReg[r1] || taintReg[r2];
      LG("branch@0x%x rs1=x%d(t%d) rs2=x%d(t%d) taintedCond=%d\n", pc, r1, taintReg[r1], r2, taintReg[r2], tc);
      if (tc) {
        tainted_branches++;
        if (force_on && !have_fork) {
          have_fork = 1;
          memcpy(&snap_st, &st, sizeof(st));        // SNAPSHOT cpu state ...
          memcpy(snap_img, image, sizeof(image));   // ... and RAM (the fork primitive)
          fork_taken = pc + (uint32_t)bimm_(ir);
          fork_fall  = pc + 4u;
        }
      }
    }
    g_endpoint_fired = 0;
    MiniRV32IMAStep(&st, image, 0, 0, 1);           // step exactly 1 instruction (POSTEXEC fires inside)
    if (g_endpoint_fired) concrete_ep = 1;

    // taint propagation for the executed instruction (sources unchanged by these forms)
    if (o == 0x03) {                                 // LOAD: taint rd iff loaded addr is the opaque input
      int32_t imm = (int32_t)(ir >> 20); if (imm & 0x800) imm |= (int32_t)0xfffff000;
      uint32_t addr = st.regs[rs1_(ir)] + (uint32_t)imm;
      taintReg[rd_(ir)] = (taintInput && addr == INPUT_ADDR) ? 1 : 0;
    } else if (o == 0x13) {                           // OP-IMM: taint = taint[rs1]
      taintReg[rd_(ir)] = taintReg[rs1_(ir)];
    } else if (o == 0x33) {                           // OP: taint = taint[rs1]||taint[rs2]
      taintReg[rd_(ir)] = taintReg[rs1_(ir)] || taintReg[rs2_(ir)];
    } else if (o == 0x37 || o == 0x17) {              // LUI/AUIPC: constant -> untainted
      taintReg[rd_(ir)] = 0;
    }
    taintReg[0] = 0;                                  // x0 is always zero/untainted
  }
  LG("concrete: endpoint=%d  (input=0 -> gate closed)\n", concrete_ep);

  // ---- forced pass: RESTORE the snapshot and drive BOTH arms ----
  if (have_fork) {
    uint32_t arms[2] = { fork_fall, fork_taken };
    const char* nm[2] = { "fall", "taken" };
    for (int a = 0; a < 2; a++) {
      memcpy(&st, &snap_st, sizeof(st));             // RESTORE cpu state ...
      memcpy(image, snap_img, sizeof(image));        // ... and RAM (isolated arm)
      st.pc = arms[a];
      g_endpoint_fired = 0;
      for (int n = 0; n < 1000 && st.pc != END_PC; n++) {
        MiniRV32IMAStep(&st, image, 0, 0, 1);
        if (g_endpoint_fired) break;
      }
      LG("forced arm %-5s from 0x%x -> endpoint=%d\n", nm[a], arms[a], g_endpoint_fired);
      if (g_endpoint_fired) forced_ep = 1;
    }
  }
  struct Res r = { concrete_ep, forced_ep, tainted_branches };
  return r;
}

EMSCRIPTEN_KEEPALIVE int run_spike(void) {
  logn = 0; logbuf[0] = 0; g_instr_count = 0;
  struct Res A = run(1, 1);  LG("A(taint+force): concrete=%d forced=%d tainted=%d\n", A.concrete, A.forced, A.tainted);
  struct Res B = run(0, 1);  LG("B(no-taint):    concrete=%d forced=%d tainted=%d\n", B.concrete, B.forced, B.tainted);
  struct Res C = run(1, 0);  LG("C(no-force):    concrete=%d forced=%d tainted=%d\n", C.concrete, C.forced, C.tainted);
  int pass = (A.forced == 1 && A.concrete == 0 && A.tainted == 1)
          && (B.tainted == 0 && B.forced == 0)
          && (C.tainted == 1 && C.forced == 0 && C.concrete == 0);
  LG("instr_hook_fired=%ld (per-instruction in-emulator POSTEXEC)\n", g_instr_count);
  LG("PASS=%d\n", pass);
  return pass;
}

EMSCRIPTEN_KEEPALIVE const char* get_log(void) { return logbuf; }
