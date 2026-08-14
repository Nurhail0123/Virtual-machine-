--[[
============================================================================
vm_core.lua — Interpreter VM untuk bytecode custom, target runtime: Luau
============================================================================
BELUM DIEKSEKUSI DI RUNTIME LUAU ASLI. Ditulis mengikuti spesifikasi format
biner yang sudah diverifikasi lewat manual reader C++ terpisah (lihat
test/test_serializer.cpp, 31/31 test lulus). Perlu verifikasi end-to-end
oleh pengguna di Luau asli sebelum dipakai produksi.

Layout bytecode yang diasumsikan file ini (harus sinkron dgn serializer.h):
  Proto:
    u8  num_params
    u8  is_vararg (0/1)
    u8  max_stack_size
    u16 num_upvals
      repeat: u8 from_parent_stack(0/1), u16 index
    u16 num_constants
      repeat: u8 type(0=nil,1=bool,2=number,3=string), lalu payload sesuai type
        bool:   u8
        number: f64 (8 byte, little-endian IEEE754)
        string: u16 len, lalu `len` byte
    u16 num_instructions
      repeat: u8 opcode(SUDAH diremap acak), i32 a, i32 b, i32 c  (12 byte/instr + 1 opcode)
    u16 num_children
      repeat: <Proto anak, rekursif>

OPCODE_DECODE (byte acak -> index kanonik) di-generate per-build oleh
Serializer::generateOpcodeMapLua() dan di-prepend ke file ini saat packaging.
Untuk pengembangan/testing modul ini SENDIRIAN, dipakai identity map (byte==index).
============================================================================
]]

--------------------------------------------------------------------------
-- OPCODE_DECODE default (identity map) — DIGANTI oleh build packager
-- dengan tabel acak hasil OpcodeMap::generateOpcodeMapLua() saat obfuscation
-- sungguhan diterapkan. Index kanonik harus PERSIS urutan enum OpCode di
-- bytecode.h (dimulai dari 0).
--
-- CATATAN BUG YG SUDAH DIPERBAIKI: fallback identity map di bawah ini semula
-- di-hardcode `for i = 0, 34` (35 entri), padahal enum OpCode di bytecode.h
-- py 39 entri (0..38, terakhir NOP=38 sblm OP_COUNT). Opcode SELF(36),
-- CLOSE(37), NOP(38) hilang dari map, menyebabkan runtime error "opcode byte
-- N not found in OPCODE_DECODE map" begitu program memakai method call atau
-- keluar dari block scope (yg SELALU terjadi, krn compiler emit CLOSE implisit
-- di akhir tiap fungsi). Diperbaiki dgn menghitung batas SECARA DINAMIS dari
-- tabel OP di bawah (satu-satunya sumber kebenaran soal jumlah opcode),
-- bukan angka hardcode terpisah yg rawan tidak disinkronkan ulang.
--------------------------------------------------------------------------
local OPCODE_DECODE_OVERRIDE = OPCODE_DECODE

-- Index opcode kanonik (HARUS sinkron persis dgn enum class OpCode di bytecode.h)
local OP = {
    LOADK=0, LOADNIL=1, LOADBOOL=2, MOVE=3, GETUPVAL=4, SETUPVAL=5,
    GETGLOBAL=6, SETGLOBAL=7, NEWTABLE=8, GETTABLE=9, SETTABLE=10,
    GETTABLEK=11, SETTABLEK=12, ADD=13, SUB=14, MUL=15, DIV=16, MOD=17, POW=18,
    UNM=19, NOT=20, LEN=21, CONCAT=22, JMP=23, EQ=24, LT=25, LE=26, TEST=27,
    CALL=28, RETURN=29, CLOSURE=30, VARARG=31, FORPREP=32, FORLOOP=33,
    TFORLOOP=34, SETLIST=35, SELF=36, CLOSE=37, NOP=38,
}

-- Jumlah opcode dihitung DINAMIS dari tabel OP di atas (satu-satunya sumber
-- kebenaran), bukan angka hardcode terpisah — ini yg menghindari kelas bug
-- yg sama terulang lagi kalau OP bertambah entri di masa depan.
local OPCODE_COUNT = 0
for _ in pairs(OP) do OPCODE_COUNT = OPCODE_COUNT + 1 end

local OPCODE_DECODE = OPCODE_DECODE_OVERRIDE
if not OPCODE_DECODE then
    OPCODE_DECODE = {}
    for i = 0, OPCODE_COUNT - 1 do OPCODE_DECODE[i] = i end -- identity: byte N -> canonical N
end

--------------------------------------------------------------------------
-- Bytecode reader: baca Proto dari string biner (posisi 1-based ala Lua)
--------------------------------------------------------------------------
local Reader = {}
Reader.__index = Reader

function Reader.new(data)
    return setmetatable({ data = data, pos = 1 }, Reader)
end

function Reader:u8()
    local b = string.byte(self.data, self.pos)
    if b == nil then error("vm_core: unexpected end of bytecode stream at pos " .. self.pos) end
    self.pos = self.pos + 1
    return b
end

function Reader:u16()
    local lo = self:u8()
    local hi = self:u8()
    return lo + hi * 256
end

function Reader:i32()
    local b0 = self:u8()
    local b1 = self:u8()
    local b2 = self:u8()
    local b3 = self:u8()
    local u = b0 + b1 * 256 + b2 * 65536 + b3 * 16777216
    -- konversi unsigned 32-bit -> signed (two's complement)
    if u >= 2147483648 then u = u - 4294967296 end
    return u
end

function Reader:f64()
    -- Baca 8 byte little-endian IEEE754. Dipakai buffer.readf64 (Luau native library,
    -- eksplisit terdokumentasi little-endian: https://luau.org/library/#buffer-library),
    -- BUKAN string.unpack("<d",...) — dihindari krn format-string string.pack/unpack
    -- lebih rawan salah ketik & kurang predictable dibanding API buffer yg langsung
    -- menerima offset numerik.
    local bytes = string.sub(self.data, self.pos, self.pos + 7)
    if #bytes < 8 then error("vm_core: truncated f64 at pos " .. self.pos) end
    self.pos = self.pos + 8
    local ok, val = pcall(function()
        local buf = buffer.fromstring(bytes)
        return buffer.readf64(buf, 0)
    end)
    if not ok then
        error("vm_core: buffer.readf64 failed for f64 decode (is `buffer` library available in this Luau build?): " .. tostring(val))
    end
    return val
end

function Reader:str()
    local len = self:u16()
    if len == 0 then return "" end
    local s = string.sub(self.data, self.pos, self.pos + len - 1)
    if #s < len then error("vm_core: truncated string at pos " .. self.pos .. " (wanted " .. len .. " got " .. #s .. ")") end
    self.pos = self.pos + len
    return s
end

--------------------------------------------------------------------------
-- Deserialize satu Proto (rekursif utk children)
--------------------------------------------------------------------------
local function readProto(r)
    local proto = {}
    proto.num_params = r:u8()
    proto.is_vararg = r:u8() == 1
    proto.max_stack_size = r:u8()

    local num_upvals = r:u16()
    proto.upvals = {}
    for i = 1, num_upvals do
        local from_parent_stack = r:u8() == 1
        local index = r:u16()
        proto.upvals[i] = { from_parent_stack = from_parent_stack, index = index }
    end

    local num_const = r:u16()
    proto.constants = {}
    for i = 1, num_const do
        local ctype = r:u8()
        if ctype == 0 then
            proto.constants[i] = { t = "nil" }
        elseif ctype == 1 then
            proto.constants[i] = { t = "bool", v = (r:u8() == 1) }
        elseif ctype == 2 then
            proto.constants[i] = { t = "number", v = r:f64() }
        elseif ctype == 3 then
            proto.constants[i] = { t = "string", v = r:str() }
        else
            error("vm_core: unknown constant type tag " .. tostring(ctype))
        end
    end

    local num_instr = r:u16()
    proto.code = {}
    for i = 1, num_instr do
        local raw_op = r:u8()
        local canonical = OPCODE_DECODE[raw_op]
        if canonical == nil then
            error("vm_core: opcode byte " .. raw_op .. " not found in OPCODE_DECODE map (corrupted bytecode or mismatched build)")
        end
        local a = r:i32()
        local b = r:i32()
        local c = r:i32()
        proto.code[i] = { op = canonical, a = a, b = b, c = c }
    end

    local num_children = r:u16()
    proto.children = {}
    for i = 1, num_children do
        proto.children[i] = readProto(r)
    end

    return proto
end

local function loadBytecode(data)
    local r = Reader.new(data)
    return readProto(r)
end

--------------------------------------------------------------------------
-- Runtime value helpers
--------------------------------------------------------------------------
local function constToLua(k)
    if k.t == "nil" then return nil
    elseif k.t == "bool" then return k.v
    elseif k.t == "number" then return k.v
    elseif k.t == "string" then return k.v
    end
    error("vm_core: unknown constant tag " .. tostring(k.t))
end

--------------------------------------------------------------------------
-- Closure representation:
--   { proto = <Proto>, upvals = { [i] = <Cell> } }
-- Cell (utk upvalue): { get = function() end, set = function(v) end }
-- Local yg "closed over" oleh closure anak direpresentasikan sbg Cell agar
-- mutasi terlihat dua arah (baca vm_core Test closure/upvalue di compiler
-- test suite C++ utk semantik yg harus dijaga persis).
--------------------------------------------------------------------------

local function makeCell(initial)
    local val = initial
    return {
        get = function() return val end,
        set = function(v) val = v end,
    }
end

--------------------------------------------------------------------------
-- Frame eksekusi
--------------------------------------------------------------------------
local function execProto(proto, closure, args, globals)
    -- register file: array 1-based; index register kanonik R[n] (0-based dari compiler)
    -- dipetakan ke reg[n+1] di Lua.
    local reg = {}
    local cells = {}  -- cells[n+1] = Cell utk register n JIKA register itu sudah di-"close" jadi upvalue anak
    local varargs = {}
    local nvarargs = 0

    for i = 1, proto.num_params do
        reg[i] = args[i]
    end
    if proto.is_vararg then
        local n = #args
        for i = proto.num_params + 1, n do
            varargs[#varargs + 1] = args[i]
        end
        nvarargs = #varargs
    end

    local function R(n) -- baca register n (0-based canonical)
        if cells[n + 1] then return cells[n + 1].get() end
        return reg[n + 1]
    end
    local function setR(n, v) -- tulis register n
        if cells[n + 1] then cells[n + 1].set(v); return end
        reg[n + 1] = v
    end
    local function cellFor(n) -- dapatkan/buat Cell utk register n (dipakai closure anak sbg upvalue)
        if not cells[n + 1] then
            cells[n + 1] = makeCell(reg[n + 1])
        end
        return cells[n + 1]
    end

    local code = proto.code
    local K = proto.constants
    local pc = 1
    local top = proto.num_params -- "top of stack" runtime, dipakai utk CALL/RETURN multret (B=0 / C=0)

    while true do
        local ins = code[pc]
        if not ins then
            -- fallthrough tanpa RETURN eksplisit (seharusnya tidak terjadi krn compiler
            -- selalu emit RETURN implisit di akhir setiap Proto, tapi dijaga di sini
            -- supaya gagal jelas alih-alih baca instruksi nil secara diam-diam)
            error("vm_core: instruction pointer ran off the end of code (pc=" .. pc .. ") without RETURN")
        end
        local op = ins.op

        if op == OP.LOADK then
            setR(ins.a, constToLua(K[ins.c + 1]))
            pc = pc + 1

        elseif op == OP.LOADNIL then
            setR(ins.a, nil)
            pc = pc + 1

        elseif op == OP.LOADBOOL then
            setR(ins.a, ins.b == 1)
            pc = pc + 1

        elseif op == OP.MOVE then
            setR(ins.a, R(ins.b))
            pc = pc + 1

        elseif op == OP.GETUPVAL then
            local uv = closure.upvals[ins.b + 1]
            setR(ins.a, uv.get())
            pc = pc + 1

        elseif op == OP.SETUPVAL then
            local uv = closure.upvals[ins.b + 1]
            uv.set(R(ins.a))
            pc = pc + 1

        elseif op == OP.GETGLOBAL then
            local name = constToLua(K[ins.c + 1])
            setR(ins.a, globals[name])
            pc = pc + 1

        elseif op == OP.SETGLOBAL then
            local name = constToLua(K[ins.c + 1])
            globals[name] = R(ins.a)
            pc = pc + 1

        elseif op == OP.NEWTABLE then
            setR(ins.a, {})
            pc = pc + 1

        elseif op == OP.GETTABLE then
            local t = R(ins.b)
            if t == nil then error("vm_core: attempt to index a nil value (register " .. ins.b .. ")") end
            setR(ins.a, t[R(ins.c)])
            pc = pc + 1

        elseif op == OP.SETTABLE then
            local t = R(ins.a)
            if t == nil then error("vm_core: attempt to index a nil value (register " .. ins.a .. ")") end
            t[R(ins.b)] = R(ins.c)
            pc = pc + 1

        elseif op == OP.GETTABLEK then
            local t = R(ins.b)
            if t == nil then error("vm_core: attempt to index a nil value (register " .. ins.b .. ")") end
            setR(ins.a, t[constToLua(K[ins.c + 1])])
            pc = pc + 1

        elseif op == OP.SETTABLEK then
            local t = R(ins.a)
            if t == nil then error("vm_core: attempt to index a nil value (register " .. ins.a .. ")") end
            t[constToLua(K[ins.c + 1])] = R(ins.b)
            pc = pc + 1

        elseif op == OP.ADD then setR(ins.a, R(ins.b) + R(ins.c)); pc = pc + 1
        elseif op == OP.SUB then setR(ins.a, R(ins.b) - R(ins.c)); pc = pc + 1
        elseif op == OP.MUL then setR(ins.a, R(ins.b) * R(ins.c)); pc = pc + 1
        elseif op == OP.DIV then setR(ins.a, R(ins.b) / R(ins.c)); pc = pc + 1
        elseif op == OP.MOD then setR(ins.a, R(ins.b) % R(ins.c)); pc = pc + 1
        elseif op == OP.POW then setR(ins.a, R(ins.b) ^ R(ins.c)); pc = pc + 1

        elseif op == OP.UNM then setR(ins.a, -R(ins.b)); pc = pc + 1
        elseif op == OP.NOT then setR(ins.a, not R(ins.b)); pc = pc + 1
        elseif op == OP.LEN then setR(ins.a, #R(ins.b)); pc = pc + 1
        elseif op == OP.CONCAT then setR(ins.a, tostring(R(ins.b)) .. tostring(R(ins.c))); pc = pc + 1

        elseif op == OP.JMP then
            pc = pc + 1 + ins.c

        elseif op == OP.EQ then
            -- Pola compiler (lihat emitBoolFromCompare di compiler.h):
            --   [CMP expected][JMP->false][LOADBOOL true][JMP->skip][LOADBOOL false]
            -- BUG SEBELUMNYA (logika terbalik, ditemukan lewat eksekusi nyata — skrip
            -- `hello.lua` awal kebetulan tidak pernah mencetak hasil comparison scr
            -- langsung sehingga bug ini luput dari audit manual sebelumnya):
            -- Kalau hasil comparison COCOK dgn expected, itu berarti harus SKIP instruksi
            -- JMP-ke-false (loncat pc+2, LANGSUNG ke LOADBOOL true) — BUKAN pc+1 spt versi
            -- lama. pc+1 (jatuh ke JMP) hanya benar utk kasus TIDAK cocok (JMP membawa ke
            -- LOADBOOL false).
            local expected = ins.a == 1
            if (R(ins.b) == R(ins.c)) == expected then pc = pc + 2 else pc = pc + 1 end

        elseif op == OP.LT then
            local expected = ins.a == 1
            if (R(ins.b) < R(ins.c)) == expected then pc = pc + 2 else pc = pc + 1 end

        elseif op == OP.LE then
            local expected = ins.a == 1
            if (R(ins.b) <= R(ins.c)) == expected then pc = pc + 2 else pc = pc + 1 end

        elseif op == OP.TEST then
            -- if bool(R[A]) ~= C then pc++ (skip instruksi berikutnya, biasanya JMP)
            local truthy = R(ins.a) and true or false
            local want = ins.c == 1
            if truthy ~= want then pc = pc + 2 else pc = pc + 1 end

        elseif op == OP.CALL then
            local fn = R(ins.a)
            if type(fn) ~= "function" then
                -- Fallback ke metamethod __call (standar Lua: table/userdata dgn
                -- metatable __call bisa dipanggil spt fungsi). Ditemukan hilang
                -- lewat eksekusi nyata (callable(3,4) gagal "attempt to call a
                -- table value" meski metatable __call sudah benar disetel).
                local mt = getmetatable(fn)
                local callHandler = mt and mt.__call
                if type(callHandler) == "function" then
                    -- __call menerima OBJEK itu sendiri sbg argumen pertama (self),
                    -- diikuti argumen asli. Kita ganti `fn` jadi wrapper yg menyisipkan
                    -- objek asli, supaya alur CALL di bawah (baca args, panggil fn,
                    -- simpan hasil) tidak perlu tahu soal __call sama sekali.
                    local originalObj = fn
                    fn = function(...) return callHandler(originalObj, ...) end
                else
                    error("vm_core: attempt to call a " .. type(fn) .. " value (register " .. ins.a .. ")")
                end
            end
            local nargs
            local callargs = {}
            if ins.b == 0 then
                -- multret dari args terakhir: baca sampai `top` runtime
                nargs = top - ins.a
                for i = 1, nargs do callargs[i] = R(ins.a + i) end
            else
                nargs = ins.b - 1
                for i = 1, nargs do callargs[i] = R(ins.a + i) end
            end
            local results = { fn(table.unpack(callargs, 1, nargs)) }
            local nresults = #results
            if ins.c == 0 then
                -- caller minta "semua hasil" -> simpan mulai R[A], update top
                for i = 1, nresults do setR(ins.a + i - 1, results[i]) end
                top = ins.a + nresults - 1
            else
                local want = ins.c - 1
                for i = 1, want do setR(ins.a + i - 1, results[i]) end
            end
            pc = pc + 1

        elseif op == OP.RETURN then
            local nret
            local out = {}
            if ins.b == 0 then
                nret = top - ins.a + 1
                for i = 1, nret do out[i] = R(ins.a + i - 1) end
            else
                nret = ins.b - 1
                for i = 1, nret do out[i] = R(ins.a + i - 1) end
            end
            return table.unpack(out, 1, nret)

        elseif op == OP.CLOSURE then
            local childProto = proto.children[ins.c + 1]
            local upvals = {}
            for i, uvdesc in ipairs(childProto.upvals) do
                if uvdesc.from_parent_stack then
                    upvals[i] = cellFor(uvdesc.index)
                else
                    upvals[i] = closure.upvals[uvdesc.index + 1]
                end
            end
            local childClosure = { proto = childProto, upvals = upvals }
            setR(ins.a, function(...)
                return execProto(childProto, childClosure, { ... }, globals)
            end)
            pc = pc + 1

        elseif op == OP.VARARG then
            local want = ins.b == 0 and nvarargs or (ins.b - 1)
            for i = 1, want do setR(ins.a + i - 1, varargs[i]) end
            if ins.b == 0 then top = ins.a + nvarargs - 1 end
            pc = pc + 1

        elseif op == OP.FORPREP then
            -- R[A]=start R[A+1]=stop R[A+2]=step, R[A+3]=var terlihat user
            local start = R(ins.a)
            local stop = R(ins.a + 1)
            local step = R(ins.a + 2)
            if type(start) ~= "number" or type(stop) ~= "number" or type(step) ~= "number" then
                error("vm_core: 'for' initial value, limit, or step must be a number")
            end
            if step == 0 then error("vm_core: 'for' step is zero") end
            setR(ins.a + 3, start - step) -- akan di-+step di FORLOOP pertama
            pc = pc + 1 + ins.c

        elseif op == OP.FORLOOP then
            local step = R(ins.a + 2)
            local newval = R(ins.a + 3) + step
            local stop = R(ins.a + 1)
            local continue_loop
            if step > 0 then continue_loop = newval <= stop else continue_loop = newval >= stop end
            if continue_loop then
                setR(ins.a + 3, newval)
                pc = pc + 1 + ins.c
            else
                pc = pc + 1
            end

        elseif op == OP.TFORLOOP then
            -- base: R[A]=iterator_fn R[A+1]=state R[A+2]=control; var results ditaruh R[A+3..]
            --
            -- Semantik (PERBAIKAN KE-3 — 2 percobaan sebelumnya masih salah krn "skip 1
            -- instruksi" ambigu ketika body_start kebetulan bertetangga langsung dgn JMP
            -- masuk-body, membuat pc+2 & pc+1(lewat JMP) mendarat di alamat yg SAMA):
            --
            -- TFORLOOP sekarang membawa OFFSET EKSPLISIT C (sBx-style, spt JMP/FORLOOP)
            -- yg menunjuk ke TITIK KELUAR LOOP (posisi SETELAH seluruh struktur for,
            -- bukan sekadar "instruksi setelahnya"). Ini pola yg SAMA dgn FORPREP/FORLOOP
            -- (numeric-for) yg sudah terbukti benar lewat eksekusi nyata sebelumnya.
            --   - HASIL ADA: isi var register, pc+1 (lanjut ke body_start, yg SELALU
            --     ditempatkan tepat setelah TFORLOOP oleh compiler, tanpa JMP perantara).
            --   - HASIL NIL: pc = pc + 1 + C (lompat LANGSUNG ke titik keluar loop yg
            --     sesungguhnya, terlepas dari sepanjang apa body-nya).
            local iter_fn = R(ins.a)
            local state = R(ins.a + 1)
            local control = R(ins.a + 2)
            local results = { iter_fn(state, control) }
            if results[1] == nil then
                pc = pc + 1 + ins.c -- lompat ke titik keluar loop (offset eksplisit)
            else
                setR(ins.a + 2, results[1]) -- control var diupdate ke hasil pertama
                for i = 1, ins.b do setR(ins.a + 2 + i, results[i]) end
                pc = pc + 1 -- lanjut normal ke body_start (selalu tepat setelah TFORLOOP)
            end

        elseif op == OP.SETLIST then
            -- R[A][C], R[A][C+1], ... = R[A+1], R[A+2], ...  (n = top-A elemen)
            -- C = array_index AWAL dari compiler (posisi berapa elemen multret ini mulai
            -- menempati array; BUKAN selalu 1, krn elemen literal sebelumnya di table
            -- constructor sudah menempati posisi 1..C-1 lewat SETTABLEK terpisah).
            -- Bug sebelumnya: field C diabaikan & loop selalu mulai t[1], menimpa elemen
            -- literal yg sudah ditulis (mis. {1,2,f()} menimpa index 1,2 alih2 mengisi 3,4).
            local t = R(ins.a)
            local n = top - ins.a
            local start_index = ins.c
            for i = 1, n do t[start_index + i - 1] = R(ins.a + i) end
            pc = pc + 1

        elseif op == OP.SELF then
            local obj = R(ins.b)
            if obj == nil then error("vm_core: attempt to index a nil value (method call, register " .. ins.b .. ")") end
            setR(ins.a + 1, obj)
            setR(ins.a, obj[constToLua(K[ins.c + 1])])
            pc = pc + 1

        elseif op == OP.CLOSE then
            -- lepas cell mulai register ins.a ke atas (keluar scope block); Cell yg sudah
            -- di-capture closure anak TETAP hidup (Lua GC via closure), hanya reg lokal
            -- di frame ini yg berhenti alias ke cell tsb.
            for i = ins.a + 1, proto.max_stack_size do
                cells[i] = nil
            end
            pc = pc + 1

        elseif op == OP.NOP then
            pc = pc + 1

        else
            error("vm_core: unimplemented opcode index " .. tostring(op) .. " at pc=" .. pc)
        end
    end
end

--------------------------------------------------------------------------
-- Entry point publik
--------------------------------------------------------------------------
local M = {}

-- Jalankan bytecode (string biner) sbg chunk top-level. `globals` opsional,
-- default memakai _G asli (getfenv/rawget di Luau berbeda dgn Lua 5.1 -> pakai _G langsung).
function M.run(bytecodeData, globals, ...)
    globals = globals or _G
    local rootProto = loadBytecode(bytecodeData)
    local rootClosure = { proto = rootProto, upvals = {} }
    return execProto(rootProto, rootClosure, { ... }, globals)
end

-- Diekspos utk keperluan testing/debug modul ini secara terpisah
M._internal = {
    loadBytecode = loadBytecode,
    execProto = execProto,
    Reader = Reader,
    OP = OP,
}

return M
