// syscalls.hpp - Linux syscall emulation for RISC-V 64-bit
// Implements the minimum viable syscall set for container workloads
#pragma once

#include <libriscv/machine.hpp>
#include "vfs.hpp"
#include <ctime>
#include <cstring>
#include <random>
#include <iostream>

namespace syscalls {

using Machine = riscv::Machine<riscv::RISCV64>;

// RISC-V 64-bit syscall numbers (from Linux kernel)
namespace nr {
    constexpr int getcwd        = 17;
    constexpr int dup           = 23;
    constexpr int dup3          = 24;
    constexpr int fcntl         = 25;
    constexpr int ioctl         = 29;
    constexpr int mkdirat       = 34;
    constexpr int unlinkat      = 35;
    constexpr int linkat        = 37;
    constexpr int renameat      = 38;
    constexpr int symlinkat     = 36;
    constexpr int ftruncate     = 46;
    constexpr int faccessat     = 48;
    constexpr int chdir         = 49;
    constexpr int openat        = 56;
    constexpr int close         = 57;
    constexpr int pipe2         = 59;
    constexpr int getdents64    = 61;
    constexpr int lseek         = 62;
    constexpr int read          = 63;
    constexpr int write         = 64;
    constexpr int readv         = 65;
    constexpr int writev        = 66;
    constexpr int pread64       = 67;
    constexpr int pwrite64      = 68;
    constexpr int readlinkat    = 78;
    constexpr int newfstatat    = 79;
    constexpr int fstat         = 80;
    constexpr int exit          = 93;
    constexpr int exit_group    = 94;
    constexpr int set_tid_address = 96;
    constexpr int clock_gettime = 113;
    constexpr int sigaction     = 134;
    constexpr int sigprocmask   = 135;
    constexpr int getpid        = 172;
    constexpr int getppid       = 173;
    constexpr int getuid        = 174;
    constexpr int geteuid       = 175;
    constexpr int getgid        = 176;
    constexpr int getegid       = 177;
    constexpr int gettid        = 178;
    constexpr int sysinfo       = 179;
    constexpr int brk           = 214;
    constexpr int munmap        = 215;
    constexpr int mmap          = 222;
    constexpr int mprotect      = 226;
    constexpr int prlimit64     = 261;
    constexpr int getrandom     = 278;
    constexpr int rseq          = 293;
}

// Linux stat64 structure for RISC-V 64
struct linux_stat64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    int32_t  __unused[2];
};

// Linux timespec
struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// AT_* constants
constexpr int AT_FDCWD = -100;
constexpr int AT_EMPTY_PATH = 0x1000;
constexpr int AT_SYMLINK_NOFOLLOW = 0x100;

// O_* flags
constexpr int O_RDONLY = 0;
constexpr int O_WRONLY = 1;
constexpr int O_RDWR = 2;
constexpr int O_CREAT = 0100;
constexpr int O_EXCL = 0200;
constexpr int O_TRUNC = 01000;
constexpr int O_APPEND = 02000;
constexpr int O_DIRECTORY = 0200000;
constexpr int O_CLOEXEC = 02000000;

// Error codes
constexpr int64_t ENOENT = -2;
constexpr int64_t EBADF = -9;
constexpr int64_t EACCES = -13;
constexpr int64_t EEXIST = -17;
constexpr int64_t ENOTDIR = -20;
constexpr int64_t EISDIR = -21;
constexpr int64_t EINVAL = -22;
constexpr int64_t ENOSYS = -38;
constexpr int64_t ENOTSUP = -95;

class SyscallHandler {
public:
    SyscallHandler(vfs::VirtualFS& fs) : fs_(fs) {
        // Initialize random generator
        std::random_device rd;
        rng_.seed(rd());
    }

    // Install all syscall handlers on the machine
    void install(Machine& machine) {
        auto& fs = fs_;
        auto* self = this;

        // Exit
        machine.install_syscall_handler(nr::exit, [](Machine& m) {
            int code = m.sysarg(0);
            m.stop();
            m.set_result(code);
        });

        machine.install_syscall_handler(nr::exit_group, [](Machine& m) {
            int code = m.sysarg(0);
            m.stop();
            m.set_result(code);
        });

        // File operations
        machine.install_syscall_handler(nr::openat, [&fs](Machine& m) {
            int dirfd = m.template sysarg<int>(0);
            auto path_addr = m.sysarg(1);
            int flags = m.template sysarg<int>(2);
            // mode is arg 3, ignored for now

            std::string path;
            try {
                path = m.memory.memstring(path_addr);
            } catch (...) {
                m.set_result(EINVAL);
                return;
            }

            // Handle AT_FDCWD
            if (dirfd != AT_FDCWD) {
                // TODO: support other dirfds
                m.set_result(ENOTSUP);
                return;
            }

            int fd;
            if (flags & O_DIRECTORY) {
                fd = fs.opendir(path);
            } else {
                fd = fs.open(path, flags);
            }

            m.set_result(fd);
        });

        machine.install_syscall_handler(nr::close, [&fs](Machine& m) {
            int fd = m.template sysarg<int>(0);
            fs.close(fd);
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::read, [&fs](Machine& m) {
            int fd = m.template sysarg<int>(0);
            auto buf_addr = m.sysarg(1);
            size_t count = m.sysarg(2);

            if (fd == 0) {
                // stdin - return EOF for now
                m.set_result(0);
                return;
            }

            std::vector<uint8_t> buf(count);
            ssize_t n = fs.read(fd, buf.data(), count);

            if (n > 0) {
                m.memory.memcpy(buf_addr, buf.data(), n);
            }

            m.set_result(n);
        });

        machine.install_syscall_handler(nr::write, [](Machine& m) {
            int fd = m.template sysarg<int>(0);
            auto buf_addr = m.sysarg(1);
            size_t count = m.sysarg(2);

            if (fd == 1 || fd == 2) {
                // stdout/stderr
                try {
                    auto view = m.memory.memview(buf_addr, count);
                    if (fd == 1) {
                        std::cout.write(reinterpret_cast<const char*>(view.data()), count);
                        std::cout.flush();
                    } else {
                        std::cerr.write(reinterpret_cast<const char*>(view.data()), count);
                        std::cerr.flush();
                    }
                    m.set_result(count);
                } catch (...) {
                    m.set_result(EINVAL);
                }
                return;
            }

            m.set_result(EBADF);
        });

        machine.install_syscall_handler(nr::writev, [](Machine& m) {
            int fd = m.template sysarg<int>(0);
            auto iov_addr = m.sysarg(1);
            int iovcnt = m.template sysarg<int>(2);

            if (fd != 1 && fd != 2) {
                m.set_result(EBADF);
                return;
            }

            size_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                // struct iovec { void* iov_base; size_t iov_len; }
                uint64_t base = m.memory.template read<uint64_t>(iov_addr + i * 16);
                uint64_t len = m.memory.template read<uint64_t>(iov_addr + i * 16 + 8);

                if (len > 0) {
                    auto view = m.memory.memview(base, len);
                    if (fd == 1) {
                        std::cout.write(reinterpret_cast<const char*>(view.data()), len);
                    } else {
                        std::cerr.write(reinterpret_cast<const char*>(view.data()), len);
                    }
                    total += len;
                }
            }

            if (fd == 1) std::cout.flush();
            else std::cerr.flush();

            m.set_result(total);
        });

        machine.install_syscall_handler(nr::lseek, [&fs](Machine& m) {
            int fd = m.template sysarg<int>(0);
            int64_t offset = m.template sysarg<int64_t>(1);
            int whence = m.template sysarg<int>(2);

            m.set_result(fs.lseek(fd, offset, whence));
        });

        machine.install_syscall_handler(nr::getdents64, [&fs](Machine& m) {
            int fd = m.template sysarg<int>(0);
            auto buf_addr = m.sysarg(1);
            size_t count = m.sysarg(2);

            std::vector<uint8_t> buf(count);
            ssize_t n = fs.getdents64(fd, buf.data(), count);

            if (n > 0) {
                m.memory.memcpy(buf_addr, buf.data(), n);
            }

            m.set_result(n);
        });

        // Stat operations
        machine.install_syscall_handler(nr::newfstatat, [&fs](Machine& m) {
            int dirfd = m.template sysarg<int>(0);
            auto path_addr = m.sysarg(1);
            auto statbuf_addr = m.sysarg(2);
            int flags = m.template sysarg<int>(3);

            std::string path;
            if (flags & AT_EMPTY_PATH) {
                // stat the fd itself
                path = "";  // TODO: lookup fd path
                m.set_result(ENOTSUP);
                return;
            } else {
                try {
                    path = m.memory.memstring(path_addr);
                } catch (...) {
                    m.set_result(EINVAL);
                    return;
                }
            }

            if (dirfd != AT_FDCWD) {
                m.set_result(ENOTSUP);
                return;
            }

            vfs::Entry entry;
            bool ok;
            if (flags & AT_SYMLINK_NOFOLLOW) {
                ok = fs.lstat(path, entry);
            } else {
                ok = fs.stat(path, entry);
            }

            if (!ok) {
                m.set_result(ENOENT);
                return;
            }

            linux_stat64 st = {};
            st.st_dev = 1;
            st.st_ino = std::hash<std::string>{}(path);
            st.st_mode = static_cast<uint32_t>(entry.type) | entry.mode;
            st.st_nlink = entry.is_dir() ? 2 : 1;
            st.st_uid = entry.uid;
            st.st_gid = entry.gid;
            st.st_size = entry.size;
            st.st_blksize = 4096;
            st.st_blocks = (entry.size + 511) / 512;
            st.st_mtime_sec = entry.mtime;
            st.st_atime_sec = entry.mtime;
            st.st_ctime_sec = entry.mtime;

            m.memory.memcpy(statbuf_addr, &st, sizeof(st));
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::fstat, [&fs](Machine& m) {
            int fd = m.template sysarg<int>(0);
            auto statbuf_addr = m.sysarg(1);

            // Special handling for stdout/stderr
            if (fd == 1 || fd == 2) {
                linux_stat64 st = {};
                st.st_dev = 1;
                st.st_mode = 020666;  // Character device
                st.st_nlink = 1;
                st.st_blksize = 4096;
                m.memory.memcpy(statbuf_addr, &st, sizeof(st));
                m.set_result(0);
                return;
            }

            m.set_result(EBADF);
        });

        machine.install_syscall_handler(nr::readlinkat, [&fs](Machine& m) {
            int dirfd = m.template sysarg<int>(0);
            auto path_addr = m.sysarg(1);
            auto buf_addr = m.sysarg(2);
            size_t bufsiz = m.sysarg(3);

            std::string path;
            try {
                path = m.memory.memstring(path_addr);
            } catch (...) {
                m.set_result(EINVAL);
                return;
            }

            if (dirfd != AT_FDCWD) {
                m.set_result(ENOTSUP);
                return;
            }

            std::vector<char> buf(bufsiz);
            ssize_t n = fs.readlink(path, buf.data(), bufsiz);

            if (n > 0) {
                m.memory.memcpy(buf_addr, buf.data(), n);
            }

            m.set_result(n);
        });

        // Directory operations
        machine.install_syscall_handler(nr::getcwd, [&fs](Machine& m) {
            auto buf_addr = m.sysarg(0);
            size_t size = m.sysarg(1);

            std::string cwd = fs.getcwd();
            if (cwd.size() + 1 > size) {
                m.set_result(-34);  // ERANGE
                return;
            }

            m.memory.memcpy(buf_addr, cwd.c_str(), cwd.size() + 1);
            m.set_result(buf_addr);
        });

        machine.install_syscall_handler(nr::chdir, [&fs](Machine& m) {
            auto path_addr = m.sysarg(0);

            std::string path;
            try {
                path = m.memory.memstring(path_addr);
            } catch (...) {
                m.set_result(EINVAL);
                return;
            }

            if (fs.chdir(path)) {
                m.set_result(0);
            } else {
                m.set_result(ENOENT);
            }
        });

        // Process info
        machine.install_syscall_handler(nr::getpid, [](Machine& m) {
            m.set_result(1);  // Always PID 1 (init)
        });

        machine.install_syscall_handler(nr::getppid, [](Machine& m) {
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::gettid, [](Machine& m) {
            m.set_result(1);
        });

        machine.install_syscall_handler(nr::getuid, [](Machine& m) {
            m.set_result(0);  // root
        });

        machine.install_syscall_handler(nr::geteuid, [](Machine& m) {
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::getgid, [](Machine& m) {
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::getegid, [](Machine& m) {
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::set_tid_address, [](Machine& m) {
            m.set_result(1);  // Return TID
        });

        // Time
        machine.install_syscall_handler(nr::clock_gettime, [](Machine& m) {
            int clk_id = m.template sysarg<int>(0);
            auto tp_addr = m.sysarg(1);

            (void)clk_id;  // Treat all clocks the same for now

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);

            linux_timespec lts;
            lts.tv_sec = ts.tv_sec;
            lts.tv_nsec = ts.tv_nsec;

            m.memory.memcpy(tp_addr, &lts, sizeof(lts));
            m.set_result(0);
        });

        // Random
        machine.install_syscall_handler(nr::getrandom, [self](Machine& m) {
            auto buf_addr = m.sysarg(0);
            size_t count = m.sysarg(1);
            // flags ignored

            std::vector<uint8_t> buf(count);
            for (size_t i = 0; i < count; i++) {
                buf[i] = self->rng_() & 0xFF;
            }

            m.memory.memcpy(buf_addr, buf.data(), count);
            m.set_result(count);
        });

        // Memory management (mostly handled by libriscv, but we need stubs)
        machine.install_syscall_handler(nr::brk, [](Machine& m) {
            // libriscv handles brk internally via setup_linux_syscalls
            // This is a fallback
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::mmap, [](Machine& m) {
            // libriscv handles mmap via setup_native_memory
            m.set_result(-12);  // ENOMEM as fallback
        });

        machine.install_syscall_handler(nr::munmap, [](Machine& m) {
            m.set_result(0);  // Always succeed
        });

        machine.install_syscall_handler(nr::mprotect, [](Machine& m) {
            m.set_result(0);  // Always succeed
        });

        // Signals (stub - single-threaded, no real signals)
        machine.install_syscall_handler(nr::sigaction, [](Machine& m) {
            m.set_result(0);
        });

        machine.install_syscall_handler(nr::sigprocmask, [](Machine& m) {
            m.set_result(0);
        });

        // ioctl (minimal)
        machine.install_syscall_handler(nr::ioctl, [](Machine& m) {
            int fd = m.template sysarg<int>(0);
            unsigned long request = m.sysarg(1);

            // TIOCGWINSZ - get window size
            if (request == 0x5413 && (fd == 0 || fd == 1 || fd == 2)) {
                // struct winsize { rows, cols, xpixel, ypixel } - all uint16_t
                auto ws_addr = m.sysarg(2);
                uint16_t ws[4] = { 24, 80, 0, 0 };  // 24 rows, 80 cols
                m.memory.memcpy(ws_addr, ws, sizeof(ws));
                m.set_result(0);
                return;
            }

            m.set_result(ENOTSUP);
        });

        // fcntl (minimal)
        machine.install_syscall_handler(nr::fcntl, [](Machine& m) {
            int fd = m.template sysarg<int>(0);
            int cmd = m.template sysarg<int>(1);

            (void)fd;

            switch (cmd) {
                case 1:  // F_GETFD
                case 3:  // F_GETFL
                    m.set_result(0);
                    break;
                case 2:  // F_SETFD
                case 4:  // F_SETFL
                    m.set_result(0);
                    break;
                default:
                    m.set_result(EINVAL);
            }
        });

        // prlimit64 (resource limits)
        machine.install_syscall_handler(nr::prlimit64, [](Machine& m) {
            // Just return success, ignore limits
            m.set_result(0);
        });

        // rseq (restartable sequences) - not supported
        machine.install_syscall_handler(nr::rseq, [](Machine& m) {
            m.set_result(ENOSYS);
        });

        // faccessat
        machine.install_syscall_handler(nr::faccessat, [&fs](Machine& m) {
            int dirfd = m.template sysarg<int>(0);
            auto path_addr = m.sysarg(1);
            // mode and flags ignored

            if (dirfd != AT_FDCWD) {
                m.set_result(ENOTSUP);
                return;
            }

            std::string path;
            try {
                path = m.memory.memstring(path_addr);
            } catch (...) {
                m.set_result(EINVAL);
                return;
            }

            vfs::Entry entry;
            if (fs.stat(path, entry)) {
                m.set_result(0);
            } else {
                m.set_result(ENOENT);
            }
        });

        // dup operations
        machine.install_syscall_handler(nr::dup, [](Machine& m) {
            m.set_result(ENOSYS);
        });

        machine.install_syscall_handler(nr::dup3, [](Machine& m) {
            m.set_result(ENOSYS);
        });

        // pipe2
        machine.install_syscall_handler(nr::pipe2, [](Machine& m) {
            m.set_result(ENOSYS);
        });
    }

private:
    vfs::VirtualFS& fs_;
    std::mt19937 rng_;
};

}  // namespace syscalls
