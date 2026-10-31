#include "ppc.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <sys/mman.h>

constexpr uint32_t manager=0x60001000, slots=0x60002000, pane=0x60003000,
                   vtable=0x60004000, close_method=0x03000000, stack=0x600F0000;
static unsigned close_calls;
extern "C" {
int g_ppc_trace=0;
volatile int g_core_preempt[3]{};
void ppc_trace_enter(uint32_t) { abort(); }
void ppc_preempt(Cpu*) { abort(); }
void f_0307CC9C(Cpu*);
void f_0307CD2C(Cpu*);
// Only the final virtual UI callback is synthetic. The object lookup, state
// predicate, branch and original close arguments are actual generated code.
void ppc_dispatch(Cpu* c) {
    assert(c->pc==close_method && c->r[3]==pane && c->r[4]==0xFFFFFFFC);
    ++close_calls;
}
}

static void run(bool legacy, unsigned state, unsigned phase, bool present, bool wrapper) {
    st32(slots+0x154,present?pane:0);
    st8(pane+0x96,state);st8(pane+0x95,phase);
    close_calls=0;
    Cpu c{};c.r[1]=stack;c.lr=0x12345678;c.r[31]=0x87654321;
    (wrapper?f_0307CD2C:f_0307CC9C)(&c);
    const bool expected=present && (!legacy || (state==2 && (int8_t)phase>=0));
    assert(close_calls==unsigned(expected) && c.r[3]==unsigned(expected));
    assert(c.r[1]==stack && c.lr==0x12345678 && c.r[31]==0x87654321);
    assert(ld32(slots+0x150)==0xAABBCCDD && ld32(slots+0x158)==0x11223344);
    // Requesting the close does not itself rewrite animation state or counts.
    assert(ld8(pane+0x96)==state && ld8(pane+0x95)==phase);
    printf("%s state=%u phase=%d present=%d wrapper=%d close_requests=%u\n",
           legacy?"legacy":"fixed",state,(int8_t)phase,present,wrapper,close_calls);
}

int main(int argc,char**) {
    for(uint32_t a:{0x10470000u,0x60000000u}) {
        size_t size=a==0x60000000?0x100000:0x10000;
        assert(mmap(ppc_ptr(a),size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==ppc_ptr(a));
    }
    st32(0x1047E650,manager);st32(manager+0x14,0x56);st32(manager+0x18,slots);
    st32(pane+0xC,vtable);st32(vtable+0x34,close_method);
    st32(slots+0x150,0xAABBCCDD);st32(slots+0x158,0x11223344);
    const bool legacy=argc>1;
    for(bool wrapper:{false,true})
      for(unsigned state:{0u,1u,2u,3u})
        for(unsigned phase:{0u,1u,255u}) run(legacy,state,phase,true,wrapper);
    run(legacy,1,0,false,false);run(legacy,1,0,false,true);
    // Consecutive death/retry/warp close requests don't depend on old animation state.
    for(unsigned cycle=0;cycle<8;++cycle) run(legacy,cycle%4,0,true,false);
    puts(legacy?"legacy skips close before fully open":"loading close requests and null guard passed");
}
