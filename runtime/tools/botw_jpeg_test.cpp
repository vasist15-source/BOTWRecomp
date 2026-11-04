#include "botw_jump.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>

namespace {
constexpr uint32_t stack=0x600F0000, output=0x60100000, pixels=0x60200000,
                   info=0x60001000, length=0x60001010, heap=0x61000000;
uint32_t cursor, limit;
unsigned creates, destroys;
}
extern "C" {
int g_ppc_trace=0;
volatile int g_core_preempt[3]{};
void ppc_trace_enter(uint32_t) { abort(); }
void ppc_preempt(Cpu*) { abort(); }
void ppc_unimplemented(Cpu*,uint32_t,uint32_t) { abort(); }
void imp_coreinit_COSWarn(Cpu*) {}
// Deliberately bounded synthetic heap; codec/control flow is the actual guest.
void imp_coreinit_MEMCreateExpHeapEx(Cpu* c) { cursor=c->r[3]+128; limit=c->r[3]+c->r[4]; ++creates; }
void imp_coreinit_MEMAllocFromExpHeapEx(Cpu* c) {
    uint32_t align=std::abs((int32_t)c->r[5]); if (!align) align=4;
    uint32_t p=(cursor+align-1)&~(align-1), n=c->r[4];
    c->r[3]=p<=limit && n<=limit-p ? p : 0; if(c->r[3]) cursor=p+n;
}
void imp_coreinit_MEMFreeToExpHeap(Cpu*) {}
void imp_coreinit_MEMDestroyExpHeap(Cpu*) { ++destroys; }
void imp_coreinit_MEMGetTotalFreeSizeForExpHeap(Cpu* c) { c->r[3]=limit-cursor; }
void imp_coreinit_memcpy(Cpu* c) { memcpy(ppc_ptr(c->r[3]),ppc_ptr(c->r[4]),c->r[5]); }
void imp_coreinit_memset(Cpu* c) { memset(ppc_ptr(c->r[3]),c->r[4],c->r[5]); }
void imp_coreinit_OSBlockSet(Cpu* c) { memset(ppc_ptr(c->r[3]),c->r[4],c->r[5]); }
void f_03C70C04(Cpu*);
void f_03C70EE4(Cpu*);
void f_0420EA7C(Cpu*);
void f_0420EBB4(Cpu*);
}

static void map(uint32_t address,size_t bytes) {
    assert(mmap(ppc_ptr(address),bytes,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==ppc_ptr(address));
}

static int encode(unsigned size, unsigned width=256, unsigned height=256) {
    Cpu c{}; c.r[1]=stack; c.lr=0x12345678;
    for(unsigned i=14;i<32;++i) c.r[i]=0x90000000+i;
    st32(info,width);st32(info+4,height);st32(info+8,3);st32(length,0xFFFFFFFF);
    c.r[3]=output;c.r[4]=size;c.r[5]=length;c.r[6]=pixels;
    c.r[7]=info;c.r[8]=50;c.r[9]=heap;c.r[10]=0x100000;
    f_03C70C04(&c);
    assert(c.r[1]==stack && c.lr==0x12345678);
    for(unsigned i=14;i<32;++i) assert(c.r[i]==0x90000000+i);
    assert(creates==destroys);
    printf("encode %ux%u capacity=%u result=%d bytes=%u\n",width,height,size,(int32_t)c.r[3],ld32(length));
    return (int32_t)c.r[3];
}

static int decode(unsigned size, unsigned scale=1) {
    Cpu c{}; c.r[1]=stack; c.lr=0x12345678;
    for(unsigned i=14;i<32;++i) c.r[i]=0x90000000+i;
    c.r[3]=pixels;c.r[4]=size;c.r[5]=output;c.r[6]=ld32(length);
    c.r[7]=scale;c.r[8]=heap;c.r[9]=0x100000;
    f_03C70EE4(&c);
    assert(c.r[1]==stack && c.lr==0x12345678);
    for(unsigned i=14;i<32;++i) assert(c.r[i]==0x90000000+i);
    assert(creates==destroys);
    printf("decode capacity=%u scale=%u result=%d\n",size,scale,(int32_t)c.r[3]);
    return (int32_t)c.r[3];
}

static void jump_registers() {
    // The emulated state lives outside the setjmp activation's automatic locals.
    auto state=std::make_unique<Cpu>(); Cpu& c=*state;
    c.r[1]=stack; c.lr=0x03C70CE4; c.r[2]=0x10008000;
    for(unsigned i=13;i<32;++i) c.r[i]=0xA0000000+i;
    for(unsigned i=14;i<32;++i) {c.f[i].ps0=i+0.25;c.f[i].ps1=i+0.5;}
    constexpr uint32_t buffer=0x60002000;
    BotwJumpFrame frame{}; botw_jump_push(&frame,&c,buffer);
    c.r[3]=buffer; f_0420EA7C(&c);
    if(setjmp(frame.host)==0) {
        c.r[3]=buffer;c.r[4]=0;
        f_0420EBB4(&c);
        abort(); // Old translated longjmp incorrectly returns here.
    }
    assert(c.r[3]==1 && c.r[1]==stack && c.lr==0x03C70CE4);
    for(unsigned i=13;i<32;++i) assert(c.r[i]==0xA0000000+i);
    for(unsigned i=14;i<32;++i) assert(c.f[i].ps0==i+0.25 && c.f[i].ps1==i+0.5);
    botw_jump_pop(&frame);
}

int main(int argc,char** argv) {
    assert(argc>=2); map(0x10000000,0x1000000);map(0x60000000,0x2000000);
    FILE* f=fopen(argv[1],"rb");assert(f);assert(fread(ppc_ptr(0x10000000),1,0x1000000,f)>0);fclose(f);
    uint32_t rng=1;
    for(unsigned i=0;i<256*256*4;++i) {rng=rng*1664525+1013904223;st8(pixels+i,rng>>24);}
    // Exercise a full encoder failure, cleanup, and a succeeding retry repeatedly.
    for(unsigned n=0;n<3;++n) {
        assert(encode(0x3C00)==-7);assert(ld32(length)==0);
        assert(encode(65536)==0);assert(ld32(length)>0x3C00 && ld32(length)<65536);
        assert(ld16(output)==0xFFD8 && ld16(output+ld32(length)-2)==0xFFD9);
    }
    assert(encode(65536,97,91)==0);
    assert(decode(16)==-6); // valid JPEG, insufficient decoded output capacity
    assert(decode(65536)==0); // same input can still be decoded after recovery
    st16(output,0);assert(decode(65536)<0); // malformed header
    assert(decode(65536,3)==-1); // early return before setjmp leaves no host frame
    assert(encode(65536)==0);
    jump_registers();
    puts("actual JPEG error/retry/register restoration passed");
}
