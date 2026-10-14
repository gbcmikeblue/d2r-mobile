// 信号上下文的寄存器访问：Apple 与 Linux/Android 的 ucontext 布局不同，这里统一成一组宏，
// 用到现场寄存器的地方（crash.c、diag.c）都经这组宏读写，源码本身保持平台中立。
// Register access on a signal context: Apple and Linux/Android lay out ucontext differently, so this
// header unifies it behind one set of macros. Everywhere that reads or writes live registers
// (crash.c, diag.c) goes through these, keeping the sources themselves platform-neutral.
//
//   Apple       ：uc_mcontext 是**指针**，寄存器在 __ss（aarch64：pc/lr/sp/fp/x[]）、ESR 在 __es
//   Linux/Android：uc_mcontext 是**结构体**，寄存器直接在 pc/sp 与 regs[]（x0..x30，x29=FP，x30=LR）
//
//   Apple        : uc_mcontext is a pointer; registers live in __ss (aarch64: pc/lr/sp/fp/x[]), ESR in __es
//   Linux/Android: uc_mcontext is a struct; registers live directly at pc/sp and in regs[] (x0..x30, x29=FP, x30=LR)
#ifndef DM_UCTX_H
#define DM_UCTX_H

#include <ucontext.h>
#include <stdint.h>

// 注意：这些宏必须是**左值**（crash.c / diag.c 会改 pc 让探针跳过），所以不能带转型。
// Note: these macros must be lvalues (crash.c / diag.c rewrite pc to step a probe past itself), so
// they must not carry a cast.
#if defined(__ANDROID__) || defined(__linux__)
#define DM_UC_PC(uc)   ((uc)->uc_mcontext.pc)
#define DM_UC_LR(uc)   ((uc)->uc_mcontext.regs[30])
#define DM_UC_SP(uc)   ((uc)->uc_mcontext.sp)
#define DM_UC_FP(uc)   ((uc)->uc_mcontext.regs[29])
#define DM_UC_X(uc, i) ((uc)->uc_mcontext.regs[i])
// Linux/Android 的 sigcontext 里没有 ESR，拿不到「读还是写」这一位；需要精确值时改从内核取。
// Linux/Android sigcontext carries no ESR, so the read/write bit is unavailable here; take it from
// the kernel if an exact value is ever needed.
#define DM_UC_HAVE_ESR 0
#else
#define DM_UC_PC(uc)   ((uc)->uc_mcontext->__ss.__pc)
#define DM_UC_LR(uc)   ((uc)->uc_mcontext->__ss.__lr)
#define DM_UC_SP(uc)   ((uc)->uc_mcontext->__ss.__sp)
#define DM_UC_FP(uc)   ((uc)->uc_mcontext->__ss.__fp)
#define DM_UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
#define DM_UC_HAVE_ESR 1
#endif

#endif // DM_UCTX_H
