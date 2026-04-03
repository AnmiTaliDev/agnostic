use crate::ast::*;
use std::collections::HashMap;

pub struct AsmGenerator {
    output: String,
    label_counter: usize,
    string_literals: Vec<String>,
    variables: HashMap<String, i32>,
    stack_offset: i32,
}

impl AsmGenerator {
    pub fn new() -> Self {
        AsmGenerator {
            output: String::new(),
            label_counter: 0,
            string_literals: Vec::new(),
            variables: HashMap::new(),
            stack_offset: 0,
        }
    }

    fn next_label(&mut self) -> String {
        let label = format!(".L{}", self.label_counter);
        self.label_counter += 1;
        label
    }

    pub fn generate(&mut self, program: &Program) -> String {
        self.output.push_str("    .text\n");

        for (module_name, module) in &program.modules {
            if module_name == "stdio" {
                continue;
            }
            for func in &module.functions {
                if func.is_exported {
                    self.generate_module_function(module_name, func);
                }
            }
        }

        for func in &program.functions {
            if func.name != "main" {
                self.generate_user_function(func);
            }
        }

        if program.modules.contains_key("stdio") {
            self.generate_stdio_functions();
        }

        self.output.push_str("    .globl main\n");
        self.output.push_str("main:\n");

        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    subq    $64, %rsp\n");

        if let Some(main_func) = program.functions.iter().find(|f| f.name == "main") {
            for stmt in &main_func.body {
                self.generate_statement(stmt);
            }
        }

        self.output.push_str("    movl    $0, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n");

        if !self.string_literals.is_empty() {
            self.output.push_str("\n    .section .rodata\n");
            for (i, s) in self.string_literals.iter().enumerate() {
                self.output.push_str(&format!(".LS{}:\n", i));
                self.output.push_str(&format!("    .string \"{}\"\n", s));
            }
        }

        self.output.clone()
    }

    fn generate_stdio_functions(&mut self) {

        self.output.push_str("    .globl stdio_Println\n");
        self.output.push_str("stdio_Println:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movq    %rdi, %rsi\n");
        let idx1 = self.string_literals.len();
        self.string_literals.push("%ld\\n".to_string());
        self.output.push_str(&format!("    leaq    .LS{}(%rip), %rdi\n", idx1));
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    call    printf@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_Print\n");
        self.output.push_str("stdio_Print:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movq    %rdi, %rsi\n");
        let idx2 = self.string_literals.len();
        self.string_literals.push("%ld".to_string());
        self.output.push_str(&format!("    leaq    .LS{}(%rip), %rdi\n", idx2));
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    call    printf@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_PrintStr\n");
        self.output.push_str("stdio_PrintStr:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movq    %rdi, %rsi\n");
        let idx3 = self.string_literals.len();
        self.string_literals.push("%s".to_string());
        self.output.push_str(&format!("    leaq    .LS{}(%rip), %rdi\n", idx3));
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    call    printf@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_PrintlnStr\n");
        self.output.push_str("stdio_PrintlnStr:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movq    %rdi, %rsi\n");
        let idx4 = self.string_literals.len();
        self.string_literals.push("%s\\n".to_string());
        self.output.push_str(&format!("    leaq    .LS{}(%rip), %rdi\n", idx4));
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    call    printf@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_PrintChar\n");
        self.output.push_str("stdio_PrintChar:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movl    %edi, %edi\n");
        self.output.push_str("    call    putchar@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_ReadInt\n");
        self.output.push_str("stdio_ReadInt:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    subq    $16, %rsp\n");
        let idx5 = self.string_literals.len();
        self.string_literals.push("%ld".to_string());
        self.output.push_str(&format!("    leaq    .LS{}(%rip), %rdi\n", idx5));
        self.output.push_str("    leaq    -8(%rbp), %rsi\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    call    scanf@PLT\n");
        self.output.push_str("    movq    -8(%rbp), %rax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_ReadChar\n");
        self.output.push_str("stdio_ReadChar:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    call    getchar@PLT\n");
        self.output.push_str("    cltq\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_ReadLine\n");
        self.output.push_str("stdio_ReadLine:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    pushq   %rbx\n");
        self.output.push_str("    movq    %rdi, %rbx\n");
        self.output.push_str("    movq    %rsi, %rdx\n");
        self.output.push_str("    movq    %rbx, %rdi\n");
        self.output.push_str("    movq    stdin@GOTPCREL(%rip), %rax\n");
        self.output.push_str("    movq    (%rax), %rsi\n");
        self.output.push_str("    call    fgets@PLT\n");
        self.output.push_str("    testq   %rax, %rax\n");
        self.output.push_str("    je      .LReadLine_fail\n");
        self.output.push_str("    movq    %rbx, %rdi\n");
        self.output.push_str("    call    strlen@PLT\n");
        self.output.push_str("    jmp     .LReadLine_end\n");
        self.output.push_str(".LReadLine_fail:\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str(".LReadLine_end:\n");
        self.output.push_str("    popq    %rbx\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");

        self.output.push_str("    .globl stdio_Flush\n");
        self.output.push_str("stdio_Flush:\n");
        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    movq    stdout@GOTPCREL(%rip), %rax\n");
        self.output.push_str("    movq    (%rax), %rdi\n");
        self.output.push_str("    call    fflush@PLT\n");
        self.output.push_str("    xorl    %eax, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");
    }

    fn generate_user_function(&mut self, func: &Function) {
        self.output.push_str(&format!("    .globl {}\n", func.name));
        self.output.push_str(&format!("{}:\n", func.name));

        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    subq    $64, %rsp\n");

        let arg_regs = ["%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"];
        let mut local_vars = HashMap::new();
        let mut local_offset = 0i32;

        for (i, param) in func.params.iter().enumerate() {
            if i < arg_regs.len() {
                local_offset -= 8;
                local_vars.insert(param.name.clone(), local_offset);
                self.output.push_str(&format!("    movq    {}, {}(%rbp)\n", arg_regs[i], local_offset));
            }
        }

        let saved_vars = self.variables.clone();
        let saved_offset = self.stack_offset;
        self.variables = local_vars;
        self.stack_offset = local_offset;

        for stmt in &func.body {
            self.generate_statement(stmt);
        }

        self.variables = saved_vars;
        self.stack_offset = saved_offset;

        self.output.push_str("    movl    $0, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");
    }

    fn generate_module_function(&mut self, module_name: &str, func: &Function) {
        self.output.push_str(&format!("    .globl {}_{}\n", module_name, func.name));
        self.output.push_str(&format!("{}_{}", module_name, func.name));
        self.output.push_str(":\n");

        self.output.push_str("    pushq   %rbp\n");
        self.output.push_str("    movq    %rsp, %rbp\n");
        self.output.push_str("    subq    $64, %rsp\n");

        let arg_regs = ["%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"];
        let mut local_vars = HashMap::new();
        let mut local_offset = 0i32;

        for (i, param) in func.params.iter().enumerate() {
            if i < arg_regs.len() {
                local_offset -= 8;
                local_vars.insert(param.name.clone(), local_offset);
                self.output.push_str(&format!("    movq    {}, {}(%rbp)\n", arg_regs[i], local_offset));
            }
        }

        let saved_vars = self.variables.clone();
        let saved_offset = self.stack_offset;
        self.variables = local_vars;
        self.stack_offset = local_offset;

        for stmt in &func.body {
            self.generate_statement(stmt);
        }

        self.variables = saved_vars;
        self.stack_offset = saved_offset;

        self.output.push_str("    movl    $0, %eax\n");
        self.output.push_str("    leave\n");
        self.output.push_str("    ret\n\n");
    }

    fn generate_statement(&mut self, stmt: &Statement) {
        match stmt {
            Statement::VarDecl { name, var_type: _, value } => {
                if let Some(expr) = value {
                    self.generate_expression(expr);
                    self.stack_offset -= 8;
                    self.variables.insert(name.clone(), self.stack_offset);
                    self.output.push_str(&format!("    movq    %rax, {}(%rbp)\n", self.stack_offset));
                }
            }
            Statement::ArrayDecl { name, element_type: _, size } => {
                let array_size = (*size as i32) * 8;
                self.stack_offset -= array_size;
                self.variables.insert(name.clone(), self.stack_offset);
                for i in 0..*size {
                    let offset = self.stack_offset + (i as i32 * 8);
                    self.output.push_str(&format!("    movq    $0, {}(%rbp)\n", offset));
                }
            }
            Statement::Assignment { name, value } => {
                self.generate_expression(value);
                if let Some(&offset) = self.variables.get(name) {
                    self.output.push_str(&format!("    movq    %rax, {}(%rbp)\n", offset));
                }
            }
            Statement::PointerAssignment { target, value } => {
                self.generate_expression(value);
                self.output.push_str("    pushq   %rax\n");
                
                self.generate_expression(target);
                
                self.output.push_str("    popq    %rcx\n");
                self.output.push_str("    movq    %rcx, (%rax)\n");
            }

            Statement::InlineAsm { parts } => {
                use crate::ast::AsmPart;
                
                self.output.push_str("    # inline asm\n");
                
                for part in parts {
                    match part {
                        AsmPart::Literal(s) => {
                            self.output.push_str(s);
                        }
                        AsmPart::Variable(var_name) => {
                            self.output.push_str(&format!("    # Variable: {}\n", var_name));
                        }
                    }
                }
                self.output.push_str("\n");
            }
            Statement::ArrayAssignment { name, index, value } => {
                self.generate_expression(value);
                self.output.push_str("    pushq   %rax\n");

                self.generate_expression(index);

                if let Some(&base_offset) = self.variables.get(name) {
                    self.output.push_str("    imulq   $8, %rax\n");
                    self.output.push_str(&format!("    addq    ${}, %rax\n", base_offset));
                    self.output.push_str("    addq    %rbp, %rax\n");

                    self.output.push_str("    popq    %rcx\n");
                    self.output.push_str("    movq    %rcx, (%rax)\n");
                }
            }
            Statement::Return(expr) => {
                if let Some(e) = expr {
                    self.generate_expression(e);
                } else {
                    self.output.push_str("    movl    $0, %eax\n");
                }
                self.output.push_str("    leave\n");
                self.output.push_str("    ret\n");
            }
            Statement::Expression(expr) => {
                self.generate_expression(expr);
            }
            Statement::If { condition, then_body, else_body } => {
                self.generate_expression(condition);
                let else_label = self.next_label();
                let end_label = self.next_label();

                self.output.push_str("    testq   %rax, %rax\n");
                self.output.push_str(&format!("    je      {}\n", else_label));

                for stmt in then_body {
                    self.generate_statement(stmt);
                }
                self.output.push_str(&format!("    jmp     {}\n", end_label));

                self.output.push_str(&format!("{}:\n", else_label));
                if let Some(body) = else_body {
                    for stmt in body {
                        self.generate_statement(stmt);
                    }
                }
                self.output.push_str(&format!("{}:\n", end_label));
            }
            Statement::For { init: _, condition, post: _, body } => {
                let loop_label = self.next_label();
                let end_label = self.next_label();

                self.output.push_str(&format!("{}:\n", loop_label));

                if let Some(cond) = condition {
                    self.generate_expression(cond);
                    self.output.push_str("    testq   %rax, %rax\n");
                    self.output.push_str(&format!("    je      {}\n", end_label));
                }

                for stmt in body {
                    self.generate_statement(stmt);
                }

                self.output.push_str(&format!("    jmp     {}\n", loop_label));
                self.output.push_str(&format!("{}:\n", end_label));
            }
        }
    }

    fn generate_expression(&mut self, expr: &Expression) {
        match expr {
            Expression::Number(n) => {
                self.output.push_str(&format!("    movq    ${}, %rax\n", n));
            }
            Expression::Identifier(name) => {
                if let Some(&offset) = self.variables.get(name) {
                    self.output.push_str(&format!("    movq    {}(%rbp), %rax\n", offset));
                }
            }
            Expression::Binary { op, left, right } => {
                self.generate_expression(right);
                self.output.push_str("    pushq   %rax\n");
                self.generate_expression(left);
                self.output.push_str("    popq    %rcx\n");

                match op {
                    BinaryOp::Add => {
                        self.output.push_str("    addq    %rcx, %rax\n");
                    }
                    BinaryOp::Sub => {
                        self.output.push_str("    subq    %rcx, %rax\n");
                    }
                    BinaryOp::Mul => {
                        self.output.push_str("    imulq   %rcx, %rax\n");
                    }
                    BinaryOp::Div => {
                        self.output.push_str("    cqto\n");
                        self.output.push_str("    idivq   %rcx\n");
                    }
                    BinaryOp::Mod => {
                        self.output.push_str("    cqto\n");
                        self.output.push_str("    idivq   %rcx\n");
                        self.output.push_str("    movq    %rdx, %rax\n");
                    }
                    BinaryOp::Equal => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    sete    %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::NotEqual => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    setne   %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::Less => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    setl    %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::LessEqual => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    setle   %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::Greater => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    setg    %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::GreaterEqual => {
                        self.output.push_str("    cmpq    %rcx, %rax\n");
                        self.output.push_str("    setge   %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                    BinaryOp::Concat => {
                    }
                    BinaryOp::BitAnd => {
                        self.output.push_str("    andq    %rcx, %rax\n");
                    }
                    BinaryOp::BitOr => {
                        self.output.push_str("    orq     %rcx, %rax\n");
                    }
                    BinaryOp::BitXor => {
                        self.output.push_str("    xorq    %rcx, %rax\n");
                    }
                    BinaryOp::Shl => {
                        self.output.push_str("    movq    %rcx, %rcx\n");
                        self.output.push_str("    shlq    %cl, %rax\n");
                    }
                    BinaryOp::Shr => {
                        self.output.push_str("    sarq    %cl, %rax\n");
                    }
                    _ => {}
                }
            }
            Expression::Unary { op, operand } => {
                self.generate_expression(operand);
                match op {
                    UnaryOp::Neg => {
                        self.output.push_str("    negq    %rax\n");
                    }
                    UnaryOp::Not => {
                        self.output.push_str("    testq   %rax, %rax\n");
                        self.output.push_str("    sete    %al\n");
                        self.output.push_str("    movzbq  %al, %rax\n");
                    }
                }
            }
            Expression::Call { function, args } => {
                let arg_regs = ["%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"];
                
                for arg in args.iter().rev() {
                    self.generate_expression(arg);
                    self.output.push_str("    pushq   %rax\n");
                }
                
                for (i, _) in args.iter().enumerate() {
                    if i < arg_regs.len() {
                        self.output.push_str(&format!("    popq    {}\n", arg_regs[i]));
                    }
                }
                
                self.output.push_str(&format!("    call    {}\n", function));
            }
            Expression::ArrayAccess { name, index } => {
                self.generate_expression(index);

                if let Some(&base_offset) = self.variables.get(name) {
                    self.output.push_str("    imulq   $8, %rax\n");
                    self.output.push_str(&format!("    addq    ${}, %rax\n", base_offset));
                    self.output.push_str("    addq    %rbp, %rax\n");

                    self.output.push_str("    movq    (%rax), %rax\n");
                }
            }
            Expression::ModuleCall { module, function, args } => {
                let arg_regs = ["%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"];

                for arg in args.iter().rev() {
                    self.generate_expression(arg);
                    self.output.push_str("    pushq   %rax\n");
                }

                for (i, _) in args.iter().enumerate() {
                    if i < arg_regs.len() {
                        self.output.push_str(&format!("    popq    {}\n", arg_regs[i]));
                    }
                }

                self.output.push_str(&format!("    call    {}_{}\n", module, function));
            }
            Expression::String(s) => {
                let idx = self.string_literals.len();
                self.string_literals.push(s.clone());
                self.output.push_str(&format!("    leaq    .LS{}(%rip), %rax\n", idx));
            }
            Expression::TemplateString { parts } => {
                use crate::ast::{TemplateStringPart, FormatType};
                
                self.output.push_str("    subq    $1024, %rsp\n");
                self.output.push_str("    movq    %rsp, %r15\n");
                
                self.output.push_str("    movb    $0, (%r15)\n");
                
                self.output.push_str("    subq    $128, %rsp\n");
                self.output.push_str("    movq    %rsp, %r14\n");
                
                for part in parts {
                    match part {
                        TemplateStringPart::Literal(lit) => {
                            let idx = self.string_literals.len();
                            self.string_literals.push(lit.clone());
                            
                            self.output.push_str("    movq    %r15, %rdi\n");
                            self.output.push_str(&format!("    leaq    .LS{}(%rip), %rsi\n", idx));
                            self.output.push_str("    call    strcat@PLT\n");
                        }
                        TemplateStringPart::Expression { expr, format } => {
                            let is_string_expr = matches!(
                                expr.as_ref(),
                                Expression::String(_) | Expression::Identifier(_)
                            );
                            
                            self.generate_expression(expr);
                            
                            if is_string_expr && format.is_none() {
                                self.output.push_str("    movq    %r15, %rdi\n");
                                self.output.push_str("    movq    %rax, %rsi\n");
                                self.output.push_str("    call    strcat@PLT\n");
                            } else {
                                let format_str = if let Some(spec) = format {
                                    match spec.format_type {
                                        FormatType::Hex => {
                                            if let Some(width) = spec.width {
                                                if spec.padding == '0' {
                                                    format!("%0{}lx", width)
                                                } else {
                                                    format!("%{}lx", width)
                                                }
                                            } else {
                                                "%lx".to_string()
                                            }
                                        }
                                        FormatType::HexUpper => {
                                            if let Some(width) = spec.width {
                                                if spec.padding == '0' {
                                                    format!("%0{}lX", width)
                                                } else {
                                                    format!("%{}lX", width)
                                                }
                                            } else {
                                                "%lX".to_string()
                                            }
                                        }
                                        FormatType::Decimal => {
                                            if let Some(width) = spec.width {
                                                if spec.padding == '0' {
                                                    format!("%0{}ld", width)
                                                } else {
                                                    format!("%{}ld", width)
                                                }
                                            } else {
                                                "%ld".to_string()
                                            }
                                        }
                                        FormatType::String => "%s".to_string(),
                                        FormatType::Auto => "%ld".to_string(),
                                    }
                                } else {
                                    "%ld".to_string()
                                };
                                
                                let fmt_idx = self.string_literals.len();
                                self.string_literals.push(format_str);
                                
                                self.output.push_str("    movq    %rax, %rdx\n");
                                self.output.push_str("    movq    %r14, %rdi\n");
                                self.output.push_str(&format!("    leaq    .LS{}(%rip), %rsi\n", fmt_idx));
                                self.output.push_str("    xorl    %eax, %eax\n");
                                self.output.push_str("    call    sprintf@PLT\n");
                                
                                self.output.push_str("    movq    %r15, %rdi\n");
                                self.output.push_str("    movq    %r14, %rsi\n");
                                self.output.push_str("    call    strcat@PLT\n");
                            }
                        }
                    }
                }
                
                self.output.push_str("    movq    %r15, %rax\n");
                
            }
            Expression::StringIndex { string, index } => {
                if let Expression::String(s) = string.as_ref() {
                    let idx = self.string_literals.len();
                    self.string_literals.push(s.clone());

                    self.generate_expression(index);

                    self.output.push_str(&format!("    leaq    .LS{}(%rip), %rcx\n", idx));
                    self.output.push_str("    addq    %rax, %rcx\n");

                    self.output.push_str("    movzbq  (%rcx), %rax\n");
                }
            }
            Expression::AddressOf { operand } => {
                if let Expression::Identifier(name) = operand.as_ref() {
                    if let Some(&offset) = self.variables.get(name) {
                        self.output.push_str(&format!("    leaq    {}(%rbp), %rax\n", offset));
                    }
                }
            }
            Expression::Deref { operand } => {
                self.generate_expression(operand);
                self.output.push_str("    movq    (%rax), %rax\n");
            }
            Expression::Eval { instruction: _ } => {
                self.output.push_str("    movq    $0, %rax\n");
            }
        }
    }
}
