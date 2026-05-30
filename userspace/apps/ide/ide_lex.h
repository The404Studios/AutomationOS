/*
 * ide_lex.h -- C tokenizer shared by the parser and the code-view highlighter.
 * Freestanding, no allocation: caller supplies the token array.
 */
#ifndef IDE_LEX_H
#define IDE_LEX_H

typedef enum {
    TK_EOF = 0,
    TK_ID,        /* identifier                    */
    TK_KW,        /* C keyword (if/for/return/...)  */
    TK_TYPE,      /* type-ish keyword (int/char/void/struct/...) */
    TK_NUM,       /* numeric literal               */
    TK_STR,       /* "string"                      */
    TK_CHAR,      /* 'c'                           */
    TK_COMMENT,   /* // or block comment           */
    TK_PREPROC,   /* #include / #define ...        */
    TK_PUNCT      /* operators / punctuation       */
} TokKind;

typedef struct {
    TokKind kind;
    const char* s;   /* points into the source buffer */
    int len;
    int line;        /* 1-based */
    int col;         /* 0-based */
} Tok;

/* Tokenize src[0..len). Fills toks[] (up to max). Returns token count
 * (a trailing TK_EOF is appended if room). */
int lex_tokenize(const char* src, int len, Tok* toks, int max);

/* Per-character color class for one source line (length len, no newline).
 * cls[i] receives one of the LEXCLS_* values below for column i. */
enum {
    LEXCLS_NORMAL = 0,
    LEXCLS_KEYWORD,
    LEXCLS_TYPE,
    LEXCLS_STRING,
    LEXCLS_COMMENT,
    LEXCLS_NUMBER,
    LEXCLS_PREPROC,
    LEXCLS_CALL      /* identifier immediately followed by '(' */
};
void lex_classify_line(const char* line, int len, unsigned char* cls);

/* 1 if [s,len) is a C keyword; 2 if it is a type keyword; 0 otherwise. */
int lex_keyword_kind(const char* s, int len);

#endif /* IDE_LEX_H */
