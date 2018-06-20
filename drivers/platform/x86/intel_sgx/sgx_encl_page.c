// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-17 Intel Corporation.
//
// Authors:
//
// Jarkko Sakkinen <jarkko.sakkinen@linux.intel.com>
// Suresh Siddha <suresh.b.siddha@intel.com>
// Serge Ayoun <serge.ayoun@intel.com>
// Shay Katz-zamir <shay.katz-zamir@intel.com>
// Sean Christopherson <sean.j.christopherson@intel.com>

#include <linux/device.h>
#include <linux/freezer.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/ratelimit.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include "sgx.h"

static bool sgx_encl_page_get(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;

	return kref_get_unless_zero(&encl->refcount) != 0;
}

static void sgx_encl_page_put(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;

	kref_put(&encl->refcount, sgx_encl_release);
}

static bool sgx_encl_page_reclaim(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;
	bool ret = false;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);
	if ((encl->flags & SGX_ENCL_DEAD) ||
	    (!sgx_test_and_clear_young(encl_page) &&
	     !(encl_page->desc & SGX_ENCL_PAGE_RESERVED))) {
		encl_page->desc |= SGX_ENCL_PAGE_RESERVED;
		ret = true;
	}
	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);
	return ret;
}

static void sgx_encl_page_block(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);
	sgx_encl_block(encl_page);
	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);
}

/**
 * sgx_write_page - write a page to the regular memory
 *
 * Writes an EPC page to the shmem file associated with the enclave. Flushes
 * CPUs and retries if there are hardware threads that can potentially have TLB
 * entries to the page (indicated by SGX_NOT_TRACKED). Clears the reserved flag
 * after the page is swapped.
 *
 * @epc_page:	an EPC page
 */
static void sgx_write_page(struct sgx_epc_page *epc_page, bool do_free)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_va_page *va_page;
	unsigned int va_offset;
	pgoff_t index;
	int ret;

	if (encl->flags & SGX_ENCL_DEAD)
		goto out;

	index = SGX_ENCL_PAGE_BACKING_INDEX(encl_page, encl);

	va_page = list_first_entry(&encl->va_pages, struct sgx_va_page, list);
	va_offset = sgx_alloc_va_slot(va_page);
	if (sgx_va_page_full(va_page))
		list_move_tail(&va_page->list, &encl->va_pages);

	ret = sgx_ewb(epc_page, va_page->epc_page, va_offset,
		      encl->backing, encl->pcmd, index, NULL);
	if (ret == SGX_NOT_TRACKED) {
		sgx_encl_track(encl);
		ret = sgx_ewb(epc_page, va_page->epc_page, va_offset,
			      encl->backing, encl->pcmd, index, NULL);
		if (ret == SGX_NOT_TRACKED) {
			/* slow path, IPI needed */
			sgx_flush_cpus(encl);
			ret = sgx_ewb(epc_page, va_page->epc_page, va_offset,
				      encl->backing, encl->pcmd, index, NULL);
		}
	}
	SGX_INVD(ret, encl, "EWB returned %d\n", ret);

	encl_page->desc |= va_offset;
	encl_page->va_page = va_page;
	encl_page->desc &= ~SGX_ENCL_PAGE_RESERVED;

out:
	encl_page->desc &= ~SGX_ENCL_PAGE_LOADED;
	if (do_free)
		sgx_free_page(epc_page);
}

static void sgx_encl_page_write(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = container_of(epc_page->impl,
						       struct sgx_encl_page,
						       impl);
	struct sgx_encl *encl = encl_page->encl;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);
	sgx_write_page(epc_page, false);
	encl->secs_child_cnt--;
	if (!encl->secs_child_cnt && (encl->flags & SGX_ENCL_INITIALIZED))
		sgx_write_page(encl->secs.epc_page, true);
	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);
}

const struct sgx_epc_page_ops sgx_encl_page_ops = {
	.get = sgx_encl_page_get,
	.put = sgx_encl_page_put,
	.reclaim = sgx_encl_page_reclaim,
	.block = sgx_encl_page_block,
	.write = sgx_encl_page_write,
};

/**
 * sgx_set_page_reclaimable - associated an EPC page with an enclave page
 * @encl_page:	an enclave page
 * @epc_page:	the EPC page to attach to @encl_page
 */
void sgx_set_epc_page(struct sgx_encl_page *encl_page,
		      struct sgx_epc_page *epc_page)
{
	encl_page->desc |= SGX_ENCL_PAGE_LOADED;
	encl_page->epc_page = epc_page;
}

/**
 * sgx_set_page_reclaimable - mark an EPC page reclaimable
 * @encl_page:	an enclave page with a loaded EPC page
 */
void sgx_set_page_reclaimable(struct sgx_encl_page *encl_page)
{
	sgx_test_and_clear_young(encl_page);

	spin_lock(&sgx_active_page_list_lock);
	list_add_tail(&encl_page->epc_page->list, &sgx_active_page_list);
	spin_unlock(&sgx_active_page_list_lock);
}

/**
 * sgx_alloc_page - allocate a VA page
 * @flags:	allocation flags
 *
 * Allocates an &sgx_epc_page instance and converts it to a VA page.
 *
 * Return:
 *   a &struct sgx_va_page instance,
 *   -errno otherwise
 */
struct sgx_epc_page *sgx_alloc_va_page(unsigned int flags)
{
	struct sgx_epc_page *epc_page;
	int ret;

	epc_page = sgx_alloc_page(NULL, flags);
	if (IS_ERR(epc_page))
		return (void *)epc_page;

	ret = sgx_epa(epc_page);
	if (ret) {
		pr_crit("EPA failed\n");
		sgx_free_page(epc_page);
		return ERR_PTR(ret);
	}

	return epc_page;
}

/**
 * sgx_alloc_va_slot - allocate a VA slot
 * @va_page:	a &struct sgx_va_page instance
 *
 * Allocates a slot from a &struct sgx_va_page instance.
 *
 * Return: offset of the slot inside the VA page
 */
unsigned int sgx_alloc_va_slot(struct sgx_va_page *va_page)
{
	int slot = find_first_zero_bit(va_page->slots, SGX_VA_SLOT_COUNT);

	if (slot < SGX_VA_SLOT_COUNT)
		set_bit(slot, va_page->slots);

	return slot << 3;
}

/**
 * sgx_free_va_slot - free a VA slot
 * @va_page:	a &struct sgx_va_page instance
 * @offset:	offset of the slot inside the VA page
 *
 * Frees a slot from a &struct sgx_va_page instance.
 */
void sgx_free_va_slot(struct sgx_va_page *va_page, unsigned int offset)
{
	clear_bit(offset >> 3, va_page->slots);
}

/**
 * sgx_va_page_full - is the VA page full?
 * @va_page:	a &struct sgx_va_page instance
 *
 * Return: true if all slots have been taken
 */
bool sgx_va_page_full(struct sgx_va_page *va_page)
{
	int slot = find_first_zero_bit(va_page->slots, SGX_VA_SLOT_COUNT);

	return slot == SGX_VA_SLOT_COUNT;
}
