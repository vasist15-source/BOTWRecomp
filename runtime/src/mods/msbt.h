// MSBT message files: BotW's texts (Message/Msg_<language>.product.ssarc inside Pack/Bootup_<language>.pack).
// A file has labels (LBL1, a hash table of names, each naming a text), attributes (ATR1: in EventFlowMsg files
// a string per text, the speaker's actor; elsewhere none) and the texts (TXT2: UTF-16 with control sequences
// for colours, pauses, choices, sounds, ...). Texts that aren't changed are kept as raw bytes, so a file is
// written back byte for byte. BCML's texts.json merge log holds entries in msyt's JSON form
// ({"attributes": "...", "contents": [{"text": "..."}, {"control": {"kind": "pause", "length": "long"}}, ...]});
// encode() turns one into the raw form, with msyt's control encodings (msyt and msbt-rs, MIT, are the reference).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mod_json.h"

namespace mods::msbt {

using Bytes = std::vector<uint8_t>;

struct Entry {
    std::string label;
    Bytes text;       // UTF-16 units in the file's byte order, control sequences inline, ending with a 0 unit
    Bytes attribute;  // its ATR1 string (UTF-16 with its 0 unit), when the file stores attribute strings
    uint32_t bucket = 0;  // its LBL1 hash bucket
};
struct File {
    bool big_endian = true;
    uint8_t encoding = 1;  // 1: UTF-16 (BotW's), 0: UTF-8
    uint8_t version = 3;
    uint16_t unknown1 = 0, unknown3 = 0;
    uint8_t reserved[10] = {};
    uint32_t groups = 1;               // LBL1 hash buckets
    std::vector<Entry> entries;        // TXT2 order
    std::vector<size_t> label_order;   // entries in LBL1 order (within their buckets)
    bool atr1 = true;
    uint32_t atr_size = 0;             // ATR1 bytes per entry (4: a string offset)
    bool atr_strings = false;          // attribute strings are stored (EventFlowMsg)
    std::vector<std::pair<std::string, Bytes>> sections;  // in file order; LBL1/ATR1/TXT2 with empty bodies (written anew)
};

uint32_t label_hash(std::string_view label, uint32_t groups);
bool parse(const uint8_t* d, size_t n, File& out, std::string* why = nullptr);
bool write(const File& f, Bytes& out, std::string* why = nullptr);
// the entry with `label`, or null
const Entry* find(const File& f, std::string_view label);
// sets an entry's text and attribute by label; a new label goes after the others
void set(File& f, const std::string& label, Bytes text, Bytes attribute);
// a new file of these entries, laid out as the game's (EventFlowMsg files store attribute strings)
File make(const std::string& name, bool big_endian);

// the raw text of msyt's "contents" list (with its 0 unit) and its attribute string (with its 0 unit)
bool encode(const json::Value& contents, bool big_endian, Bytes& text, std::string* why = nullptr);
Bytes encode_string(std::string_view utf8, bool big_endian, bool terminate);
// the reverse, as msyt reads texts (tests and diffs)
bool decode(const Bytes& text, bool big_endian, json::Value& contents, std::string* why = nullptr);

}  // namespace mods::msbt
