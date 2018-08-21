/*
 * Copyright(c) 2015 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/radix-tree.h>
#include <linux/memremap.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/memory_hotplug.h>

#ifndef ioremap_cache
/* temporary while we convert existing ioremap_cache users to memremap */
__weak void __iomem *ioremap_cache(resource_size_t offset, unsigned long size)
{
	return ioremap(offset, size);
}
#endif

static void *try_ram_remap(resource_size_t offset, size_t size)
{
	struct page *page = pfn_to_page(offset >> PAGE_SHIFT);

	/* In the simple case just return the existing linear address */
	if (!PageHighMem(page))
		return __va(offset);
	return NULL; /* fallback to ioremap_cache */
}

/**
 * memremap() - remap an iomem_resource as cacheable memory
 * @offset: iomem resource start address
 * @size: size of remap
 * @flags: either MEMREMAP_WB or MEMREMAP_WT
 *
 * memremap() is "ioremap" for cases where it is known that the resource
 * being mapped does not have i/o side effects and the __iomem
 * annotation is not applicable.
 *
 * MEMREMAP_WB - matches the default mapping for "System RAM" on
 * the architecture.  This is usually a read-allocate write-back cache.
 * Morever, if MEMREMAP_WB is specified and the requested remap region is RAM
 * memremap() will bypass establishing a new mapping and instead return
 * a pointer into the direct map.
 *
 * MEMREMAP_WT - establish a mapping whereby writes either bypass the
 * cache or are written through to memory and never exist in a
 * cache-dirty state with respect to program visibility.  Attempts to
 * map "System RAM" with this mapping type will fail.
 */
void *memremap(resource_size_t offset, size_t size, unsigned long flags)
{
	int is_ram = region_intersects(offset, size, "System RAM");
	void *addr = NULL;

	if (is_ram == REGION_MIXED) {
		WARN_ONCE(1, "memremap attempted on mixed range %pa size: %#lx\n",
				&offset, (unsigned long) size);
		return NULL;
	}

	/* Try all mapping types requested until one returns non-NULL */
	if (flags & MEMREMAP_WB) {
		flags &= ~MEMREMAP_WB;
		/*
		 * MEMREMAP_WB is special in that it can be satisifed
		 * from the direct map.  Some archs depend on the
		 * capability of memremap() to autodetect cases where
		 * the requested range is potentially in "System RAM"
		 */
		if (is_ram == REGION_INTERSECTS)
			addr = try_ram_remap(offset, size);
		if (!addr)
			addr = ioremap_cache(offset, size);
	}

	/*
	 * If we don't have a mapping yet and more request flags are
	 * pending then we will be attempting to establish a new virtual
	 * address mapping.  Enforce that this mapping is not aliasing
	 * "System RAM"
	 */
	if (!addr && is_ram == REGION_INTERSECTS && flags) {
		WARN_ONCE(1, "memremap attempted on ram %pa size: %#lx\n",
				&offset, (unsigned long) size);
		return NULL;
	}

	if (!addr && (flags & MEMREMAP_WT)) {
		flags &= ~MEMREMAP_WT;
		addr = ioremap_wt(offset, size);
	}

	return addr;
}
EXPORT_SYMBOL(memremap);

void memunmap(void *addr)
{
	if (is_vmalloc_addr(addr))
		iounmap((void __iomem *) addr);
}
EXPORT_SYMBOL(memunmap);

static void devm_memremap_release(struct device *dev, void *res)
{
	memunmap(*(void **)res);
}

static int devm_memremap_match(struct device *dev, void *res, void *match_data)
{
	return *(void **)res == match_data;
}

void *devm_memremap(struct device *dev, resource_size_t offset,
		size_t size, unsigned long flags)
{
	void **ptr, *addr;

	ptr = devres_alloc_node(devm_memremap_release, sizeof(*ptr), GFP_KERNEL,
			dev_to_node(dev));
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	addr = memremap(offset, size, flags);
	if (addr) {
		*ptr = addr;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
		return ERR_PTR(-ENXIO);
	}

	return addr;
}
EXPORT_SYMBOL(devm_memremap);

void devm_memunmap(struct device *dev, void *addr)
{
	WARN_ON(devres_release(dev, devm_memremap_release,
				devm_memremap_match, addr));
}
EXPORT_SYMBOL(devm_memunmap);

#ifdef CONFIG_ZONE_DEVICE
static DEFINE_MUTEX(pgmap_lock);
static RADIX_TREE(pgmap_radix, GFP_KERNEL);
#define SECTION_MASK ~((1UL << PA_SECTION_SHIFT) - 1)
#define SECTION_SIZE (1UL << PA_SECTION_SHIFT)

struct page_map {
	struct resource res;
	struct percpu_ref *ref;
	struct dev_pagemap pgmap;
};

static void pgmap_radix_release(struct resource *res)
{
	resource_size_t key;

	mutex_lock(&pgmap_lock);
	for (key = res->start; key <= res->end; key += SECTION_SIZE)
		radix_tree_delete(&pgmap_radix, key >> PA_SECTION_SHIFT);
	mutex_unlock(&pgmap_lock);
}

static void devm_memremap_pages_release(struct device *dev, void *data)
{
	struct page_map *page_map = data;
	struct resource *res = &page_map->res;
	resource_size_t align_start, align_size;

	pgmap_radix_release(res);

	align_start = res->start & ~(SECTION_SIZE - 1);
	align_size = ALIGN(resource_size(res), SECTION_SIZE);
	arch_remove_memory(align_start, align_size);
}
 /* assumes rcu_read_lock() held at entry */
struct dev_pagemap *find_dev_pagemap(resource_size_t phys)
{
	struct page_map *page_map;
 	WARN_ON_ONCE(!rcu_read_lock_held());
 	page_map = radix_tree_lookup(&pgmap_radix, phys >> PA_SECTION_SHIFT);
	return page_map ? &page_map->pgmap : NULL;
}

void *devm_memremap_pages(struct device *dev, struct resource *res)
{
	int is_ram = region_intersects(res->start, resource_size(res),
			"System RAM");
	resource_size_t key, align_start, align_size;
	struct page_map *page_map;
	int error, nid;

	if (is_ram == REGION_MIXED) {
		WARN_ONCE(1, "%s attempted on mixed region %pr\n",
				__func__, res);
		return ERR_PTR(-ENXIO);
	}

	if (is_ram == REGION_INTERSECTS)
		return __va(res->start);

	page_map = devres_alloc_node(devm_memremap_pages_release,
			sizeof(*page_map), GFP_KERNEL, dev_to_node(dev));
	if (!page_map)
		return ERR_PTR(-ENOMEM);

	memcpy(&page_map->res, res, sizeof(*res));

	page_map->pgmap.dev = dev;
	mutex_lock(&pgmap_lock);
	error = 0;
	for (key = res->start; key <= res->end; key += SECTION_SIZE) {
		struct dev_pagemap *dup;

		rcu_read_lock();
		dup = find_dev_pagemap(key);
		rcu_read_unlock();
		if (dup) {
			dev_err(dev, "%s: %pr collides with mapping for %s\n",
					__func__, res, dev_name(dup->dev));
			error = -EBUSY;
			break;
		}
		error = radix_tree_insert(&pgmap_radix, key >> PA_SECTION_SHIFT,
				page_map);
		if (error) {
			dev_err(dev, "%s: failed: %d\n", __func__, error);
			break;
		}
	}
	mutex_unlock(&pgmap_lock);
	if (error)
		goto err_radix;

	nid = dev_to_node(dev);
	if (nid < 0)
		nid = numa_mem_id();

	align_start = res->start & ~(SECTION_SIZE - 1);
	align_size = ALIGN(resource_size(res), SECTION_SIZE);
	error = arch_add_memory(nid, align_start, align_size, true);
	if (error)
		goto err_add_memory;

	devres_add(dev, page_map);
	return __va(res->start);

 err_add_memory:
 err_radix:
	pgmap_radix_release(res);
	devres_free(page_map);
	return ERR_PTR(error);
}
EXPORT_SYMBOL(devm_memremap_pages);
#endif /* CONFIG_ZONE_DEVICE */
