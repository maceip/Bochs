import initModule from './build/friscy.js';
import { readFileSync } from 'fs';
import { argv } from 'process';

async function run() {
    const guestPath = argv[2];
    if (!guestPath) {
        console.error("Usage: node test_node.js <riscv64-elf-binary>");
        console.error("  Cross-compile guest with: riscv64-linux-gnu-gcc -static -o guest guest.c");
        process.exit(1);
    }

    console.log(`Loading guest binary: ${guestPath}`);
    const guestBinary = readFileSync(guestPath);

    const Module = await initModule({
        print: (text) => console.log(text),
        printErr: (text) => console.error(text),
        // Mount the guest binary into Emscripten's virtual FS
        preRun: [(mod) => {
            mod.FS.writeFile('/guest.elf', new Uint8Array(guestBinary));
        }],
        // Pass the guest path as argv to main()
        arguments: ['/guest.elf'],
    });

    console.log("Execution complete.");
}

run().catch(err => {
    console.error("Runtime error:", err);
    process.exit(1);
});
