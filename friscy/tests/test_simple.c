// test_simple.c - Minimal RISC-V test for rv2wasm
// Statically linked, uses only raw syscalls (no libc)

// RISC-V Linux syscall numbers
#define SYS_write 64
#define SYS_exit  93

static long syscall1(long n, long a0) {
    register long a7 __asm__("a7") = n;
    register long _a0 __asm__("a0") = a0;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(a7)
                     : "memory");
    return _a0;
}

static long syscall3(long n, long a0, long a1, long a2) {
    register long a7 __asm__("a7") = n;
    register long _a0 __asm__("a0") = a0;
    register long _a1 __asm__("a1") = a1;
    register long _a2 __asm__("a2") = a2;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(a7), "r"(_a1), "r"(_a2)
                     : "memory");
    return _a0;
}

static int my_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void write_str(const char *s) {
    syscall3(SYS_write, 1, (long)s, my_strlen(s));
}

// Simple computation: sum 1..10
static int sum_to_n(int n) {
    int result = 0;
    for (int i = 1; i <= n; i++) {
        result += i;
    }
    return result;
}

void _start(void) {
    write_str("rv2wasm test: hello from RISC-V!\n");

    int result = sum_to_n(10);

    // Expected: 55
    if (result == 55) {
        write_str("PASS: sum(1..10) = 55\n");
    } else {
        write_str("FAIL: sum(1..10) != 55\n");
    }

    // Simple arithmetic
    int a = 42;
    int b = 13;
    if (a + b == 55 && a - b == 29 && a * b == 546) {
        write_str("PASS: arithmetic checks\n");
    } else {
        write_str("FAIL: arithmetic checks\n");
    }

    write_str("rv2wasm test: done\n");
    syscall1(SYS_exit, 0);
}
