// guest.cpp -- Example RISC-V guest program
// Cross-compile with: riscv64-linux-gnu-gcc -static -O2 -o guest guest.cpp
#include <cstdio>

// 9P bridge syscall (custom syscall 500 handled by host)
static long ecall_9p(const char* buf, unsigned len) {
    register long a0 asm("a0") = (long)buf;
    register long a1 asm("a1") = (long)len;
    register long syscall_id asm("a7") = 500;
    asm volatile("ecall"
        : "+r"(a0)
        : "r"(a1), "r"(syscall_id)
        : "memory");
    return a0;
}

int main() {
    printf("Hello from RISC-V guest!\n");

    // Test 9P bridge
    const char* msg = "9P test message";
    long result = ecall_9p(msg, 15);
    printf("9P syscall returned: %ld\n", result);

    // Simple compute test
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
        sum += i;
    }
    printf("Compute test: sum(0..999) = %d\n", sum);

    return 0;
}
