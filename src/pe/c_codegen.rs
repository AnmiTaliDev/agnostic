use crate::ast::*;
use std::process::Command;
use std::fs;

use std::collections::HashMap;

pub struct CCodeGen {
    output: String,
    var_types: HashMap<String, bool>,
    temp_counter: usize,
}

impl CCodeGen {
    pub fn new() -> Self {
        CCodeGen {
            output: String::new(),
            var_types: HashMap::new(),
            temp_counter: 0,
        }
    }

    pub fn generate(&mut self, program: &Program) -> Result<String, String> {
        self.output.push_str("#include <stdio.h>\n");
        self.output.push_str("#include <stdlib.h>\n");
        self.output.push_str("#include <string.h>\n\n");

        for func in &program.functions {
            self.generate_function(func)?;
        }

        Ok(self.output.clone())
    }

    fn generate_function(&mut self, func: &Function) -> Result<(), String> {
        self.output.push_str("void ");
        self.output.push_str(&func.name);
        self.output.push_str("(");
        
        for (i, param) in func.params.iter().enumerate() {
            if i > 0 {
                self.output.push_str(", ");
            }
            self.output.push_str("long long ");
            self.output.push_str(&param.name);
        }
        
        self.output.push_str(") {\n");
        
        for stmt in &func.body {
            self.generate_statement(stmt)?;
        }
        
        self.output.push_str("}\n\n");
        Ok(())
    }

    fn generate_statement(&mut self, stmt: &Statement) -> Result<(), String> {
        match stmt {
            Statement::VarDecl { name, value, .. } => {
                let is_string = if let Some(val) = value {
                    matches!(val, Expression::String(_) | Expression::TemplateString {..})
                } else {
                    false
                };
                
                self.var_types.insert(name.clone(), is_string);
                
                if let Some(Expression::TemplateString { parts }) = value {
                    use crate::ast::TemplateStringPart;
                    
                    self.output.push_str("    char* ");
                    self.output.push_str(name);
                    self.output.push_str(" = malloc(2048);\n    sprintf(");
                    self.output.push_str(name);
                    self.output.push_str(", \"");
                    
                    let mut args = Vec::new();
                    for part in parts {
                        match part {
                            TemplateStringPart::Literal(lit) => {
                                self.output.push_str(&lit.replace("%", "%%"));
                            }
                            TemplateStringPart::Expression { expr, .. } => {
                                let is_str = match **expr {
                                    Expression::Identifier(ref n) => {
                                        self.var_types.get(n).copied().unwrap_or(false)
                                    }
                                    Expression::String(_) => true,
                                    _ => false,
                                };
                                self.output.push_str(if is_str { "%s" } else { "%lld" });
                                args.push((expr.clone(), is_str));
                            }
                        }
                    }
                    
                    self.output.push_str("\"");
                    for (arg, is_str) in &args {
                        self.output.push_str(", ");
                        if *is_str {
                            self.generate_expression(arg)?;
                        } else {
                            self.output.push_str("(long long)(");
                            self.generate_expression(arg)?;
                            self.output.push_str(")");
                        }
                    }
                    self.output.push_str(");\n");
                } else {
                    self.output.push_str("    ");
                    if is_string {
                        self.output.push_str("char* ");
                    } else {
                        self.output.push_str("long long ");
                    }
                    self.output.push_str(name);
                    
                    if let Some(val) = value {
                        self.output.push_str(" = ");
                        self.generate_expression(val)?;
                    }
                    self.output.push_str(";\n");
                }
            }
            Statement::Expression(expr) => {
                self.output.push_str("    ");
                self.generate_expression(expr)?;
                self.output.push_str(";\n");
            }
            Statement::Return(expr) => {
                self.output.push_str("    return ");
                if let Some(e) = expr {
                    self.generate_expression(e)?;
                }
                self.output.push_str(";\n");
            }
            _ => {}
        }
        Ok(())
    }

    fn generate_expression(&mut self, expr: &Expression) -> Result<(), String> {
        match expr {
            Expression::Number(n) => {
                self.output.push_str(&n.to_string());
            }
            Expression::String(s) => {
                if s.contains("$(") {
                    self.generate_string_interpolation(s)?;
                } else {
                    self.output.push('"');
                    self.output.push_str(&s.replace("\\", "\\\\").replace("\"", "\\\""));
                    self.output.push('"');
                }
            }
            Expression::Identifier(name) => {
                self.output.push_str(name);
            }
            Expression::ModuleCall { module, function, args } => {
                if module == "stdio" {
                    match function.as_str() {
                        "PrintlnStr" => {
                            self.output.push_str("printf(\"%s\\n\", ");
                            if !args.is_empty() {
                                self.generate_expression(&args[0])?;
                            }
                            self.output.push_str(")");
                        }
                        "PrintStr" => {
                            self.output.push_str("printf(\"%s\", ");
                            if !args.is_empty() {
                                self.generate_expression(&args[0])?;
                            }
                            self.output.push_str(")");
                        }
                        "Println" => {
                            self.output.push_str("printf(\"%lld\\n\", (long long)");
                            if !args.is_empty() {
                                self.generate_expression(&args[0])?;
                            }
                            self.output.push_str(")");
                        }
                        _ => return Err(format!("Unknown stdio function: {}", function)),
                    }
                }
            }
            Expression::Binary { op, left, right } => {
                use crate::ast::BinaryOp;
                let op_str = match op {
                    BinaryOp::Add => "+",
                    BinaryOp::Sub => "-",
                    BinaryOp::Mul => "*",
                    BinaryOp::Div => "/",
                    BinaryOp::Mod => "%",
                    BinaryOp::Equal => "==",
                    BinaryOp::NotEqual => "!=",
                    BinaryOp::Less => "<",
                    BinaryOp::LessEqual => "<=",
                    BinaryOp::Greater => ">",
                    BinaryOp::GreaterEqual => ">=",
                    BinaryOp::And => "&&",
                    BinaryOp::Or => "||",
                    _ => "+",
                };
                self.output.push_str("(");
                self.generate_expression(left)?;
                self.output.push_str(&format!(" {} ", op_str));
                self.generate_expression(right)?;
                self.output.push_str(")");
            }
            Expression::TemplateString { parts: _ } => {
                let temp_name = format!("_temp_str_{}", self.temp_counter);
                self.temp_counter += 1;
                
                self.output.push_str(&temp_name);
            }
            _ => {}
        }
        Ok(())
    }

    fn generate_string_interpolation(&mut self, s: &str) -> Result<(), String> {
        self.output.push('"');
        self.output.push_str(&s.replace("\\", "\\\\").replace("\"", "\\\""));
        self.output.push('"');
        Ok(())
    }


    pub fn compile_c_code(&self, c_code: &str, output_path: &str) -> Result<(), String> {
        fs::create_dir_all("build").map_err(|e| e.to_string())?;
        
        let temp_c = "build/temp_perano.c";
        fs::write(temp_c, c_code).map_err(|e| e.to_string())?;

        let result = if let Ok(output) = Command::new("cl.exe")
            .args(&["/nologo", "/O2", temp_c, &format!("/Fe:{}", output_path)])
            .current_dir(".")
            .output()
        {
            let _ = fs::remove_file("build/temp_perano.obj");
            
            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                if stderr.contains("is not recognized") || stderr.contains("cannot open") {
                    None
                } else {
                    Some(Err(stderr.to_string()))
                }
            } else {
                Some(Ok(()))
            }
        } else {
            None
        };
        
        let result = if let Some(r) = result {
            r
        } else if Command::new("gcc").arg("--version").output().is_ok() {
            let output = Command::new("gcc")
                .args(&["-O2", temp_c, "-o", output_path])
                .output()
                .map_err(|e| format!("Failed to run gcc: {}", e))?;
            
            if !output.status.success() {
                Err(String::from_utf8_lossy(&output.stderr).to_string())
            } else {
                Ok(())
            }
        } else if Command::new("wsl").arg("gcc").arg("--version").output().is_ok() {
            let wsl_temp_c = temp_c.replace("\\", "/").replace("E:", "/mnt/e");
            let wsl_output = output_path.replace("\\", "/").replace("E:", "/mnt/e");
            
            let output = Command::new("wsl")
                .args(&["gcc", "-O2", &wsl_temp_c, "-o", &wsl_output])
                .output()
                .map_err(|e| format!("Failed to run WSL gcc: {}", e))?;
            
            if !output.status.success() {
                Err(String::from_utf8_lossy(&output.stderr).to_string())
            } else {
                Ok(())
            }
        } else {
            Err("No compiler found. Install Visual Studio (cl.exe), MinGW (gcc), or WSL with gcc".to_string())
        };

        if result.is_ok() {
            println!("Compilation successful: {}", output_path);
        }
        result
    }
}
