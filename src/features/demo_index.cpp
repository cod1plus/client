// demo_index.cpp - see demo_index.h. Every rule below is the engine's own, RE'd from
// CoDMP.exe 1.5 (2026-09-26); the addresses are there to go and look again.
//
//   CL_ReadDemoMessage     0x40f690  [seq][len][bytes]; MSG_ReadLong = reliableAcknowledge
//   CL_ParseServerMessage  0x418090  MSG_ReadBitsCompress (0x4476d0) of the rest, then
//                                    byte commands: 1 nop, 2 gamestate, 5 serverCommand,
//                                    6 download, 7 snapshot, 8 end
//   CL_ParseGamestate      0x417890  long seq; {3 short index + bigstring | 4 bits(10) +
//                                    delta entity} ... 8; long clientNum; long checksumFeed
//   CL_ParseCommandString  0x418030  long seq + string
//   CL_ParseSnapshot       0x4173e0  long serverTime, byte delta, byte snapFlags,
//                                    playerstate (0x44a630), entities (0x416c90),
//                                    clients (0x417020)
//   MSG_ReadBits           0x4475e0  LSB first; a fresh byte is taken at readcount
//                                    whenever the bit cursor is byte-aligned
//   MSG_ReadDeltaStruct    0x449110  bit removed, bit changed, BYTE lc, fields
//   MSG_ReadDeltaField     0x448ec0  bit changed; float: bit zero, bit full (long) or
//                                    5 bits + byte-128 << 5; int: bit zero, low bits then bytes
//   hudelems               0x4494e0  bits(5) count; per elem bits(5) lc, lc+1 fields
//   Huffman                Quake 3 static tree from msg_hData (0x5bb018), read LSB first
#include "features/demo_index.h"
#include "features/demo_tables.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <utility>

namespace patches {

namespace {

using namespace demo_tables;

// ------------------------------------------------------------------ Huffman
// Quake 3's adaptive Huffman, used here only to BUILD the static tree the engine builds
// at startup (MSG_initHuffman: every byte added msg_hData[b] times). The resulting tree
// is flattened into arrays and never changes again.
constexpr int HMAX = 256, NYT = HMAX, INTERNAL_NODE = HMAX + 1;

struct HNode {
    HNode *left, *right, *parent, *next, *prev;
    HNode** head;
    int weight, symbol;
};

struct Huff {
    int blocNode, blocPtrs;
    HNode* tree;
    HNode* lhead;
    HNode* ltail;
    HNode* loc[HMAX + 1];
    HNode** freelist;
    HNode nodeList[768];
    HNode* nodePtrs[768];
};

HNode** get_ppnode(Huff* h) {
    if (!h->freelist) return &h->nodePtrs[h->blocPtrs++];
    HNode** t = h->freelist;
    h->freelist = (HNode**)*t;
    return t;
}
void free_ppnode(Huff* h, HNode** pp) { *pp = (HNode*)h->freelist; h->freelist = pp; }

void swap_nodes(Huff* h, HNode* n1, HNode* n2) {
    HNode* p1 = n1->parent;
    HNode* p2 = n2->parent;
    if (p1) { if (p1->left == n1) p1->left = n2; else p1->right = n2; } else h->tree = n2;
    if (p2) { if (p2->left == n2) p2->left = n1; else p2->right = n1; } else h->tree = n1;
    n1->parent = p2;
    n2->parent = p1;
}

void swaplist(HNode* n1, HNode* n2) {
    HNode* p = n1->next; n1->next = n2->next; n2->next = p;
    p = n1->prev; n1->prev = n2->prev; n2->prev = p;
    if (n1->next == n1) n1->next = n2;
    if (n2->next == n2) n2->next = n1;
    if (n1->next) n1->next->prev = n1;
    if (n2->next) n2->next->prev = n2;
    if (n1->prev) n1->prev->next = n1;
    if (n2->prev) n2->prev->next = n2;
}

// Quake 3's increment(): recursive, the tail step needs the parent already incremented.
void increment_rec(Huff* h, HNode* node) {
    if (!node) return;
    if (node->next && node->next->weight == node->weight) {
        HNode* lnode = *node->head;
        if (lnode != node->parent) swap_nodes(h, lnode, node);
        swaplist(lnode, node);
    }
    if (node->prev && node->prev->weight == node->weight) {
        *node->head = node->prev;
    } else {
        *node->head = nullptr;
        free_ppnode(h, node->head);
    }
    node->weight++;
    if (node->next && node->next->weight == node->weight) {
        node->head = node->next->head;
    } else {
        node->head = get_ppnode(h);
        *node->head = node;
    }
    if (node->parent) {
        increment_rec(h, node->parent);
        if (node->prev == node->parent) {
            swaplist(node, node->parent);
            if (*node->head == node) *node->head = node->parent;
        }
    }
}

void add_ref(Huff* h, int ch) {
    if (!h->loc[ch]) {
        HNode* tnode = &h->nodeList[h->blocNode++];
        HNode* tnode2 = &h->nodeList[h->blocNode++];
        tnode2->symbol = INTERNAL_NODE;
        tnode2->weight = 1;
        tnode2->next = h->lhead->next;
        if (h->lhead->next) {
            h->lhead->next->prev = tnode2;
            if (h->lhead->next->weight == 1) {
                tnode2->head = h->lhead->next->head;
            } else {
                tnode2->head = get_ppnode(h);
                *tnode2->head = tnode2;
            }
        } else {
            tnode2->head = get_ppnode(h);
            *tnode2->head = tnode2;
        }
        h->lhead->next = tnode2;
        tnode2->prev = h->lhead;

        tnode->symbol = ch;
        tnode->weight = 1;
        tnode->next = h->lhead->next;
        if (h->lhead->next) {
            h->lhead->next->prev = tnode;
            if (h->lhead->next->weight == 1) {
                tnode->head = h->lhead->next->head;
            } else {
                tnode->head = get_ppnode(h);
                *tnode->head = tnode2;
            }
        } else {
            tnode->head = get_ppnode(h);
            *tnode->head = tnode;
        }
        h->lhead->next = tnode;
        tnode->prev = h->lhead;
        tnode->left = tnode->right = nullptr;

        if (h->lhead->parent) {
            if (h->lhead->parent->left == h->lhead) h->lhead->parent->left = tnode2;
            else h->lhead->parent->right = tnode2;
        } else {
            h->tree = tnode2;
        }
        tnode2->right = tnode;
        tnode2->left = h->lhead;
        tnode2->parent = h->lhead->parent;
        h->lhead->parent = tnode->parent = tnode2;
        h->loc[ch] = tnode;
        increment_rec(h, tnode2->parent);
    } else {
        increment_rec(h, h->loc[ch]);
    }
}

// The flattened decoder: node i -> child[i][bit], leaves hold -1 - symbol.
// fast[p]: the next LUT_BITS input bits (LSB first) -> the symbol and its code length,
// or, for a longer code, the node reached after LUT_BITS bits (len = 0). Four times
// faster than walking bit by bit - a 10 MB demo in ~0.1 s.
constexpr int LUT_BITS = 11;
struct Decoder {
    int16_t child[768][2];
    struct { int16_t v; uint8_t len; } fast[1 << LUT_BITS];
    int root = 0;
    bool built = false;
};
Decoder g_dec;

void build_lut() {
    for (int p = 0; p < (1 << LUT_BITS); ++p) {
        int node = g_dec.root, len = 0;
        while (node >= 0 && len < LUT_BITS) node = g_dec.child[node][(p >> len++) & 1];
        if (node < 0) { g_dec.fast[p].v = (int16_t)(-1 - node); g_dec.fast[p].len = (uint8_t)len; }
        else { g_dec.fast[p].v = (int16_t)node; g_dec.fast[p].len = 0; }
    }
}

void build_decoder() {
    Huff* h = (Huff*)calloc(1, sizeof(Huff));
    if (!h) return;
    h->tree = h->lhead = h->ltail = h->loc[NYT] = &h->nodeList[h->blocNode++];
    h->tree->symbol = NYT;
    h->tree->weight = 0;
    h->lhead->next = h->lhead->prev = nullptr;
    h->tree->parent = h->tree->left = h->tree->right = nullptr;
    for (int i = 0; i < 256; ++i)
        for (int j = 0; j < kHuffData[i]; ++j) add_ref(h, i);
    // flatten: internal node index = its position in nodeList, leaf = -1 - symbol
    auto idx = [&](HNode* n) -> int16_t {
        if (!n) return (int16_t)(-1 - 0);                  // engine: a null child reads as symbol 0
        if (n->symbol != INTERNAL_NODE) return (int16_t)(-1 - (n->symbol == NYT ? 0 : n->symbol));
        return (int16_t)(n - h->nodeList);
    };
    for (int i = 0; i < h->blocNode; ++i) {
        g_dec.child[i][0] = idx(h->nodeList[i].left);
        g_dec.child[i][1] = idx(h->nodeList[i].right);
    }
    g_dec.root = idx(h->tree);
    g_dec.built = g_dec.root >= 0;
    free(h);
    if (g_dec.built) build_lut();
}

// MSG_ReadBitsCompress: symbols until the input bits run out (padding symbols included,
// harmless: the command stream ends with 8 before them)
int huff_decompress(const uint8_t* in, int in_size, uint8_t* out, int out_max) {
    const int bits = in_size * 8;
    int off = 0, n = 0;
    // fast path while 4 whole bytes can be read at the cursor
    while (n < out_max && (off >> 3) + 4 <= in_size) {
        uint32_t w;
        memcpy(&w, in + (off >> 3), 4);
        const int p = (int)((w >> (off & 7)) & ((1u << LUT_BITS) - 1));
        if (g_dec.fast[p].len) {
            out[n++] = (uint8_t)g_dec.fast[p].v;
            off += g_dec.fast[p].len;
            continue;
        }
        int node = g_dec.fast[p].v;                    // long code: finish bit by bit
        off += LUT_BITS;
        while (node >= 0) {
            if (off >= bits) { node = -1; break; }
            node = g_dec.child[node][(in[off >> 3] >> (off & 7)) & 1];
            ++off;
        }
        out[n++] = (uint8_t)(-1 - node);
    }
    while (off < bits && n < out_max) {
        int node = g_dec.root;
        while (node >= 0) {
            if (off >= bits) { node = -1; break; }
            const int b = (in[off >> 3] >> (off & 7)) & 1;
            ++off;
            node = g_dec.child[node][b];
        }
        out[n++] = (uint8_t)(-1 - node);
    }
    return n;
}

// ------------------------------------------------------------------ msg reader
struct Msg {
    const uint8_t* data;
    int cursize;
    int readcount = 0;
    int bit = 0;
    bool overflow = false;

    int bitval() {
        if (!(bit & 7)) {
            if (readcount >= cursize) { overflow = true; return 0; }
            bit = readcount * 8;
            ++readcount;
        }
        const int v = (data[bit >> 3] >> (bit & 7)) & 1;
        ++bit;
        return v;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v |= bitval() << i;
        return v;
    }
    int byte() {
        if (readcount >= cursize) { overflow = true; return -1; }
        return data[readcount++];
    }
    int shrt() {
        if (readcount + 2 > cursize) { overflow = true; return -1; }
        const int16_t v = (int16_t)(data[readcount] | (data[readcount + 1] << 8));
        readcount += 2;
        return v;
    }
    int lng() {
        if (readcount + 4 > cursize) { overflow = true; return -1; }
        int32_t v;
        memcpy(&v, data + readcount, 4);
        readcount += 4;
        return v;
    }
    std::string str(int max) {                      // MSG_ReadString / ReadBigString
        std::string s;
        for (;;) {
            const int c = byte();
            if (c <= 0) break;
            if ((int)s.size() < max) s.push_back((char)c);
        }
        return s;
    }
};

// ------------------------------------------------------------------ structs
constexpr int ENT_SIZE = 0xf0, PS_SIZE = 0x20cc, CLI_SIZE = 0x5c;
constexpr int MAX_GENTITIES = 1024, ENTITYNUM_NONE = 1023;
constexpr int ET_OBITUARY = 12 + 201;
constexpr int ES_ETYPE = 0x04, ES_OTHER = 0x74, ES_ATTACKER = 0x78, ES_EVENTPARM = 0xa0;
constexpr int PS_CLIENTNUM = 0xac, PS_WEAPON = 0xb0;
constexpr int CS_TEAM = 0x04, CS_NAME = 0x3c;   // clientState_t
constexpr int CS_WEAPONS = 7;                     // configstring: "bar_mp bar_slow_mp ..." = weapon 1, 2 ...

inline int32_t& I(uint8_t* s, int off) { return *(int32_t*)(s + off); }
inline int32_t  I(const uint8_t* s, int off) { return *(const int32_t*)(s + off); }

// generic field (MSG_ReadDeltaField 0x448ec0): has the "zero" bit
void read_field(Msg& m, const uint8_t* from, uint8_t* to, const Field& f) {
    if (!m.bitval()) { I(to, f.offset) = I(from, f.offset); return; }
    if (f.bits == 0) {
        if (!m.bitval()) { I(to, f.offset) = 0; return; }
        if (m.bitval()) { I(to, f.offset) = m.lng(); return; }
        int v = m.bits(5);
        v += (m.byte() - 128) << 5;
        const float fv = (float)v;
        memcpy(to + f.offset, &fv, 4);
        return;
    }
    if (!m.bitval()) { I(to, f.offset) = 0; return; }
    const int n = f.bits < 0 ? -f.bits : f.bits;
    const int low = n & 7;
    int v = low ? m.bits(low) : 0;
    for (int i = low; i < n; i += 8) v |= m.byte() << i;
    I(to, f.offset) = v;
}

// playerstate field (inline in 0x44a630): NO "zero" bit
void read_ps_field(Msg& m, const uint8_t* from, uint8_t* to, const Field& f) {
    if (!m.bitval()) { I(to, f.offset) = I(from, f.offset); return; }
    if (f.bits == 0) {
        if (m.bitval()) { I(to, f.offset) = m.lng(); return; }
        int v = m.bits(5);
        v += (m.byte() - 128) << 5;
        const float fv = (float)v;
        memcpy(to + f.offset, &fv, 4);
        return;
    }
    const int n = f.bits < 0 ? -f.bits : f.bits;
    const int low = n & 7;
    int v = low ? m.bits(low) : 0;
    for (int i = low; i < n; i += 8) v |= m.byte() << i;
    I(to, f.offset) = v;
}

// MSG_ReadDeltaStruct 0x449110: returns true when the entity / client was removed
bool read_delta_struct(Msg& m, const uint8_t* from, uint8_t* to, int number,
                       const Field* fields, int nf, int struct_size) {
    if (m.bitval()) return true;
    if (!m.bitval()) { memcpy(to, from, struct_size); return false; }
    int lc = m.byte();
    if (lc > nf) lc = nf;
    I(to, 0) = number;
    int i = 0;
    for (; i < lc; ++i) read_field(m, from, to, fields[i]);
    for (; i < nf; ++i) I(to, fields[i].offset) = I(from, fields[i].offset);
    return false;
}

void read_hudelems(Msg& m, const uint8_t* from, uint8_t* to, int count) {
    constexpr int HE = 0x70, NF = 28;
    int n = m.bits(5);
    if (n > count) n = count;
    for (int i = 0; i < n; ++i) {
        const uint8_t* f = from + i * HE;
        uint8_t* t = to + i * HE;
        const int lc = m.bits(5);
        int j = 0;
        for (; j <= lc && j < NF; ++j) read_field(m, f, t, kHudElemFields[j]);
        for (; j < NF; ++j) I(t, kHudElemFields[j].offset) = I(f, kHudElemFields[j].offset);
    }
    for (int i = n; i < count; ++i) memset(to + i * HE, 0, HE);
}

void read_playerstate(Msg& m, const uint8_t* from, uint8_t* to) {
    static const uint8_t zero[PS_SIZE] = {};
    if (!from) from = zero;
    memcpy(to, from, PS_SIZE);
    const int lc = m.byte();
    const int nf = (int)(sizeof(kPlayerFields) / sizeof(kPlayerFields[0]));
    for (int i = 0; i < lc && i < nf; ++i) read_ps_field(m, from, to, kPlayerFields[i]);
    if (m.bitval()) {                                   // stats
        const int b = m.bits(6);
        if (b & 1)    I(to, 0xf4)  = m.shrt();
        if (b & 2)    I(to, 0xf8)  = m.shrt();
        if (b & 4)    I(to, 0xfc)  = m.shrt();
        if (b & 8)    I(to, 0x100) = m.bits(6);
        if (b & 0x10) I(to, 0x104) = m.shrt();
        if (b & 0x20) I(to, 0x108) = m.byte();
    }
    if (m.bitval()) {                                   // ammo[64]
        for (int g = 0; g < 4; ++g) {
            if (!m.bitval()) continue;
            const int mask = m.shrt();
            for (int k = 0; k < 16; ++k)
                if (mask & (1 << k)) I(to, 0x10c + (g * 16 + k) * 4) = m.shrt();
        }
    }
    for (int g = 0; g < 4; ++g) {                       // ammoclip[64]: no outer bit
        if (!m.bitval()) continue;
        const int mask = m.shrt();
        for (int k = 0; k < 16; ++k)
            if (mask & (1 << k)) I(to, 0x20c + (g * 16 + k) * 4) = m.shrt();
    }
    if (m.bitval()) {                                   // objectives[16]
        for (int i = 0; i < 16; ++i) {
            uint8_t* t = to + 0x3e8 + i * 0x1c;
            const uint8_t* f = from + 0x3e8 + i * 0x1c;
            I(t, 0) = m.bits(3);
            if (m.bitval()) {
                for (int j = 0; j < 6; ++j) read_field(m, f, t, kObjectiveFields[j]);
            } else {
                for (int j = 0; j < 6; ++j) I(t, kObjectiveFields[j].offset) = I(f, kObjectiveFields[j].offset);
            }
        }
    }
    if (m.bitval()) {                                   // hudelems: current, then archival
        read_hudelems(m, from + 0x1338, to + 0x1338, 31);
        read_hudelems(m, from + 0x5a8, to + 0x5a8, 31);
    }
}

// ------------------------------------------------------------------ the parse
std::string info_value(const std::string& info, const char* key);

struct Snap {
    bool valid = false;
    int  messageNum = -1;
    int  serverTime = 0;
    std::vector<uint8_t> ps;                            // PS_SIZE
    std::vector<uint8_t> ents;                          // n * ENT_SIZE, sorted by number
    std::vector<uint8_t> clis;                          // n * CLI_SIZE, sorted by client index
};

struct Parser {
    DemoInfo* out;
    std::vector<std::string> cs;                        // configstrings
    std::vector<std::string> weapons;                   // CS 7 split: index 1 = weapons[0]
    std::vector<uint8_t> baselines;                     // MAX_GENTITIES * ENT_SIZE
    Snap ring[32];
    Snap work;                                          // the snapshot being parsed
    Snap* prev = nullptr;                               // the last parsed snapshot (cgame's "previous")
    int  serverMessageSequence = 0;
    bool have_gamestate = false;
    std::map<int, std::string> names;                   // client -> last known name
    std::string err;

    Parser(DemoInfo* o) : out(o), cs(2048), baselines((size_t)MAX_GENTITIES * ENT_SIZE, 0) {}

    const uint8_t* find_ent(const Snap* s, int num) const {
        if (!s) return nullptr;
        const size_t n = s->ents.size() / ENT_SIZE;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* e = s->ents.data() + i * ENT_SIZE;
            if (I(e, 0) == num) return e;
        }
        return nullptr;
    }

    bool gamestate(Msg& m) {
        m.lng();                                        // serverCommandSequence
        for (;;) {
            const int cmd = m.byte();
            if (cmd == 8) break;
            if (cmd == 3) {
                const int idx = m.shrt();
                std::string s = m.str(8191);
                if (idx < 0 || idx >= 2048) { err = "gamestate: configstring index out of range"; return false; }
                cs[idx] = s;
            } else if (cmd == 4) {
                const int num = m.bits(10);
                if (num < 0 || num >= MAX_GENTITIES) { err = "gamestate: baseline number out of range"; return false; }
                static const uint8_t zero[ENT_SIZE] = {};
                read_delta_struct(m, zero, baselines.data() + (size_t)num * ENT_SIZE, num,
                                  kEntityFields, 59, ENT_SIZE);
            } else {
                err = "gamestate: bad command byte";
                return false;
            }
            if (m.overflow) { err = "gamestate: read past the end"; return false; }
        }
        out->recorder = m.lng();
        m.lng();                                        // checksumFeed
#ifdef DEMO_INDEX_DUMP_CS
        for (int i = 0; i < 2048; ++i)
            if (!cs[i].empty()) printf("CS %4d: %s\n", i, cs[i].c_str());
#endif
        for (Snap& s : ring) s = Snap();
        prev = nullptr;
        have_gamestate = true;
        ++out->gamestates;
        out->maps.push_back(info_value(cs[0], "mapname"));
        if (out->gamestates == 1) {
            out->hostname = info_value(cs[0], "sv_hostname");
            out->gametype = info_value(cs[0], "g_gametype");
        }
        weapons.clear();
        {
            const std::string& wl = cs[CS_WEAPONS];
            size_t p = 0;
            while (p < wl.size()) {
                size_t e = wl.find(' ', p);
                if (e == std::string::npos) e = wl.size();
                if (e > p) weapons.push_back(wl.substr(p, e - p));
                p = e + 1;
            }
        }
        return true;
    }

    void entities(Msg& m, const Snap* old, Snap& ns) {
        ns.ents.clear();
        size_t oldi = 0;
        const size_t oldn = old ? old->ents.size() / ENT_SIZE : 0;
        auto oldnum = [&]() { return oldi < oldn ? I(old->ents.data() + oldi * ENT_SIZE, 0) : 99999; };
        uint8_t tmp[ENT_SIZE];
        for (;;) {
            const int newnum = m.bits(10);
            if (newnum == ENTITYNUM_NONE || m.overflow) break;
            while (oldnum() < newnum) {
                ns.ents.insert(ns.ents.end(), old->ents.data() + oldi * ENT_SIZE, old->ents.data() + (oldi + 1) * ENT_SIZE);
                ++oldi;
            }
            const uint8_t* from;
            if (oldnum() == newnum) { from = old->ents.data() + oldi * ENT_SIZE; ++oldi; }
            else from = baselines.data() + (size_t)newnum * ENT_SIZE;
            if (!read_delta_struct(m, from, tmp, newnum, kEntityFields, 59, ENT_SIZE))
                ns.ents.insert(ns.ents.end(), tmp, tmp + ENT_SIZE);
        }
        while (oldi < oldn) {
            ns.ents.insert(ns.ents.end(), old->ents.data() + oldi * ENT_SIZE, old->ents.data() + (oldi + 1) * ENT_SIZE);
            ++oldi;
        }
    }

    void clients(Msg& m, const Snap* old, Snap& ns) {
        ns.clis.clear();
        size_t oldi = 0;
        const size_t oldn = old ? old->clis.size() / CLI_SIZE : 0;
        auto oldnum = [&]() { return oldi < oldn ? I(old->clis.data() + oldi * CLI_SIZE, 0) : 99999; };
        static const uint8_t zero[CLI_SIZE] = {};
        uint8_t tmp[CLI_SIZE];
        while (m.bitval() && !m.overflow) {
            const int newnum = m.bits(6);
            while (oldnum() < newnum) {
                ns.clis.insert(ns.clis.end(), old->clis.data() + oldi * CLI_SIZE, old->clis.data() + (oldi + 1) * CLI_SIZE);
                ++oldi;
            }
            const uint8_t* from = zero;
            if (oldnum() == newnum) { from = old->clis.data() + oldi * CLI_SIZE; ++oldi; }
            if (!read_delta_struct(m, from, tmp, newnum, kClientFields, 22, CLI_SIZE))
                ns.clis.insert(ns.clis.end(), tmp, tmp + CLI_SIZE);
        }
        while (oldi < oldn) {
            ns.clis.insert(ns.clis.end(), old->clis.data() + oldi * CLI_SIZE, old->clis.data() + (oldi + 1) * CLI_SIZE);
            ++oldi;
        }
    }

    bool snapshot(Msg& m) {
        Snap& ns = work;                                // buffers recycled through the ring (swap)
        ns.valid = false;
        ns.serverTime = m.lng();
        ns.messageNum = serverMessageSequence;
        const int db = m.byte();
        const int deltaNum = db > 0 ? ns.messageNum - db : -1;
        m.byte();                                       // snapFlags
        const Snap* old = nullptr;
        ns.valid = true;
        if (deltaNum > 0) {
            const Snap& o = ring[deltaNum & 31];
            if (o.valid && o.messageNum == deltaNum) old = &o;
            else ns.valid = false;                      // the engine drops it too
        }
        ns.ps.assign(PS_SIZE, 0);
        read_playerstate(m, old ? old->ps.data() : nullptr, ns.ps.data());
        entities(m, old, ns);
        clients(m, old, ns);
        if (m.overflow) { err = "snapshot: read past the end"; return false; }
        if (!ns.valid) return true;

        // names from the client states (team + 32-byte name)
        for (size_t i = 0; i < ns.clis.size() / CLI_SIZE; ++i) {
            const uint8_t* c = ns.clis.data() + i * CLI_SIZE;
            char nm[33];
            memcpy(nm, c + CS_NAME, 32);
            nm[32] = 0;
            if (nm[0]) names[I(c, 0)] = nm;
        }

        const int pov = I(ns.ps.data(), PS_CLIENTNUM);
        if (!out->snapshots) out->first_time = ns.serverTime;
        else if (ns.serverTime < out->last_time) out->time_went_back = true;
        out->last_time = ns.serverTime;
        ++out->snapshots;

        // obituaries: an event entity counts on the first snapshot it appears in
        for (size_t i = 0; i < ns.ents.size() / ENT_SIZE; ++i) {
            const uint8_t* e = ns.ents.data() + i * ENT_SIZE;
            if (I(e, ES_ETYPE) != ET_OBITUARY) continue;
            const uint8_t* pe = find_ent(prev, I(e, 0));
            if (pe && I(pe, ES_ETYPE) == ET_OBITUARY && I(pe, ES_OTHER) == I(e, ES_OTHER) &&
                I(pe, ES_ATTACKER) == I(e, ES_ATTACKER))
                continue;                               // already seen
            DemoKill k;
            k.server_time = ns.serverTime;
            k.t_ms = ns.serverTime - out->first_time;
            k.victim = I(e, ES_OTHER);
            k.attacker = I(e, ES_ATTACKER);
            const int parm = I(e, ES_EVENTPARM);
            if (parm & 0x80) k.mod = parm & 0x7f;
            else k.weapon = parm;
            // the weapon's name: the obituary's, or for a flagged death (headshot...)
            // the one in the killer's hands when the demo shows his eyes
            int wi = k.weapon;
            if (wi < 0 && k.attacker == pov) wi = I(ns.ps.data(), PS_WEAPON);
            if (wi > 0 && wi <= (int)weapons.size()) k.weapon_name = demo_weapon_label(weapons[wi - 1].c_str());
            k.pov = pov;
            k.segment = out->gamestates > 0 ? out->gamestates - 1 : 0;
            k.pov_weapon = I(ns.ps.data(), PS_WEAPON);
            auto nm = names.find(k.victim);
            if (nm != names.end()) k.victim_name = nm->second;
            nm = names.find(k.attacker);
            if (nm != names.end()) k.attacker_name = nm->second;
            out->kills.push_back(k);
        }

        Snap& slot = ring[ns.messageNum & 31];
        std::swap(slot, work);
        prev = &slot;
        return true;
    }

    // one demo message; false = stop
    bool message(const uint8_t* data, int len) {
        if (len < 4) return true;
        static thread_local uint8_t buf[0x4000];
        const int n = huff_decompress(data + 4, len - 4, buf, (int)sizeof(buf));
        Msg m{ buf, n };
        for (;;) {
            const int cmd = m.byte();
            if (cmd == 8) return true;
            switch (cmd) {
            case 1: break;
            case 2: if (!gamestate(m)) return false; break;
            case 5: m.lng(); m.str(1023); break;
            case 7:
                if (!have_gamestate) { err = "snapshot before the gamestate"; return false; }
                if (!snapshot(m)) return false;
                break;
            case 6: return true;                        // download: never in a demo worth reading
            default:
                err = "illegible server message";
                return false;
            }
            if (m.overflow) { err = "read past the end of a message"; return false; }
        }
    }
};

std::string info_value(const std::string& info, const char* key) {
    // "\key\value\key\value"
    size_t p = 0;
    while (p < info.size()) {
        if (info[p] == '\\') ++p;
        size_t e = info.find('\\', p);
        if (e == std::string::npos) break;
        const std::string k = info.substr(p, e - p);
        size_t v = e + 1;
        size_t ve = info.find('\\', v);
        const std::string val = info.substr(v, ve == std::string::npos ? std::string::npos : ve - v);
        if (_stricmp(k.c_str(), key) == 0) return val;
        if (ve == std::string::npos) break;
        p = ve;
    }
    return "";
}

}  // namespace

std::string demo_weapon_label(const char* w) {
    static const struct { const char* key; const char* label; } k[] = {
        { "bar_slow_mp", "BAR" }, { "bar_mp", "BAR" }, { "bren_mp", "Bren" }, { "colt_mp", "Colt" },
        { "enfield_mp", "Lee-Enfield" }, { "fg42_semi_mp", "FG42" }, { "fg42_mp", "FG42" },
        { "fraggrenade_mp", "Grenade" }, { "kar98k_sniper_mp", "Kar98k Sniper" }, { "kar98k_mp", "Kar98k" },
        { "luger_mp", "Luger" }, { "m1carbine_mp", "M1 Carbine" }, { "m1garand_mp", "M1 Garand" },
        { "mg42_bipod_duck_mp", "MG42" }, { "mg42_bipod_prone_mp", "MG42" }, { "mg42_bipod_stand_mp", "MG42" },
        { "mk1britishfrag_mp", "Grenade" }, { "mosin_nagant_sniper_mp", "Mosin Sniper" },
        { "mosin_nagant_mp", "Mosin-Nagant" }, { "mp40_mp", "MP40" }, { "mp44_semi_mp", "MP44" },
        { "mp44_mp", "MP44" }, { "panzerfaust_mp", "Panzerfaust" }, { "ppsh_semi_mp", "PPSh" },
        { "ppsh_mp", "PPSh" }, { "ptrs41_antitank_rifle_mp", "PTRS-41" }, { "rgd-33russianfrag_mp", "Grenade" },
        { "springfield_mp", "Springfield" }, { "sten_mp", "Sten" }, { "stielhandgranate_mp", "Grenade" },
        { "thompson_semi_mp", "Thompson" }, { "thompson_mp", "Thompson" },
    };
    for (const auto& e : k) if (_stricmp(w, e.key) == 0) return e.label;
    std::string s = w;                              // a mod's weapon: "foo_mp" -> "foo"
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "_mp") == 0) s.resize(s.size() - 3);
    return s;
}

const char* demo_mod_label(int mod) {
    switch (mod) {
    case 7:  return "melee";
    case 8:  return "headshot";
    case 16: return "drowned";
    case 17: return "slime";
    case 19: return "crushed";
    case 21: return "fall";
    case 22: return "suicide";
    default: return mod >= 0 ? "killed" : "";
    }
}

bool demo_index_buffer(const unsigned char* data, size_t size, DemoInfo* out) {
    static const bool s_built = (build_decoder(), true);   // thread-safe local static
    (void)s_built;
    *out = DemoInfo();
    if (!g_dec.built) { out->error = "huffman tree not built"; return false; }
    Parser p(out);
    size_t pos = 0;
    while (pos + 8 <= size) {
        int32_t seq, len;
        memcpy(&seq, data + pos, 4);
        memcpy(&len, data + pos + 4, 4);
        if (len == -1) break;                           // end of demo
        if (len < 0 || len > 0x4000 || pos + 8 + (size_t)len > size) {
            p.err = "truncated or corrupt message";
            break;
        }
        p.serverMessageSequence = seq;
        ++out->messages;
        if (!p.message(data + pos + 8, len)) break;
        pos += 8 + (size_t)len;
    }
    out->map = out->maps.empty() ? std::string() : out->maps.front();
    auto rn = p.names.find(out->recorder);
    if (rn != p.names.end()) out->recorder_name = rn->second;
    out->duration_ms = out->last_time - out->first_time;
    out->error = p.err;
    out->ok = out->snapshots > 0;
    if (!out->ok && out->error.empty()) out->error = "no snapshot";
    return out->ok;
}

bool demo_index_file(const char* path, DemoInfo* out) {
    *out = DemoInfo();
    FILE* f = fopen(path, "rb");
    if (!f) { out->error = "cannot open"; return false; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); out->error = "empty file"; return false; }
    std::vector<unsigned char> buf((size_t)sz);
    const size_t rd = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    return demo_index_buffer(buf.data(), rd, out);
}

}  // namespace patches
