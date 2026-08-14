// ============================================================================
// parser.h — Recursive descent parser dengan precedence climbing utk ekspresi
// ============================================================================
#pragma once
#include "lexer.h"
#include "ast.h"
#include <stdexcept>
#include <set>

namespace vmc {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, int line)
        : std::runtime_error("parse error [line " + std::to_string(line) + "]: " + msg) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    // Return: block top-level, diperlakukan sbg body fungsi vararg implisit (chunk utama)
    std::vector<StatPtr> parseChunk() {
        auto block = parseBlock();
        expect(TokType::EOF_TOK, "expected end of file");
        return block;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token& peek(int off = 0) const {
        size_t p = pos_ + off;
        return p < toks_.size() ? toks_[p] : toks_.back();
    }
    const Token& advance() { return toks_[pos_++ < toks_.size()-1 ? pos_-1 : toks_.size()-1]; }
    bool check(TokType t) const { return peek().type == t; }
    bool match(TokType t) { if (check(t)) { advance(); return true; } return false; }
    const Token& expect(TokType t, const std::string& msg) {
        if (!check(t)) throw ParseError(msg + " (got '" + peek().text + "')", peek().line);
        return advance();
    }

    static bool isBlockEnd(TokType t) {
        switch (t) {
            case TokType::KW_END: case TokType::KW_ELSE: case TokType::KW_ELSEIF:
            case TokType::KW_UNTIL: case TokType::EOF_TOK: return true;
            default: return false;
        }
    }

    // ---------------- Block & Statement ----------------
    std::vector<StatPtr> parseBlock() {
        std::vector<StatPtr> stats;
        while (!isBlockEnd(peek().type)) {
            if (check(TokType::KW_RETURN)) {
                stats.push_back(parseReturn());
                break; // return harus statement terakhir di block
            }
            auto s = parseStatement();
            if (s) stats.push_back(std::move(s));
        }
        return stats;
    }

    StatPtr parseStatement() {
        int line = peek().line;
        switch (peek().type) {
            case TokType::SEMI: advance(); return nullptr;
            case TokType::KW_LOCAL: return parseLocal();
            case TokType::KW_IF: return parseIf();
            case TokType::KW_WHILE: return parseWhile();
            case TokType::KW_FOR: return parseFor();
            case TokType::KW_FUNCTION: return parseFunctionDecl();
            case TokType::KW_DO: {
                advance();
                auto body = parseBlock();
                expect(TokType::KW_END, "expected 'end' after do block");
                auto s = std::make_unique<Stat>(); s->kind=StatKind::DO; s->line=line; s->body=std::move(body);
                return s;
            }
            case TokType::KW_REPEAT: {
                advance();
                auto body = parseBlock();
                expect(TokType::KW_UNTIL, "expected 'until' after repeat block");
                auto cond = parseExpr();
                auto s = std::make_unique<Stat>(); s->kind=StatKind::REPEAT; s->line=line;
                s->body=std::move(body); s->cond=std::move(cond);
                return s;
            }
            case TokType::KW_BREAK: {
                advance();
                auto s = std::make_unique<Stat>(); s->kind=StatKind::BREAK; s->line=line;
                return s;
            }
            default: return parseExprStat();
        }
    }

    StatPtr parseReturn() {
        int line = peek().line;
        advance(); // 'return'
        auto s = std::make_unique<Stat>(); s->kind=StatKind::RETURN; s->line=line;
        if (!isBlockEnd(peek().type) && !check(TokType::SEMI)) {
            s->exprs.push_back(parseExpr());
            while (match(TokType::COMMA)) s->exprs.push_back(parseExpr());
        }
        match(TokType::SEMI);
        return s;
    }

    StatPtr parseLocal() {
        int line = peek().line;
        advance(); // 'local'
        if (match(TokType::KW_FUNCTION)) {
            std::string name = expect(TokType::NAME, "expected function name").text;
            auto func = parseFunctionBody();
            auto s = std::make_unique<Stat>(); s->kind=StatKind::LOCAL_FUNCTION; s->line=line;
            s->names.push_back(name); s->func = func;
            return s;
        }
        auto s = std::make_unique<Stat>(); s->kind=StatKind::LOCAL; s->line=line;
        s->names.push_back(expect(TokType::NAME, "expected variable name").text);
        while (match(TokType::COMMA)) s->names.push_back(expect(TokType::NAME, "expected variable name").text);
        if (match(TokType::ASSIGN)) {
            s->exprs.push_back(parseExpr());
            while (match(TokType::COMMA)) s->exprs.push_back(parseExpr());
        }
        return s;
    }

    StatPtr parseIf() {
        int line = peek().line;
        advance(); // 'if'
        auto s = std::make_unique<Stat>(); s->kind=StatKind::IF; s->line=line;
        IfClause c1; c1.cond = parseExpr();
        expect(TokType::KW_THEN, "expected 'then'");
        c1.body = parseBlock();
        s->clauses.push_back(std::move(c1));
        while (check(TokType::KW_ELSEIF)) {
            advance();
            IfClause c; c.cond = parseExpr();
            expect(TokType::KW_THEN, "expected 'then'");
            c.body = parseBlock();
            s->clauses.push_back(std::move(c));
        }
        if (match(TokType::KW_ELSE)) {
            IfClause c; c.cond = nullptr;
            c.body = parseBlock();
            s->clauses.push_back(std::move(c));
        }
        expect(TokType::KW_END, "expected 'end' to close 'if'");
        return s;
    }

    StatPtr parseWhile() {
        int line = peek().line;
        advance(); // 'while'
        auto cond = parseExpr();
        expect(TokType::KW_DO, "expected 'do' after while condition");
        auto body = parseBlock();
        expect(TokType::KW_END, "expected 'end' to close 'while'");
        auto s = std::make_unique<Stat>(); s->kind=StatKind::WHILE; s->line=line;
        s->cond=std::move(cond); s->body=std::move(body);
        return s;
    }

    StatPtr parseFor() {
        int line = peek().line;
        advance(); // 'for'
        std::string first = expect(TokType::NAME, "expected loop variable").text;
        if (check(TokType::ASSIGN)) {
            advance();
            auto s = std::make_unique<Stat>(); s->kind=StatKind::NUMERIC_FOR; s->line=line;
            s->for_var = first;
            s->for_start = parseExpr();
            expect(TokType::COMMA, "expected ',' in numeric for");
            s->for_stop = parseExpr();
            if (match(TokType::COMMA)) s->for_step = parseExpr();
            expect(TokType::KW_DO, "expected 'do' in for loop");
            s->body = parseBlock();
            expect(TokType::KW_END, "expected 'end' to close 'for'");
            return s;
        }
        // generic for: for k,v in ... do
        auto s = std::make_unique<Stat>(); s->kind=StatKind::GENERIC_FOR; s->line=line;
        s->for_names.push_back(first);
        while (match(TokType::COMMA)) s->for_names.push_back(expect(TokType::NAME, "expected name").text);
        expect(TokType::KW_IN, "expected 'in' in generic for");
        s->for_exprs.push_back(parseExpr());
        while (match(TokType::COMMA)) s->for_exprs.push_back(parseExpr());
        expect(TokType::KW_DO, "expected 'do' in for loop");
        s->body = parseBlock();
        expect(TokType::KW_END, "expected 'end' to close 'for'");
        return s;
    }

    StatPtr parseFunctionDecl() {
        int line = peek().line;
        advance(); // 'function'
        // funcname ::= Name {'.' Name} [':' Name]
        std::vector<std::string> path;
        path.push_back(expect(TokType::NAME, "expected function name").text);
        bool is_method = false;
        while (check(TokType::DOT)) {
            advance();
            path.push_back(expect(TokType::NAME, "expected name after '.'").text);
        }
        if (match(TokType::COLON)) {
            path.push_back(expect(TokType::NAME, "expected method name after ':'").text);
            is_method = true;
        }
        auto func = parseFunctionBody();
        if (is_method) func->params.insert(func->params.begin(), "self");
        auto s = std::make_unique<Stat>(); s->kind=StatKind::FUNCTION_DECL; s->line=line;
        s->names = path; s->func = func; s->is_method = is_method;
        return s;
    }

    std::shared_ptr<FunctionBody> parseFunctionBody() {
        auto func = std::make_shared<FunctionBody>();
        func->line_defined = peek().line;
        expect(TokType::LPAREN, "expected '(' after function name");
        if (!check(TokType::RPAREN)) {
            for (;;) {
                if (check(TokType::ELLIPSIS)) { advance(); func->is_vararg = true; break; }
                func->params.push_back(expect(TokType::NAME, "expected parameter name").text);
                if (!match(TokType::COMMA)) break;
            }
        }
        expect(TokType::RPAREN, "expected ')' after parameters");
        func->body = parseBlock();
        expect(TokType::KW_END, "expected 'end' to close function body");
        return func;
    }

    // statement yang diawali ekspresi: assignment atau call
    StatPtr parseExprStat() {
        int line = peek().line;
        ExprPtr first = parseSuffixedExpr();
        if (check(TokType::ASSIGN) || check(TokType::COMMA)) {
            auto s = std::make_unique<Stat>(); s->kind=StatKind::ASSIGN; s->line=line;
            s->lhs.push_back(std::move(first));
            while (match(TokType::COMMA)) s->lhs.push_back(parseSuffixedExpr());
            expect(TokType::ASSIGN, "expected '=' in assignment");
            s->exprs.push_back(parseExpr());
            while (match(TokType::COMMA)) s->exprs.push_back(parseExpr());
            return s;
        }
        if (first->kind != ExprKind::CALL && first->kind != ExprKind::METHODCALL) {
            throw ParseError("syntax error: expression as statement must be a function call", line);
        }
        auto s = std::make_unique<Stat>(); s->kind=StatKind::CALL_STAT; s->line=line;
        s->call_expr = std::move(first);
        return s;
    }

    // ---------------- Expression (precedence climbing) ----------------
    // Prioritas naik: or < and < cmp < concat < add/sub < mul/div/mod < unary < pow

    ExprPtr parseExpr() { return parseOr(); }

    ExprPtr parseOr() {
        auto e = parseAnd();
        while (check(TokType::KW_OR)) {
            int line = advance().line;
            auto rhs = parseAnd();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::OR; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); e=std::move(n);
        }
        return e;
    }
    ExprPtr parseAnd() {
        auto e = parseCmp();
        while (check(TokType::KW_AND)) {
            int line = advance().line;
            auto rhs = parseCmp();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::AND; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); e=std::move(n);
        }
        return e;
    }
    ExprPtr parseCmp() {
        auto e = parseConcat();
        for (;;) {
            BinOpKind op;
            if (check(TokType::EQ)) op=BinOpKind::EQ;
            else if (check(TokType::NE)) op=BinOpKind::NE;
            else if (check(TokType::LT)) op=BinOpKind::LT;
            else if (check(TokType::LE)) op=BinOpKind::LE;
            else if (check(TokType::GT)) op=BinOpKind::GT;
            else if (check(TokType::GE)) op=BinOpKind::GE;
            else break;
            int line = advance().line;
            auto rhs = parseConcat();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::BINOP; n->binop=op; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); e=std::move(n);
        }
        return e;
    }
    ExprPtr parseConcat() {
        auto e = parseAdd();
        if (check(TokType::DOTDOT)) { // right-assoc
            int line = advance().line;
            auto rhs = parseConcat();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::BINOP; n->binop=BinOpKind::CONCAT; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); return n;
        }
        return e;
    }
    ExprPtr parseAdd() {
        auto e = parseMul();
        for (;;) {
            BinOpKind op;
            if (check(TokType::PLUS)) op=BinOpKind::ADD;
            else if (check(TokType::MINUS)) op=BinOpKind::SUB;
            else break;
            int line = advance().line;
            auto rhs = parseMul();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::BINOP; n->binop=op; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); e=std::move(n);
        }
        return e;
    }
    ExprPtr parseMul() {
        auto e = parseUnary();
        for (;;) {
            BinOpKind op;
            if (check(TokType::STAR)) op=BinOpKind::MUL;
            else if (check(TokType::SLASH)) op=BinOpKind::DIV;
            else if (check(TokType::PERCENT)) op=BinOpKind::MOD;
            else break;
            int line = advance().line;
            auto rhs = parseUnary();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::BINOP; n->binop=op; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); e=std::move(n);
        }
        return e;
    }
    ExprPtr parseUnary() {
        if (check(TokType::KW_NOT) || check(TokType::MINUS) || check(TokType::HASH)) {
            UnOpKind op = check(TokType::KW_NOT) ? UnOpKind::NOT :
                          check(TokType::MINUS) ? UnOpKind::NEG : UnOpKind::LEN;
            int line = advance().line;
            auto operand = parseUnary();
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::UNOP; n->unop=op; n->line=line;
            n->a=std::move(operand);
            return n;
        }
        return parsePow();
    }
    ExprPtr parsePow() {
        auto e = parseSuffixedExpr();
        if (check(TokType::CARET)) { // right-assoc, precedence tertinggi
            int line = advance().line;
            auto rhs = parseUnary(); // pow mengikat lebih erat drpd unary di kanan: 2^-2 valid
            auto n = std::make_unique<Expr>(); n->kind=ExprKind::BINOP; n->binop=BinOpKind::POW; n->line=line;
            n->a=std::move(e); n->b=std::move(rhs); return n;
        }
        return e;
    }

    // primary + suffix (call, index, method call) berantai
    ExprPtr parseSuffixedExpr() {
        auto e = parsePrimary();
        for (;;) {
            if (check(TokType::DOT)) {
                int line = advance().line;
                std::string field = expect(TokType::NAME, "expected field name after '.'").text;
                auto n = std::make_unique<Expr>(); n->kind=ExprKind::INDEX; n->line=line;
                auto key = std::make_unique<Expr>(); key->kind=ExprKind::STRING; key->str=field; key->line=line;
                n->a=std::move(e); n->b=std::move(key); e=std::move(n);
            } else if (check(TokType::LBRACKET)) {
                int line = advance().line;
                auto key = parseExpr();
                expect(TokType::RBRACKET, "expected ']'");
                auto n = std::make_unique<Expr>(); n->kind=ExprKind::INDEX; n->line=line;
                n->a=std::move(e); n->b=std::move(key); e=std::move(n);
            } else if (check(TokType::COLON)) {
                int line = advance().line;
                std::string mname = expect(TokType::NAME, "expected method name after ':'").text;
                auto n = std::make_unique<Expr>(); n->kind=ExprKind::METHODCALL; n->line=line;
                n->method_name = mname; n->a=std::move(e);
                n->args = parseArgs();
                e=std::move(n);
            } else if (check(TokType::LPAREN) || check(TokType::STRING) || check(TokType::LBRACE)) {
                int line = peek().line;
                auto n = std::make_unique<Expr>(); n->kind=ExprKind::CALL; n->line=line;
                n->a=std::move(e);
                n->args = parseArgs();
                e=std::move(n);
            } else break;
        }
        return e;
    }

    std::vector<ExprPtr> parseArgs() {
        std::vector<ExprPtr> args;
        if (check(TokType::STRING)) {
            auto s = std::make_unique<Expr>(); s->kind=ExprKind::STRING; s->str=peek().text; s->line=peek().line;
            advance();
            args.push_back(std::move(s));
            return args;
        }
        if (check(TokType::LBRACE)) {
            args.push_back(parseTable());
            return args;
        }
        expect(TokType::LPAREN, "expected '(' for call arguments");
        if (!check(TokType::RPAREN)) {
            args.push_back(parseExpr());
            while (match(TokType::COMMA)) args.push_back(parseExpr());
        }
        expect(TokType::RPAREN, "expected ')' after arguments");
        return args;
    }

    ExprPtr parsePrimary() {
        int line = peek().line;
        switch (peek().type) {
            case TokType::KW_NIL: advance(); { auto n=std::make_unique<Expr>(); n->kind=ExprKind::NIL; n->line=line; return n; }
            case TokType::KW_TRUE: advance(); { auto n=std::make_unique<Expr>(); n->kind=ExprKind::TRUE; n->line=line; return n; }
            case TokType::KW_FALSE: advance(); { auto n=std::make_unique<Expr>(); n->kind=ExprKind::FALSE; n->line=line; return n; }
            case TokType::ELLIPSIS: advance(); { auto n=std::make_unique<Expr>(); n->kind=ExprKind::VARARG; n->line=line; return n; }
            case TokType::NUMBER: {
                auto n=std::make_unique<Expr>(); n->kind=ExprKind::NUMBER; n->num=peek().num; n->line=line;
                advance(); return n;
            }
            case TokType::STRING: {
                auto n=std::make_unique<Expr>(); n->kind=ExprKind::STRING; n->str=peek().text; n->line=line;
                advance(); return n;
            }
            case TokType::NAME: {
                auto n=std::make_unique<Expr>(); n->kind=ExprKind::NAME; n->str=peek().text; n->line=line;
                advance(); return n;
            }
            case TokType::LPAREN: {
                advance();
                auto inner = parseExpr();
                expect(TokType::RPAREN, "expected ')'");
                auto n=std::make_unique<Expr>(); n->kind=ExprKind::PAREN; n->line=line; n->a=std::move(inner);
                return n;
            }
            case TokType::LBRACE: return parseTable();
            case TokType::KW_FUNCTION: {
                advance();
                auto func = parseFunctionBody();
                auto n=std::make_unique<Expr>(); n->kind=ExprKind::FUNCTION; n->line=line; n->func=func;
                return n;
            }
            default:
                throw ParseError("unexpected token '" + peek().text + "' in expression", line);
        }
    }

    ExprPtr parseTable() {
        int line = peek().line;
        expect(TokType::LBRACE, "expected '{'");
        auto n = std::make_unique<Expr>(); n->kind=ExprKind::TABLE; n->line=line;
        while (!check(TokType::RBRACE)) {
            TableField f;
            if (check(TokType::LBRACKET)) {
                advance();
                f.key = parseExpr();
                expect(TokType::RBRACKET, "expected ']'");
                expect(TokType::ASSIGN, "expected '=' in table field");
                f.value = parseExpr();
            } else if (check(TokType::NAME) && peek(1).type == TokType::ASSIGN) {
                auto key = std::make_unique<Expr>(); key->kind=ExprKind::STRING; key->str=peek().text; key->line=peek().line;
                advance(); advance(); // name '='
                f.key = std::move(key);
                f.value = parseExpr();
            } else {
                f.value = parseExpr(); // array-style, key null
            }
            n->fields.push_back(std::move(f));
            if (!match(TokType::COMMA) && !match(TokType::SEMI)) break;
        }
        expect(TokType::RBRACE, "expected '}' to close table constructor");
        return n;
    }
};

} // namespace vmc
