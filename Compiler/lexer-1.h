// ============================================================================
// lexer.h — Tokenizer untuk subset Lua/Luau
// ============================================================================
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace vmc {

enum class TokType {
    // literal
    NUMBER, STRING, NAME,
    // keyword
    KW_AND, KW_BREAK, KW_DO, KW_ELSE, KW_ELSEIF, KW_END, KW_FALSE, KW_FOR,
    KW_FUNCTION, KW_IF, KW_IN, KW_LOCAL, KW_NIL, KW_NOT, KW_OR, KW_REPEAT,
    KW_RETURN, KW_THEN, KW_TRUE, KW_UNTIL, KW_WHILE,
    // simbol
    PLUS, MINUS, STAR, SLASH, PERCENT, CARET, HASH,
    EQ, NE, LE, GE, LT, GT, ASSIGN,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    SEMI, COLON, COMMA, DOT, DOTDOT, ELLIPSIS,
    DBCOLON, // '::' label Luau/5.2+, dicadangkan
    EOF_TOK
};

struct Token {
    TokType type;
    std::string text;
    double num = 0;
    int line = 1;
};

class LexError : public std::runtime_error {
public:
    LexError(const std::string& msg, int line)
        : std::runtime_error("lex error [line " + std::to_string(line) + "]: " + msg) {}
};

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        for (;;) {
            Token t = next();
            out.push_back(t);
            if (t.type == TokType::EOF_TOK) break;
        }
        return out;
    }

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;

    static const std::unordered_map<std::string, TokType>& keywords() {
        static const std::unordered_map<std::string, TokType> kw = {
            {"and",TokType::KW_AND},{"break",TokType::KW_BREAK},{"do",TokType::KW_DO},
            {"else",TokType::KW_ELSE},{"elseif",TokType::KW_ELSEIF},{"end",TokType::KW_END},
            {"false",TokType::KW_FALSE},{"for",TokType::KW_FOR},{"function",TokType::KW_FUNCTION},
            {"if",TokType::KW_IF},{"in",TokType::KW_IN},{"local",TokType::KW_LOCAL},
            {"nil",TokType::KW_NIL},{"not",TokType::KW_NOT},{"or",TokType::KW_OR},
            {"repeat",TokType::KW_REPEAT},{"return",TokType::KW_RETURN},{"then",TokType::KW_THEN},
            {"true",TokType::KW_TRUE},{"until",TokType::KW_UNTIL},{"while",TokType::KW_WHILE},
        };
        return kw;
    }

    char peek(int off = 0) const {
        size_t p = pos_ + off;
        return p < src_.size() ? src_[p] : '\0';
    }
    char advance() {
        char c = src_[pos_++];
        if (c == '\n') line_++;
        return c;
    }
    bool match(char c) {
        if (peek() == c) { advance(); return true; }
        return false;
    }

    void skipWhitespaceAndComments() {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
            if (c == '-' && peek(1) == '-') {
                advance(); advance();
                // block comment --[[ ]] atau --[=[ ]=]
                if (peek() == '[') {
                    size_t save = pos_;
                    int eq = 0;
                    size_t p = pos_ + 1;
                    while (p < src_.size() && src_[p] == '=') { eq++; p++; }
                    if (p < src_.size() && src_[p] == '[') {
                        pos_ = p + 1;
                        skipLongBracket(eq, true);
                        continue;
                    }
                    pos_ = save;
                }
                while (peek() != '\n' && peek() != '\0') advance();
                continue;
            }
            break;
        }
    }

    // dipanggil setelah '[' pembuka & '='*eq sudah dilewati; mengonsumsi sampai ]=*eq]
    std::string skipLongBracket(int eq, bool isComment) {
        std::string out;
        if (peek() == '\n') advance(); // aturan Lua: newline pertama setelah [[ diabaikan
        for (;;) {
            if (peek() == '\0') throw LexError("unterminated long bracket", line_);
            if (peek() == ']') {
                size_t save = pos_; int line_save = line_;
                advance();
                int e = 0;
                while (peek() == '=') { e++; advance(); }
                if (e == eq && peek() == ']') { advance(); return out; }
                pos_ = save; line_ = line_save;
                if (!isComment) out.push_back(advance()); else advance();
                continue;
            }
            char c = advance();
            if (!isComment) out.push_back(c);
        }
    }

    Token makeTok(TokType t, const std::string& text, int line) {
        Token tok; tok.type=t; tok.text=text; tok.line=line; return tok;
    }

    Token next() {
        skipWhitespaceAndComments();
        int line = line_;
        if (pos_ >= src_.size()) return makeTok(TokType::EOF_TOK, "", line);

        char c = advance();

        if (isalpha((unsigned char)c) || c == '_') {
            std::string id(1, c);
            while (isalnum((unsigned char)peek()) || peek() == '_') id.push_back(advance());
            auto it = keywords().find(id);
            if (it != keywords().end()) return makeTok(it->second, id, line);
            return makeTok(TokType::NAME, id, line);
        }

        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek()))) {
            std::string num(1, c);
            bool isHex = (c=='0' && (peek()=='x' || peek()=='X'));
            if (isHex) { num.push_back(advance()); }
            while (isalnum((unsigned char)peek()) || peek()=='.' ||
                   ((peek()=='+'||peek()=='-') && (num.back()=='e'||num.back()=='E'))) {
                num.push_back(advance());
            }
            Token t = makeTok(TokType::NUMBER, num, line);
            t.num = isHex ? (double)strtoll(num.c_str(), nullptr, 16) : strtod(num.c_str(), nullptr);
            return t;
        }

        if (c == '"' || c == '\'') {
            char quote = c;
            std::string s;
            while (peek() != quote) {
                if (peek() == '\0' || peek() == '\n') throw LexError("unterminated string", line_);
                char ch = advance();
                if (ch == '\\') {
                    char e = advance();
                    switch (e) {
                        case 'n': s.push_back('\n'); break;
                        case 't': s.push_back('\t'); break;
                        case 'r': s.push_back('\r'); break;
                        case 'a': s.push_back('\a'); break;
                        case 'b': s.push_back('\b'); break;
                        case 'f': s.push_back('\f'); break;
                        case 'v': s.push_back('\v'); break;
                        case '\\': s.push_back('\\'); break;
                        case '"': s.push_back('"'); break;
                        case '\'': s.push_back('\''); break;
                        case '\n': s.push_back('\n'); break;
                        default:
                            if (isdigit((unsigned char)e)) {
                                std::string d(1, e);
                                for (int i=0;i<2 && isdigit((unsigned char)peek());i++) d.push_back(advance());
                                s.push_back((char)atoi(d.c_str()));
                            } else s.push_back(e);
                    }
                } else s.push_back(ch);
            }
            advance(); // closing quote
            return makeTok(TokType::STRING, s, line);
        }

        if (c == '[' && (peek() == '[' || peek() == '=')) {
            size_t save = pos_; int line_save = line_;
            int eq = 0;
            while (peek() == '=') { eq++; advance(); }
            if (peek() == '[') {
                advance();
                std::string s = skipLongBracket(eq, false);
                return makeTok(TokType::STRING, s, line);
            }
            pos_ = save; line_ = line_save;
        }

        switch (c) {
            case '+': return makeTok(TokType::PLUS, "+", line);
            case '-': return makeTok(TokType::MINUS, "-", line);
            case '*': return makeTok(TokType::STAR, "*", line);
            case '/': return makeTok(TokType::SLASH, "/", line);
            case '%': return makeTok(TokType::PERCENT, "%", line);
            case '^': return makeTok(TokType::CARET, "^", line);
            case '#': return makeTok(TokType::HASH, "#", line);
            case '(': return makeTok(TokType::LPAREN, "(", line);
            case ')': return makeTok(TokType::RPAREN, ")", line);
            case '{': return makeTok(TokType::LBRACE, "{", line);
            case '}': return makeTok(TokType::RBRACE, "}", line);
            case '[': return makeTok(TokType::LBRACKET, "[", line);
            case ']': return makeTok(TokType::RBRACKET, "]", line);
            case ';': return makeTok(TokType::SEMI, ";", line);
            case ',': return makeTok(TokType::COMMA, ",", line);
            case '=': return match('=') ? makeTok(TokType::EQ,"==",line) : makeTok(TokType::ASSIGN,"=",line);
            case '~':
                if (match('=')) return makeTok(TokType::NE, "~=", line);
                throw LexError("unexpected '~'", line);
            case '<': return match('=') ? makeTok(TokType::LE,"<=",line) : makeTok(TokType::LT,"<",line);
            case '>': return match('=') ? makeTok(TokType::GE,">=",line) : makeTok(TokType::GT,">",line);
            case ':':
                if (match(':')) return makeTok(TokType::DBCOLON, "::", line);
                return makeTok(TokType::COLON, ":", line);
            case '.':
                if (match('.')) {
                    if (match('.')) return makeTok(TokType::ELLIPSIS, "...", line);
                    return makeTok(TokType::DOTDOT, "..", line);
                }
                return makeTok(TokType::DOT, ".", line);
        }
        throw LexError(std::string("unexpected character '") + c + "'", line);
    }
};

} // namespace vmc
