/* Userspace-only fixture: deliver an actual #DE, then reproduce the Linux
 * x86-64 IDT stack layout. No VM, module loading, or privileged access. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

struct pt_regs {
    unsigned long r15, r14, r13, r12, bp, bx, r11, r10, r9, r8;
    unsigned long ax, cx, dx, si, di, orig_ax, ip, cs, flags, sp, ss;
};
extern void asm_exc_divide_error(void);
struct pt_regs *captured;

__attribute__((noinline, noreturn)) void panic(const char *reason)
{
    asm volatile("" : : "r"(reason) : "memory");
    _Exit(99);
}

__attribute__((noinline)) void die(const char *str, struct pt_regs *regs, long err)
{
    asm volatile("" : : "r"(str), "r"(regs), "r"(err) : "memory");
    panic(str);
}

__attribute__((noinline)) void exc_divide_error(struct pt_regs *regs)
{
    die("divide error", regs, 0);
    _Exit(99);
}

static void exception(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    greg_t *r = uc->uc_mcontext.gregs;
    struct pt_regs saved = {
        .r15 = r[REG_R15], .r14 = r[REG_R14], .r13 = r[REG_R13], .r12 = r[REG_R12],
        .bp = r[REG_RBP], .bx = r[REG_RBX], .r11 = r[REG_R11], .r10 = r[REG_R10],
        .r9 = r[REG_R9], .r8 = r[REG_R8], .ax = r[REG_RAX], .cx = r[REG_RCX],
        .dx = r[REG_RDX], .si = r[REG_RSI], .di = r[REG_RDI], .orig_ax = -1UL,
        .ip = r[REG_RIP], .cs = 0x10, .flags = r[REG_EFL], .sp = r[REG_RSP], .ss = 0x18,
    };
    (void)signal; (void)info;
    captured = (void *)((saved.sp & ~15UL) - sizeof(saved));
    *captured = saved;
    /* Only the fixture's own signal-return context is modified. The unwinder
     * under test has no write path and never performs this setup. */
    r[REG_RSP] = (greg_t)captured;
    r[REG_RIP] = (greg_t)asm_exc_divide_error;
}

__attribute__((noinline)) int divide_by_zero(int numerator)
{
    volatile int divisor = 0;
    volatile int marker = 1234;
    volatile int result = numerator / divisor; /* FAULT_LINE */
    return result + marker;
}

__attribute__((noinline)) int command(const char *path)
{
    volatile int request_id = 42;
    int result = divide_by_zero(5);
    return result + request_id + path[0];
}

int main(void)
{
    stack_t stack = {.ss_sp = malloc(SIGSTKSZ), .ss_size = SIGSTKSZ};
    struct sigaction action = {.sa_sigaction = exception, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    if (!stack.ss_sp || sigaltstack(&stack, NULL) || sigaction(SIGFPE, &action, NULL))
        return 1;
    return command("/fixture/file");
}
