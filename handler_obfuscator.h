// ============================================================================
// handler_obfuscator.h — Full virtualization: extract handler dari
// handlers_source.lua, enkripsi XOR (offset unik per-handler), generate
// vm_runtime.lua siap pakai (digabung dengan runtime_tail.lua terpisah).
// ============================================================================
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <random>
#include <stdexcept>
#include <regex>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace vmc {

// Urutan opcode kanonik — HARUS sinkron persis dgn enum class OpCode di
// bytecode.h (index array ini = index kanonik opcode, 0-based).
inline const std::vector<std::string>& handlerOpcodeOrder() {
    static const std::vector<std::string> order = {
        "LOADK","LOADNIL","LOADBOOL","MOVE","GETUPVAL","SETUPVAL",
        "GETGLOBAL","SETGLOBAL","NEWTABLE","GETTABLE","SETTABLE",
        "GETTABLEK","SETTABLEK","ADD","SUB","MUL","DIV","MOD","POW",
        "UNM","NOT","LEN","CONCAT","JMP","EQ","LT","LE","TEST",
        "CALL","RETURN","CLOSURE","VARARG","FORPREP","FORLOOP",
        "TFORLOOP","SETLIST","SELF","CLOSE","NOP",
    };
    return order;
}

// Extract handler dari handlers_source.lua: cari "-- HANDLER_BEGIN NAME" di
// AWAL BARIS (bukan substring longgar, supaya tidak salah tangkap komentar
// penjelasan yg menyebut pola serupa), sampai "-- HANDLER_END".
inline std::map<std::string, std::string> extractHandlers(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open handlers source: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    std::map<std::string, std::string> handlers;
    std::istringstream lines(content);
    std::string line;
    std::string currentName;
    bool inHandler = false;
    std::ostringstream body;
    static const std::regex beginRe("^-- HANDLER_BEGIN (\\w+)$");
    static const std::regex endRe("^-- HANDLER_END\\s*$");

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::smatch m;
        if (!inHandler && std::regex_match(line, m, beginRe)) {
            currentName = m[1].str();
            inHandler = true;
            body.str("");
            body.clear();
            continue;
        }
        if (inHandler && std::regex_match(line, endRe)) {
            std::string b = body.str();
            if (!b.empty() && b.back() == '\n') b.pop_back();
            handlers[currentName] = b;
            inHandler = false;
            continue;
        }
        if (inHandler) {
            body << line << "\n";
        }
    }

    if (inHandler) {
        throw std::runtime_error("handlers source: unterminated HANDLER_BEGIN " + currentName + " (missing HANDLER_END)");
    }
    return handlers;
}

inline void validateHandlerSet(const std::map<std::string, std::string>& handlers) {
    auto& order = handlerOpcodeOrder();
    std::set<std::string> expected(order.begin(), order.end());
    std::set<std::string> got;
    for (auto& kv : handlers) got.insert(kv.first);
    if (expected != got) {
        std::string msg = "handler set mismatch.";
        for (auto& name : expected) if (!got.count(name)) msg += " MISSING:" + name;
        for (auto& name : got) if (!expected.count(name)) msg += " UNEXPECTED:" + name;
        throw std::runtime_error(msg);
    }
}

inline std::vector<uint8_t> xorEncryptWithOffset(const std::string& data, const std::vector<uint8_t>& key, int offset) {
    std::vector<uint8_t> out(data.size());
    size_t klen = key.size();
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t k = key[(offset + (int)i) % (int)klen];
        out[i] = (uint8_t)((uint8_t)data[i] ^ k);
    }
    return out;
}

inline std::string luaEscapeBytesVec(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(data.size() * 4);
    for (uint8_t b : data) {
        out += "\\";
        out += std::to_string((int)b);
    }
    return out;
}

// Generate isi vm_runtime.lua (bagian handler terenkripsi saja; runtime_tail.lua
// yg berisi execProto/Reader/dll digabung terpisah oleh pemanggil, supaya bagian
// "mesin eksekusi" tetap satu sumber kebenaran yg mudah dibaca & di-maintain,
// tidak ikut di-generate dari C++ berulang).
inline std::string generateVmRuntimeHeader(const std::string& handlersSourcePath, uint32_t seed) {
    auto handlers = extractHandlers(handlersSourcePath);
    validateHandlerSet(handlers);
    auto& order = handlerOpcodeOrder();

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byteDist(1, 255); // hindari 0 (XOR no-op)
    std::vector<uint8_t> key(64);
    for (auto& b : key) b = (uint8_t)byteDist(rng);

    std::vector<int> storageOrder(order.size());
    for (size_t i = 0; i < storageOrder.size(); i++) storageOrder[i] = (int)i;
    std::shuffle(storageOrder.begin(), storageOrder.end(), rng);

    std::uniform_int_distribution<int> offsetDist(0, (int)key.size() - 1);

    std::ostringstream out;
    out << "-- AUTO-GENERATED vm_runtime.lua — full virtualization layer\n";
    out << "-- Handler tiap opcode disimpan TERENKRIPSI (XOR stream + offset unik per-handler),\n";
    out << "-- didekripsi+loadstring saat runtime, bukan disimpan sbg source Lua polos.\n\n";

    out << "local KEY = {";
    for (size_t i = 0; i < key.size(); i++) { if (i) out << ","; out << (int)key[i]; }
    out << "}\n\n";

    out << "local function xorDecrypt(data, key, offset)\n";
    out << "    local out = {}\n";
    out << "    local klen = #key\n";
    out << "    for i = 1, #data do\n";
    out << "        local b = string.byte(data, i)\n";
    out << "        local k = key[((offset + i - 1) % klen) + 1]\n";
    out << "        out[i] = string.char(bit32.bxor(b, k))\n";
    out << "    end\n";
    out << "    return table.concat(out)\n";
    out << "end\n\n";

    out << "local ENCRYPTED_HANDLERS = {}\n";
    out << "local HANDLER_OFFSETS = {}\n";
    for (size_t canonical_idx = 0; canonical_idx < order.size(); canonical_idx++) {
        const std::string& opname = order[canonical_idx];
        int storage_idx = storageOrder[canonical_idx] + 1; // 1-based utk Lua
        std::string src = "return " + handlers.at(opname);
        int offset = offsetDist(rng);
        auto encrypted = xorEncryptWithOffset(src, key, offset);
        std::string escaped = luaEscapeBytesVec(encrypted);
        out << "ENCRYPTED_HANDLERS[" << storage_idx << "] = \"" << escaped << "\"\n";
        out << "HANDLER_OFFSETS[" << storage_idx << "] = " << offset << "\n";
    }
    out << "\n";

    out << "local STORAGE_MAP = {\n";
    for (size_t canonical_idx = 0; canonical_idx < order.size(); canonical_idx++) {
        int storage_idx = storageOrder[canonical_idx] + 1;
        out << "    [" << canonical_idx << "] = " << storage_idx << ", -- " << order[canonical_idx] << "\n";
    }
    out << "}\n\n";

    return out.str();
}

} // namespace vmc
