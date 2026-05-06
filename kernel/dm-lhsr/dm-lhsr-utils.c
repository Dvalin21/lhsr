/* dm-lhsr-utils.c - Helper functions for LHSR module */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include "dm-lhsr-utils.h"

struct page **lhsr_alloc_page_vec(size_t size, gfp_t gfp)
{
	unsigned int nr_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	struct page **pages;
	unsigned int i;

	pages = kcalloc(nr_pages, sizeof(struct page *), gfp);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < nr_pages; i++) {
		pages[i] = alloc_page(gfp);
		if (!pages[i])
			goto err_free;
	}
	return pages;

err_free:
	while (i--)
		__free_page(pages[i]);
	kfree(pages);
	return ERR_PTR(-ENOMEM);
}

void lhsr_free_page_vec(struct page **pages, size_t size)
{
	unsigned int nr_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned int i;

	if (!pages || IS_ERR(pages))
		return;

	for (i = 0; i < nr_pages; i++)
		__free_page(pages[i]);
	kfree(pages);
}
