#include "qrc_des.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {

using Block = std::array<uint8_t, 8>;
using RoundKey = std::array<uint8_t, 6>;
using Schedule = std::array<RoundKey, 16>;
using TripleSchedule = std::array<Schedule, 3>;

constexpr uint8_t kKey[24] = {'!', '@', '#', ')', '(', '*', '$', '%',
                              '1', '2', '3', 'Z', 'X', 'C', '!', '@',
                              '!', '@', '#', ')', '(', 'N', 'H', 'L'};

constexpr uint8_t sbox[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,15,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,10,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

uint32_t bitNum(const uint8_t* a, int b, int c) {
    return static_cast<uint32_t>((a[b / 32 * 4 + 3 - b % 32 / 8] >> (7 - b % 8)) & 1) << c;
}
uint8_t bitNumIntr(uint32_t a, int b, int c) {
    return static_cast<uint8_t>(((a >> (31 - b)) & 1) << c);
}
uint32_t bitNumIntl(uint32_t a, int b, int c) {
    return ((a << b) & 0x80000000u) >> c;
}
uint32_t sboxBit(uint8_t a) {
    return (a & 0x20u) | ((a & 0x1fu) >> 1) | ((a & 1u) << 4);
}

void keySchedule(const uint8_t* key, Schedule& schedule, bool decrypt) {
    constexpr int shifts[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
    constexpr int pc[56] = {56,48,40,32,24,16,8,0,57,49,41,33,25,17,9,1,58,50,42,34,
                            26,18,10,2,59,51,43,35,62,54,46,38,30,22,14,6,61,53,45,37,
                            29,21,13,5,60,52,44,36,28,20,12,4,27,19,11,3};
    constexpr int compression[48] = {13,16,10,23,0,4,2,27,14,5,20,9,22,18,11,3,25,7,
                                      15,6,26,19,12,1,40,51,30,36,46,54,29,39,50,44,32,47,
                                      43,48,38,55,33,52,45,41,49,35,28,31};
    uint32_t c = 0, d = 0;
    for (int i = 0, j = 31; i < 28; ++i, --j) c |= bitNum(key, pc[i], j);
    for (int i = 28, j = 31; i < 56; ++i, --j) d |= bitNum(key, pc[i], j);
    for (int i = 0; i < 16; ++i) {
        c = ((c << shifts[i]) | (c >> (28 - shifts[i]))) & 0xfffffff0u;
        d = ((d << shifts[i]) | (d >> (28 - shifts[i]))) & 0xfffffff0u;
        RoundKey& round = schedule[decrypt ? 15 - i : i];
        round.fill(0);
        int j = 0;
        for (; j < 24; ++j)
            round[j / 8] |= bitNumIntr(c, compression[j], 7 - j % 8);
        for (; j < 48; ++j)
            round[j / 8] |= bitNumIntr(d, compression[j] - 27, 7 - j % 8);
    }
}

void initialPermutation(uint32_t state[2], const uint8_t* in) {
    state[0] = 0; state[1] = 0;
    constexpr int left[32] = {57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,
                              61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7};
    constexpr int right[32] = {56,48,40,32,24,16,8,0,58,50,42,34,26,18,10,2,
                               60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6};
    for (int i = 0; i < 32; ++i) {
        state[0] |= bitNum(in, left[i], 31 - i);
        state[1] |= bitNum(in, right[i], 31 - i);
    }
}

void inversePermutation(const uint32_t state[2], uint8_t* out) {
    out[3]=bitNumIntr(state[1],7,7)|bitNumIntr(state[0],7,6)|bitNumIntr(state[1],15,5)|bitNumIntr(state[0],15,4)|bitNumIntr(state[1],23,3)|bitNumIntr(state[0],23,2)|bitNumIntr(state[1],31,1)|bitNumIntr(state[0],31,0);
    out[2]=bitNumIntr(state[1],6,7)|bitNumIntr(state[0],6,6)|bitNumIntr(state[1],14,5)|bitNumIntr(state[0],14,4)|bitNumIntr(state[1],22,3)|bitNumIntr(state[0],22,2)|bitNumIntr(state[1],30,1)|bitNumIntr(state[0],30,0);
    out[1]=bitNumIntr(state[1],5,7)|bitNumIntr(state[0],5,6)|bitNumIntr(state[1],13,5)|bitNumIntr(state[0],13,4)|bitNumIntr(state[1],21,3)|bitNumIntr(state[0],21,2)|bitNumIntr(state[1],29,1)|bitNumIntr(state[0],29,0);
    out[0]=bitNumIntr(state[1],4,7)|bitNumIntr(state[0],4,6)|bitNumIntr(state[1],12,5)|bitNumIntr(state[0],12,4)|bitNumIntr(state[1],20,3)|bitNumIntr(state[0],20,2)|bitNumIntr(state[1],28,1)|bitNumIntr(state[0],28,0);
    out[7]=bitNumIntr(state[1],3,7)|bitNumIntr(state[0],3,6)|bitNumIntr(state[1],11,5)|bitNumIntr(state[0],11,4)|bitNumIntr(state[1],19,3)|bitNumIntr(state[0],19,2)|bitNumIntr(state[1],27,1)|bitNumIntr(state[0],27,0);
    out[6]=bitNumIntr(state[1],2,7)|bitNumIntr(state[0],2,6)|bitNumIntr(state[1],10,5)|bitNumIntr(state[0],10,4)|bitNumIntr(state[1],18,3)|bitNumIntr(state[0],18,2)|bitNumIntr(state[1],26,1)|bitNumIntr(state[0],26,0);
    out[5]=bitNumIntr(state[1],1,7)|bitNumIntr(state[0],1,6)|bitNumIntr(state[1],9,5)|bitNumIntr(state[0],9,4)|bitNumIntr(state[1],17,3)|bitNumIntr(state[0],17,2)|bitNumIntr(state[1],25,1)|bitNumIntr(state[0],25,0);
    out[4]=bitNumIntr(state[1],0,7)|bitNumIntr(state[0],0,6)|bitNumIntr(state[1],8,5)|bitNumIntr(state[0],8,4)|bitNumIntr(state[1],16,3)|bitNumIntr(state[0],16,2)|bitNumIntr(state[1],24,1)|bitNumIntr(state[0],24,0);
}

uint32_t f(uint32_t state, const RoundKey& key) {
    uint8_t large[6];
    uint32_t t1 = bitNumIntl(state,31,0) | ((state & 0xf0000000u)>>1) |
        bitNumIntl(state,4,5) | bitNumIntl(state,3,6) | ((state & 0x0f000000u)>>3) |
        bitNumIntl(state,8,11) | bitNumIntl(state,7,12) | ((state & 0x00f00000u)>>5) |
        bitNumIntl(state,12,17) | bitNumIntl(state,11,18) | ((state & 0x000f0000u)>>7) |
        bitNumIntl(state,16,23);
    uint32_t t2 = bitNumIntl(state,15,0) | ((state & 0x0000f000u)<<15) |
        bitNumIntl(state,20,5) | bitNumIntl(state,19,6) | ((state & 0x00000f00u)<<13) |
        bitNumIntl(state,24,11) | bitNumIntl(state,23,12) | ((state & 0x000000f0u)<<11) |
        bitNumIntl(state,28,17) | bitNumIntl(state,27,18) | ((state & 0x0000000fu)<<9) |
        bitNumIntl(state,0,23);
    large[0]=t1>>24; large[1]=t1>>16; large[2]=t1>>8;
    large[3]=t2>>24; large[4]=t2>>16; large[5]=t2>>8;
    for (int i=0;i<6;++i) large[i]^=key[i];
    uint32_t v = (uint32_t{sbox[0][sboxBit(large[0]>>2)]}<<28) |
        (uint32_t{sbox[1][sboxBit(((large[0]&3)<<4)|(large[1]>>4))]}<<24) |
        (uint32_t{sbox[2][sboxBit(((large[1]&15)<<2)|(large[2]>>6))]}<<20) |
        (uint32_t{sbox[3][sboxBit(large[2]&63)]}<<16) |
        (uint32_t{sbox[4][sboxBit(large[3]>>2)]}<<12) |
        (uint32_t{sbox[5][sboxBit(((large[3]&3)<<4)|(large[4]>>4))]}<<8) |
        (uint32_t{sbox[6][sboxBit(((large[4]&15)<<2)|(large[5]>>6))]}<<4) |
        uint32_t{sbox[7][sboxBit(large[5]&63)]};
    constexpr int p[32] = {15,6,19,20,28,11,27,16,0,14,22,25,4,17,30,9,
                           1,7,23,13,31,26,2,8,18,12,29,5,21,10,3,24};
    uint32_t result=0;
    for(int i=0;i<32;++i) result |= bitNumIntl(v,p[i],i);
    return result;
}

void cryptBlock(const uint8_t* input, uint8_t* output, const Schedule& key) {
    uint32_t state[2];
    initialPermutation(state,input);
    for(int i=0;i<15;++i){ uint32_t t=state[1]; state[1]=f(state[1],key[i])^state[0]; state[0]=t; }
    state[0]=f(state[1],key[15])^state[0];
    inversePermutation(state,output);
}

} // namespace

bool decryptQrcDes(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    if (input.empty() || input.size()%8!=0) return false;
    TripleSchedule schedule;
    keySchedule(kKey, schedule[2], true);
    keySchedule(kKey+8, schedule[1], false);
    keySchedule(kKey+16, schedule[0], true);
    output.resize(input.size());
    for(size_t i=0;i<input.size();i+=8){
        Block a{},b{};
        cryptBlock(input.data()+i,a.data(),schedule[0]);
        cryptBlock(a.data(),b.data(),schedule[1]);
        cryptBlock(b.data(),output.data()+i,schedule[2]);
    }
    return true;
}
