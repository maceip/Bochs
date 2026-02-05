#include <libriscv/machine.hpp>
#include <iostream>
#include <vector>
#include <string_view>
#include <fstream>

using Machine = riscv::Machine<riscv::RISCV64>;
static constexpr uint64_t MAX_INSTRUCTIONS = 16'000'000'000ULL;
static constexpr uint32_t HEAP_SYSCALLS_BASE = 480;
static constexpr uint32_t MEMORY_SYSCALLS_BASE = 485;

int main(int argc, char** argv)
{
    // Load guest binary from file or embedded data
    std::vector<uint8_t> binary;

    if (argc > 1) {
        // Load from file path argument
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "Error: Could not open " << argv[1] << std::endl;
            return 1;
        }
        auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        binary.resize(size);
        file.read(reinterpret_cast<char*>(binary.data()), size);
    } else {
        std::cerr << "Usage: friscy <riscv64-elf-binary>" << std::endl;
        std::cerr << "  Cross-compile guest with: riscv64-linux-gnu-gcc -static -o guest guest.c" << std::endl;
        return 1;
    }

    try {
        Machine machine{binary};

        // Set up Linux syscall emulation
        machine.setup_linux_syscalls();

        // Set up heap and memory management syscalls
        const auto heap_area = machine.memory.mmap_allocate(32ULL << 20); // 32MB heap
        machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, 32ULL << 20);
        machine.setup_native_memory(MEMORY_SYSCALLS_BASE);

        // Route guest stdout/stderr to host
        machine.set_printer([](const auto&, const char* data, size_t len) {
            std::cout.write(data, len);
        });

        // Custom 9P syscall handler (syscall 500)
        machine.install_syscall_handler(500, [](Machine& m) {
            auto buffer_addr = m.sysarg(0);
            auto buffer_len  = m.template sysarg<uint32_t>(1);
            try {
                auto view = m.memory.memview(buffer_addr, buffer_len);
                std::cout << "[9P] " << std::string_view(
                    reinterpret_cast<const char*>(view.data()), buffer_len
                ) << std::endl;
                m.set_result(0);
            } catch (const std::exception& e) {
                std::cerr << "[9P] Memory error: " << e.what() << std::endl;
                m.set_result(-1);
            }
        });

        // Run the guest
        machine.simulate(MAX_INSTRUCTIONS);

        auto [instructions, _] = machine.get_counters();
        std::cout << "Instructions executed: " << instructions << std::endl;

    } catch (const riscv::MachineException& e) {
        std::cerr << "Machine exception: " << e.what()
                  << " (data: " << e.data() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
