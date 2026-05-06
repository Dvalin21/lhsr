/* dm-lhsr-utils.h - Helper functions for LHSR module */
#ifndef DM_LHSR_UTILS_H
#define DM_LHSR_UTILS_H

#include <linux/mm.h>
#include <linux/gfp.h>

struct page **lhsr_alloc_page_vec(size_t size, gfp_t gfp);
void lhsr_free_page_vec(struct page **pages, size_t size);

#endif /* DM_LHSR_UTILS_H */
