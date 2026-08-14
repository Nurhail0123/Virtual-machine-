// ============================================================================
// bytecode.h — Format bytecode custom untuk Lua/Luau VM Obfuscator
// ============================================================================
// Instruksi di-encode 32-bit, mirip skema Lua asli tapi field & opcode
// diacak per-build lewat OPCODE_MAP (lihat obf_config.h, digenerate compiler).
//
// Layout instruksi (little endian, dibaca dari LSB):
//   [ 8 bit OP ][ 8 bit A ][ 8 bit B ][ 8 bit C ]   -> Format ABC
//   [ 8 bit OP ][ 8 bit A ][      16 bit Bx      ]   -> Format ABx
//   [ 8 bit OP ][ 8 bit A ][   16 bit sBx(signed) ]   -> Format AsBx
//
// Field A/B/C mengacu ke "register" (slot lokal di stack frame VM),
// Bx/sBx dipakai untuk index constant pool atau offset jump.
// ============================================================================
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <memory>

namespace vmc {

// ---- Opcode kanonik (SEBELUM diacak). Compiler emit opcode ini,
//      lalu obfuscation pass memetakan ke nilai acak per-build. ----
enum class OpCode : uint8_t {
    LOADK = 0,      // A Bx     : R[A] = K[Bx]
    LOADNIL,        // A        : R[A] = nil
    LOADBOOL,       // A B      : R[A] = (bool)B
    MOVE,           // A B      : R[A] = R[B]
    GETUPVAL,       // A B      : R[A] = Upval[B]
    SETUPVAL,       // A B      : Upval[B] = R[A]
    GETGLOBAL,      // A Bx     : R[A] = Global[K[Bx]]
    SETGLOBAL,      // A Bx     : Global[K[Bx]] = R[A]
    NEWTABLE,       // A        : R[A] = {}
    GETTABLE,       // A B C    : R[A] = R[B][R[C]]
    SETTABLE,       // A B C    : R[A][R[B]] = R[C]
    GETTABLEK,      // A B C    : R[A] = R[B][K[C]]   (field access literal, C = const index, max 255)
    SETTABLEK,      // A B C    : R[A][K[C]] = R[B]   (A=object reg, B=value reg, C=const index)
    ADD, SUB, MUL, DIV, MOD, POW, // A B C : R[A] = R[B] op R[C]
    UNM,            // A B      : R[A] = -R[B]
    NOT,            // A B      : R[A] = not R[B]
    LEN,            // A B      : R[A] = #R[B]
    CONCAT,         // A B C    : R[A] = R[B] .. .. R[C]  (rentang register)
    JMP,            // sBx      : pc += sBx
    EQ, LT, LE,     // A B C    : if (R[B] op R[C]) ~= A then pc++
    TEST,           // A C      : if bool(R[A]) ~= C then pc++
    CALL,           // A B C    : R[A..A+C-2] = R[A](R[A+1..A+B-1])
    RETURN,         // A B      : return R[A..A+B-2]
    CLOSURE,        // A Bx     : R[A] = closure(Proto[Bx], upvals...)
    VARARG,         // A B      : R[A..A+B-2] = ...
    FORPREP,        // A sBx    : siapkan numeric for, lompat ke FORLOOP
    FORLOOP,        // A sBx    : iterasi numeric for
    TFORLOOP,       // A C      : generic for (pairs/ipairs) satu langkah
    SETLIST,        // A B      : R[A][i] = R[A+i], batch table constructor
    SELF,           // A B C    : R[A+1]=R[B]; R[A]=R[B][K[C]]  (method call, C = const index, max 255)
    CLOSE,          // A        : tutup upvalue >= R[A] (keluar scope block)
    NOP,            // (junk instruction untuk obfuscation, no-op)
    OP_COUNT
};

enum class InstrFormat : uint8_t { ABC, ABx, AsBx };

struct Instruction {
    OpCode op;
    int32_t a = 0, b = 0, c = 0; // c juga dipakai sbg Bx/sBx tergantung format
    InstrFormat fmt = InstrFormat::ABC;
    int line = 0; // untuk error reporting, dibuang saat strip-debug

    static Instruction ABC_(OpCode op_, int a_, int b_, int c_, int line_) {
        Instruction i; i.op=op_; i.a=a_; i.b=b_; i.c=c_; i.fmt=InstrFormat::ABC; i.line=line_; return i;
    }
    static Instruction ABx_(OpCode op_, int a_, int bx_, int line_) {
        Instruction i; i.op=op_; i.a=a_; i.c=bx_; i.fmt=InstrFormat::ABx; i.line=line_; return i;
    }
    static Instruction AsBx_(OpCode op_, int a_, int sbx_, int line_) {
        Instruction i; i.op=op_; i.a=a_; i.c=sbx_; i.fmt=InstrFormat::AsBx; i.line=line_; return i;
    }
};

// ---- Constant pool value ----
enum class ConstType : uint8_t { NIL, BOOL, NUMBER, STRING };
struct Constant {
    ConstType type;
    bool     b = false;
    double   n = 0;
    std::string s;

    static Constant Nil()            { Constant c; c.type=ConstType::NIL; return c; }
    static Constant Bool(bool v)     { Constant c; c.type=ConstType::BOOL; c.b=v; return c; }
    static Constant Number(double v) { Constant c; c.type=ConstType::NUMBER; c.n=v; return c; }
    static Constant Str(std::string v){ Constant c; c.type=ConstType::STRING; c.s=std::move(v); return c; }
};

// Info upvalue: apakah menunjuk ke local di parent (stack) atau upvalue parent lagi
struct UpvalDesc {
    bool from_parent_stack; // true = local di enclosing function, false = upvalue enclosing
    int index;
    std::string name; // untuk debug, di-strip di build release
};

// ---- Proto = satu unit fungsi terkompilasi (mirip Proto di Lua asli) ----
struct Proto {
    std::vector<Instruction> code;
    std::vector<Constant> constants;
    std::vector<std::shared_ptr<Proto>> children; // nested function protos (untuk CLOSURE)
    std::vector<UpvalDesc> upvals;
    int num_params = 0;
    bool is_vararg = false;
    int max_stack_size = 2;
    std::string source_name;
    int line_defined = 0;
};

} // namespace vmc
