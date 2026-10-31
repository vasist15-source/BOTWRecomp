#pragma once
#include "ppc.h"
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Host stack counterpart of v208's two JPEG setjmp call sites. The guest
 * routines still save/restore the original register buffer in guest memory. */
typedef struct BotwJumpFrame {
    jmp_buf host;
    struct BotwJumpFrame* previous;
    Cpu* cpu;
    uint32_t buffer;
} BotwJumpFrame;
void botw_jump_push(BotwJumpFrame*, Cpu*, uint32_t);
void botw_jump_pop(BotwJumpFrame*);
__attribute__((noreturn)) void botw_jump_resume(Cpu*, uint32_t);
#ifdef __cplusplus
}
#endif
