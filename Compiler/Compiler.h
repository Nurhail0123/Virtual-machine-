// ============================================================================
// compiler.h — AST -> Bytecode, dengan resolusi local/upvalue/global
// ============================================================================
// Model register: setiap FunctionState punya "register stack" virtual.
// Local variable dipetakan ke slot register tetap selama scope hidup.
// Ekspresi sementara memakai register di atas local yang aktif (freereg_).
//
// Upvalue resolution mengikuti algoritma standar Lua: saat NAME tidak
// ditemukan sbg local di fungsi ini, cari di enclosing function secara
// rekursif; kalau ketemu di parent, buat upvalue chain (bisa nested).
// ============================================================================
#pragma once
#include "ast.h"
#include "bytecode.h"
#include <unordered_map>
#include <stdexcept>

namespace vmc {

class CompileError : public std::runtime_error {
public:
    CompileError(const std::string& msg, int line)
        : std::runtime_error("compile error [line " + std::to_string(line) + "]: " + msg) {}
};

struct LocalVar {
    std::string name;
    int reg;
};

struct BlockScope {
    int local_count_at_entry; // untuk pop locals saat keluar block
    bool is_loop;
    std::vector<int> break_jumps; // index instruksi JMP yang perlu di-patch ke akhir loop
};

// State per-fungsi yang sedang dikompilasi (satu Proto)
struct FunctionState {
    FunctionState* parent = nullptr;
    std::shared_ptr<Proto> proto = std::make_shared<Proto>();
    std::vector<LocalVar> locals; // stack local aktif (sesuai scope saat ini)
    std::vector<BlockScope> blocks;
    int freereg = 0; // register bebas berikutnya (di atas semua local aktif)
    std::unordered_map<std::string, int> const_cache; // dedup constant string/number

    int reserveReg() {
        int r = freereg++;
        if (freereg > proto->max_stack_size) proto->max_stack_size = freereg;
        return r;
    }
};

class Compiler {
public:
    // Kompilasi chunk top-level jadi Proto akar (vararg implisit, seperti main chunk Lua)
    std::shared_ptr<Proto> compileChunk(const std::vector<StatPtr>& chunk) {
        FunctionState fs;
        fs.proto->is_vararg = true;
        fs.proto->num_params = 0;
        fs_ = &fs;
        pushBlock(false);
        compileBlock(chunk);
        // pastikan ada RETURN di akhir (implicit return kalau tidak eksplisit)
        emitABC(OpCode::RETURN, 0, 1, 0, 0);
        popBlock();
        return fs.proto;
    }

private:
    FunctionState* fs_ = nullptr;

    // ---------------- Emit helpers ----------------
    int emitABC(OpCode op, int a, int b, int c, int line) {
        fs_->proto->code.push_back(Instruction::ABC_(op, a, b, c, line));
        return (int)fs_->proto->code.size() - 1;
    }
    int emitABx(OpCode op, int a, int bx, int line) {
        fs_->proto->code.push_back(Instruction::ABx_(op, a, bx, line));
        return (int)fs_->proto->code.size() - 1;
    }
    int emitAsBx(OpCode op, int a, int sbx, int line) {
        fs_->proto->code.push_back(Instruction::AsBx_(op, a, sbx, line));
        return (int)fs_->proto->code.size() - 1;
    }
    void patchJumpToHere(int instr_index) {
        int here = (int)fs_->proto->code.size();
        fs_->proto->code[instr_index].c = here - instr_index - 1;
    }
    void patchJumpTo(int instr_index, int target) {
        fs_->proto->code[instr_index].c = target - instr_index - 1;
    }

    int addConstant(Constant c) {
        std::string key;
        if (c.type == ConstType::STRING) key = "s:" + c.s;
        else if (c.type == ConstType::NUMBER) key = "n:" + std::to_string(c.n);
        else if (c.type == ConstType::BOOL) key = std::string("b:") + (c.b?"1":"0");
        else key = "nil";
        auto it = fs_->const_cache.find(key);
        if (it != fs_->const_cache.end()) return it->second;
        fs_->proto->constants.push_back(c);
        int idx = (int)fs_->proto->constants.size() - 1;
        fs_->const_cache[key] = idx;
        return idx;
    }

    // GETTABLEK/SETTABLEK menaruh constant index di field C (ABC format, 8-bit -> maks 255).
    // Field access literal (obj.foo) yang melebihi 255 constant unik per-fungsi memakai
    // GETTABLE/SETTABLE biasa (lewat register) sbg fallback, supaya tidak overflow diam-diam.
    static constexpr int kMaxInlineConstIndex = 255;
    bool constFitsInlineC(int idx) const { return idx <= kMaxInlineConstIndex; }

    // emit GETTABLEK dgn fallback ke GETTABLE (lewat register sementara) jika const index > 255
    void emitGetField(int dest, int obj_reg, const std::string& field_name, int line) {
        int k = addConstant(Constant::Str(field_name));
        if (constFitsInlineC(k)) { emitABC(OpCode::GETTABLEK, dest, obj_reg, k, line); return; }
        int save = fs_->freereg;
        int key_reg = fs_->reserveReg();
        emitABx(OpCode::LOADK, key_reg, k, line);
        emitABC(OpCode::GETTABLE, dest, obj_reg, key_reg, line);
        fs_->freereg = save;
    }
    // emit SETTABLEK dgn fallback serupa. obj_reg=target table, val_reg=nilai yg disimpan.
    void emitSetField(int obj_reg, int val_reg, const std::string& field_name, int line) {
        emitSetConstKey(obj_reg, val_reg, Constant::Str(field_name), line);
    }
    // Versi general: key bisa Constant apa pun (string field ATAU number array-index).
    void emitSetConstKey(int obj_reg, int val_reg, Constant key, int line) {
        int k = addConstant(std::move(key));
        if (constFitsInlineC(k)) { emitABC(OpCode::SETTABLEK, obj_reg, val_reg, k, line); return; }
        int save = fs_->freereg;
        int key_reg = fs_->reserveReg();
        emitABx(OpCode::LOADK, key_reg, k, line);
        emitABC(OpCode::SETTABLE, obj_reg, key_reg, val_reg, line);
        fs_->freereg = save;
    }

    // ---------------- Scope management ----------------
    void pushBlock(bool is_loop) {
        BlockScope b; b.local_count_at_entry = (int)fs_->locals.size(); b.is_loop = is_loop;
        fs_->blocks.push_back(b);
    }
    void popBlock() {
        BlockScope b = fs_->blocks.back();
        fs_->blocks.pop_back();
        // lepas local yg keluar scope; freereg turun kembali
        int target = b.local_count_at_entry;
        if ((int)fs_->locals.size() > target) {
            int first_freed_reg = fs_->locals[target].reg;
            fs_->locals.resize(target);
            fs_->freereg = first_freed_reg;
            // CLOSE menutup upvalue yg merujuk ke register yg baru dilepas (penting utk closure benar dlm loop)
            emitABC(OpCode::CLOSE, first_freed_reg, 0, 0, 0);
        }
    }
    int declareLocal(const std::string& name) {
        int reg = fs_->reserveReg();
        fs_->locals.push_back({name, reg});
        return reg;
    }
    // Cari local by name di fungsi tertentu (scan mundur agar shadowing benar)
    int findLocal(FunctionState* fs, const std::string& name) {
        for (int i = (int)fs->locals.size() - 1; i >= 0; i--)
            if (fs->locals[i].name == name) return fs->locals[i].reg;
        return -1;
    }
    // Cari/buat upvalue di fungsi fs yg merujuk ke `name`, cari rekursif ke parent.
    // Return index upvalue di fs->proto->upvals, atau -1 jika name adalah global.
    int findOrAddUpval(FunctionState* fs, const std::string& name) {
        if (!fs->parent) return -1; // top-level chunk, tidak punya parent -> pasti global
        // sudah ada upvalue dgn nama ini?
        for (size_t i = 0; i < fs->proto->upvals.size(); i++)
            if (fs->proto->upvals[i].name == name) return (int)i;
        // coba local di parent
        int preg = findLocal(fs->parent, name);
        if (preg >= 0) {
            UpvalDesc uv; uv.from_parent_stack = true; uv.index = preg; uv.name = name;
            fs->proto->upvals.push_back(uv);
            return (int)fs->proto->upvals.size() - 1;
        }
        // coba upvalue di parent (nested closure lebih dalam)
        int puv = findOrAddUpval(fs->parent, name);
        if (puv >= 0) {
            UpvalDesc uv; uv.from_parent_stack = false; uv.index = puv; uv.name = name;
            fs->proto->upvals.push_back(uv);
            return (int)fs->proto->upvals.size() - 1;
        }
        return -1; // global
    }

    // ---------------- Statement compile ----------------
    void compileBlock(const std::vector<StatPtr>& stats) {
        for (auto& s : stats) compileStat(*s);
    }

    void compileStat(Stat& s) {
        switch (s.kind) {
            case StatKind::LOCAL: return compileLocal(s);
            case StatKind::ASSIGN: return compileAssign(s);
            case StatKind::CALL_STAT: {
                int save = fs_->freereg;
                compileExprToNextReg(*s.call_expr, /*want_results=*/0);
                fs_->freereg = save;
                return;
            }
            case StatKind::IF: return compileIf(s);
            case StatKind::WHILE: return compileWhile(s);
            case StatKind::NUMERIC_FOR: return compileNumericFor(s);
            case StatKind::GENERIC_FOR: return compileGenericFor(s);
            case StatKind::FUNCTION_DECL: return compileFunctionDecl(s);
            case StatKind::LOCAL_FUNCTION: return compileLocalFunction(s);
            case StatKind::RETURN: return compileReturn(s);
            case StatKind::BREAK: return compileBreak(s);
            case StatKind::DO: {
                pushBlock(false);
                compileBlock(s.body);
                popBlock();
                return;
            }
            case StatKind::REPEAT: return compileRepeat(s);
        }
    }

    void compileLocal(Stat& s) {
        // Kasus umum: 1 nama = 1 ekspresi non-multret (mayoritas kode nyata).
        // Di sini RHS bisa dikompilasi LANGSUNG ke register yg akan jadi local final,
        // tanpa MOVE, SELAMA urutan evaluasi tetap benar: RHS dievaluasi memakai
        // binding LAMA dari nama yg sama (semantik "local x = x" harus baca x lama).
        // Ini aman karena declareLocal() hanya mendaftarkan nama ke fs_->locals SETELAH
        // compileExprInto selesai jalan — jadi findLocal() selama evaluasi RHS masih
        // menemukan x lama (kalau ada), persis seperti sebelumnya, hanya saja tanpa
        // register perantara yg terbuang.
        if (s.names.size() == 1 && s.exprs.size() == 1 &&
            s.exprs[0]->kind != ExprKind::CALL && s.exprs[0]->kind != ExprKind::METHODCALL &&
            s.exprs[0]->kind != ExprKind::VARARG) {
            int reg = fs_->reserveReg();       // slot local final direservasi duluan...
            compileExprInto(*s.exprs[0], reg, 1); // ...tapi NAME lookup di dalamnya masih pre-deklarasi
            fs_->locals.push_back({s.names[0], reg}); // baru SEKARANG nama terdaftar
            return;
        }
        // Kasus umum (multi-assign, atau RHS call/vararg yg butuh semantik multret):
        // tetap evaluasi ke register sementara dulu, karena jumlah nilai riil dari
        // CALL/VARARG baru diketahui saat itu emit, sehingga tidak bisa dijamin
        // register final duluan seperti kasus di atas.
        int save = fs_->freereg;
        std::vector<int> val_regs;
        assignExprListToRegs(s.exprs, (int)s.names.size(), val_regs);
        for (size_t i = 0; i < s.names.size(); i++) {
            int reg = declareLocal(s.names[i]);
            if (reg != val_regs[i]) emitABC(OpCode::MOVE, reg, val_regs[i], 0, s.line);
        }
        fs_->freereg = std::max(fs_->freereg, save + (int)s.names.size());
    }

    // Evaluasi list ekspresi (dgn aturan multret utk elemen TERAKHIR jika CALL/VARARG),
    // hasil ditaruh di register berurutan mulai freereg saat ini. out_regs diisi index register per nama.
    void assignExprListToRegs(const std::vector<ExprPtr>& exprs, int want_count, std::vector<int>& out_regs) {
        int base = fs_->freereg;
        if (exprs.empty()) {
            for (int i = 0; i < want_count; i++) {
                int r = fs_->reserveReg();
                emitABC(OpCode::LOADNIL, r, 0, 0, 0);
                out_regs.push_back(r);
            }
            return;
        }
        for (size_t i = 0; i + 1 < exprs.size(); i++) {
            int r = compileExprToNextReg(*exprs[i], 1);
            out_regs.push_back(r);
        }
        // elemen terakhir: kalau want_count belum terpenuhi & expr terakhir CALL/VARARG, minta multret
        int remaining = want_count - (int)out_regs.size();
        if (remaining < 1) remaining = 1;
        auto& last = *exprs.back();
        if (last.kind == ExprKind::CALL || last.kind == ExprKind::METHODCALL || last.kind == ExprKind::VARARG) {
            int r = compileExprToNextReg(last, remaining);
            for (int i = 0; i < remaining; i++) out_regs.push_back(r + i);
        } else {
            int r = compileExprToNextReg(last, 1);
            out_regs.push_back(r);
            for (int i = 1; i < remaining; i++) {
                int rr = fs_->reserveReg();
                emitABC(OpCode::LOADNIL, rr, 0, 0, 0);
                out_regs.push_back(rr);
            }
        }
        // pad/truncate agar out_regs.size() == want_count
        while ((int)out_regs.size() < want_count) {
            int rr = fs_->reserveReg();
            emitABC(OpCode::LOADNIL, rr, 0, 0, 0);
            out_regs.push_back(rr);
        }
        out_regs.resize(want_count);
        (void)base;
    }

    void compileAssign(Stat& s) {
        int save = fs_->freereg;
        std::vector<int> val_regs;
        assignExprListToRegs(s.exprs, (int)s.lhs.size(), val_regs);
        for (size_t i = 0; i < s.lhs.size(); i++) storeToTarget(*s.lhs[i], val_regs[i]);
        fs_->freereg = save;
    }

    // Simpan nilai dari register `val_reg` ke target assignment (NAME atau INDEX)
    void storeToTarget(Expr& target, int val_reg) {
        if (target.kind == ExprKind::NAME) {
            int lreg = findLocal(fs_, target.str);
            if (lreg >= 0) { if (lreg != val_reg) emitABC(OpCode::MOVE, lreg, val_reg, 0, target.line); return; }
            int uv = findOrAddUpval(fs_, target.str);
            if (uv >= 0) { emitABC(OpCode::SETUPVAL, val_reg, uv, 0, target.line); return; }
            int k = addConstant(Constant::Str(target.str));
            emitABx(OpCode::SETGLOBAL, val_reg, k, target.line);
            return;
        }
        if (target.kind == ExprKind::INDEX) {
            int save = fs_->freereg;
            int obj_reg = compileExprToNextReg(*target.a, 1);
            if (target.b->kind == ExprKind::STRING) {
                emitSetField(obj_reg, val_reg, target.b->str, target.line);
            } else {
                int key_reg = compileExprToNextReg(*target.b, 1);
                emitABC(OpCode::SETTABLE, obj_reg, key_reg, val_reg, target.line);
            }
            fs_->freereg = save;
            return;
        }
        throw CompileError("invalid assignment target", target.line);
    }

    void compileIf(Stat& s) {
        std::vector<int> end_jumps;
        for (size_t i = 0; i < s.clauses.size(); i++) {
            auto& clause = s.clauses[i];
            if (clause.cond) {
                int save = fs_->freereg;
                int cond_reg = compileExprToNextReg(*clause.cond, 1);
                fs_->freereg = save;
                // TEST kontrak: if bool(R[A]) ~= C then pc+2 else pc+1 (lihat OP.TEST di VM).
                // Body ada TEPAT SETELAH instruksi JMP-exit berikutnya; supaya cond=true
                // membuat eksekusi SKIP JMP-exit itu (masuk body, hasil pc+2), C harus
                // BERNILAI BEDA dari cond=true, yaitu C=0 (false) — BUKAN C=1 spt versi lama.
                // BUG SEBELUMNYA (C=1) membuat cond=true justru JATUH ke JMP-exit (keluar/
                // skip body), kebalikan dari yang dimaksud — luput dari audit manual krn
                // skrip test awal (hello.lua) kebetulan tidak pernah memanifestasikannya
                // scr terlihat, tertutup jg oleh bug EQ/LT/LE yg sudah diperbaiki terpisah.
                int test = emitABC(OpCode::TEST, cond_reg, 0, 0, clause.body.empty()?0:0);
                int jmp_over = emitAsBx(OpCode::JMP, 0, 0, 0);
                (void)test;
                pushBlock(false);
                compileBlock(clause.body);
                popBlock();
                if (i + 1 < s.clauses.size()) {
                    int jend = emitAsBx(OpCode::JMP, 0, 0, 0);
                    end_jumps.push_back(jend);
                }
                patchJumpToHere(jmp_over);
            } else {
                // else block
                pushBlock(false);
                compileBlock(clause.body);
                popBlock();
            }
        }
        for (int j : end_jumps) patchJumpToHere(j);
    }

    void compileWhile(Stat& s) {
        int loop_start = (int)fs_->proto->code.size();
        int save = fs_->freereg;
        int cond_reg = compileExprToNextReg(*s.cond, 1);
        fs_->freereg = save;
        // Sama spt compileIf: C=0 (bukan C=1) supaya cond=true SKIP JMP-exit (masuk body).
        emitABC(OpCode::TEST, cond_reg, 0, 0, 0);
        int jmp_exit = emitAsBx(OpCode::JMP, 0, 0, 0);
        pushBlock(true);
        compileBlock(s.body);
        int loop_end_jump = emitAsBx(OpCode::JMP, 0, 0, 0);
        patchJumpTo(loop_end_jump, loop_start);
        for (int bj : fs_->blocks.back().break_jumps) patchJumpToHere(bj);
        popBlock();
        patchJumpToHere(jmp_exit);
    }

    void compileRepeat(Stat& s) {
        int loop_start = (int)fs_->proto->code.size();
        pushBlock(true);
        compileBlock(s.body);
        // cond dievaluasi MASIH dlm scope body (bisa akses local body) — sesuai semantik Lua
        int save = fs_->freereg;
        int cond_reg = compileExprToNextReg(*s.cond, 1);
        fs_->freereg = save;
        emitABC(OpCode::TEST, cond_reg, 0, 0, 0); // until true -> keluar; jadi skip jmp-back kalau true
        int back = emitAsBx(OpCode::JMP, 0, 0, 0);
        patchJumpTo(back, loop_start);
        for (int bj : fs_->blocks.back().break_jumps) patchJumpToHere(bj);
        popBlock();
    }

    void compileBreak(Stat& s) {
        // cari block loop terdekat, catat jump utk dipatch saat loop selesai
        for (int i = (int)fs_->blocks.size() - 1; i >= 0; i--) {
            if (fs_->blocks[i].is_loop) {
                int j = emitAsBx(OpCode::JMP, 0, 0, s.line);
                fs_->blocks[i].break_jumps.push_back(j);
                return;
            }
        }
        throw CompileError("'break' outside loop", s.line);
    }

    void compileNumericFor(Stat& s) {
        int save = fs_->freereg;
        int start_reg = compileExprToNextReg(*s.for_start, 1);
        compileExprToNextReg(*s.for_stop, 1); // start_reg+1
        if (s.for_step) compileExprToNextReg(*s.for_step, 1);
        else { int r = fs_->reserveReg(); int k = addConstant(Constant::Number(1)); emitABx(OpCode::LOADK, r, k, s.line); }
        // start_reg, start_reg+1(stop), start_reg+2(step) = kontrol; start_reg+3 = var loop yg terlihat user
        int prep = emitAsBx(OpCode::FORPREP, start_reg, 0, s.line);
        pushBlock(true);
        int loop_var_reg = fs_->reserveReg();
        fs_->locals.push_back({s.for_var, loop_var_reg});
        compileBlock(s.body);
        int loop_reg_after = fs_->freereg;
        (void)loop_reg_after;
        int loop_instr = emitAsBx(OpCode::FORLOOP, start_reg, 0, s.line);
        patchJumpTo(loop_instr, prep + 1);
        // FORPREP harus melompat TEPAT ke instruksi FORLOOP (supaya FORLOOP yg mengevaluasi
        // kondisi loop pertama kali), BUKAN ke posisi setelahnya. patchJumpToHere() memakai
        // "posisi sekarang" yg SUDAH lewat loop_instr krn loop_instr br saja di-emit di atas —
        // maka di sini dipakai patchJumpTo(prep, loop_instr) eksplisit, bukan patchJumpToHere(prep).
        patchJumpTo(prep, loop_instr);
        for (int bj : fs_->blocks.back().break_jumps) patchJumpToHere(bj);
        popBlock();
        fs_->freereg = save;
    }

    void compileGenericFor(Stat& s) {
        int save = fs_->freereg;
        // for k,v in EXPLIST do — EXPLIST menghasilkan iterator func, state, control (3 slot)
        std::vector<int> iter_regs;
        assignExprListToRegs(s.for_exprs, 3, iter_regs);
        int base = iter_regs[0]; // asumsi 3 register berurutan (dijamin oleh assignExprListToRegs)
        pushBlock(true);
        std::vector<int> var_regs;
        for (auto& name : s.for_names) {
            int r = fs_->reserveReg();
            fs_->locals.push_back({name, r});
            var_regs.push_back(r);
        }
        // STRUKTUR (perbaikan ke-3, mengikuti pola FORPREP/FORLOOP yg sudah terbukti
        // benar: TFORLOOP membawa offset C EKSPLISIT menuju titik KELUAR loop):
        //   test_pos: TFORLOOP base,nvars,C  -- C diisi belakangan, menuju EXIT (bukan body).
        //             HASIL ADA -> pc+1 (body_start, SELALU menempel tepat setelah ini,
        //             TANPA JMP perantara). HASIL NIL -> pc+1+C (loncat ke EXIT).
        //   body_start: body block (menempel LANGSUNG setelah TFORLOOP, TIDAK ada JMP
        //               masuk-body terpisah lagi — sumber bug percobaan sebelumnya).
        //   back: JMP kembali ke test_pos.
        //   EXIT: posisi tepat setelah `back` — di sinilah C di-patch menuju, dan di
        //         sinilah break_jumps juga mendarat (exit loop dari break sama dgn exit
        //         loop dari iterator selesai, keduanya titik yg sama persis).
        int test_pos = emitABC(OpCode::TFORLOOP, base, (int)s.for_names.size(), 0, s.line);
        int body_start = (int)fs_->proto->code.size();
        compileBlock(s.body);
        int back = emitAsBx(OpCode::JMP, 0, 0, s.line);
        patchJumpTo(back, test_pos);
        patchJumpToHere(test_pos); // C TFORLOOP -> posisi SEKARANG (tepat setelah `back`) = EXIT
        (void)body_start;
        for (int bj : fs_->blocks.back().break_jumps) patchJumpToHere(bj);
        popBlock();
        fs_->freereg = save;
    }

    void compileReturn(Stat& s) {
        if (s.exprs.empty()) { emitABC(OpCode::RETURN, 0, 1, 0, s.line); return; }
        int save = fs_->freereg;
        std::vector<int> regs;
        // return mendukung multret di elemen terakhir; want_count = -1 artinya "as many as available"
        int base = fs_->freereg;
        for (size_t i = 0; i + 1 < s.exprs.size(); i++) compileExprToNextReg(*s.exprs[i], 1);
        auto& last = *s.exprs.back();
        int b_field;
        if (last.kind == ExprKind::CALL || last.kind == ExprKind::METHODCALL || last.kind == ExprKind::VARARG) {
            compileExprToNextReg(last, -1); // -1 = multret, B=0 di CALL berarti "sampai top stack"
            b_field = 0; // 0 berarti "sampai top", VM baca dari base s/d top runtime
        } else {
            compileExprToNextReg(last, 1);
            b_field = (int)s.exprs.size() + 1;
        }
        emitABC(OpCode::RETURN, base, b_field, 0, s.line);
        fs_->freereg = save;
        (void)regs;
    }

    void compileFunctionDecl(Stat& s) {
        // function a.b.c(...) end  ==  a.b.c = function(...) end
        // function a:m(...) end    ==  a.m = function(self,...) end  (self sdh disisipkan parser)
        auto funcExpr = std::make_unique<Expr>();
        funcExpr->kind = ExprKind::FUNCTION;
        funcExpr->func = s.func;
        funcExpr->line = s.line;

        if (s.names.size() == 1) {
            auto target = std::make_unique<Expr>();
            target->kind = ExprKind::NAME; target->str = s.names[0]; target->line = s.line;
            int save = fs_->freereg;
            int vreg = compileExprToNextReg(*funcExpr, 1);
            storeToTarget(*target, vreg);
            fs_->freereg = save;
            return;
        }
        // bangun rantai INDEX: names[0].names[1]. ... .names[n-1]
        ExprPtr obj = std::make_unique<Expr>();
        obj->kind = ExprKind::NAME; obj->str = s.names[0]; obj->line = s.line;
        for (size_t i = 1; i + 1 < s.names.size(); i++) {
            auto idx = std::make_unique<Expr>();
            idx->kind = ExprKind::INDEX; idx->line = s.line;
            auto key = std::make_unique<Expr>(); key->kind=ExprKind::STRING; key->str=s.names[i]; key->line=s.line;
            idx->a = std::move(obj); idx->b = std::move(key);
            obj = std::move(idx);
        }
        auto finalTarget = std::make_unique<Expr>();
        finalTarget->kind = ExprKind::INDEX; finalTarget->line = s.line;
        auto finalKey = std::make_unique<Expr>(); finalKey->kind=ExprKind::STRING; finalKey->str=s.names.back(); finalKey->line=s.line;
        finalTarget->a = std::move(obj); finalTarget->b = std::move(finalKey);

        int save = fs_->freereg;
        int vreg = compileExprToNextReg(*funcExpr, 1);
        storeToTarget(*finalTarget, vreg);
        fs_->freereg = save;
    }

    void compileLocalFunction(Stat& s) {
        // local function f() ... end : deklarasikan f DULU (sblm compile body) agar f bisa rekursif
        int reg = declareLocal(s.names[0]);
        auto funcExpr = std::make_unique<Expr>();
        funcExpr->kind = ExprKind::FUNCTION; funcExpr->func = s.func; funcExpr->line = s.line;
        int vreg = compileExprToNextReg(*funcExpr, 1);
        if (vreg != reg) emitABC(OpCode::MOVE, reg, vreg, 0, s.line);
    }

    // ---------------- Expression compile ----------------
    // Kompilasi expr, hasil diletakkan mulai register fs_->freereg (lalu freereg naik).
    // want_results: 1 = single value (default), -1 = multret (CALL/VARARG), N = N value eksplisit.
    // Return: register basis hasil.
    int compileExprToNextReg(Expr& e, int want_results) {
        // CATATAN: sempat ada optimisasi di sini utk NAME->local yg langsung return
        // register local tanpa reserveReg(), guna menghindari MOVE berlebihan di
        // compileLocal. Itu DIBATALKAN karena melanggar invariant yg dipegang SELURUH
        // compiler: setiap panggilan compileExprToNextReg WAJIB menaikkan fs_->freereg
        // tepat 1. BINOP (dan banyak tempat lain) memanggil compileExprToNextReg utk
        // operand kiri lalu kanan back-to-back dan mengasumsikan keduanya mendapat
        // register BERBEDA justru krn freereg naik di antara dua panggilan itu. Ketika
        // operand kiri berupa NAME lokal dan "pintas" tsb aktif, freereg tidak naik,
        // sehingga operand kanan bisa dialokasikan ke register yg SAMA dgn tujuan akhir
        // (dest ekspresi BINOP itu sendiri) dan menimpa nilai sebelum sempat dipakai
        // (lihat: `local y = x + 1` menghasilkan ADD dari dua salinan literal `1`,
        // bukan `x + 1`, krn operand kanan menimpa slot yg jadi tujuan hasil BINOP).
        // Optimisasi MOVE-berlebih yg lebih aman dilakukan di level pemanggil yg TAHU
        // konteksnya (mis. compileLocal versi 1-nama-1-ekspresi), BUKAN di sini.
        int r = fs_->reserveReg();
        compileExprInto(e, r, want_results);
        return r;
    }

    void compileExprInto(Expr& e, int dest, int want_results) {
        switch (e.kind) {
            case ExprKind::NIL: emitABC(OpCode::LOADNIL, dest, 0, 0, e.line); return;
            case ExprKind::TRUE: emitABC(OpCode::LOADBOOL, dest, 1, 0, e.line); return;
            case ExprKind::FALSE: emitABC(OpCode::LOADBOOL, dest, 0, 0, e.line); return;
            case ExprKind::NUMBER: { int k = addConstant(Constant::Number(e.num)); emitABx(OpCode::LOADK, dest, k, e.line); return; }
            case ExprKind::STRING: { int k = addConstant(Constant::Str(e.str)); emitABx(OpCode::LOADK, dest, k, e.line); return; }
            case ExprKind::VARARG: emitABC(OpCode::VARARG, dest, want_results<0?0:want_results+1, 0, e.line); return;
            case ExprKind::PAREN: { compileExprInto(*e.a, dest, 1); return; } // paren memaksa 1 nilai
            case ExprKind::NAME: {
                int lreg = findLocal(fs_, e.str);
                if (lreg >= 0) { if (lreg != dest) emitABC(OpCode::MOVE, dest, lreg, 0, e.line); return; }
                int uv = findOrAddUpval(fs_, e.str);
                if (uv >= 0) { emitABC(OpCode::GETUPVAL, dest, uv, 0, e.line); return; }
                int k = addConstant(Constant::Str(e.str));
                emitABx(OpCode::GETGLOBAL, dest, k, e.line);
                return;
            }
            case ExprKind::INDEX: {
                int save = fs_->freereg;
                int obj_reg = compileExprToNextReg(*e.a, 1);
                if (e.b->kind == ExprKind::STRING) {
                    emitGetField(dest, obj_reg, e.b->str, e.line);
                } else {
                    int key_reg = compileExprToNextReg(*e.b, 1);
                    emitABC(OpCode::GETTABLE, dest, obj_reg, key_reg, e.line);
                }
                fs_->freereg = save;
                if (dest >= fs_->freereg) fs_->freereg = dest + 1;
                return;
            }
            case ExprKind::AND: {
                // a and b: kalau a truthy, HARUS eval b (short-circuit hanya saat a falsy).
                // Pola [TEST][JMP skip][eval b][skip:] — a truthy harus SKIP JMP (masuk eval
                // b, pc+2); a falsy harus JATUH ke JMP (skip eval b, pc+1). Kontrak TEST:
                // pc+2 terjadi saat truthy(R[A])~=C. a truthy ingin pc+2 -> C harus BEDA dari
                // true, yaitu C=0. (Bug yg sama polanya dgn if/while, C=1 sebelumnya salah.)
                compileExprInto(*e.a, dest, 1);
                emitABC(OpCode::TEST, dest, 0, 0, e.line);
                int skip = emitAsBx(OpCode::JMP, 0, 0, e.line);
                compileExprInto(*e.b, dest, 1);
                patchJumpToHere(skip);
                return;
            }
            case ExprKind::OR: {
                // a or b: kalau a truthy, SKIP eval b (short-circuit); kalau a falsy, eval b.
                // a truthy ingin JATUH ke JMP (skip eval b, pc+1) -> truthy(true)~=C harus
                // FALSE (supaya masuk cabang else:pc+1) -> C harus SAMA dgn true -> C=1.
                compileExprInto(*e.a, dest, 1);
                emitABC(OpCode::TEST, dest, 0, 1, e.line);
                int skip = emitAsBx(OpCode::JMP, 0, 0, e.line);
                compileExprInto(*e.b, dest, 1);
                patchJumpToHere(skip);
                return;
            }
            case ExprKind::UNOP: {
                int save = fs_->freereg;
                int a_reg = compileExprToNextReg(*e.a, 1);
                OpCode op = e.unop==UnOpKind::NEG ? OpCode::UNM : e.unop==UnOpKind::NOT ? OpCode::NOT : OpCode::LEN;
                emitABC(op, dest, a_reg, 0, e.line);
                fs_->freereg = save;
                if (dest >= fs_->freereg) fs_->freereg = dest + 1;
                return;
            }
            case ExprKind::BINOP: {
                if (e.binop == BinOpKind::NE || e.binop == BinOpKind::GT || e.binop == BinOpKind::GE) {
                    // NE = not EQ; GT(a,b) = LT(b,a); GE(a,b) = LE(b,a)  — kurangi jumlah opcode unik
                    compileComparisonNormalized(e, dest);
                    return;
                }
                int save = fs_->freereg;
                int a_reg = compileExprToNextReg(*e.a, 1);
                int b_reg = compileExprToNextReg(*e.b, 1);
                OpCode op;
                switch (e.binop) {
                    case BinOpKind::ADD: op=OpCode::ADD; break;
                    case BinOpKind::SUB: op=OpCode::SUB; break;
                    case BinOpKind::MUL: op=OpCode::MUL; break;
                    case BinOpKind::DIV: op=OpCode::DIV; break;
                    case BinOpKind::MOD: op=OpCode::MOD; break;
                    case BinOpKind::POW: op=OpCode::POW; break;
                    case BinOpKind::CONCAT: op=OpCode::CONCAT; break;
                    case BinOpKind::EQ: op=OpCode::EQ; break;
                    case BinOpKind::LT: op=OpCode::LT; break;
                    case BinOpKind::LE: op=OpCode::LE; break;
                    default: throw CompileError("unreachable binop", e.line);
                }
                if (op==OpCode::EQ || op==OpCode::LT || op==OpCode::LE) {
                    emitBoolFromCompare(op, a_reg, b_reg, dest, e.line, false);
                } else {
                    emitABC(op, dest, a_reg, b_reg, e.line);
                }
                fs_->freereg = save;
                if (dest >= fs_->freereg) fs_->freereg = dest + 1;
                return;
            }
            case ExprKind::CALL: return compileCall(e, dest, want_results, false);
            case ExprKind::METHODCALL: return compileCall(e, dest, want_results, true);
            case ExprKind::FUNCTION: return compileClosure(e, dest);
            case ExprKind::TABLE: return compileTable(e, dest);
        }
    }

    void compileComparisonNormalized(Expr& e, int dest) {
        int save = fs_->freereg;
        int a_reg = compileExprToNextReg(*e.a, 1);
        int b_reg = compileExprToNextReg(*e.b, 1);
        switch (e.binop) {
            case BinOpKind::NE: emitBoolFromCompare(OpCode::EQ, a_reg, b_reg, dest, e.line, true); break;
            case BinOpKind::GT: emitBoolFromCompare(OpCode::LT, b_reg, a_reg, dest, e.line, false); break;
            case BinOpKind::GE: emitBoolFromCompare(OpCode::LE, b_reg, a_reg, dest, e.line, false); break;
            default: throw CompileError("unreachable", e.line);
        }
        fs_->freereg = save;
        if (dest >= fs_->freereg) fs_->freereg = dest + 1;
    }

    // pola standar Lua utk comparison->boolean: EQ/LT/LE pakai "skip next if not match" + LOADBOOL x2
    void emitBoolFromCompare(OpCode cmpOp, int a_reg, int b_reg, int dest, int line, bool negate) {
        emitABC(cmpOp, negate?0:1, a_reg, b_reg, line); // A field = expected truthiness sblm skip
        int jf = emitAsBx(OpCode::JMP, 0, 0, line);
        emitABC(OpCode::LOADBOOL, dest, 1, 1, line); // true, skip next
        int je = emitAsBx(OpCode::JMP, 0, 0, line);
        patchJumpToHere(jf);
        emitABC(OpCode::LOADBOOL, dest, 0, 0, line); // false
        patchJumpToHere(je);
    }

    void compileCall(Expr& e, int dest, int want_results, bool is_method) {
        int save = fs_->freereg;
        // Panggilan HARUS: fn di register dest, args berurutan sesudahnya (utk encoding CALL standar).
        // Kontrak sama seperti seluruh compileExprInto: `dest` SUDAH direserve oleh caller
        // (lewat compileExprToNextReg atau alur lain) — di sini kita hanya perlu memastikan
        // freereg tidak lebih RENDAH dari dest+1 (fn butuh minimal slot dest), bukan menganggap
        // freereg > dest+1 sbg error, karena itu situasi normal saat dest bukan register teratas
        // (mis. dest adalah slot argumen di panggilan luar yang sedang dibangun berlapis).
        int fn_reg = dest;
        if (fs_->freereg < fn_reg + 1) fs_->freereg = fn_reg + 1;

        int arg_count = 0;
        if (!is_method) {
            // Panggilan biasa fn(args): compile callee (e.a) LANGSUNG ke fn_reg.
            // Ini yang sebelumnya HILANG — tanpa baris ini fn_reg tidak pernah diisi
            // nilai fungsi yang sebenarnya (mis. GETGLOBAL utk `print`), sehingga CALL
            // memanggil register kosong/sampah alih-alih fungsi yang dimaksud.
            compileExprInto(*e.a, fn_reg, 1);
        }
        if (is_method) {
            // Evaluasi objek ke register SEMENTARA (bukan fn_reg+1 langsung), karena SELF
            // sendiri yang bertanggung jawab menyalin objek ke fn_reg+1 sesuai kontrak instruksi:
            //   SELF A B C  =>  R[A+1] = R[B] ; R[A] = R[B][K[C]]
            // Kalau objek dievaluasi langsung ke fn_reg+1 lalu SELF juga menulis ke fn_reg+1
            // dari sumber R[B] yg SAMA dgn fn_reg+1, hasilnya kebetulan benar utk kasus sederhana
            // tapi rusak begitu evaluasi objek (e.a) sendiri butuh register di atas fn_reg+1
            // (mis. objek adalah hasil pemanggilan fungsi lain) — maka dipisah tegas di sini.
            int obj_reg = compileExprToNextReg(*e.a, 1); // register bebas, BUKAN fn_reg+1
            int k = addConstant(Constant::Str(e.method_name));
            if (!constFitsInlineC(k)) {
                // method name konstanta index > 255: fallback GETTABLE manual + siapkan self
                int key_reg = fs_->reserveReg();
                emitABx(OpCode::LOADK, key_reg, k, e.line);
                emitABC(OpCode::MOVE, fn_reg + 1, obj_reg, 0, e.line);
                emitABC(OpCode::GETTABLE, fn_reg, obj_reg, key_reg, e.line);
            } else {
                emitABC(OpCode::SELF, fn_reg, obj_reg, k, e.line);
            }
            fs_->freereg = fn_reg + 2; // fn_reg=method, fn_reg+1=self; args mulai fn_reg+2
            arg_count = 1; // self terhitung sbg argumen pertama
        }
        bool last_is_multret = false;
        for (size_t i = 0; i < e.args.size(); i++) {
            bool is_last = (i + 1 == e.args.size());
            auto& arg = *e.args[i];
            if (is_last && (arg.kind == ExprKind::CALL || arg.kind == ExprKind::METHODCALL || arg.kind == ExprKind::VARARG)) {
                compileExprToNextReg(arg, -1);
                last_is_multret = true;
            } else {
                compileExprToNextReg(arg, 1);
                arg_count++;
            }
        }
        int b_field = last_is_multret ? 0 : (arg_count + 1);
        int c_field = (want_results < 0) ? 0 : (want_results + 1);
        emitABC(OpCode::CALL, fn_reg, b_field, c_field, e.line);
        fs_->freereg = save;
        // PENTING: hasil CALL menempati register fn_reg..fn_reg+N-1 (N=jumlah hasil yg
        // DIMINTA, want_results). Kalau N>1 (mis. want_results=3 utk generic-for iterator
        // setup: iter_fn,state,control), register fn_reg+1 dan fn_reg+2 HARUS tetap dianggap
        // "terpakai" setelah CALL — TIDAK boleh direset ke `save` yg lebih rendah, karena
        // caller (mis. compileGenericFor) akan mengalokasikan register BERIKUTNYA berdasarkan
        // freereg saat ini, dan kalau freereg turun terlalu jauh, register alokasi berikutnya
        // akan bertabrakan dgn slot hasil CALL yg belum sempat "dibaca" pemanggil.
        // Untuk want_results<0 (multret tak terbatas, C=0), jumlah hasil riil BARU diketahui
        // saat runtime, sehingga freereg TIDAK bisa dipastikan statis di sini — kasus itu
        // ditangani oleh pemanggil yg sudah tahu konteksnya (compileReturn pakai B=0 khusus,
        // assignExprListToRegs sudah menangani elemen terakhir CALL/VARARG secara terpisah
        // dan TIDAK memanggil compileExprToNextReg dgn want_results<0 di jalur yg freereg-nya
        // dibutuhkan lagi setelahnya tanpa penyesuaian).
        int min_freereg_after_call = fn_reg + (want_results > 0 ? want_results : 1);
        if (fs_->freereg < min_freereg_after_call) fs_->freereg = min_freereg_after_call;
        if (dest >= fs_->freereg) fs_->freereg = dest + 1;
    }

    void compileClosure(Expr& e, int dest) {
        FunctionState child;
        child.parent = fs_;
        child.proto->num_params = (int)e.func->params.size();
        child.proto->is_vararg = e.func->is_vararg;
        child.proto->line_defined = e.func->line_defined;

        FunctionState* saved = fs_;
        fs_ = &child;
        pushBlock(false);
        for (auto& p : e.func->params) declareLocal(p);
        compileBlock(e.func->body);
        emitABC(OpCode::RETURN, 0, 1, 0, 0); // implicit return
        popBlock();
        fs_ = saved;

        fs_->proto->children.push_back(child.proto);
        int child_idx = (int)fs_->proto->children.size() - 1;
        emitABx(OpCode::CLOSURE, dest, child_idx, e.line);
        if (dest >= fs_->freereg) fs_->freereg = dest + 1;
    }

    void compileTable(Expr& e, int dest) {
        emitABC(OpCode::NEWTABLE, dest, 0, 0, e.line);
        if (dest >= fs_->freereg) fs_->freereg = dest + 1;
        int save = fs_->freereg;
        int array_index = 1;
        for (size_t i = 0; i < e.fields.size(); i++) {
            auto& f = e.fields[i];
            if (f.key) {
                int vreg = compileExprToNextReg(*f.value, 1);
                if (f.key->kind == ExprKind::STRING) {
                    emitSetField(dest, vreg, f.key->str, e.line);
                } else {
                    int kreg = compileExprToNextReg(*f.key, 1);
                    emitABC(OpCode::SETTABLE, dest, kreg, vreg, e.line);
                }
                fs_->freereg = save;
            } else {
                bool is_last = (i + 1 == e.fields.size());
                if (is_last && (f.value->kind == ExprKind::CALL || f.value->kind == ExprKind::METHODCALL || f.value->kind == ExprKind::VARARG)) {
                    compileExprToNextReg(*f.value, -1);
                    emitABC(OpCode::SETLIST, dest, 0, array_index, e.line); // B=0 -> pakai sampai top stack
                    fs_->freereg = save;
                } else {
                    int vreg = compileExprToNextReg(*f.value, 1);
                    emitSetConstKey(dest, vreg, Constant::Number((double)array_index), e.line);
                    array_index++;
                    fs_->freereg = save;
                }
            }
        }
    }
};

} // namespace vmc
