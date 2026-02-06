// wasm_builder.rs - Wasm binary generation
//
// Converts the intermediate WasmModule to actual Wasm bytecode using wasm-encoder.

use crate::translate::{WasmInst, WasmModule};
use anyhow::Result;
use wasm_encoder::{
    CodeSection, ConstExpr, DataSection, EntityType, ExportKind, ExportSection, Function,
    FunctionSection, ImportSection, Instruction, MemoryType, Module, TableSection, TableType,
    TypeSection, ValType,
};

/// Offset in linear memory where the PC→index dispatch mapping table is stored.
/// Located right after the register file (x0-x31, 256 bytes).
const DISPATCH_MAP_OFFSET: u32 = 256;

/// Dispatch table metadata computed from block addresses.
/// Maps PC values to dense function indices via a byte array in linear memory.
struct DispatchTable {
    /// Minimum block address (used to compute relative offset)
    min_addr: u32,
    /// The mapping table bytes: table[i] = function index for half-word offset i,
    /// or `default_idx` if no block starts at that address.
    data: Vec<u8>,
    /// Number of block functions (also the default/invalid index for br_table)
    num_functions: u32,
}

/// Build the dispatch table mapping PC → dense function index.
///
/// RISC-V instructions are either 2 bytes (compressed) or 4 bytes, so all
/// block addresses are 2-byte aligned. We build a byte-indexed lookup table
/// where `table[(pc - min_addr) / 2]` gives the br_table case index for that PC.
/// Unmapped addresses map to `num_functions` which is the br_table default (halt).
fn build_dispatch_table(module: &WasmModule) -> DispatchTable {
    let n = module.functions.len() as u32;

    if module.functions.is_empty() {
        return DispatchTable {
            min_addr: 0,
            data: vec![],
            num_functions: 0,
        };
    }

    // Collect (block_addr, function_index) pairs and sort by address
    let mut addr_to_idx: Vec<(u64, u32)> = module
        .functions
        .iter()
        .enumerate()
        .map(|(idx, f)| (f.block_addr, idx as u32))
        .collect();
    addr_to_idx.sort_by_key(|&(addr, _)| addr);

    let min_addr = addr_to_idx.first().unwrap().0 as u32;
    let max_addr = addr_to_idx.last().unwrap().0 as u32;

    // Table size: one entry per 2-byte-aligned address in the range
    let table_size = ((max_addr - min_addr) / 2 + 1) as usize;

    // Initialize all entries to the default (invalid) index
    let default_idx = if n < 255 { n as u8 } else { 255 };
    let mut data = vec![default_idx; table_size];

    // Fill in the known block addresses
    for &(addr, idx) in &addr_to_idx {
        let slot = ((addr as u32 - min_addr) / 2) as usize;
        if slot < data.len() {
            data[slot] = idx as u8;
        }
    }

    DispatchTable {
        min_addr,
        data,
        num_functions: n,
    }
}

/// Build the final Wasm binary
pub fn build(module: &WasmModule) -> Result<Vec<u8>> {
    let mut wasm = Module::new();

    // Pre-compute the dispatch table so we can reference it during code generation
    let dispatch_table = build_dispatch_table(module);

    // ==========================================================================
    // Type section
    // ==========================================================================
    let mut types = TypeSection::new();

    // Type 0: Block function (param $m i32) (result i32)
    types.function(vec![ValType::I32], vec![ValType::I32]);

    // Type 1: Dispatch function (param $m i32, $pc i32) (result i32)
    types.function(vec![ValType::I32, ValType::I32], vec![ValType::I32]);

    // Type 2: Syscall handler (param $m i32, $pc i32) (result i32)
    types.function(vec![ValType::I32, ValType::I32], vec![ValType::I32]);

    wasm.section(&types);

    // ==========================================================================
    // Import section
    // ==========================================================================
    let mut imports = ImportSection::new();

    // Import memory from environment
    imports.import(
        "env",
        "memory",
        MemoryType {
            minimum: module.memory_pages as u64,
            maximum: Some((module.memory_pages * 4) as u64),
            memory64: false,
            shared: false,
        },
    );

    // Import syscall handler
    imports.import("env", "syscall", EntityType::Function(2));

    wasm.section(&imports);

    // ==========================================================================
    // Function section (declare function types)
    // ==========================================================================
    let mut functions = FunctionSection::new();

    // Dispatch function (index 1 after import)
    functions.function(1);

    // Block functions (type 0)
    for _ in &module.functions {
        functions.function(0);
    }

    wasm.section(&functions);

    // ==========================================================================
    // Table section (for indirect calls)
    // ==========================================================================
    let mut tables = TableSection::new();

    // Table for block dispatch
    tables.table(TableType {
        element_type: wasm_encoder::RefType::FUNCREF,
        minimum: module.functions.len() as u32,
        maximum: Some(module.functions.len() as u32),
    });

    wasm.section(&tables);

    // ==========================================================================
    // Memory section (if not imported)
    // ==========================================================================
    // Memory is imported, so skip this

    // ==========================================================================
    // Export section
    // ==========================================================================
    let mut exports = ExportSection::new();

    // Export dispatch function
    exports.export("run", ExportKind::Func, 1);

    // Export individual block functions for debugging
    for (idx, func) in module.functions.iter().enumerate() {
        exports.export(&func.name, ExportKind::Func, (idx + 2) as u32);
    }

    wasm.section(&exports);

    // ==========================================================================
    // Code section
    // ==========================================================================
    let mut codes = CodeSection::new();

    // Dispatch function (uses br_table for O(1) block dispatch)
    let dispatch_func = build_dispatch_function(module, &dispatch_table);
    codes.function(&dispatch_func);

    // Block functions
    for func in &module.functions {
        let wasm_func = build_block_function(func)?;
        codes.function(&wasm_func);
    }

    wasm.section(&codes);

    // ==========================================================================
    // Data section (dispatch mapping table)
    // ==========================================================================
    if !dispatch_table.data.is_empty() {
        let mut data_section = DataSection::new();
        // Active data segment: initialize memory at DISPATCH_MAP_OFFSET
        data_section.active(
            0, // memory index
            &ConstExpr::i32_const(DISPATCH_MAP_OFFSET as i32),
            dispatch_table.data.iter().copied(),
        );
        wasm.section(&data_section);
    }

    Ok(wasm.finish())
}

/// Build the main dispatch function using br_table for O(1) block dispatch.
///
/// The dispatch loop structure:
/// ```text
/// func $run (param $m i32) (param $start_pc i32) (result i32)
///   local $pc i32
///   $pc = $start_pc
///   loop $dispatch
///     if $pc == -1: return 0 (halt)
///     if $pc & 0x80000000: $pc = syscall($m, $pc); continue
///     ;; br_table dispatch:
///     block $default
///       block $case_{n-1}
///         ...
///           block $case_0
///             index = memory[DISPATCH_MAP_OFFSET + ($pc - min_addr) / 2]
///             br_table $case_0 $case_1 ... $case_{n-1} $default
///           end
///           $pc = call block_func_0($m)
///           br $dispatch
///         end
///         $pc = call block_func_1($m)
///         br $dispatch
///       ...
///     end
///     return -1 (unknown PC)
///   end
/// ```
fn build_dispatch_function(module: &WasmModule, table: &DispatchTable) -> Function {
    let mut func = Function::new(vec![(1, ValType::I32)]); // 1 local: $pc (local 2)
    let n = table.num_functions;

    // Locals: param $m=0, param $start_pc=1, local $pc=2

    // Initialize $pc from $start_pc parameter
    func.instruction(&Instruction::LocalGet(1));
    func.instruction(&Instruction::LocalSet(2));

    // --- Main dispatch loop ---
    func.instruction(&Instruction::Loop(wasm_encoder::BlockType::Empty));

    // Check for halt: if ($pc == -1) return 0
    func.instruction(&Instruction::LocalGet(2));
    func.instruction(&Instruction::I32Const(-1));
    func.instruction(&Instruction::I32Eq);
    func.instruction(&Instruction::If(wasm_encoder::BlockType::Empty));
    func.instruction(&Instruction::I32Const(0));
    func.instruction(&Instruction::Return);
    func.instruction(&Instruction::End); // end if

    // Check for syscall: if ($pc & 0x80000000) { $pc = syscall($m, $pc); continue }
    func.instruction(&Instruction::LocalGet(2));
    func.instruction(&Instruction::I32Const(0x80000000u32 as i32));
    func.instruction(&Instruction::I32And);
    func.instruction(&Instruction::If(wasm_encoder::BlockType::Empty));
    func.instruction(&Instruction::LocalGet(0)); // $m
    func.instruction(&Instruction::LocalGet(2)); // $pc (with syscall flag)
    func.instruction(&Instruction::Call(0));      // call imported syscall handler
    func.instruction(&Instruction::LocalSet(2));  // $pc = result
    func.instruction(&Instruction::Br(1));        // br $dispatch (loop is depth 1)
    func.instruction(&Instruction::End); // end if

    if n == 0 {
        // No block functions - just halt
        func.instruction(&Instruction::I32Const(-1));
        func.instruction(&Instruction::Return);
    } else {
        // --- br_table dispatch ---
        //
        // Nesting (from innermost):
        //   depth 0: $case_0
        //   depth 1: $case_1
        //   ...
        //   depth n-1: $case_{n-1}
        //   depth n: $default
        //   depth n+1: $dispatch (loop)
        //
        // After $case_i ends, the case handler runs and branches to $dispatch.
        // The br depth to reach $dispatch from case i's handler is (n - i).

        // Open the $default block
        func.instruction(&Instruction::Block(wasm_encoder::BlockType::Empty));

        // Open N case blocks (innermost = $case_0)
        for _ in 0..n {
            func.instruction(&Instruction::Block(wasm_encoder::BlockType::Empty));
        }

        // --- Compute the br_table index from $pc ---
        // index = i32.load8_u(DISPATCH_MAP_OFFSET + ($pc - min_addr) / 2)
        func.instruction(&Instruction::LocalGet(2));                          // $pc
        func.instruction(&Instruction::I32Const(table.min_addr as i32));      // min_addr
        func.instruction(&Instruction::I32Sub);                               // $pc - min_addr
        func.instruction(&Instruction::I32Const(1));
        func.instruction(&Instruction::I32ShrU);                              // / 2 (half-word index)
        func.instruction(&Instruction::I32Const(DISPATCH_MAP_OFFSET as i32)); // add map base
        func.instruction(&Instruction::I32Add);

        // Load the function index from the mapping table
        func.instruction(&Instruction::I32Load8U(wasm_encoder::MemArg {
            offset: 0,
            align: 0,
            memory_index: 0,
        }));

        // br_table: labels[0]=$case_0 (depth 0), ..., labels[n-1]=$case_{n-1} (depth n-1)
        //           default=$default (depth n)
        let labels: Vec<u32> = (0..n).collect();
        func.instruction(&Instruction::BrTable(
            labels.as_slice().into(),
            n, // default label = $default block
        ));

        // Close the innermost block ($case_0) and emit case handlers
        for i in 0..n {
            func.instruction(&Instruction::End); // end $case_i block

            // Case i handler: $pc = call block_func_i($m)
            // Block functions start at function index 2 (0=syscall import, 1=dispatch)
            func.instruction(&Instruction::LocalGet(0));          // $m
            func.instruction(&Instruction::Call(i + 2));           // call block_func_i
            func.instruction(&Instruction::LocalSet(2));           // $pc = result
            func.instruction(&Instruction::Br(n - i));             // br $dispatch loop
        }

        // Close the $default block
        func.instruction(&Instruction::End); // end $default

        // Default handler: unknown PC address, halt
        func.instruction(&Instruction::I32Const(-1));
        func.instruction(&Instruction::Return);
    }

    // Close the dispatch loop
    func.instruction(&Instruction::End); // end loop

    // Unreachable return (loop always exits via return)
    func.instruction(&Instruction::I32Const(0));
    func.instruction(&Instruction::End); // end function

    func
}

/// Build a block function from our IR
fn build_block_function(func: &crate::translate::WasmFunction) -> Result<Function> {
    let mut wasm_func = Function::new(vec![(func.num_locals, ValType::I64)]);

    for inst in &func.body {
        emit_instruction(&mut wasm_func, inst)?;
    }

    wasm_func.instruction(&Instruction::End);

    Ok(wasm_func)
}

/// Emit a single instruction
fn emit_instruction(func: &mut Function, inst: &WasmInst) -> Result<()> {
    match inst {
        // Control flow
        WasmInst::Block { label: _ } => {
            func.instruction(&Instruction::Block(wasm_encoder::BlockType::Empty));
        }
        WasmInst::Loop { label: _ } => {
            func.instruction(&Instruction::Loop(wasm_encoder::BlockType::Empty));
        }
        WasmInst::End => {
            func.instruction(&Instruction::End);
        }
        WasmInst::Br { label } => {
            func.instruction(&Instruction::Br(*label));
        }
        WasmInst::BrIf { label } => {
            func.instruction(&Instruction::BrIf(*label));
        }
        WasmInst::Return => {
            func.instruction(&Instruction::Return);
        }
        WasmInst::Call { func_idx } => {
            func.instruction(&Instruction::Call(*func_idx));
        }

        // Locals
        WasmInst::LocalGet { idx } => {
            func.instruction(&Instruction::LocalGet(*idx));
        }
        WasmInst::LocalSet { idx } => {
            func.instruction(&Instruction::LocalSet(*idx));
        }
        WasmInst::LocalTee { idx } => {
            func.instruction(&Instruction::LocalTee(*idx));
        }

        // Constants
        WasmInst::I32Const { value } => {
            func.instruction(&Instruction::I32Const(*value));
        }
        WasmInst::I64Const { value } => {
            func.instruction(&Instruction::I64Const(*value));
        }

        // Memory loads
        WasmInst::I32Load { offset } => {
            func.instruction(&Instruction::I32Load(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 2,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load { offset } => {
            func.instruction(&Instruction::I64Load(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 3,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load8S { offset } => {
            func.instruction(&Instruction::I64Load8S(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 0,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load8U { offset } => {
            func.instruction(&Instruction::I64Load8U(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 0,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load16S { offset } => {
            func.instruction(&Instruction::I64Load16S(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 1,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load16U { offset } => {
            func.instruction(&Instruction::I64Load16U(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 1,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load32S { offset } => {
            func.instruction(&Instruction::I64Load32S(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 2,
                memory_index: 0,
            }));
        }
        WasmInst::I64Load32U { offset } => {
            func.instruction(&Instruction::I64Load32U(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 2,
                memory_index: 0,
            }));
        }

        // Memory stores
        WasmInst::I32Store { offset } => {
            func.instruction(&Instruction::I32Store(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 2,
                memory_index: 0,
            }));
        }
        WasmInst::I64Store { offset } => {
            func.instruction(&Instruction::I64Store(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 3,
                memory_index: 0,
            }));
        }
        WasmInst::I64Store8 { offset } => {
            func.instruction(&Instruction::I64Store8(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 0,
                memory_index: 0,
            }));
        }
        WasmInst::I64Store16 { offset } => {
            func.instruction(&Instruction::I64Store16(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 1,
                memory_index: 0,
            }));
        }
        WasmInst::I64Store32 { offset } => {
            func.instruction(&Instruction::I64Store32(wasm_encoder::MemArg {
                offset: *offset as u64,
                align: 2,
                memory_index: 0,
            }));
        }

        // i64 arithmetic
        WasmInst::I64Add => {
            func.instruction(&Instruction::I64Add);
        }
        WasmInst::I64Sub => {
            func.instruction(&Instruction::I64Sub);
        }
        WasmInst::I64Mul => {
            func.instruction(&Instruction::I64Mul);
        }
        WasmInst::I64DivS => {
            func.instruction(&Instruction::I64DivS);
        }
        WasmInst::I64DivU => {
            func.instruction(&Instruction::I64DivU);
        }
        WasmInst::I64RemS => {
            func.instruction(&Instruction::I64RemS);
        }
        WasmInst::I64RemU => {
            func.instruction(&Instruction::I64RemU);
        }
        WasmInst::I64And => {
            func.instruction(&Instruction::I64And);
        }
        WasmInst::I64Or => {
            func.instruction(&Instruction::I64Or);
        }
        WasmInst::I64Xor => {
            func.instruction(&Instruction::I64Xor);
        }
        WasmInst::I64Shl => {
            func.instruction(&Instruction::I64Shl);
        }
        WasmInst::I64ShrS => {
            func.instruction(&Instruction::I64ShrS);
        }
        WasmInst::I64ShrU => {
            func.instruction(&Instruction::I64ShrU);
        }
        WasmInst::I64Eqz => {
            func.instruction(&Instruction::I64Eqz);
        }
        WasmInst::I64Eq => {
            func.instruction(&Instruction::I64Eq);
        }
        WasmInst::I64Ne => {
            func.instruction(&Instruction::I64Ne);
        }
        WasmInst::I64LtS => {
            func.instruction(&Instruction::I64LtS);
        }
        WasmInst::I64LtU => {
            func.instruction(&Instruction::I64LtU);
        }
        WasmInst::I64GtS => {
            func.instruction(&Instruction::I64GtS);
        }
        WasmInst::I64GtU => {
            func.instruction(&Instruction::I64GtU);
        }
        WasmInst::I64LeS => {
            func.instruction(&Instruction::I64LeS);
        }
        WasmInst::I64LeU => {
            func.instruction(&Instruction::I64LeU);
        }
        WasmInst::I64GeS => {
            func.instruction(&Instruction::I64GeS);
        }
        WasmInst::I64GeU => {
            func.instruction(&Instruction::I64GeU);
        }

        // i32 arithmetic
        WasmInst::I32Add => {
            func.instruction(&Instruction::I32Add);
        }
        WasmInst::I32Sub => {
            func.instruction(&Instruction::I32Sub);
        }
        WasmInst::I32Eqz => {
            func.instruction(&Instruction::I32Eqz);
        }
        WasmInst::I32Eq => {
            func.instruction(&Instruction::I32Eq);
        }
        WasmInst::I32Ne => {
            func.instruction(&Instruction::I32Ne);
        }

        // Conversions
        WasmInst::I32WrapI64 => {
            func.instruction(&Instruction::I32WrapI64);
        }
        WasmInst::I64ExtendI32S => {
            func.instruction(&Instruction::I64ExtendI32S);
        }
        WasmInst::I64ExtendI32U => {
            func.instruction(&Instruction::I64ExtendI32U);
        }

        // Stack
        WasmInst::Drop => {
            func.instruction(&Instruction::Drop);
        }
        WasmInst::Select => {
            func.instruction(&Instruction::Select);
        }

        // Comments are no-ops
        WasmInst::Comment { .. } => {}

        // Unimplemented instructions
        _ => {
            // Skip unimplemented for now
        }
    }

    Ok(())
}
