#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
double botw_frame_ratio(void);
void botw_cutscene_begin(uint32_t object, uint32_t id, uint32_t name);
void botw_cutscene_end(uint32_t object, uint32_t id);
int botw_cutscene_limit(void);
int botw_effective_fps(int requested);
#ifdef __cplusplus
}
#endif
