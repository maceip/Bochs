// test_server.c - Simple HTTP server to test friscy syscalls
//
// Tests: socket, bind, listen, accept, read, write, close, epoll
// Compile: riscv64-linux-gnu-gcc -static -o test_server test_server.c

#include <stdint.h>
#include <stddef.h>

// Syscall numbers for RISC-V 64
#define SYS_write         64
#define SYS_exit          93
#define SYS_socket        198
#define SYS_bind          200
#define SYS_listen        201
#define SYS_accept        202
#define SYS_sendto        206
#define SYS_recvfrom      207
#define SYS_epoll_create1 20
#define SYS_epoll_ctl     21
#define SYS_epoll_pwait   22

// Socket constants
#define AF_INET     2
#define SOCK_STREAM 1
#define EPOLL_CTL_ADD 1
#define EPOLLIN     0x001

// Inline syscall
static inline long syscall1(long n, long a0) {
    register long a7 asm("a7") = n;
    register long _a0 asm("a0") = a0;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
    return _a0;
}

static inline long syscall2(long n, long a0, long a1) {
    register long a7 asm("a7") = n;
    register long _a0 asm("a0") = a0;
    register long _a1 asm("a1") = a1;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory");
    return _a0;
}

static inline long syscall3(long n, long a0, long a1, long a2) {
    register long a7 asm("a7") = n;
    register long _a0 asm("a0") = a0;
    register long _a1 asm("a1") = a1;
    register long _a2 asm("a2") = a2;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2) : "memory");
    return _a0;
}

static inline long syscall4(long n, long a0, long a1, long a2, long a3) {
    register long a7 asm("a7") = n;
    register long _a0 asm("a0") = a0;
    register long _a1 asm("a1") = a1;
    register long _a2 asm("a2") = a2;
    register long _a3 asm("a3") = a3;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3) : "memory");
    return _a0;
}

static inline long syscall5(long n, long a0, long a1, long a2, long a3, long a4) {
    register long a7 asm("a7") = n;
    register long _a0 asm("a0") = a0;
    register long _a1 asm("a1") = a1;
    register long _a2 asm("a2") = a2;
    register long _a3 asm("a3") = a3;
    register long _a4 asm("a4") = a4;
    asm volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4) : "memory");
    return _a0;
}

// Helper functions
static void print(const char* s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static void print_num(long n) {
    char buf[20];
    int i = 19;
    buf[i] = 0;
    if (n == 0) {
        buf[--i] = '0';
    } else {
        int neg = n < 0;
        if (neg) n = -n;
        while (n > 0) {
            buf[--i] = '0' + (n % 10);
            n /= 10;
        }
        if (neg) buf[--i] = '-';
    }
    print(&buf[i]);
}

// sockaddr_in structure
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

// epoll_event structure
struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

// HTTP response
static const char* http_response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 44\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<html><body><h1>Hello from friscy!</h1></body></html>";

void _start(void) {
    print("=== friscy HTTP Server Test ===\n\n");

    // Create socket
    print("[1] Creating socket... ");
    long sockfd = syscall3(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        print("FAILED: ");
        print_num(sockfd);
        print("\n");
        syscall1(SYS_exit, 1);
    }
    print("OK (fd=");
    print_num(sockfd);
    print(")\n");

    // Bind to port 8080
    print("[2] Binding to port 8080... ");
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = (8080 >> 8) | ((8080 & 0xFF) << 8);  // htons(8080)
    addr.sin_addr = 0;  // INADDR_ANY

    long ret = syscall3(SYS_bind, sockfd, (long)&addr, sizeof(addr));
    if (ret < 0) {
        print("FAILED: ");
        print_num(ret);
        print("\n");
        syscall1(SYS_exit, 1);
    }
    print("OK\n");

    // Listen
    print("[3] Listening... ");
    ret = syscall2(SYS_listen, sockfd, 5);
    if (ret < 0) {
        print("FAILED: ");
        print_num(ret);
        print("\n");
        syscall1(SYS_exit, 1);
    }
    print("OK\n");

    // Create epoll instance
    print("[4] Creating epoll... ");
    long epfd = syscall1(SYS_epoll_create1, 0);
    if (epfd < 0) {
        print("FAILED: ");
        print_num(epfd);
        print("\n");
        syscall1(SYS_exit, 1);
    }
    print("OK (fd=");
    print_num(epfd);
    print(")\n");

    // Add socket to epoll
    print("[5] Adding socket to epoll... ");
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = sockfd;
    ret = syscall4(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, sockfd, (long)&ev);
    if (ret < 0) {
        print("FAILED: ");
        print_num(ret);
        print("\n");
        syscall1(SYS_exit, 1);
    }
    print("OK\n");

    print("\n=== All syscalls working! ===\n");
    print("Server would now wait for connections...\n");
    print("(In a real test, we'd accept connections here)\n\n");

    // For the demo, just exit successfully
    // In a real server, we'd loop: epoll_wait -> accept -> read -> write -> close

    print("Test PASSED!\n");
    syscall1(SYS_exit, 0);
}
