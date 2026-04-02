mod lexer;
mod parser;
mod ast;
mod elf;
mod pe;
mod nvm;
mod llvm;
mod error;
mod typechecker;

use std::fs;
use std::env;
use std::process;
use std::collections::HashSet;
use std::path::Path;

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        eprintln!("Usage: {} <source.agn> [--llvm|--elf|--nvm-code|--novaria|--pe-asm]", args[0]);
        process::exit(1);
    }

    let source_file = &args[1];
    let source = match fs::read_to_string(source_file) {
        Ok(s) => s,
        Err(e) => {
            let err = error::CompileError::new(
                error::ErrorKind::ModuleError,
                format!("failed to read source file: {}", e),
                source_file.to_string(),
                1,
                1,
            );
            err.display();
            process::exit(1);
        }
    };

    let mut lexer = lexer::Lexer::new_with_file(&source, source_file);
    let tokens = lexer.tokenize();

    let mut parser = parser::Parser::new(tokens, source_file);
    let mut ast = match parser.parse() {
        Ok(ast) => ast,
        Err(e) => {
            e.display();
            process::exit(1);
        }
    };

    let source_dir = std::path::Path::new(source_file).parent().unwrap_or(std::path::Path::new("."));
    if let Err(e) = load_modules(&mut ast, source_dir, &mut std::collections::HashSet::new()) {
        e.display();
        process::exit(1);
    }

    let mut type_checker = typechecker::TypeChecker::new();
    if let Err(errors) = type_checker.check_program(&ast) {
        eprintln!("Type checking failed with {} error(s):", errors.len());
        type_checker.print_errors();
        process::exit(1);
    }

    let target = if args.len() > 2 {
        match args[2].as_str() {
            "--llvm"     => "llvm",
            "--elf"      => "elf",
            "--nvm-code" => "nvm-code",
            "--novaria"  => "novaria",
            "--pe-asm"   => "pe-asm",
            _ => {
                eprintln!("Unknown target: {}", args[2]);
                eprintln!("Valid targets: --llvm, --elf, --nvm-code, --novaria, --pe-asm");
                process::exit(1);
            }
        }
    } else {
        // LLVM is the default backend; ELF/PE are additional targets
        "llvm"
    };

    let stem = source_file
        .trim_end_matches(".agn")
        .trim_end_matches(".per");

    let output_file = match target {
        "nvm-code" => format!("{}.asm", stem),
        "novaria"  => format!("{}.bin", stem),
        "pe-asm"   => format!("{}.exe", stem),
        _          => stem.to_string(), // llvm / elf produce native binary
    };

    match target {
        "llvm"     => compile_llvm(&ast, &output_file),
        "novaria"  => compile_nvm(&ast, &output_file),
        "nvm-code" => compile_nvm_asm(&ast, &output_file),
        "elf"      => compile_elf(&ast, &output_file),
        "pe-asm"   => {
            let mut codegen = pe::CodeGen::new(target);
            let machine_code = codegen.generate(&ast);
            let mut pe_writer = pe::PEWriter::new();
            pe_writer.write(&output_file, &machine_code)
                .expect("Failed to write PE executable");
            println!("Compilation successful: {}", output_file);
        }
        _ => compile_pe_with_c(&ast, &output_file),
    }
}

fn load_modules(ast: &mut ast::Program, base_dir: &Path, loaded: &mut HashSet<String>) -> error::Result<()> {
    let imports = ast.imports.clone();

    for import in &imports {
        let module_name = import.path.clone();

        if loaded.contains(&module_name) {
            continue;
        }

        loaded.insert(module_name.clone());

        let module_filename = format!("{}.agn", module_name);
        let mut module_file = base_dir.join(&module_filename);

        if !module_file.exists() {
            let per_filename = format!("{}.per", module_name);
            module_file = base_dir.join(&per_filename);
        }

        if !module_file.exists() {
            module_file = Path::new("stdlib").join(format!("{}.agn", module_name));
        }

        if !module_file.exists() {
            module_file = Path::new("stdlib").join(format!("{}.per", module_name));
        }

        if !module_file.exists() {
            if let Ok(exe_path) = env::current_exe() {
                if let Some(exe_dir) = exe_path.parent() {
                    module_file = exe_dir.join("stdlib").join(format!("{}.agn", module_name));
                    if !module_file.exists() {
                        module_file = exe_dir.join("stdlib").join(format!("{}.per", module_name));
                    }
                }
            }
        }

        let module_source = match fs::read_to_string(&module_file) {
            Ok(s) => s,
            Err(_) => {
                return Err(error::CompileError::new(
                    error::ErrorKind::ModuleError,
                    format!("could not find module '{}'", module_name),
                    module_file.to_string_lossy().to_string(),
                    1,
                    1,
                ).with_source_line(format!("import \"{}\"", module_name)));
            }
        };

        let mut module_lexer = lexer::Lexer::new_with_file(&module_source, &module_file.to_string_lossy());
        let module_tokens = module_lexer.tokenize();
        let mut module_parser = parser::Parser::new(module_tokens, &module_file.to_string_lossy());
        let mut module_ast = module_parser.parse()?;

        load_modules(&mut module_ast, base_dir, loaded)?;

        for (mod_name, module) in module_ast.modules {
            ast.modules.insert(mod_name, module);
        }

        let module = ast::Module {
            name: module_name.clone(),
            functions: module_ast.functions,
        };

        ast.modules.insert(module_name, module);
    }

    Ok(())
}

fn compile_llvm(ast: &ast::Program, output_file: &str) {
    use std::io::Write;

    let mut ir_gen = llvm::LLVMIRGenerator::new();
    let ir_code = ir_gen.generate(ast);

    let ll_file = format!("{}.ll", output_file);
    let mut file = fs::File::create(&ll_file).expect("Failed to create .ll file");
    file.write_all(ir_code.as_bytes()).expect("Failed to write LLVM IR");

    let status = process::Command::new("clang")
        .arg("-o")
        .arg(output_file)
        .arg(&ll_file)
        .status();

    match status {
        Ok(s) if s.success() => {
            let _ = fs::remove_file(&ll_file);
            println!("Compilation successful: {}", output_file);
        }
        Ok(s) => {
            eprintln!("Clang failed with exit code: {:?}", s.code());
            eprintln!("LLVM IR kept at: {}", ll_file);
            process::exit(1);
        }
        Err(e) => {
            eprintln!("Failed to run clang: {}", e);
            eprintln!("Make sure clang is installed");
            eprintln!("LLVM IR kept at: {}", ll_file);
            process::exit(1);
        }
    }
}

fn compile_nvm(ast: &ast::Program, output_file: &str) {
    use std::io::Write;

    let mut nvm_gen = nvm::NVMCodeGen::new();
    let bytecode = nvm_gen.generate(ast);

    let mut file = fs::File::create(output_file).expect("Failed to create .bin file");
    file.write_all(&bytecode).expect("Failed to write NVM bytecode");
    println!("Compilation successful: {}", output_file);
}

fn compile_nvm_asm(ast: &ast::Program, output_file: &str) {
    use std::io::Write;

    let mut nvm_asm_gen = nvm::NVMAssemblyGenerator::new();
    let asm_code = nvm_asm_gen.generate(ast);

    let mut file = fs::File::create(output_file).expect("Failed to create .asm file");
    file.write_all(asm_code.as_bytes()).expect("Failed to write NVM assembly");
    println!("Compilation successful: {}", output_file);
}

fn compile_elf(ast: &ast::Program, output_file: &str) {
    use std::io::Write;

    let mut asm_gen = elf::AsmGenerator::new();
    let asm_code = asm_gen.generate(ast);

    let asm_file = format!("{}.s", output_file);
    let mut file = fs::File::create(&asm_file).expect("Failed to create .s file");
    file.write_all(asm_code.as_bytes()).expect("Failed to write assembly");

    let status = process::Command::new("gcc")
        .arg("-o")
        .arg(output_file)
        .arg(&asm_file)
        .arg("-no-pie")
        .status();

    match status {
        Ok(s) if s.success() => {
            let _ = fs::remove_file(&asm_file);
            println!("Compilation successful: {}", output_file);
        }
        Ok(s) => {
            eprintln!("GCC failed with exit code: {:?}", s.code());
            eprintln!("Assembly file kept at: {}", asm_file);
            process::exit(1);
        }
        Err(e) => {
            eprintln!("Failed to run gcc: {}", e);
            eprintln!("Make sure gcc is installed");
            eprintln!("Assembly file kept at: {}", asm_file);
            process::exit(1);
        }
    }
}

fn compile_pe_with_c(ast: &ast::Program, output_file: &str) {
    let mut c_gen = pe::c_codegen::CCodeGen::new();
    let c_code = c_gen.generate(ast).expect("Failed to generate C code");

    if let Err(e) = c_gen.compile_c_code(&c_code, output_file) {
        eprintln!("Failed to compile C code: {}", e);
        eprintln!("Make sure cl.exe is available (run from Developer Command Prompt)");
        process::exit(1);
    }
}
