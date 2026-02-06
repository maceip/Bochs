// test_dynamic.c - Dynamically-linked RISC-V test for rv2wasm
// Uses standard libc, will require ld-linux/ld-musl dynamic linker

#include <stdio.h>

int main(int argc, char *argv[]) {
    printf("rv2wasm dynamic test: hello from RISC-V!\n");

    int sum = 0;
    for (int i = 1; i <= 10; i++) {
        sum += i;
    }

    if (sum == 55) {
        printf("PASS: sum(1..10) = %d\n", sum);
    } else {
        printf("FAIL: sum(1..10) = %d\n", sum);
    }

    return 0;
}
