#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    Package,
    Import,
    Func,
    Var,
    If,
    Else,
    For,
    Return,
    Asm,

    Identifier(String),
    Number(i64),
    String(String),

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
    Not,
    Pipe,
    Caret,
    LShift,
    RShift,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    Dot,
    Arrow,
    Ampersand,
    DoublePlus,
    Dollar,

    Newline,
    Eof,
}

pub struct Lexer {
    input: Vec<char>,
    position: usize,
    current_char: Option<char>,
    line: usize,
    column: usize,
    file: String,
}

impl Lexer {
    #[allow(dead_code)]
    pub fn new(input: &str) -> Self {
        Self::new_with_file(input, "<stdin>")
    }

    pub fn new_with_file(input: &str, file: &str) -> Self {
        let chars: Vec<char> = input.chars().collect();
        let current_char = if chars.is_empty() { None } else { Some(chars[0]) };

        Lexer {
            input: chars,
            position: 0,
            current_char,
            line: 1,
            column: 1,
            file: file.to_string(),
        }
    }

    fn advance(&mut self) {
        if let Some(ch) = self.current_char {
            if ch == '\n' {
                self.line += 1;
                self.column = 1;
            } else {
                self.column += 1;
            }
        }

        self.position += 1;
        if self.position < self.input.len() {
            self.current_char = Some(self.input[self.position]);
        } else {
            self.current_char = None;
        }
    }

    fn peek(&self, offset: usize) -> Option<char> {
        let pos = self.position + offset;
        if pos < self.input.len() {
            Some(self.input[pos])
        } else {
            None
        }
    }

    fn skip_whitespace(&mut self) {
        while let Some(ch) = self.current_char {
            if ch == ' ' || ch == '\t' || ch == '\r' {
                self.advance();
            } else {
                break;
            }
        }
    }

    fn skip_comment(&mut self) {
        if self.current_char == Some('/') && self.peek(1) == Some('/') {
            while self.current_char.is_some() && self.current_char != Some('\n') {
                self.advance();
            }
        }
    }

    fn read_number(&mut self) -> Token {
        let mut num_str = String::new();

        while let Some(ch) = self.current_char {
            if ch.is_ascii_digit() {
                num_str.push(ch);
                self.advance();
            } else {
                break;
            }
        }

        match num_str.parse::<i64>() {
            Ok(num) => Token::Number(num),
            Err(_) => {
                eprintln!("Warning: Number '{}' is too large, using i64::MAX ({})", num_str, i64::MAX);
                Token::Number(i64::MAX)
            }
        }
    }

    fn read_identifier(&mut self) -> Token {
        let mut id = String::new();

        while let Some(ch) = self.current_char {
            if ch.is_alphanumeric() || ch == '_' {
                id.push(ch);
                self.advance();
            } else {
                break;
            }
        }

        match id.as_str() {
            "package" => Token::Package,
            "import" => Token::Import,
            "use" => Token::Import,
            "func" => Token::Func,
            "fn" => Token::Func,
            "var" => Token::Var,
            "let" => Token::Var,
            "if" => Token::If,
            "else" => Token::Else,
            "for" => Token::For,
            "while" => Token::For,
            "loop" => Token::For,
            "return" => Token::Return,
            "asm" => Token::Asm,
            "pub" => Token::Identifier(id),
            _ => Token::Identifier(id),
        }
    }

    fn read_string(&mut self) -> Token {
        self.advance();
        let mut string = String::new();

        while let Some(ch) = self.current_char {
            if ch == '"' {
                self.advance();
                break;
            } else if ch == '\\' {
                self.advance();
                if let Some(escape_ch) = self.current_char {
                    match escape_ch {
                        'n' => string.push('\n'),
                        't' => string.push('\t'),
                        'r' => string.push('\r'),
                        '\\' => string.push('\\'),
                        '"' => string.push('"'),
                        _ => string.push(escape_ch),
                    }
                    self.advance();
                }
            } else {
                string.push(ch);
                self.advance();
            }
        }

        Token::String(string)
    }

    pub fn tokenize(&mut self) -> Vec<Token> {
        let mut tokens = Vec::new();

        loop {
            self.skip_whitespace();
            self.skip_comment();

            match self.current_char {
                None => {
                    tokens.push(Token::Eof);
                    break;
                }
                Some('\n') => {
                    tokens.push(Token::Newline);
                    self.advance();
                }
                Some('+') => {
                    self.advance();
                    if self.current_char == Some('+') {
                        tokens.push(Token::DoublePlus);
                        self.advance();
                    } else {
                        tokens.push(Token::Plus);
                    }
                }
                Some('-') => {
                    self.advance();
                    if self.current_char == Some('>') {
                        tokens.push(Token::Arrow);
                        self.advance();
                    } else {
                        tokens.push(Token::Minus);
                    }
                }
                Some('*') => {
                    tokens.push(Token::Star);
                    self.advance();
                }
                Some('/') => {
                    tokens.push(Token::Slash);
                    self.advance();
                }
                Some('%') => {
                    tokens.push(Token::Percent);
                    self.advance();
                }
                Some('=') => {
                    self.advance();
                    if self.current_char == Some('=') {
                        tokens.push(Token::Equal);
                        self.advance();
                    } else {
                        tokens.push(Token::Assign);
                    }
                }
                Some('!') => {
                    self.advance();
                    if self.current_char == Some('=') {
                        tokens.push(Token::NotEqual);
                        self.advance();
                    } else {
                        tokens.push(Token::Not);
                    }
                }
                Some('<') => {
                    self.advance();
                    if self.current_char == Some('<') {
                        tokens.push(Token::LShift);
                        self.advance();
                    } else if self.current_char == Some('=') {
                        tokens.push(Token::LessEqual);
                        self.advance();
                    } else {
                        tokens.push(Token::Less);
                    }
                }
                Some('>') => {
                    self.advance();
                    if self.current_char == Some('>') {
                        tokens.push(Token::RShift);
                        self.advance();
                    } else if self.current_char == Some('=') {
                        tokens.push(Token::GreaterEqual);
                        self.advance();
                    } else {
                        tokens.push(Token::Greater);
                    }
                }
                Some('&') => {
                    self.advance();
                    if self.current_char == Some('&') {
                        tokens.push(Token::And);
                        self.advance();
                    } else {
                        tokens.push(Token::Ampersand);
                    }
                }
                Some('|') => {
                    self.advance();
                    if self.current_char == Some('|') {
                        tokens.push(Token::Or);
                        self.advance();
                    } else {
                        tokens.push(Token::Pipe);
                    }
                }
                Some('^') => {
                    tokens.push(Token::Caret);
                    self.advance();
                }
                Some('(') => {
                    tokens.push(Token::LeftParen);
                    self.advance();
                }
                Some(')') => {
                    tokens.push(Token::RightParen);
                    self.advance();
                }
                Some('{') => {
                    tokens.push(Token::LeftBrace);
                    self.advance();
                }
                Some('}') => {
                    tokens.push(Token::RightBrace);
                    self.advance();
                }
                Some('[') => {
                    tokens.push(Token::LBracket);
                    self.advance();
                }
                Some(']') => {
                    tokens.push(Token::RBracket);
                    self.advance();
                }
                Some(',') => {
                    tokens.push(Token::Comma);
                    self.advance();
                }
                Some(';') => {
                    tokens.push(Token::Semicolon);
                    self.advance();
                }
                Some(':') => {
                    tokens.push(Token::Colon);
                    self.advance();
                }
                Some('.') => {
                    tokens.push(Token::Dot);
                    self.advance();
                }
                Some('$') => {
                    tokens.push(Token::Dollar);
                    self.advance();
                }
                Some('#') => {
                    self.advance();
                    while let Some(ch) = self.current_char {
                        if ch == '\n' {
                            break;
                        }
                        self.advance();
                    }
                }
                Some('"') => {
                    tokens.push(self.read_string());
                }
                Some(ch) if ch.is_ascii_digit() => {
                    tokens.push(self.read_number());
                }
                Some(ch) if ch.is_alphabetic() || ch == '_' => {
                    tokens.push(self.read_identifier());
                }
                Some(ch) => {
                    use crate::error::{CompileError, ErrorKind};
                    let err = CompileError::new(
                        ErrorKind::LexerError,
                        format!("unexpected character: '{}'", ch),
                        self.file.clone(),
                        self.line,
                        self.column,
                    );
                    err.display();
                    std::process::exit(1);
                }
            }
        }

        tokens
    }
}
