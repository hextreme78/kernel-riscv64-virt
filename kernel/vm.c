#include <kernel/vm.h>
#include <kernel/alloc.h>
#include <kernel/ram.h>
#include <kernel/riscv64.h>
#include <kernel/errno.h>
#include <kernel/kprintf.h>

extern u64 *ktext;
extern u64 *trampoline;
extern u64 *krodata;
extern u64 *kdata;
extern u64 *kbss;
extern u64 *kend;

__attribute__((aligned(PAGESZ)))
pte_t kpagetable[PTE_MAX];

void vm_pagetable_init(pte_t *pagetable)
{
	for (size_t i = 0; i < PTE_MAX; i++) {
		PTE_RESET(&pagetable[i]);
	}
}

pte_t *vm_getpte(pte_t *pagetable0, size_t vpn)
{
	pte_t *pagetable1, *pagetable2;
	pte_t *pte0, *pte1, *pte2;
	size_t vpn0 = VPN0(vpn);
	size_t vpn1 = VPN1(vpn);
	size_t vpn2 = VPN2(vpn);

	pte0 = &pagetable0[vpn0];
	if (!pte0->v) {
		return NULL;
	}
	pagetable1 = (pte_t *) PN_TO_PA(pte0->ppn);

	pte1 = &pagetable1[vpn1];
	if (!pte1->v) {
		return NULL;
	}
	pagetable2 = (pte_t *) PN_TO_PA(pte1->ppn);

	pte2 = &pagetable2[vpn2];
	if (!pte2->v) {
		return NULL;
	}

	return pte2;
}

bool vm_ismapped(pte_t *pagetable, size_t vpn)
{
	return vm_getpte(pagetable, vpn);
}

/* vm_pageunmap will free only pages allocated by vm_pagemap call */
void vm_pageunmap(pte_t *pagetable0, size_t vpn)
{
	pte_t *pagetable1, *pagetable2;
	pte_t *pte0, *pte1, *pte2;
	size_t vpn0 = VPN0(vpn);
	size_t vpn1 = VPN1(vpn);
	size_t vpn2 = VPN2(vpn);

	pte0 = &pagetable0[vpn0];
	if (!pte0->v) {
		return;
	}
	pagetable1 = (pte_t *) PN_TO_PA(pte0->ppn);

	pte1 = &pagetable1[vpn1];
	if (!pte1->v) {
		return;
	}
	pagetable2 = (pte_t *) PN_TO_PA(pte1->ppn);

	pte2 = &pagetable2[vpn2];
	if (!pte2->v) {
		return;
	}
	PTE_RESET(pte2);

	for (size_t i = 0; i < PTE_MAX; i++) {
		pte_t *pte = &pagetable2[i];
		if (pte->v) {
			return;
		}
	}
	kpage_free((void *) PN_TO_PA(pte1->ppn));
	PTE_RESET(pte1);

	for (size_t i = 0; i < PTE_MAX; i++) {
		pte_t *pte = &pagetable1[i];
		if (pte->v) {
			return;
		}
	}
	kpage_free((void *) PN_TO_PA(pte0->ppn));
	PTE_RESET(pte0);
}

/* vm_pageunmap_range will free only pages allocated by vm_pagemap call */
void vm_pageunmap_range(pte_t *pagetable, size_t vpn_first, size_t len)
{
	for (size_t i = vpn_first; i < vpn_first + len; i++) {
		vm_pageunmap(pagetable, i);
	}
}

/* vm_pageunmap_all will free only pages allocated by vm_pagemap call */
void vm_pageunmap_all(pte_t *pagetable0)
{
	for (size_t i = 0; i < PTE_MAX; i++) {
		pte_t *pte0 = &pagetable0[i];
		if (!pte0->v) {
			continue;
		}
		pte_t *pagetable1 = (pte_t *) PN_TO_PA(pte0->ppn);

		for (size_t j = 0; j < PTE_MAX; j++) {
			pte_t *pte1 = &pagetable1[j];
			if (!pte1->v) {
				continue;
			}
			pte_t *pagetable2 = (pte_t *) PN_TO_PA(pte1->ppn);
			kpage_free(pagetable2);
		}
		kpage_free(pagetable1);
		PTE_RESET(pte0);
	}
}

int vm_pagemap(pte_t *pagetable0, u8 rwxug, size_t vpn, size_t ppn)
{
	bool pt1_alloc = false;
	pte_t *pagetable1, *pagetable2;
	pte_t *pte0, *pte1, *pte2;
	size_t vpn0 = VPN0(vpn);
	size_t vpn1 = VPN1(vpn);
	size_t vpn2 = VPN2(vpn);

	pte0 = &pagetable0[vpn0];
	if (!pte0->v) {
		pagetable1 = kpage_alloc(1);
		if (!pagetable1) {
			return -ENOMEM;
		}
		pt1_alloc = true;
		vm_pagetable_init(pagetable1);
		PTE_RESET(pte0);
		pte0->ppn = PA_TO_PN(pagetable1);
		pte0->v = true;
	} else {
		pagetable1 = (pte_t *) PN_TO_PA(pte0->ppn);
	}

	pte1 = &pagetable1[vpn1];
	if (!pte1->v) {
		pagetable2 = kpage_alloc(1);
		if (!pagetable2 && pt1_alloc) {
			PTE_RESET(pte0);
			kpage_free(pagetable1);
			return -ENOMEM;
		} else if (!pagetable2 && !pt1_alloc) {
			return -ENOMEM;
		}
		vm_pagetable_init(pagetable2);
		pte1->ppn = PA_TO_PN(pagetable2);
		pte1->v = true;
	} else {
		pagetable2 = (pte_t *) PN_TO_PA(pte1->ppn);
	}

	pte2 = &pagetable2[vpn2];
	PTE_RESET(pte2);
	if (rwxug & PTE_R) pte2->r = true;
	if (rwxug & PTE_W) pte2->w = true;
	if (rwxug & PTE_X) pte2->x = true;
	if (rwxug & PTE_U) pte2->u = true;
	if (rwxug & PTE_G) pte2->g = true;
	pte2->ppn = ppn;
	pte2->v = true;

	return 0;
}

int vm_pagemap_range(pte_t *pagetable, u8 rwxug,
		size_t vpn_first, size_t ppn_first, size_t npages)
{
	int err = 0;
	size_t vpn = vpn_first;
	size_t ppn = ppn_first;
	while (vpn < vpn_first + npages) {
		err = vm_pagemap(pagetable, rwxug, vpn, ppn);
		if (err) {
			vm_pageunmap_range(pagetable, vpn_first,
					vpn - vpn_first);
			return err;
		}
		vpn++;
		ppn++;
	}

	return 0;
}

void vm_pageunmap_kpagetable(pte_t *kpagetable)
{
	/* syscon mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_TEST),
			VIRT_TEST_LEN / PAGESZ);

	/* rtc mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_RTC),
			VIRT_RTC_LEN / PAGESZ);

	/* clint mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_CLINT),
			VIRT_CLINT_LEN / PAGESZ);

	/* plic mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_PLIC),
			VIRT_PLIC_LEN / PAGESZ);
	
	/* uart mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_UART0),
			VIRT_UART0_LEN / PAGESZ);

	/* virtio mapping */
	vm_pageunmap_range(kpagetable, PA_TO_PN(VIRT_VIRTIO),
			VIRT_VIRTIO_LEN / PAGESZ);

	/* kernel text mapping */
	u64 ktextsz = (u64) &trampoline - (u64) &ktext;
	vm_pageunmap_range(kpagetable, PA_TO_PN(&ktext),
			ktextsz / PAGESZ);

	/* trampoline mapping */
	vm_pageunmap(kpagetable, PA_TO_PN(VA_TRAMPOLINE));

	/* kernel rodata mapping */
	u64 krodatasz = (u64) &kdata - (u64) &krodata;
	vm_pageunmap_range(kpagetable, PA_TO_PN(&krodata),
			krodatasz / PAGESZ);

	/* kernel rodata mapping */
	u64 kdatasz = (u64) &kbss - (u64) &kdata;
	vm_pageunmap_range(kpagetable, PA_TO_PN(&kdata),
			kdatasz / PAGESZ);

	/* kernel bss mapping */
	u64 kbsssz = (u64) &kend - (u64) &kbss;
	vm_pageunmap_range(kpagetable, PA_TO_PN(&kbss),
			kbsssz / PAGESZ);

	/* kernel heap mapping */
	u64 kheapsz = ram_end() + 1 - (u64) &kend;
	vm_pageunmap_range(kpagetable, PA_TO_PN(&kend),
			kheapsz / PAGESZ);
}

int vm_pagemap_kpagetable(pte_t *kpagetable)
{
	int err;
	vm_pagetable_init(kpagetable);

	/* syscon mapping */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_TEST),
			PA_TO_PN(VIRT_TEST),
			VIRT_TEST_LEN / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* rtc mapping */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_RTC),
			PA_TO_PN(VIRT_RTC),
			VIRT_RTC_LEN / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* clint mapping
	 * Actually we do not need to map clint because
	 * sifive clint does not support s-mode ipi
	 */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_CLINT),
			PA_TO_PN(VIRT_CLINT),
			VIRT_CLINT_LEN / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* plic mapping */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_PLIC),
			PA_TO_PN(VIRT_PLIC),
			VIRT_PLIC_LEN / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* uart mapping */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_UART0),
			PA_TO_PN(VIRT_UART0),
			PAGEROUND(VIRT_UART0_LEN) / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* virtio mapping */
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(VIRT_VIRTIO),
			PA_TO_PN(VIRT_VIRTIO),
			VIRT_VIRTIO_LEN / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* kernel text mapping */
	u64 ktextsz = (u64) &trampoline - (u64) &ktext;
	err = vm_pagemap_range(kpagetable, PTE_X,
			PA_TO_PN(&ktext),
			PA_TO_PN(&ktext),
			ktextsz / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}
	
	/* trampoline mapping
	 * We need to map trampoline code to the highest virtual page
	 */
	err = vm_pagemap(kpagetable, PTE_X | PTE_G,
			PA_TO_PN(VA_TRAMPOLINE),
			PA_TO_PN(&trampoline));
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* kernel rodata mapping */
	u64 krodatasz = (u64) &kdata - (u64) &krodata;
	err = vm_pagemap_range(kpagetable, PTE_R,
			PA_TO_PN(&krodata),
			PA_TO_PN(&krodata),
			krodatasz / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* kernel data mapping */
	u64 kdatasz = (u64) &kbss - (u64) &kdata;
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(&kdata),
			PA_TO_PN(&kdata),
			kdatasz / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* kernel bss mapping */
	u64 kbsssz = (u64) &kend - (u64) &kbss;
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(&kbss),
			PA_TO_PN(&kbss),
			kbsssz / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	/* kernel heap mapping */
	u64 kheapsz = ram_end() + 1 - (u64) &kend;
	err = vm_pagemap_range(kpagetable, PTE_R | PTE_W,
			PA_TO_PN(&kend),
			PA_TO_PN(&kend),
			kheapsz / PAGESZ);
	if (err) {
		vm_pageunmap_kpagetable(kpagetable);
		return err;
	}

	return 0;
}

void vm_init(void)
{
	if (vm_pagemap_kpagetable(kpagetable)) {
		panic("vm_init failed");
	}
}

void vm_hart_init(void)
{
	sfence_vma();
	w_satp(SATP_MODE_SV39 | PA_TO_PN(kpagetable));
	sfence_vma();
}

