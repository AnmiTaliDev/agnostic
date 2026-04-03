#[derive(Debug, Clone)]
pub struct Program {
    #[allow(dead_code)]
    pub package: String,
    pub imports: Vec<Import>,
    pub functions: Vec<Function>,
    pub modules: std::collections::HashMap<String, Module>,
}

#[derive(Debug, Clone)]
pub struct Import {
    pub path: String,
    #[allow(dead_code)]
    pub alias: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Module {
    #[allow(dead_code)]
    pub name: String,
    pub functions: Vec<Function>,
}

#[derive(Debug, Clone)]
pub struct Function {
    pub name: String,
    pub params: Vec<Parameter>,
    #[allow(dead_code)]
    pub return_type: Option<String>,
    pub body: Vec<Statement>,
    pub is_exported: bool,
}

#[derive(Debug, Clone)]
pub struct Parameter {
    pub name: String,
    #[allow(dead_code)]
    pub param_type: String,
}

#[derive(Debug, Clone)]
pub enum Statement {
    VarDecl {
        name: String,
        #[allow(dead_code)]
        var_type: Option<String>,
        value: Option<Expression>,
    },
    ArrayDecl {
        name: String,
        #[allow(dead_code)]
        element_type: String,
        size: usize,
    },
    Assignment {
        name: String,
        value: Expression,
    },
    ArrayAssignment {
        name: String,
        index: Expression,
        value: Expression,
    },
    PointerAssignment {
        target: Expression,
        value: Expression,
    },
    If {
        condition: Expression,
        then_body: Vec<Statement>,
        else_body: Option<Vec<Statement>>,
    },
    For {
        #[allow(dead_code)]
        init: Option<Box<Statement>>,
        condition: Option<Expression>,
        #[allow(dead_code)]
        post: Option<Box<Statement>>,
        body: Vec<Statement>,
    },
    Return(Option<Expression>),
    Expression(Expression),
    InlineAsm {
        parts: Vec<AsmPart>,
    },
}

#[derive(Debug, Clone)]
pub enum Expression {
    Number(i64),
    String(String),
    TemplateString {
        parts: Vec<TemplateStringPart>,
    },
    Identifier(String),
    Binary {
        op: BinaryOp,
        left: Box<Expression>,
        right: Box<Expression>,
    },
    Unary {
        op: UnaryOp,
        operand: Box<Expression>,
    },
    Call {
        function: String,
        args: Vec<Expression>,
    },
    ModuleCall {
        module: String,
        function: String,
        args: Vec<Expression>,
    },
    ArrayAccess {
        name: String,
        index: Box<Expression>,
    },
    StringIndex {
        string: Box<Expression>,
        index: Box<Expression>,
    },
    AddressOf {
        operand: Box<Expression>,
    },
    Deref {
        operand: Box<Expression>,
    },
    Eval {
        instruction: Box<Expression>,
    },
}

#[derive(Debug, Clone)]
pub enum TemplateStringPart {
    Literal(String),
    Expression {
        expr: Box<Expression>,
        format: Option<FormatSpec>,
    },
}

#[derive(Debug, Clone)]
pub enum AsmPart {
    Literal(String),
    Variable(String),
}

#[derive(Debug, Clone)]
pub struct FormatSpec {
    pub width: Option<usize>,
    #[allow(dead_code)]
    pub precision: Option<usize>,
    pub format_type: FormatType,
    pub padding: char,
}

#[derive(Debug, Clone, PartialEq)]
pub enum FormatType {
    Decimal,
    Hex,
    HexUpper,
    String,
    Auto,
}

#[derive(Debug, Clone, PartialEq)]
pub enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
    Concat,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
}

#[derive(Debug, Clone, PartialEq)]
pub enum UnaryOp {
    Neg,
    Not,
}
