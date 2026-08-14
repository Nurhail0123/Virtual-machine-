// ============================================================================
// ast.h — Node AST untuk subset Lua/Luau yang didukung
// ============================================================================
#pragma once
#include <string>
#include <vector>
#include <memory>

namespace vmc {

struct Expr; struct Stat;
using ExprPtr = std::unique_ptr<Expr>;
using StatPtr = std::unique_ptr<Stat>;

// ---------------- Expression ----------------
enum class ExprKind {
    NIL, TRUE, FALSE, NUMBER, STRING, VARARG,
    NAME,                 // identifier (resolusi local/upval/global terjadi di compiler)
    INDEX,                // obj[key]  atau  obj.key (key jadi STRING const)
    CALL,                 // fn(args...)
    METHODCALL,           // obj:method(args...)
    FUNCTION,              // function(params) ... end  (closure literal)
    TABLE,                 // { ... } constructor
    BINOP, UNOP,
    AND, OR,
    PAREN,                 // (expr) — memaksa single-value, redam multret
};

enum class BinOpKind { ADD,SUB,MUL,DIV,MOD,POW,CONCAT,EQ,NE,LT,LE,GT,GE };
enum class UnOpKind { NEG, NOT, LEN };

struct TableField {
    ExprPtr key;   // null => array-style (auto index)
    ExprPtr value;
};

struct FunctionBody {
    std::vector<std::string> params;
    bool is_vararg = false;
    std::vector<StatPtr> body;
    int line_defined = 0;
};

struct Expr {
    ExprKind kind;
    int line = 0;

    double num = 0;
    std::string str;                 // STRING literal / NAME identifier

    ExprPtr a, b;                    // operand umum (INDEX: a=obj,b=key; BINOP: a,b)
    BinOpKind binop{};
    UnOpKind unop{};

    std::vector<ExprPtr> args;       // CALL/METHODCALL arguments
    std::string method_name;         // METHODCALL nama method

    std::shared_ptr<FunctionBody> func; // FUNCTION
    std::vector<TableField> fields;     // TABLE
};

// ---------------- Statement ----------------
enum class StatKind {
    LOCAL,          // local a,b = e1,e2
    ASSIGN,         // lhs1,lhs2 = e1,e2
    CALL_STAT,      // panggilan sbg statement berdiri sendiri
    IF,
    WHILE,
    NUMERIC_FOR,    // for i=a,b,c do ... end
    GENERIC_FOR,    // for k,v in pairs(t) do ... end
    FUNCTION_DECL,  // function name(...) ... end  (gula utk assign)
    LOCAL_FUNCTION,  // local function name(...) ... end
    RETURN,
    BREAK,
    DO,             // do ... end (scope block murni)
    REPEAT,         // repeat ... until cond
};

struct IfClause {
    ExprPtr cond; // null utk else terakhir
    std::vector<StatPtr> body;
};

struct Stat {
    StatKind kind;
    int line = 0;

    std::vector<std::string> names;   // LOCAL: nama var; FUNCTION_DECL/LOCAL_FUNCTION: path/nama
    std::vector<ExprPtr> lhs;         // ASSIGN: target
    std::vector<ExprPtr> exprs;       // RHS umum (LOCAL/ASSIGN/RETURN values)

    ExprPtr call_expr;                // CALL_STAT

    std::vector<IfClause> clauses;    // IF (termasuk elseif, else = cond null)

    ExprPtr cond;                     // WHILE/REPEAT
    std::vector<StatPtr> body;        // WHILE/REPEAT/DO/FOR body

    // numeric for
    std::string for_var;
    ExprPtr for_start, for_stop, for_step;

    // generic for
    std::vector<std::string> for_names;
    std::vector<ExprPtr> for_exprs;

    std::shared_ptr<FunctionBody> func; // FUNCTION_DECL / LOCAL_FUNCTION
    bool is_method = false;             // FUNCTION_DECL: function obj:name() -> implicit self
};

} // namespace vmc
