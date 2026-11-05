//! A tokenizer for arithmetic expressions, with spans for its errors.

use std::fmt;
use std::iter::Peekable;
use std::str::CharIndices;

const MAX_DEPTH: usize = 64;

#[derive(Debug, Clone, PartialEq)]
pub enum Token<'a> {
    Number(f64),
    Ident(&'a str),
    Op(char),
    Open,
    Close,
}

#[derive(Debug)]
pub struct Error {
    pub at: usize,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at byte {}", self.message, self.at)
    }
}

pub struct Lexer<'a> {
    text: &'a str,
    chars: Peekable<CharIndices<'a>>,
    depth: usize,
}

impl<'a> Lexer<'a> {
    pub fn new(text: &'a str) -> Self {
        Self { text, chars: text.char_indices().peekable(), depth: 0 }
    }

    fn take_while(&mut self, start: usize, keep: impl Fn(char) -> bool) -> &'a str {
        let mut end = start;
        while let Some(&(i, c)) = self.chars.peek() {
            if !keep(c) {
                break;
            }
            end = i + c.len_utf8();
            self.chars.next();
        }
        &self.text[start..end]
    }
}

impl<'a> Iterator for Lexer<'a> {
    type Item = Result<Token<'a>, Error>;

    fn next(&mut self) -> Option<Self::Item> {
        let (at, c) = loop {
            let (i, c) = self.chars.next()?;
            if !c.is_whitespace() {
                break (i, c);
            }
        };
        Some(match c {
            '0'..='9' | '.' => {
                let digits = self.take_while(at, |c| c.is_ascii_digit() || c == '.');
                digits.parse().map(Token::Number).map_err(|e| Error { at, message: e.to_string() })
            }
            'a'..='z' | 'A'..='Z' | '_' => Ok(Token::Ident(self.take_while(at, |c| c.is_alphanumeric() || c == '_'))),
            '+' | '-' | '*' | '/' | '^' => Ok(Token::Op(c)),
            '(' if self.depth < MAX_DEPTH => {
                self.depth += 1;
                Ok(Token::Open)
            }
            ')' => {
                self.depth = self.depth.saturating_sub(1);
                Ok(Token::Close)
            }
            other => Err(Error { at, message: format!("unexpected {other:?}\n") }),
        })
    }
}

fn main() {
    for token in Lexer::new("2 * (radius + 1.5) ^ 2") {
        match token {
            Ok(token) => println!("{token:?}"),
            Err(error) => eprintln!("{error}"),
        }
    }
}
