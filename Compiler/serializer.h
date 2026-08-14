// ============================================================================
// serializer.h — Proto (C++) -> stream biner yang dibaca vm_core.lua
// ============================================================================
// Layout per Proto (little-endian, semua integer 32-bit kecuali disebutkan):
//   [u8  num_params]
//   [u8  is_vararg]        (0/1)
//   [u8  max_stack_size]
//   [u16 num_upvals]
//   repeat num_upvals:
//     [u8  from_parent_stack] (0/1)
//     [u16 index]
//   [u16 num_constants]
//   repeat num_constants:
//     [u8 type]  0=nil 1=bool(u8 next) 2=number(f64 next) 3=string(u16 len + bytes)
//   [u16 num_instructions]
//   repeat num_instructions:
//     [u8  opcode]  <- SUDAH diremap lewat OpcodeMap sebelum ditulis (obfuscation)
//     [i32 a][i32 b][i32 c]   (untuk ABx/AsBx, b diisi 0 & c = nilai Bx/sBx asli)
//   [u16 num_children]
//   repeat num_children: <Proto anak, rekursif>
//
// Field b/c untuk instruksi non-ABC boros 4 byte (bisa dipadatkan nanti);
// untuk versi kerja awal ini, kejelasan/kemudahan-debug diprioritaskan
// di atas ukuran file, karena file bytecode akan di-obfuscate/dikompres
// terpisah di tahap selanjutnya.
// ============================================================================
#pragma once
#include "bytecode.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <array>
#include <algorithm>

namespace vmc {

// Peta OpCode kanonik -> byte acak unik per-build. Compiler & VM harus
// memakai OpcodeMap yang SAMA (map ditulis ke header file terpisah yg
// di-embed ke source VM Lua saat build, lihat generateOpcodeMapLua()).
class OpcodeMap {
public:
    // identity=true: byte == index kanonik (0,1,2,...), TIDAK acak. Dipakai utk testing/
    // debugging supaya bytecode gampang dibaca manual & cocok dgn OPCODE_DECODE fallback
    // bawaan di vm_core.lua tanpa perlu embed map terpisah. identity=false (default utk
    // build obfuscation sungguhan): permutasi acak berbasis `seed`, WAJIB diikuti embed
    // generateOpcodeMapLua() ke source VM, krn vm_core.lua TIDAK BISA menebak mapping acak.
    explicit OpcodeMap(uint32_t seed, bool identity = false) {
        std::array<uint8_t, 256> pool;
        for (int i = 0; i < 256; i++) pool[i] = (uint8_t)i;
        if (!identity) {
            std::mt19937 rng(seed);
            std::shuffle(pool.begin(), pool.end(), rng);
        }
        int n = (int)OpCode::OP_COUNT;
        for (int i = 0; i < n; i++) fwd_[i] = pool[i];
        for (int i = 0; i < n; i++) rev_[fwd_[i]] = (uint8_t)i;
    }
    uint8_t encode(OpCode op) const { return fwd_[(int)op]; }
    OpCode decode(uint8_t b) const { return (OpCode)rev_[b]; }

    // Emit tabel mapping sbg deklarasi Lua, di-embed ke source VM interpreter.
    // VM baca byte opcode mentah dari stream, lookup lewat tabel ini utk dapat
    // index kanonik yg dipakai dispatch table VM (lihat vm_core.lua).
    std::string generateOpcodeMapLua() const {
        std::string out = "local OPCODE_DECODE = {\n";
        int n = (int)OpCode::OP_COUNT;
        for (int i = 0; i < n; i++) {
            out += "  [" + std::to_string((int)fwd_[i]) + "] = " + std::to_string(i) + ",\n";
        }
        out += "}\n";
        return out;
    }

private:
    std::array<uint8_t, 256> fwd_{}; // canonical index -> obfuscated byte
    std::array<uint8_t, 256> rev_{}; // obfuscated byte -> canonical index
};

class Serializer {
public:
    explicit Serializer(const OpcodeMap& map) : map_(map) {}

    std::vector<uint8_t> serialize(const Proto& root) {
        buf_.clear();
        writeProto(root);
        return buf_;
    }

    void writeToFile(const Proto& root, const std::string& path) {
        auto data = serialize(root);
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)data.data(), (std::streamsize)data.size());
    }

private:
    std::vector<uint8_t> buf_;
    const OpcodeMap& map_;

    void writeU8(uint8_t v) { buf_.push_back(v); }
    void writeU16(uint16_t v) { writeU8(v & 0xFF); writeU8((v >> 8) & 0xFF); }
    void writeI32(int32_t v) {
        uint32_t u = (uint32_t)v;
        writeU8(u & 0xFF); writeU8((u>>8)&0xFF); writeU8((u>>16)&0xFF); writeU8((u>>24)&0xFF);
    }
    void writeF64(double v) {
        uint64_t bits; std::memcpy(&bits, &v, 8);
        for (int i = 0; i < 8; i++) writeU8((bits >> (8*i)) & 0xFF);
    }
    void writeString(const std::string& s) {
        if (s.size() > 0xFFFF) throw std::runtime_error("serializer: string exceeds u16 length limit: " + std::to_string(s.size()));
        writeU16((uint16_t)s.size());
        for (char c : s) writeU8((uint8_t)c);
    }

    void writeProto(const Proto& p) {
        if (p.num_params > 255) throw std::runtime_error("serializer: too many params (>255)");
        if (p.max_stack_size > 255) throw std::runtime_error("serializer: max_stack_size exceeds 255, increase field width");
        writeU8((uint8_t)p.num_params);
        writeU8(p.is_vararg ? 1 : 0);
        writeU8((uint8_t)p.max_stack_size);

        if (p.upvals.size() > 0xFFFF) throw std::runtime_error("serializer: too many upvalues");
        writeU16((uint16_t)p.upvals.size());
        for (auto& uv : p.upvals) {
            writeU8(uv.from_parent_stack ? 1 : 0);
            if (uv.index > 0xFFFF) throw std::runtime_error("serializer: upvalue index exceeds u16");
            writeU16((uint16_t)uv.index);
        }

        if (p.constants.size() > 0xFFFF) throw std::runtime_error("serializer: too many constants (>65535) in one function");
        writeU16((uint16_t)p.constants.size());
        for (auto& c : p.constants) {
            switch (c.type) {
                case ConstType::NIL: writeU8(0); break;
                case ConstType::BOOL: writeU8(1); writeU8(c.b?1:0); break;
                case ConstType::NUMBER: writeU8(2); writeF64(c.n); break;
                case ConstType::STRING: writeU8(3); writeString(c.s); break;
            }
        }

        if (p.code.size() > 0xFFFF) throw std::runtime_error("serializer: too many instructions (>65535) in one function");
        writeU16((uint16_t)p.code.size());
        for (auto& ins : p.code) {
            writeU8(map_.encode(ins.op));
            writeI32(ins.a);
            if (ins.fmt == InstrFormat::ABC) { writeI32(ins.b); writeI32(ins.c); }
            else { writeI32(0); writeI32(ins.c); } // ABx/AsBx: b slot dipakai flag format implisit di VM via opcode itu sendiri
        }

        if (p.children.size() > 0xFFFF) throw std::runtime_error("serializer: too many nested functions (>65535)");
        writeU16((uint16_t)p.children.size());
        for (auto& child : p.children) writeProto(*child);
    }
};

} // namespace vmc
