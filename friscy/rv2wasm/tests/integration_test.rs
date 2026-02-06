// integration_test.rs - End-to-end test for rv2wasm
//
// Compiles a real RISC-V binary through the pipeline and validates the output Wasm.

use std::path::PathBuf;

fn test_elf_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("tests")
        .join("test_simple.elf")
}

#[test]
fn test_compile_simple_elf() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        eprintln!(
            "Skipping integration test: {} not found (run: riscv64-linux-gnu-gcc -static -nostdlib -o tests/test_simple.elf tests/test_simple.c)",
            elf_path.display()
        );
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");
    let wasm_bytes = rv2wasm::compile(&elf_data, 2, false).expect("Compilation failed");

    // Validate output is valid Wasm
    assert!(wasm_bytes.len() > 8, "Output too small to be valid Wasm");
    assert_eq!(&wasm_bytes[0..4], b"\0asm", "Invalid Wasm magic");
    assert_eq!(
        &wasm_bytes[4..8],
        &[1, 0, 0, 0],
        "Unexpected Wasm version"
    );

    eprintln!("Output Wasm size: {} bytes", wasm_bytes.len());
}

#[test]
fn test_validate_wasm_structure() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");
    let wasm_bytes = rv2wasm::compile(&elf_data, 2, false).expect("Compilation failed");

    // Use wasmparser to validate the Wasm binary
    let mut validator = wasmparser::Validator::new();
    match validator.validate_all(&wasm_bytes) {
        Ok(_) => eprintln!("Wasm validation passed"),
        Err(e) => panic!("Wasm validation failed: {}", e),
    }
}

#[test]
fn test_elf_parsing() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");
    let info = rv2wasm::elf::parse(&elf_data).expect("ELF parse failed");

    assert_eq!(info.entry, 0x10144, "Entry point mismatch");
    assert!(!info.is_pie, "Should not be PIE");
    assert!(info.interpreter.is_none(), "Static binary should have no interpreter");
    assert!(!info.segments.is_empty(), "Should have segments");
}

#[test]
fn test_disassembly() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");
    let info = rv2wasm::elf::parse(&elf_data).expect("ELF parse failed");
    let sections = rv2wasm::elf::extract_code_sections(&elf_data, &info).expect("Code extraction failed");

    assert!(!sections.is_empty(), "Should have code sections");

    let mut total_instructions = 0;
    for section in &sections {
        let instructions = rv2wasm::disasm::disassemble(section).expect("Disassembly failed");
        total_instructions += instructions.len();
    }

    assert!(total_instructions > 0, "Should have instructions");
    eprintln!("Total instructions: {}", total_instructions);
}

#[test]
fn test_cfg_construction() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");
    let info = rv2wasm::elf::parse(&elf_data).expect("ELF parse failed");
    let sections = rv2wasm::elf::extract_code_sections(&elf_data, &info).expect("Code extraction failed");

    let mut all_instructions = Vec::new();
    for section in &sections {
        let instructions = rv2wasm::disasm::disassemble(section).expect("Disassembly failed");
        all_instructions.extend(instructions);
    }

    let cfg = rv2wasm::cfg::build(&all_instructions, info.entry).expect("CFG build failed");

    assert!(!cfg.blocks.is_empty(), "Should have basic blocks");
    assert!(!cfg.functions.is_empty(), "Should have functions");
    assert_eq!(cfg.entry, 0x10144, "CFG entry point mismatch");

    eprintln!("Basic blocks: {}", cfg.blocks.len());
    eprintln!("Functions: {}", cfg.functions.len());
}

#[test]
fn test_debug_mode() {
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read test ELF");

    // Should compile with debug info without errors
    let wasm_bytes = rv2wasm::compile(&elf_data, 0, true).expect("Debug compilation failed");
    assert!(wasm_bytes.len() > 8, "Debug output too small");

    // Optimized compilation
    let wasm_opt = rv2wasm::compile(&elf_data, 2, false).expect("Optimized compilation failed");

    // Debug output is typically larger (has comments, but they compile out)
    eprintln!("Debug size: {}, Optimized size: {}", wasm_bytes.len(), wasm_opt.len());
}

// ===========================================================================
// Dynamic linking / PIE tests
// ===========================================================================

fn dynamic_elf_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("tests")
        .join("test_dynamic.elf")
}

#[test]
fn test_dynamic_elf_detection() {
    let elf_path = dynamic_elf_path();
    if !elf_path.exists() {
        eprintln!("Skipping: test_dynamic.elf not found");
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read dynamic ELF");
    let info = rv2wasm::elf::parse(&elf_data).expect("ELF parse failed");

    // Dynamic binary should be PIE
    assert!(info.is_pie, "Dynamic binary should be PIE (ET_DYN)");

    // Should have an interpreter
    assert!(
        info.interpreter.is_some(),
        "Dynamic binary should have PT_INTERP interpreter"
    );

    let interp = info.interpreter.as_ref().unwrap();
    eprintln!("Interpreter: {}", interp);

    // Should be a RISC-V dynamic linker
    assert!(
        interp.contains("riscv64") || interp.contains("ld-linux") || interp.contains("ld-musl"),
        "Interpreter '{}' doesn't look like a RISC-V dynamic linker",
        interp
    );

    // Should have loadable segments
    assert!(!info.segments.is_empty(), "Should have segments");
    eprintln!("Entry: 0x{:x}, Segments: {}", info.entry, info.segments.len());
}

#[test]
fn test_dynamic_elf_compilation() {
    let elf_path = dynamic_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read dynamic ELF");
    let wasm_bytes = rv2wasm::compile(&elf_data, 2, false).expect("Dynamic ELF compilation failed");

    // Validate Wasm output
    assert!(wasm_bytes.len() > 8, "Output too small");
    assert_eq!(&wasm_bytes[0..4], b"\0asm", "Invalid Wasm magic");

    // Validate with wasmparser
    let mut validator = wasmparser::Validator::new();
    match validator.validate_all(&wasm_bytes) {
        Ok(_) => eprintln!("Dynamic ELF Wasm validation passed ({} bytes)", wasm_bytes.len()),
        Err(e) => panic!("Dynamic ELF Wasm validation failed: {}", e),
    }
}

#[test]
fn test_dynamic_elf_has_more_blocks() {
    let static_path = test_elf_path();
    let dynamic_path = dynamic_elf_path();
    if !static_path.exists() || !dynamic_path.exists() {
        return;
    }

    let static_data = std::fs::read(&static_path).unwrap();
    let dynamic_data = std::fs::read(&dynamic_path).unwrap();

    let static_wasm = rv2wasm::compile(&static_data, 2, false).unwrap();
    let dynamic_wasm = rv2wasm::compile(&dynamic_data, 2, false).unwrap();

    // Dynamic binary (with PLT/GOT stubs, libc startup) should produce more code
    assert!(
        dynamic_wasm.len() > static_wasm.len(),
        "Dynamic binary ({} bytes) should produce larger Wasm than static ({} bytes)",
        dynamic_wasm.len(),
        static_wasm.len()
    );

    eprintln!(
        "Static: {} bytes, Dynamic: {} bytes (ratio: {:.1}x)",
        static_wasm.len(),
        dynamic_wasm.len(),
        dynamic_wasm.len() as f64 / static_wasm.len() as f64
    );
}

#[test]
fn test_auxv_structure_requirements() {
    // Verify the aux vector entries required by ld-musl are available in the ELF info
    let elf_path = dynamic_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).expect("Failed to read dynamic ELF");
    let info = rv2wasm::elf::parse(&elf_data).expect("ELF parse failed");

    // For aux vector setup, we need:
    // AT_PHDR: phdr_vaddr must be non-zero (program headers location)
    assert!(info.phdr_vaddr != 0 || info.is_pie,
        "AT_PHDR: phdr_vaddr should be set (or PIE where it's relative)");

    // AT_PHNUM: must match the ELF header
    assert!(info.phdr_count > 0, "AT_PHNUM: must have program headers");

    // AT_ENTRY: entry point must be set
    assert!(info.entry != 0, "AT_ENTRY: entry point must be non-zero");

    // For dynamic binaries, interpreter info is critical
    if info.is_pie {
        let interp = info.interpreter.as_ref().expect("Dynamic binary needs interpreter");

        // ld-musl uses AT_BASE to find itself in memory
        // Verify the interpreter path is plausible
        assert!(
            interp.starts_with("/lib") || interp.starts_with("/usr/lib"),
            "Interpreter path '{}' should be in /lib or /usr/lib",
            interp
        );
    }

    eprintln!("Aux vector prerequisites satisfied:");
    eprintln!("  AT_PHDR:  0x{:x}", info.phdr_vaddr);
    eprintln!("  AT_PHNUM: {}", info.phdr_count);
    eprintln!("  AT_ENTRY: 0x{:x}", info.entry);
    if let Some(ref interp) = info.interpreter {
        eprintln!("  Interpreter: {}", interp);
    }
}

// ===========================================================================
// br_table dispatch tests
// ===========================================================================

#[test]
fn test_br_table_dispatch_produces_valid_wasm() {
    // Compile both static and dynamic binaries and verify the br_table-based
    // dispatch function produces valid Wasm in both cases.
    for (name, path) in &[
        ("static", test_elf_path()),
        ("dynamic", dynamic_elf_path()),
    ] {
        if !path.exists() {
            continue;
        }
        let elf_data = std::fs::read(path).unwrap();
        let wasm_bytes = rv2wasm::compile(&elf_data, 2, false)
            .unwrap_or_else(|e| panic!("{} binary: br_table compilation failed: {}", name, e));

        let mut validator = wasmparser::Validator::new();
        validator
            .validate_all(&wasm_bytes)
            .unwrap_or_else(|e| panic!("{} binary: br_table Wasm validation failed: {}", name, e));

        eprintln!("{}: br_table dispatch Wasm validated ({} bytes)", name, wasm_bytes.len());
    }
}

#[test]
fn test_wasm_has_data_section() {
    // The br_table dispatch requires a DataSection containing the PC→index mapping table.
    // Verify it exists in the output by checking the Wasm binary has a data section (id=11).
    let elf_path = test_elf_path();
    if !elf_path.exists() {
        return;
    }

    let elf_data = std::fs::read(&elf_path).unwrap();
    let wasm_bytes = rv2wasm::compile(&elf_data, 2, false).unwrap();

    // Parse the Wasm and look for sections
    let parser = wasmparser::Parser::new(0);
    let mut has_data_section = false;
    let mut has_code_section = false;

    for payload in parser.parse_all(&wasm_bytes) {
        match payload.unwrap() {
            wasmparser::Payload::DataSection(_) => has_data_section = true,
            wasmparser::Payload::CodeSectionStart { .. } => has_code_section = true,
            _ => {}
        }
    }

    assert!(has_code_section, "Should have a code section");
    assert!(has_data_section, "Should have a data section (dispatch mapping table)");
    eprintln!("Data section present: dispatch mapping table verified");
}
