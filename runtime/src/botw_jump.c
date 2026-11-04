#include "botw_jump.h"
#include <stdio.h>
#include <stdlib.h>

static _Thread_local BotwJumpFrame* active;

void botw_jump_push(BotwJumpFrame* frame, Cpu* cpu, uint32_t buffer) {
    frame->previous = active;
    frame->cpu = cpu;
    frame->buffer = buffer;
    active = frame;
}

void botw_jump_pop(BotwJumpFrame* frame) {
    if (!frame->cpu) return; /* early return before guest setjmp */
    if (active != frame) abort();
    active = frame->previous;
}

void botw_jump_resume(Cpu* cpu, uint32_t buffer) {
    BotwJumpFrame* frame = active;
    while (frame && (frame->cpu != cpu || frame->buffer != buffer)) frame = frame->previous;
    if (!frame) {
        fprintf(stderr, "[jpeg] unmatched guest longjmp buffer=%08X lr=%08X\n", buffer, cpu->lr);
        abort();
    }
    /* Any inner guest frames have been unwound by the original guest longjmp.
     * Match that on the host; returning normally would resume a stale decoder. */
    active = frame;
    fprintf(stderr, "[jpeg] recovered error=%d resume=%08X\n", (int32_t)cpu->r[3], cpu->lr);
    longjmp(frame->host, 1);
}
