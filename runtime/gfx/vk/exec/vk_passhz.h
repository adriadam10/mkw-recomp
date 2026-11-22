// vk_exec_passhz.c: what the passes and image clears since the last full barrier touch (MKW_PASS_HAZARDS).
#pragma once

#include "vk_exec.h"

// The image and the levels [l0, l1) and layers [a0, a1) a view covers; bound: the image is over a physical pool.
typedef struct { VkImage image; uint32_t l0, l1, a0, a1, bound; } PhRange;

int ph_mode(void);
void ph_log(void);
extern _Thread_local uint32_t ph_gen;
void ph_new_cb(void);
void ph_barrier(const VkExec *ex);
int ph_pass_begin(VkExec *ex, int fresh);
void ph_pass_end(VkExec *ex);
int ph_following(void);
int ph_sample(const PhRange *s);
int ph_stores(void);
int ph_conflict(void);
void ph_drew(void);
int ph_clear(VkExec *ex, VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int bound);
void ph_cleared(VkExec *ex, VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int bound);
