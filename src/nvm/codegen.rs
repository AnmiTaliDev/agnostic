use crate::ast::*;
use std::collections::HashMap;
const PUSH32: u8 = 0x02;
const POP: u8 = 0x04;
const SWAP: u8 = 0x06;

const ADD: u8 = 0x10;
const SUB: u8 = 0x11;
const MUL: u8 = 0x12;
const DIV: u8 = 0x13;
const MOD: u8 = 0x14;

const EQ: u8 = 0x21;
const NEQ: u8 = 0x22;
const GT: u8 = 0x23;
const LT: u8 = 0x24;

const JMP32: u8 = 0x30;
const JZ32: u8 = 0x31;
const JNZ32: u8 = 0x32;
const CALL32: u8 = 0x33;
const RET: u8 = 0x34;

const LOAD: u8 = 0x40;
const STORE: u8 = 0x41;
const LOAD_ABS: u8 = 0x44;
const STORE_ABS: u8 = 0x45;

const SYSCALL: u8 = 0x50;

const SYSCALL_EXIT: u8 = 0x00;
const SYSCALL_EXEC: u8 = 0x01;
const SYSCALL_OPEN: u8 = 0x02;
const SYSCALL_READ: u8 = 0x03;
const SYSCALL_WRITE: u8 = 0x04;
const SYSCALL_CREATE: u8 = 0x05;
const SYSCALL_DELETE: u8 = 0x06;
const SYSCALL_CAP_CHECK: u8 = 0x07;
const SYSCALL_CAP_SPAWN: u8 = 0x08;
const SYSCALL_MSG_SEND: u8 = 0x0A;
const SYSCALL_MSG_RECEIVE: u8 = 0x0B;
const SYSCALL_PORT_IN_BYTE: u8 = 0x0C;
const SYSCALL_PORT_OUT_BYTE: u8 = 0x0D;
const SYSCALL_PRINT: u8 = 0x0E;

pub struct NVMCodeGen {
    bytecode: Vec<u8>,
    labels: HashMap<String, u32>,
    label_patches: Vec<(u32, String)>,
    local_vars: HashMap<String, u8>,
    next_local: u8,
    loop_stack: Vec<(String, String)>,
    current_function: String,
    string_literals: Vec<(String, String)>,
    compile_time_strings: HashMap<String, String>,
    vga_cursor: u32,
}

impl NVMCodeGen {
    pub fn new() -> Self {
        Self {
            bytecode: Vec::new(),
            labels: HashMap::new(),
            label_patches: Vec::new(),
            local_vars: HashMap::new(),
            next_local: 0,
            loop_stack: Vec::new(),
            current_function: String::new(),
            string_literals: Vec::new(),
            compile_time_strings: HashMap::new(),
            vga_cursor: 0xB8000 + (18 * 160),
        }
    }
    
    fn has_return_or_exit(&self, stmts: &[Statement]) -> bool {
        for stmt in stmts {
            match stmt {
                Statement::Return(_) => return true,
                Statement::InlineAsm { parts } => {
                    for part in parts {
                        if let crate::ast::AsmPart::Literal(s) = part {
                            if s.contains("syscall") && s.contains("exit") {
                                return true;
                            }
                        }
                    }
                }
                Statement::If { then_body, else_body, .. } => {
                    if self.has_return_or_exit(then_body) {
                        return true;
                    }
                    if let Some(else_stmts) = else_body {
                        if self.has_return_or_exit(else_stmts) {
                            return true;
                        }
                    }
                }
                Statement::For { body, .. } => {
                    if self.has_return_or_exit(body) {
                        return true;
                    }
                }
                _ => {}
            }
        }
        false
    }

    pub fn generate(&mut self, program: &Program) -> Vec<u8> {
        self.bytecode.extend_from_slice(&[b'N', b'V', b'M', b'0']);

        if let Some(main_func) = program.functions.iter().find(|f| f.name == "main") {
            self.generate_function(main_func, program);
        }

        for func in &program.functions {
            if func.name != "main" {
                self.generate_function(func, program);
            }
        }

        for (module_name, module) in &program.modules {
            if module_name == "stdio" {
                continue;
            }
            for func in &module.functions {
                if func.is_exported {
                    let full_name = format!("{}_{}", module.name, func.name);
                    self.generate_module_function(func, &full_name, program);
                }
            }
        }

        if program.modules.contains_key("stdio") {
            self.generate_print_int_vga_helper();
        }

        self.emit_string_literals();
        self.patch_labels();

        self.bytecode.clone()
    }

    fn generate_function(&mut self, func: &Function, program: &Program) {
        self.current_function = func.name.clone();
        self.local_vars.clear();
        self.compile_time_strings.clear();
        self.next_local = 0;

        let func_label = format!("func_{}", func.name);
        self.add_label(&func_label);

        for param in &func.params {
            self.local_vars.insert(param.name.clone(), self.next_local);
            self.next_local += 1;
        }

        for stmt in &func.body {
            self.generate_statement(stmt, program);
        }

        if func.name == "main" && !self.has_return_or_exit(&func.body) {
            self.emit_push32(0);
            self.emit_byte(SYSCALL);
            self.emit_byte(SYSCALL_EXIT);
        }
        
        self.emit_byte(RET);
    }

    fn generate_module_function(&mut self, func: &Function, full_name: &str, program: &Program) {
        self.current_function = full_name.to_string();
        self.local_vars.clear();
        self.next_local = 0;

        let func_label = format!("func_{}", full_name);
        self.add_label(&func_label);

        for param in &func.params {
            self.local_vars.insert(param.name.clone(), self.next_local);
            self.next_local += 1;
        }

        for stmt in &func.body {
            self.generate_statement(stmt, program);
        }

        self.emit_byte(RET);
    }

    fn generate_statement(&mut self, stmt: &Statement, program: &Program) {
        match stmt {
            Statement::VarDecl { name, var_type: _, value } => {
                if let Some(init_expr) = value {
                    if let Expression::String(s) = init_expr {
                        self.compile_time_strings.insert(name.clone(), s.clone());
                    }
                    self.generate_expression(init_expr, program);
                } else {
                    self.emit_push32(0);
                }
                
                let local_index = self.next_local;
                self.local_vars.insert(name.clone(), local_index);
                self.next_local += 1;
                
                self.emit_byte(STORE);
                self.emit_byte(local_index);
            }

            Statement::Assignment { name, value } => {
                self.generate_expression(value, program);
                
                if let Some(&local_index) = self.local_vars.get(name) {
                    self.emit_byte(STORE);
                    self.emit_byte(local_index);
                } else {
                    
                    panic!("Variable not found: {}", name);
                }
            }

            Statement::If { condition, then_body, else_body } => {
                self.generate_expression(condition, program);
                
                let else_label = self.generate_label("else");
                let end_label = self.generate_label("endif");
                
                self.emit_byte(JZ32);
                self.emit_label_ref(&else_label);
                
                for stmt in then_body {
                    self.generate_statement(stmt, program);
                }
                
                self.emit_byte(JMP32);
                self.emit_label_ref(&end_label);
                
                self.add_label(&else_label);
                
                if let Some(else_stmts) = else_body {
                    for stmt in else_stmts {
                        self.generate_statement(stmt, program);
                    }
                }
                
                self.add_label(&end_label);
            }

            Statement::For { init, condition, post, body } => {
                
                if let Some(init_stmt) = init {
                    self.generate_statement(init_stmt, program);
                }
                
                let loop_start = self.generate_label("for_start");
                let loop_end = self.generate_label("for_end");
                let loop_continue = self.generate_label("for_continue");
                
                self.loop_stack.push((loop_end.clone(), loop_continue.clone()));
                
                self.add_label(&loop_start);
                
                
                if let Some(cond) = condition {
                    self.generate_expression(cond, program);
                    self.emit_byte(JZ32);
                    self.emit_label_ref(&loop_end);
                }
                
                
                for stmt in body {
                    self.generate_statement(stmt, program);
                }
                
                self.add_label(&loop_continue);
                
                
                if let Some(post_stmt) = post {
                    self.generate_statement(post_stmt, program);
                }
                
                self.emit_byte(JMP32);
                self.emit_label_ref(&loop_start);
                
                self.add_label(&loop_end);
                self.loop_stack.pop();
            }

            Statement::Return(value) => {
                if let Some(expr) = value {
                    self.generate_expression(expr, program);
                }
                self.emit_byte(RET);
            }

            Statement::Expression(expr) => {
                self.generate_expression(expr, program);
                self.emit_byte(POP);
            }

            Statement::InlineAsm { parts } => {
                use crate::ast::AsmPart;
                
                let mut asm_text = String::new();
                for part in parts {
                    match part {
                        AsmPart::Literal(s) => {
                            asm_text.push_str(s);
                        }
                        AsmPart::Variable(var_name) => {
                            if let Some(string_value) = self.compile_time_strings.get(var_name) {
                                asm_text.push_str(string_value);
                                asm_text.push('\n');
                            } else if let Some(&local_index) = self.local_vars.get(var_name) {
                                asm_text.push_str(&format!("load {}\n", local_index));
                            } else {
                                eprintln!("Warning: Variable '{}' not found in asm block", var_name);
                            }
                        }
                    }
                }
                
                for line in asm_text.lines() {
                    let line = line.trim();
                    if line.is_empty() || line.starts_with(';') {
                        continue;
                    }
                    let code = if let Some(comment_pos) = line.find(';') {
                        line[..comment_pos].trim()
                    } else {
                        line
                    };
                    if !code.is_empty() {
                        self.emit_asm_instruction(code);
                    }
                }
            }

            Statement::PointerAssignment { target, value } => {
                self.generate_expression(target, program);
                self.generate_expression(value, program);
                self.emit_byte(STORE_ABS);
            }

            _ => {}
        }
    }

    fn generate_expression(&mut self, expr: &Expression, program: &Program) {
        match expr {
            Expression::Number(n) => {
                self.emit_push32(*n as i32);
            }

            Expression::String(s) => {
                let string_label = self.generate_label("str");
                self.string_literals.push((string_label.clone(), s.clone()));
                self.emit_push32(0);
                let patch_pos = self.bytecode.len() - 4;
                self.label_patches.push((patch_pos as u32, string_label));
            }

            Expression::TemplateString { parts } => {
                use crate::ast::TemplateStringPart;
                
                for part in parts {
                    match part {
                        TemplateStringPart::Literal(lit) => {
                            for ch in lit.as_bytes() {
                                self.emit_push32(*ch as i32);
                                self.emit_byte(SYSCALL);
                                self.emit_byte(SYSCALL_PRINT);
                            }
                        }
                        TemplateStringPart::Expression { expr, format: _ } => {
                            self.generate_expression(expr, program);
                            self.emit_byte(CALL32);
                            self.emit_label_ref("__print_int");
                        }
                    }
                }
                self.emit_push32(0);
            }

            Expression::Identifier(name) => {
                if let Some(&local_index) = self.local_vars.get(name) {
                    self.emit_byte(LOAD);
                    self.emit_byte(local_index);
                } else {
                    panic!("Variable not found: {}", name);
                }
            }

            Expression::Binary { op, left, right } => {
                self.generate_expression(left, program);
                self.generate_expression(right, program);
                
                match op {
                    BinaryOp::Add => self.emit_byte(ADD),
                    BinaryOp::Sub => self.emit_byte(SUB),
                    BinaryOp::Mul => self.emit_byte(MUL),
                    BinaryOp::Div => self.emit_byte(DIV),
                    BinaryOp::Mod => self.emit_byte(MOD),
                    BinaryOp::Equal => self.emit_byte(EQ),
                    BinaryOp::NotEqual => self.emit_byte(NEQ),
                    BinaryOp::Less => self.emit_byte(LT),
                    BinaryOp::Greater => self.emit_byte(GT),
                    BinaryOp::LessEqual => {
                        self.emit_byte(GT);
                        self.emit_push32(0);
                        self.emit_byte(EQ);
                    }
                    BinaryOp::GreaterEqual => {
                        self.emit_byte(LT);
                        self.emit_push32(0);
                        self.emit_byte(EQ);
                    }
                    _ => {}
                }
            }

            Expression::Unary { op, operand } => {
                self.generate_expression(operand, program);
                
                match op {
                    UnaryOp::Neg => {
                        self.emit_push32(0);
                        self.emit_byte(SWAP);
                        self.emit_byte(SUB);
                    }
                    UnaryOp::Not => {
                        self.emit_push32(0);
                        self.emit_byte(EQ);
                    }
                }
            }

            Expression::Call { function, args } => {
                for arg in args.iter().rev() {
                    self.generate_expression(arg, program);
                }
                
                let func_label = format!("func_{}", function);
                self.emit_byte(CALL32);
                self.emit_label_ref(&func_label);
            }

            Expression::ModuleCall { module, function, args } => {
                if module == "stdio" {
                    match function.as_str() {
                        "Print" => {
                            if !args.is_empty() {
                                if let Expression::String(s) = &args[0] {
                                    for ch in s.as_bytes() {
                                        self.emit_push32(*ch as i32);
                                        self.emit_byte(SYSCALL);
                                        self.emit_byte(SYSCALL_PRINT);
                                    }
                                    self.emit_push32(0);
                                    return;
                                } else {
                                    self.generate_expression(&args[0], program);
                                    self.emit_byte(CALL32);
                                    self.emit_label_ref("__print_int");
                                    self.emit_push32(0);
                                    return;
                                }
                            }
                        }
                        "Println" => {
                            if !args.is_empty() {
                                if let Expression::String(s) = &args[0] {
                                    for ch in s.as_bytes() {
                                        self.emit_push32(*ch as i32);
                                        self.emit_byte(SYSCALL);
                                        self.emit_byte(SYSCALL_PRINT);
                                    }
                                    self.emit_push32('\n' as i32);
                                    self.emit_byte(SYSCALL);
                                    self.emit_byte(SYSCALL_PRINT);
                                    self.emit_push32(0);
                                    return;
                                } else if let Expression::TemplateString { .. } = &args[0] {
                                    self.generate_expression(&args[0], program);
                                    self.emit_push32('\n' as i32);
                                    self.emit_byte(SYSCALL);
                                    self.emit_byte(SYSCALL_PRINT);
                                    return;
                                } else {
                                    self.generate_expression(&args[0], program);
                                    self.emit_byte(CALL32);
                                    self.emit_label_ref("__print_int");
                                    self.emit_push32('\n' as i32);
                                    self.emit_byte(SYSCALL);
                                    self.emit_byte(SYSCALL_PRINT);
                                    self.emit_push32(0);
                                    return;
                                }
                            }
                        }
                        _ => {}
                    }
                }

                if module == "novaria" {
                    match function.as_str() {
                        "FileCreateStr" => {
                            if args.len() >= 2 {
                                if let (Expression::String(filename), Expression::String(content)) = (&args[0], &args[1]) {
                                    self.emit_push32(content.len() as i32);
                                    let _content_label = self.generate_label("str_content");
                                    self.emit_push32(0);
                                    let _content_patch_pos = self.bytecode.len() - 4;
                                    let _filename_label = self.generate_label("str_filename");
                                    self.emit_push32(0);
                                    let _filename_patch_pos = self.bytecode.len() - 4;
                                    self.emit_byte(SYSCALL);
                                    self.emit_byte(SYSCALL_CREATE);
                                    let skip_label = self.generate_label("skip_strings");
                                    self.emit_byte(JMP32);
                                    self.emit_label_ref(&skip_label);
                                    let filename_pos = self.bytecode.len();
                                    for ch in filename.as_bytes() {
                                        self.emit_byte(*ch);
                                    }
                                    self.emit_byte(0);
                                    let content_pos = self.bytecode.len();
                                    for ch in content.as_bytes() {
                                        self.emit_byte(*ch);
                                    }
                                    self.emit_byte(0);
                                    let _filename_addr = (filename_pos + 0x100000) as i32;
                                    let _content_addr = (content_pos + 0x100000) as i32;
                                    self.add_label(&skip_label);
                                    self.emit_push32(0);
                                    return;
                                }
                            }
                        }
                        _ => {}
                    }
                    
                    for arg in args.iter().rev() {
                        self.generate_expression(arg, program);
                    }
                    match function.as_str() {
                        "Exit" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_EXIT);
                        }
                        "Exec" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_EXEC);
                        }
                        "FileRead" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_READ);
                        }
                        "FileWrite" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_WRITE);
                        }
                        "FileCreate" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_CREATE);
                        }
                        "FileDelete" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_DELETE);
                        }
                        "CapCheck" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_CAP_CHECK);
                        }
                        "CapSpawn" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_CAP_SPAWN);
                        }
                        "MsgSend" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_MSG_SEND);
                        }
                        "MsgReceive" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_MSG_RECEIVE);
                        }
                        "PortInByte" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_PORT_IN_BYTE);
                        }
                        "PortOutByte" => {
                            self.emit_byte(SYSCALL);
                            self.emit_byte(SYSCALL_PORT_OUT_BYTE);
                        }
                        "CAP_FS_READ" => {
                            self.emit_push32(1);
                        }
                        "CAP_FS_WRITE" => {
                            self.emit_push32(2);
                        }
                        "CAP_FS_CREATE" => {
                            self.emit_push32(4);
                        }
                        "CAP_FS_DELETE" => {
                            self.emit_push32(8);
                        }
                        "CAP_DRV_ACCESS" => {
                            self.emit_push32(16);
                        }
                        "CAP_CAPS_MGMT" => {
                            self.emit_push32(32);
                        }
                        "CAP_ALL" => {
                            self.emit_push32(65535);
                        }
                        _ => {
                            let func_label = format!("func_{}_{}", module, function);
                            self.emit_byte(CALL32);
                            self.emit_label_ref(&func_label);
                        }
                    }
                    return;
                }

                for arg in args.iter().rev() {
                    self.generate_expression(arg, program);
                }

                let func_label = format!("func_{}_{}", module, function);
                self.emit_byte(CALL32);
                self.emit_label_ref(&func_label);
            }

            Expression::AddressOf { .. } => {
                // AddressOf is not supported by the NVM instruction set
                self.emit_push32(0);
            }

            Expression::Deref { operand } => {
                self.generate_expression(operand, program);
                self.emit_byte(LOAD_ABS);
            }

            Expression::Eval { instruction } => {
                self.generate_expression(instruction, program);
                
                if let Expression::String(instr_str) = instruction.as_ref() {
                    self.emit_asm_instruction(instr_str.trim());
                } else {
                    eprintln!("Warning: eval() with non-literal string not fully supported yet");
                }
            }

            _ => {
                self.emit_push32(0);
            }
        }
    }

    fn emit_byte(&mut self, byte: u8) {
        self.bytecode.push(byte);
    }

    fn emit_push32(&mut self, value: i32) {
        self.emit_byte(PUSH32);
        let bytes = value.to_be_bytes();
        self.bytecode.extend_from_slice(&bytes);
    }
    
    fn emit_vga_char(&mut self, ch: u8, attr: u8) {
        self.emit_push32(self.vga_cursor as i32);
        self.emit_push32(((attr as u32) << 8 | ch as u32) as i32);
        self.emit_byte(STORE_ABS);
        self.vga_cursor += 2;
    }
    
    fn vga_newline(&mut self) {
        self.vga_cursor = ((self.vga_cursor - 0xB8000) / 160 + 1) * 160 + 0xB8000;
        self.vga_cursor += 160;
        if self.vga_cursor >= 0xB8FA0 {
            self.vga_cursor = 0xB8000 + (18 * 160);
        }
    }

    fn emit_asm_instruction(&mut self, line: &str) {
        let line = line.trim();
        if line.is_empty() {
            return;
        }
        
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.is_empty() {
            return;
        }

        let instr = parts[0].to_lowercase();
        match instr.as_str() {
            "push32" | "push" => {
                if parts.len() > 1 {
                    if let Ok(value) = parts[1].parse::<i32>() {
                        self.emit_push32(value);
                    }
                }
            }
            "pop" => self.emit_byte(POP),
            "add" => self.emit_byte(ADD),
            "sub" => self.emit_byte(SUB),
            "mul" => self.emit_byte(MUL),
            "div" => self.emit_byte(DIV),
            "mod" => self.emit_byte(MOD),
            "syscall" => {
                self.emit_byte(SYSCALL);
                if parts.len() > 1 {
                    let syscall_arg = parts[1];
                    if let Ok(value) = syscall_arg.parse::<u8>() {
                        self.emit_byte(value);
                    } else {
                        let syscall_num = match syscall_arg.to_lowercase().as_str() {
                            "exit" => SYSCALL_EXIT,
                            "exec" => SYSCALL_EXEC,
                            "read" => SYSCALL_READ,
                            "write" => SYSCALL_WRITE,
                            "create" => SYSCALL_CREATE,
                            "delete" => SYSCALL_DELETE,
                            "cap_check" => SYSCALL_CAP_CHECK,
                            "cap_spawn" => SYSCALL_CAP_SPAWN,
                            "msg_send" => SYSCALL_MSG_SEND,
                            "msg_receive" | "msg_recv" => SYSCALL_MSG_RECEIVE,
                            "inb" | "port_in_byte" => SYSCALL_PORT_IN_BYTE,
                            "outb" | "port_out_byte" => SYSCALL_PORT_OUT_BYTE,
                            "print" => SYSCALL_PRINT,
                            _ => {
                                eprintln!("Warning: Unknown syscall name '{}', defaulting to 0", syscall_arg);
                                0
                            }
                        };
                        self.emit_byte(syscall_num);
                    }
                } else {
                    eprintln!("Warning: syscall instruction without argument, defaulting to 0");
                    self.emit_byte(0);
                }
            }
            "ret" => self.emit_byte(RET),
            _ => {}
        }
    }

    fn emit_label_ref(&mut self, label: &str) {
        let pos = self.bytecode.len() as u32;
        self.label_patches.push((pos, label.to_string()));
        self.bytecode.extend_from_slice(&[0, 0, 0, 0]);
    }

    fn add_label(&mut self, label: &str) {
        let pos = self.bytecode.len() as u32;
        self.labels.insert(label.to_string(), pos);
    }

    fn generate_label(&self, prefix: &str) -> String {
        use std::sync::atomic::{AtomicU32, Ordering};
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        let count = COUNTER.fetch_add(1, Ordering::Relaxed);
        format!("{}_{}_{}", prefix, self.current_function, count)
    }

    fn patch_labels(&mut self) {
        for (pos, label) in &self.label_patches {
            if let Some(&target) = self.labels.get(label) {
                let bytes = target.to_be_bytes();
                let pos = *pos as usize;
                self.bytecode[pos..pos + 4].copy_from_slice(&bytes);
            } else {
                eprintln!("Warning: Unresolved label: {}", label);
            }
        }
    }

    fn emit_string_literals(&mut self) {
        let literals = self.string_literals.clone();
        for (label, content) in literals {
            self.add_label(&label);
            for ch in content.as_bytes() {
                self.emit_byte(*ch);
            }
            self.emit_byte(0);
        }
    }

    fn generate_print_int_vga_helper(&mut self) {
        self.add_label("__print_int");
        
        self.emit_byte(STORE);
        self.emit_byte(255);
        
        self.emit_byte(STORE);
        self.emit_byte(250);
        
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_push32(0);
        self.emit_byte(LT);
        
        let not_negative_label = self.generate_label("not_negative");
        self.emit_byte(JZ32);
        self.emit_label_ref(&not_negative_label);
        
        self.emit_push32('-' as i32);
        self.emit_byte(SYSCALL);
        self.emit_byte(SYSCALL_PRINT);
        
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_push32(0);
        self.emit_byte(SWAP);
        self.emit_byte(SUB);
        self.emit_byte(STORE);
        self.emit_byte(250);
        
        self.add_label(&not_negative_label);
        
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_push32(0);
        self.emit_byte(EQ);
        
        let not_zero = self.generate_label("not_zero");
        self.emit_byte(JZ32);
        self.emit_label_ref(&not_zero);
        
        self.emit_push32('0' as i32);
        self.emit_byte(SYSCALL);
        self.emit_byte(SYSCALL_PRINT);
        
        self.emit_byte(LOAD);
        self.emit_byte(255);
        self.emit_byte(RET);
        
        self.add_label(&not_zero);
        
        self.emit_push32(1);
        self.emit_byte(STORE);
        self.emit_byte(251);
        
        let find_power_loop = self.generate_label("find_power");
        let find_power_done = self.generate_label("find_power_done");
        
        self.add_label(&find_power_loop);
        
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_push32(10);
        self.emit_byte(MUL);
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_byte(GT);
        
        self.emit_byte(JNZ32);
        self.emit_label_ref(&find_power_done);
        
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_push32(10);
        self.emit_byte(MUL);
        self.emit_byte(STORE);
        self.emit_byte(251);
        
        self.emit_byte(JMP32);
        self.emit_label_ref(&find_power_loop);
        
        self.add_label(&find_power_done);
        
        let print_loop = self.generate_label("print_digit_loop");
        let print_done = self.generate_label("print_done");
        
        self.add_label(&print_loop);
        
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_push32(0);
        self.emit_byte(GT);
        
        self.emit_byte(JZ32);
        self.emit_label_ref(&print_done);
        
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_byte(DIV);
        
        self.emit_push32('0' as i32);
        self.emit_byte(ADD);
        self.emit_byte(SYSCALL);
        self.emit_byte(SYSCALL_PRINT);
        
        self.emit_byte(LOAD);
        self.emit_byte(250);
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_byte(MOD);
        self.emit_byte(STORE);
        self.emit_byte(250);
        
        self.emit_byte(LOAD);
        self.emit_byte(251);
        self.emit_push32(10);
        self.emit_byte(DIV);
        self.emit_byte(STORE);
        self.emit_byte(251);
        
        self.emit_byte(JMP32);
        self.emit_label_ref(&print_loop);
        
        self.add_label(&print_done);
        
        self.emit_byte(LOAD);
        self.emit_byte(255);
        self.emit_byte(RET);
    }
}
