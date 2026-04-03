use crate::ast::*;
use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    I64,
    I32,
    I8,
    U64,
    U32,
    U8,
    Bool,
    String,
    Ptr(Box<Type>),
    Array(Box<Type>, usize),
    Void,
    Unknown,
}

impl Type {
    pub fn from_string(s: &str) -> Self {
        match s {
            "i64" => Type::I64,
            "i32" => Type::I32,
            "i8" => Type::I8,
            "u64" => Type::U64,
            "u32" => Type::U32,
            "u8" => Type::U8,
            "bool" => Type::Bool,
            "string" => Type::String,
            "void" => Type::Void,
            _ => {
                if s.starts_with('*') {
                    let inner = Type::from_string(&s[1..]);
                    return Type::Ptr(Box::new(inner));
                }
                if s.starts_with('[') && s.ends_with(']') {
                    let inner = &s[1..s.len()-1];
                    if let Some(semicolon_pos) = inner.find(';') {
                        let type_str = inner[..semicolon_pos].trim();
                        let size_str = inner[semicolon_pos+1..].trim();
                        if let Ok(size) = size_str.parse::<usize>() {
                            let elem_type = Type::from_string(type_str);
                            return Type::Array(Box::new(elem_type), size);
                        }
                    }
                }
                Type::Unknown
            }
        }
    }

    pub fn is_numeric(&self) -> bool {
        matches!(self, Type::I64 | Type::I32 | Type::I8 | Type::U64 | Type::U32 | Type::U8)
    }

    pub fn is_integer(&self) -> bool {
        self.is_numeric()
    }

    pub fn can_assign_to(&self, other: &Type) -> bool {
        if self == other {
            return true;
        }
        
        if self.is_numeric() && other.is_numeric() {
            return true;
        }
        
        if matches!(self, Type::Unknown) || matches!(other, Type::Unknown) {
            return true;
        }
        
        false
    }
}

pub struct TypeChecker {
    variables: HashMap<String, Type>,
    functions: HashMap<String, FunctionSignature>,
    errors: Vec<TypeError>,
    current_function: Option<String>,
}

#[derive(Debug, Clone)]
pub struct FunctionSignature {
    pub params: Vec<(String, Type)>,
    pub return_type: Type,
}

#[derive(Debug, Clone)]
pub struct TypeError {
    pub message: String,
    pub location: String,
}

impl TypeChecker {
    pub fn new() -> Self {
        let mut checker = Self {
            variables: HashMap::new(),
            functions: HashMap::new(),
            errors: Vec::new(),
            current_function: None,
        };
        
        checker.functions.insert("stdio.Print".to_string(), FunctionSignature {
            params: vec![("value".to_string(), Type::Unknown)],
            return_type: Type::Void,
        });
        checker.functions.insert("stdio.Println".to_string(), FunctionSignature {
            params: vec![("value".to_string(), Type::Unknown)],
            return_type: Type::Void,
        });
        checker.functions.insert("stdio.PrintStr".to_string(), FunctionSignature {
            params: vec![("s".to_string(), Type::String)],
            return_type: Type::Void,
        });
        checker.functions.insert("stdio.PrintlnStr".to_string(), FunctionSignature {
            params: vec![("s".to_string(), Type::String)],
            return_type: Type::Void,
        });
        
        checker
    }

    pub fn check_program(&mut self, program: &Program) -> Result<(), Vec<TypeError>> {
        for func in &program.functions {
            self.collect_function_signature(func);
        }
        
        for (_module_name, module) in &program.modules {
            for func in &module.functions {
                if func.is_exported {
                    self.collect_function_signature(func);
                }
            }
        }
        
        for func in &program.functions {
            self.check_function(func);
        }
        
        if self.errors.is_empty() {
            Ok(())
        } else {
            Err(self.errors.clone())
        }
    }

    fn collect_function_signature(&mut self, func: &Function) {
        let params: Vec<(String, Type)> = func.params.iter()
            .map(|p| (p.name.clone(), Type::from_string(&p.param_type)))
            .collect();
        
        let return_type = func.return_type.as_ref()
            .map(|t| Type::from_string(t))
            .unwrap_or(Type::Void);
        
        self.functions.insert(func.name.clone(), FunctionSignature {
            params,
            return_type,
        });
    }

    fn check_function(&mut self, func: &Function) {
        self.current_function = Some(func.name.clone());
        self.variables.clear();
        
        for param in &func.params {
            let param_type = Type::from_string(&param.param_type);
            self.variables.insert(param.name.clone(), param_type);
        }
        
        for stmt in &func.body {
            self.check_statement(stmt);
        }
        
        self.current_function = None;
    }

    fn check_statement(&mut self, stmt: &Statement) {
        match stmt {
            Statement::VarDecl { name, var_type, value } => {
                let declared_type = var_type.as_ref()
                    .map(|t| Type::from_string(t))
                    .unwrap_or(Type::Unknown);
                
                if let Some(expr) = value {
                    let expr_type = self.infer_expression(expr);
                    
                    if !declared_type.can_assign_to(&expr_type) && !expr_type.can_assign_to(&declared_type) {
                        self.add_error(format!(
                            "Type mismatch in variable '{}': declared as {:?}, but initialized with {:?}",
                            name, declared_type, expr_type
                        ));
                    }
                    
                    let final_type = if matches!(declared_type, Type::Unknown) {
                        expr_type
                    } else {
                        declared_type
                    };
                    
                    self.variables.insert(name.clone(), final_type);
                } else {
                    self.variables.insert(name.clone(), declared_type);
                }
            }
            
            Statement::Assignment { name, value } => {
                let expr_type = self.infer_expression(value);
                
                if let Some(var_type) = self.variables.get(name) {
                    if !expr_type.can_assign_to(var_type) {
                        self.add_error(format!(
                            "Type mismatch in assignment to '{}': expected {:?}, got {:?}",
                            name, var_type, expr_type
                        ));
                    }
                } else {
                    self.add_error(format!("Variable '{}' not declared", name));
                }
            }
            
            Statement::ArrayAssignment { name, index, value } => {
                let var_type_opt = self.variables.get(name).cloned();
                if let Some(var_type) = var_type_opt {
                    if let Type::Array(elem_type, _) = var_type {
                        let index_type = self.infer_expression(index);
                        if !index_type.is_integer() {
                            self.add_error(format!(
                                "Array index must be an integer, got {:?}",
                                index_type
                            ));
                        }
                        
                        let value_type = self.infer_expression(value);
                        if !value_type.can_assign_to(&elem_type) {
                            self.add_error(format!(
                                "Type mismatch in array assignment: expected {:?}, got {:?}",
                                elem_type, value_type
                            ));
                        }
                    } else {
                        self.add_error(format!(
                            "Cannot index into non-array type {:?}",
                            var_type
                        ));
                    }
                } else {
                    self.add_error(format!("Variable '{}' not declared", name));
                }
            }
            
            Statement::If { condition, then_body, else_body } => {
                let cond_type = self.infer_expression(condition);
                if !matches!(cond_type, Type::Bool | Type::I64 | Type::Unknown) {
                    self.add_error(format!(
                        "If condition must be boolean or numeric, got {:?}",
                        cond_type
                    ));
                }
                
                for stmt in then_body {
                    self.check_statement(stmt);
                }
                
                if let Some(else_stmts) = else_body {
                    for stmt in else_stmts {
                        self.check_statement(stmt);
                    }
                }
            }
            
            Statement::For { init, condition, post, body } => {
                if let Some(init_stmt) = init {
                    self.check_statement(init_stmt);
                }
                
                if let Some(cond) = condition {
                    let cond_type = self.infer_expression(cond);
                    if !matches!(cond_type, Type::Bool | Type::I64 | Type::Unknown) {
                        self.add_error(format!(
                            "Loop condition must be boolean or numeric, got {:?}",
                            cond_type
                        ));
                    }
                }
                
                if let Some(post_stmt) = post {
                    self.check_statement(post_stmt);
                }
                
                for stmt in body {
                    self.check_statement(stmt);
                }
            }
            
            Statement::Return(value) => {
                if let Some(func_name) = &self.current_function {
                    let sig_opt = self.functions.get(func_name).cloned();
                    if let Some(sig) = sig_opt {
                        if let Some(expr) = value {
                            let expr_type = self.infer_expression(expr);
                            if !expr_type.can_assign_to(&sig.return_type) {
                                self.add_error(format!(
                                    "Return type mismatch: expected {:?}, got {:?}",
                                    sig.return_type, expr_type
                                ));
                            }
                        } else if !matches!(sig.return_type, Type::Void) {
                            self.add_error(format!(
                                "Function '{}' must return a value of type {:?}",
                                func_name, sig.return_type
                            ));
                        }
                    }
                }
            }
            
            Statement::Expression(expr) => {
                self.infer_expression(expr);
            }
            
            Statement::PointerAssignment { target, value } => {
                let target_type = self.infer_expression(target);
                if !matches!(target_type, Type::Ptr(_) | Type::Unknown) {
                    self.add_error(format!(
                        "Pointer dereference assignment requires a pointer type, got {:?}",
                        target_type
                    ));
                }
                
                self.infer_expression(value);
            }
            
            Statement::InlineAsm { .. } => {
            }
            
            Statement::ArrayDecl { name, element_type, size } => {
                let elem_type = Type::from_string(element_type);
                let array_type = Type::Array(Box::new(elem_type), *size);
                self.variables.insert(name.clone(), array_type);
            }
        }
    }

    fn infer_expression(&mut self, expr: &Expression) -> Type {
        match expr {
            Expression::Number(_) => Type::I64,
            
            Expression::String(_) => Type::String,
            
            Expression::TemplateString { .. } => Type::String,
            
            Expression::Identifier(name) => {
                self.variables.get(name).cloned().unwrap_or_else(|| {
                    self.add_error(format!("Variable '{}' not declared", name));
                    Type::Unknown
                })
            }
            
            Expression::Binary { op, left, right } => {
                let left_type = self.infer_expression(left);
                let right_type = self.infer_expression(right);
                
                match op {
                    BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Div | BinaryOp::Mod |
                    BinaryOp::BitAnd | BinaryOp::BitOr | BinaryOp::BitXor | BinaryOp::Shl | BinaryOp::Shr => {
                        if !left_type.is_numeric() {
                            self.add_error(format!(
                                "Left operand of {:?} must be numeric, got {:?}",
                                op, left_type
                            ));
                        }
                        if !right_type.is_numeric() {
                            self.add_error(format!(
                                "Right operand of {:?} must be numeric, got {:?}",
                                op, right_type
                            ));
                        }
                        left_type
                    }
                    
                    BinaryOp::Equal | BinaryOp::NotEqual | 
                    BinaryOp::Less | BinaryOp::LessEqual | 
                    BinaryOp::Greater | BinaryOp::GreaterEqual => {
                        Type::Bool
                    }
                    
                    BinaryOp::And | BinaryOp::Or => {
                        Type::Bool
                    }
                    
                    BinaryOp::Concat => {
                        Type::String
                    }
                }
            }
            
            Expression::Unary { op, operand } => {
                let operand_type = self.infer_expression(operand);
                
                match op {
                    UnaryOp::Neg => {
                        if !operand_type.is_numeric() {
                            self.add_error(format!(
                                "Negation operand must be numeric, got {:?}",
                                operand_type
                            ));
                        }
                        operand_type
                    }
                    
                    UnaryOp::Not => {
                        Type::Bool
                    }
                }
            }
            
            Expression::Call { function, args } => {
                let sig_opt = self.functions.get(function).cloned();
                if let Some(sig) = sig_opt {
                    if args.len() != sig.params.len() {
                        self.add_error(format!(
                            "Function '{}' expects {} arguments, got {}",
                            function, sig.params.len(), args.len()
                        ));
                    } else {
                        for (i, (arg, (_, param_type))) in args.iter().zip(sig.params.iter()).enumerate() {
                            let arg_type = self.infer_expression(arg);
                            if !arg_type.can_assign_to(param_type) {
                                self.add_error(format!(
                                    "Argument {} of function '{}': expected {:?}, got {:?}",
                                    i, function, param_type, arg_type
                                ));
                            }
                        }
                    }
                    sig.return_type.clone()
                } else {
                    self.add_error(format!("Function '{}' not declared", function));
                    Type::Unknown
                }
            }
            
            Expression::ModuleCall { module, function, args } => {
                let full_name = format!("{}.{}", module, function);
                let sig_opt = self.functions.get(&full_name).cloned();
                if let Some(sig) = sig_opt {
                    if args.len() != sig.params.len() {
                        self.add_error(format!(
                            "Function '{}' expects {} arguments, got {}",
                            full_name, sig.params.len(), args.len()
                        ));
                    }
                    sig.return_type.clone()
                } else {
                    Type::Unknown
                }
            }
            
            Expression::ArrayAccess { name, index } => {
                let index_type = self.infer_expression(index);
                if !index_type.is_integer() {
                    self.add_error(format!(
                        "Array index must be an integer, got {:?}",
                        index_type
                    ));
                }
                
                let var_type_opt = self.variables.get(name).cloned();
                if let Some(var_type) = var_type_opt {
                    if let Type::Array(elem_type, _) = var_type {
                        (*elem_type).clone()
                    } else {
                        self.add_error(format!(
                            "Cannot index into non-array type {:?}",
                            var_type
                        ));
                        Type::Unknown
                    }
                } else {
                    self.add_error(format!("Variable '{}' not declared", name));
                    Type::Unknown
                }
            }
            
            Expression::StringIndex { string, index } => {
                let _string_type = self.infer_expression(string);
                let index_type = self.infer_expression(index);
                
                if !index_type.is_integer() {
                    self.add_error(format!(
                        "String index must be an integer, got {:?}",
                        index_type
                    ));
                }
                
                Type::U8
            }
            
            Expression::AddressOf { operand } => {
                let inner_type = self.infer_expression(operand);
                Type::Ptr(Box::new(inner_type))
            }
            
            Expression::Deref { operand } => {
                let operand_type = self.infer_expression(operand);
                if let Type::Ptr(inner) = operand_type {
                    (*inner).clone()
                } else {
                    self.add_error(format!(
                        "Cannot dereference non-pointer type {:?}",
                        operand_type
                    ));
                    Type::Unknown
                }
            }
            
            Expression::Eval { instruction } => {
                self.infer_expression(instruction);
                Type::Unknown
            }
        }
    }

    fn add_error(&mut self, message: String) {
        let location = self.current_function.clone().unwrap_or_else(|| "global".to_string());
        self.errors.push(TypeError {
            message,
            location,
        });
    }

    pub fn print_errors(&self) {
        for error in &self.errors {
            eprintln!("Type error in {}: {}", error.location, error.message);
        }
    }
}
