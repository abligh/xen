/******************************************************************************
 * xc_hvm_build.c
 */

#define ELFSIZE 32
#include <stddef.h>
#include <inttypes.h>
#include "xg_private.h"
#include "xc_private.h"
#include "xc_elf.h"
#include <stdlib.h>
#include <unistd.h>
#include <zlib.h>
#include <xen/hvm/hvm_info_table.h>
#include <xen/hvm/params.h>
#include <xen/hvm/e820.h>

#define HVM_LOADER_ENTR_ADDR  0x00100000
static int
parseelfimage(
    char *elfbase, unsigned long elfsize, struct domain_setup_info *dsi);
static int
loadelfimage(
    char *elfbase, int xch, uint32_t dom, unsigned long *parray,
    struct domain_setup_info *dsi);

static void xc_set_hvm_param(int handle,
                             domid_t dom, int param, unsigned long value)
{
    DECLARE_HYPERCALL;
    xen_hvm_param_t arg;
    int rc;

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_set_param;
    hypercall.arg[1] = (unsigned long)&arg;
    arg.domid = dom;
    arg.index = param;
    arg.value = value;
    if ( lock_pages(&arg, sizeof(arg)) != 0 )
    {
        PERROR("Could not lock memory for set parameter");
        return;
    }
    rc = do_xen_hypercall(handle, &hypercall);
    unlock_pages(&arg, sizeof(arg));
    if (rc < 0)
        PERROR("set HVM parameter failed (%d)", rc);
}

static void build_e820map(void *e820_page, unsigned long long mem_size)
{
    struct e820entry *e820entry =
        (struct e820entry *)(((unsigned char *)e820_page) + E820_MAP_OFFSET);
    unsigned long long extra_mem_size = 0;
    unsigned char nr_map = 0;

    /*
     * Physical address space from HVM_BELOW_4G_RAM_END to 4G is reserved
     * for PCI devices MMIO. So if HVM has more than HVM_BELOW_4G_RAM_END
     * RAM, memory beyond HVM_BELOW_4G_RAM_END will go to 4G above.
     */
    if ( mem_size > HVM_BELOW_4G_RAM_END )
    {
        extra_mem_size = mem_size - HVM_BELOW_4G_RAM_END;
        mem_size = HVM_BELOW_4G_RAM_END;
    }

    e820entry[nr_map].addr = 0x0;
    e820entry[nr_map].size = 0x9F000;
    e820entry[nr_map].type = E820_RAM;
    nr_map++;

    e820entry[nr_map].addr = 0x9F000;
    e820entry[nr_map].size = 0x1000;
    e820entry[nr_map].type = E820_RESERVED;
    nr_map++;

    e820entry[nr_map].addr = 0xEA000;
    e820entry[nr_map].size = 0x01000;
    e820entry[nr_map].type = E820_ACPI;
    nr_map++;

    e820entry[nr_map].addr = 0xF0000;
    e820entry[nr_map].size = 0x10000;
    e820entry[nr_map].type = E820_RESERVED;
    nr_map++;

    /* Low RAM goes here. Remove 3 pages for ioreq, bufioreq, and xenstore. */
    e820entry[nr_map].addr = 0x100000;
    e820entry[nr_map].size = mem_size - 0x100000 - PAGE_SIZE * 3;
    e820entry[nr_map].type = E820_RAM;
    nr_map++;

    if ( extra_mem_size )
    {
        e820entry[nr_map].addr = (1ULL << 32);
        e820entry[nr_map].size = extra_mem_size;
        e820entry[nr_map].type = E820_RAM;
        nr_map++;
    }

    *(((unsigned char *)e820_page) + E820_MAP_NR_OFFSET) = nr_map;
}

static void set_hvm_info_checksum(struct hvm_info_table *t)
{
    uint8_t *ptr = (uint8_t *)t, sum = 0;
    unsigned int i;

    t->checksum = 0;

    for (i = 0; i < t->length; i++)
        sum += *ptr++;

    t->checksum = -sum;
}

/*
 * Use E820 reserved memory 0x9F800 to pass HVM info to hvmloader
 * hvmloader will use this info to set BIOS accordingly
 */
static int set_hvm_info(int xc_handle, uint32_t dom,
                        xen_pfn_t *pfn_list, unsigned int vcpus,
                        unsigned int acpi)
{
    char *va_map;
    struct hvm_info_table *va_hvm;

    va_map = xc_map_foreign_range(xc_handle, dom, PAGE_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  pfn_list[HVM_INFO_PFN]);

    if ( va_map == NULL )
        return -1;

    va_hvm = (struct hvm_info_table *)(va_map + HVM_INFO_OFFSET);
    memset(va_hvm, 0, sizeof(*va_hvm));

    strncpy(va_hvm->signature, "HVM INFO", 8);
    va_hvm->length       = sizeof(struct hvm_info_table);
    va_hvm->acpi_enabled = acpi;
    va_hvm->nr_vcpus     = vcpus;

    set_hvm_info_checksum(va_hvm);

    munmap(va_map, PAGE_SIZE);

    return 0;
}

static int setup_guest(int xc_handle,
                       uint32_t dom, int memsize,
                       char *image, unsigned long image_size,
                       vcpu_guest_context_t *ctxt,
                       unsigned long shared_info_frame,
                       unsigned int vcpus,
                       unsigned int pae,
                       unsigned int acpi,
                       unsigned int apic,
                       unsigned int store_evtchn,
                       unsigned long *store_mfn)
{
    xen_pfn_t *page_array = NULL;
    unsigned long i, nr_pages = (unsigned long)memsize << (20 - PAGE_SHIFT);
    unsigned long shared_page_nr;
    shared_info_t *shared_info;
    void *e820_page;
    struct domain_setup_info dsi;
    uint64_t v_end;
    int rc;

    memset(&dsi, 0, sizeof(struct domain_setup_info));

    if ( (parseelfimage(image, image_size, &dsi)) != 0 )
        goto error_out;

    if ( (dsi.v_kernstart & (PAGE_SIZE - 1)) != 0 )
    {
        PERROR("Guest OS must load to a page boundary.\n");
        goto error_out;
    }

    v_end = (unsigned long long)memsize << 20;

    IPRINTF("VIRTUAL MEMORY ARRANGEMENT:\n"
           "  Loaded HVM loader:    %016"PRIx64"->%016"PRIx64"\n"
           "  TOTAL:                %016"PRIx64"->%016"PRIx64"\n",
           dsi.v_kernstart, dsi.v_kernend,
           dsi.v_start, v_end);
    IPRINTF("  ENTRY ADDRESS:        %016"PRIx64"\n", dsi.v_kernentry);

    if ( (v_end - dsi.v_start) > ((unsigned long long)nr_pages << PAGE_SHIFT) )
    {
        PERROR("Initial guest OS requires too much space: "
               "(%lluMB is greater than %lluMB limit)\n",
               (unsigned long long)(v_end - dsi.v_start) >> 20,
               ((unsigned long long)nr_pages << PAGE_SHIFT) >> 20);
        goto error_out;
    }

    if ( (page_array = malloc(nr_pages * sizeof(xen_pfn_t))) == NULL )
    {
        PERROR("Could not allocate memory.\n");
        goto error_out;
    }

    for ( i = 0; i < nr_pages; i++ )
        page_array[i] = i;
    for ( i = HVM_BELOW_4G_RAM_END >> PAGE_SHIFT; i < nr_pages; i++ )
        page_array[i] += HVM_BELOW_4G_MMIO_LENGTH >> PAGE_SHIFT;

    /* Allocate memory for HVM guest, skipping VGA hole 0xA0000-0xC0000. */
    rc = xc_domain_memory_populate_physmap(
        xc_handle, dom, (nr_pages > 0xa0) ? 0xa0 : nr_pages,
        0, 0, &page_array[0x00]);
    if ( (rc == 0) && (nr_pages > 0xc0) )
        rc = xc_domain_memory_populate_physmap(
            xc_handle, dom, nr_pages - 0xc0, 0, 0, &page_array[0xc0]);
    if ( rc != 0 )
    {
        PERROR("Could not allocate memory for HVM guest.\n");
        goto error_out;
    }

    if ( (nr_pages > 0xa0) &&
         xc_domain_memory_decrease_reservation(
             xc_handle, dom, (nr_pages < 0xc0) ? (nr_pages - 0xa0) : 0x20,
             0, &page_array[0xa0]) )
    {
        PERROR("Could not free VGA hole.\n");
        goto error_out;
    }

    if ( xc_domain_translate_gpfn_list(xc_handle, dom, nr_pages,
                                       page_array, page_array) )
    {
        PERROR("Could not translate addresses of HVM guest.\n");
        goto error_out;
    }

    loadelfimage(image, xc_handle, dom, page_array, &dsi);

    if ( set_hvm_info(xc_handle, dom, page_array, vcpus, acpi) )
    {
        ERROR("Couldn't set hvm info for HVM guest.\n");
        goto error_out;
    }

    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_PAE_ENABLED, pae);
    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_APIC_ENABLED, apic);

    if ( (e820_page = xc_map_foreign_range(
              xc_handle, dom, PAGE_SIZE, PROT_READ | PROT_WRITE,
              page_array[E820_MAP_PAGE >> PAGE_SHIFT])) == NULL )
        goto error_out;
    memset(e820_page, 0, PAGE_SIZE);
    build_e820map(e820_page, v_end);
    munmap(e820_page, PAGE_SIZE);

    /* shared_info page starts its life empty. */
    if ( (shared_info = xc_map_foreign_range(
              xc_handle, dom, PAGE_SIZE, PROT_READ | PROT_WRITE,
              shared_info_frame)) == NULL )
        goto error_out;
    memset(shared_info, 0, PAGE_SIZE);
    /* Mask all upcalls... */
    for ( i = 0; i < MAX_VIRT_CPUS; i++ )
        shared_info->vcpu_info[i].evtchn_upcall_mask = 1;
    memset(&shared_info->evtchn_mask[0], 0xff,
           sizeof(shared_info->evtchn_mask));
    munmap(shared_info, PAGE_SIZE);

    if ( v_end > HVM_BELOW_4G_RAM_END )
        shared_page_nr = (HVM_BELOW_4G_RAM_END >> PAGE_SHIFT) - 1;
    else
        shared_page_nr = (v_end >> PAGE_SHIFT) - 1;

    /* Paranoia: clean pages. */
    if ( xc_clear_domain_page(xc_handle, dom, page_array[shared_page_nr]) ||
         xc_clear_domain_page(xc_handle, dom, page_array[shared_page_nr-1]) ||
         xc_clear_domain_page(xc_handle, dom, page_array[shared_page_nr-2]) )
        goto error_out;

    *store_mfn = page_array[shared_page_nr - 1];
    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_STORE_PFN, shared_page_nr-1);
    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_STORE_EVTCHN, store_evtchn);
    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_BUFIOREQ_PFN, shared_page_nr-2);
    xc_set_hvm_param(xc_handle, dom, HVM_PARAM_IOREQ_PFN, shared_page_nr);

    free(page_array);

    ctxt->user_regs.eip = dsi.v_kernentry;

    return 0;

 error_out:
    free(page_array);
    return -1;
}

static int xc_hvm_build_internal(int xc_handle,
                                 uint32_t domid,
                                 int memsize,
                                 char *image,
                                 unsigned long image_size,
                                 unsigned int vcpus,
                                 unsigned int pae,
                                 unsigned int acpi,
                                 unsigned int apic,
                                 unsigned int store_evtchn,
                                 unsigned long *store_mfn)
{
    struct xen_domctl launch_domctl, domctl;
    int rc, i;
    vcpu_guest_context_t st_ctxt, *ctxt = &st_ctxt;

    if ( (image == NULL) || (image_size == 0) )
    {
        ERROR("Image required");
        goto error_out;
    }

    if ( lock_pages(&st_ctxt, sizeof(st_ctxt) ) )
    {
        PERROR("%s: ctxt mlock failed", __func__);
        return 1;
    }

    domctl.cmd = XEN_DOMCTL_getdomaininfo;
    domctl.domain = (domid_t)domid;
    if ( (xc_domctl(xc_handle, &domctl) < 0) ||
         ((uint16_t)domctl.domain != domid) )
    {
        PERROR("Could not get info on domain");
        goto error_out;
    }

    memset(ctxt, 0, sizeof(*ctxt));
    ctxt->flags = VGCF_HVM_GUEST;

    if ( setup_guest(xc_handle, domid, memsize, image, image_size,
                     ctxt, domctl.u.getdomaininfo.shared_info_frame,
                     vcpus, pae, acpi, apic, store_evtchn, store_mfn) < 0)
    {
        ERROR("Error constructing guest OS");
        goto error_out;
    }

    /* FPU is set up to default initial state. */
    memset(&ctxt->fpu_ctxt, 0, sizeof(ctxt->fpu_ctxt));

    /* Virtual IDT is empty at start-of-day. */
    for ( i = 0; i < 256; i++ )
    {
        ctxt->trap_ctxt[i].vector = i;
        ctxt->trap_ctxt[i].cs     = FLAT_KERNEL_CS;
    }

    /* No LDT. */
    ctxt->ldt_ents = 0;

    /* Use the default Xen-provided GDT. */
    ctxt->gdt_ents = 0;

    /* No debugging. */
    memset(ctxt->debugreg, 0, sizeof(ctxt->debugreg));

    /* No callback handlers. */
#if defined(__i386__)
    ctxt->event_callback_cs     = FLAT_KERNEL_CS;
    ctxt->event_callback_eip    = 0;
    ctxt->failsafe_callback_cs  = FLAT_KERNEL_CS;
    ctxt->failsafe_callback_eip = 0;
#elif defined(__x86_64__)
    ctxt->event_callback_eip    = 0;
    ctxt->failsafe_callback_eip = 0;
    ctxt->syscall_callback_eip  = 0;
#endif

    memset(&launch_domctl, 0, sizeof(launch_domctl));

    launch_domctl.domain = (domid_t)domid;
    launch_domctl.u.vcpucontext.vcpu   = 0;
    set_xen_guest_handle(launch_domctl.u.vcpucontext.ctxt, ctxt);

    launch_domctl.cmd = XEN_DOMCTL_setvcpucontext;
    rc = xc_domctl(xc_handle, &launch_domctl);

    return rc;

 error_out:
    return -1;
}

static inline int is_loadable_phdr(Elf32_Phdr *phdr)
{
    return ((phdr->p_type == PT_LOAD) &&
            ((phdr->p_flags & (PF_W|PF_X)) != 0));
}

static int parseelfimage(char *elfbase,
                         unsigned long elfsize,
                         struct domain_setup_info *dsi)
{
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)elfbase;
    Elf32_Phdr *phdr;
    Elf32_Shdr *shdr;
    unsigned long kernstart = ~0UL, kernend=0UL;
    char *shstrtab;
    int h;

    if ( !IS_ELF(*ehdr) )
    {
        ERROR("Kernel image does not have an ELF header.");
        return -EINVAL;
    }

    if ( (ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize)) > elfsize )
    {
        ERROR("ELF program headers extend beyond end of image.");
        return -EINVAL;
    }

    if ( (ehdr->e_shoff + (ehdr->e_shnum * ehdr->e_shentsize)) > elfsize )
    {
        ERROR("ELF section headers extend beyond end of image.");
        return -EINVAL;
    }

    /* Find the section-header strings table. */
    if ( ehdr->e_shstrndx == SHN_UNDEF )
    {
        ERROR("ELF image has no section-header strings table (shstrtab).");
        return -EINVAL;
    }
    shdr = (Elf32_Shdr *)(elfbase + ehdr->e_shoff +
                          (ehdr->e_shstrndx*ehdr->e_shentsize));
    shstrtab = elfbase + shdr->sh_offset;

    for ( h = 0; h < ehdr->e_phnum; h++ )
    {
        phdr = (Elf32_Phdr *)(elfbase + ehdr->e_phoff + (h*ehdr->e_phentsize));
        if ( !is_loadable_phdr(phdr) )
            continue;
        if ( phdr->p_paddr < kernstart )
            kernstart = phdr->p_paddr;
        if ( (phdr->p_paddr + phdr->p_memsz) > kernend )
            kernend = phdr->p_paddr + phdr->p_memsz;
    }

    if ( (kernstart > kernend) ||
         (ehdr->e_entry < kernstart) ||
         (ehdr->e_entry > kernend) )
    {
        ERROR("Malformed ELF image.");
        return -EINVAL;
    }

    dsi->v_start = 0x00000000;

    dsi->v_kernstart = kernstart;
    dsi->v_kernend   = kernend;
    dsi->v_kernentry = HVM_LOADER_ENTR_ADDR;

    dsi->v_end       = dsi->v_kernend;

    return 0;
}

static int
loadelfimage(
    char *elfbase, int xch, uint32_t dom, unsigned long *parray,
    struct domain_setup_info *dsi)
{
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)elfbase;
    Elf32_Phdr *phdr;
    int h;

    char         *va;
    unsigned long pa, done, chunksz;

    for ( h = 0; h < ehdr->e_phnum; h++ )
    {
        phdr = (Elf32_Phdr *)(elfbase + ehdr->e_phoff + (h*ehdr->e_phentsize));
        if ( !is_loadable_phdr(phdr) )
            continue;

        for ( done = 0; done < phdr->p_filesz; done += chunksz )
        {
            pa = (phdr->p_paddr + done) - dsi->v_start;
            if ((va = xc_map_foreign_range(
                xch, dom, PAGE_SIZE, PROT_WRITE,
                parray[pa >> PAGE_SHIFT])) == 0)
                return -1;
            chunksz = phdr->p_filesz - done;
            if ( chunksz > (PAGE_SIZE - (pa & (PAGE_SIZE-1))) )
                chunksz = PAGE_SIZE - (pa & (PAGE_SIZE-1));
            memcpy(va + (pa & (PAGE_SIZE-1)),
                   elfbase + phdr->p_offset + done, chunksz);
            munmap(va, PAGE_SIZE);
        }

        for ( ; done < phdr->p_memsz; done += chunksz )
        {
            pa = (phdr->p_paddr + done) - dsi->v_start;
            if ((va = xc_map_foreign_range(
                xch, dom, PAGE_SIZE, PROT_WRITE,
                parray[pa >> PAGE_SHIFT])) == 0)
                return -1;
            chunksz = phdr->p_memsz - done;
            if ( chunksz > (PAGE_SIZE - (pa & (PAGE_SIZE-1))) )
                chunksz = PAGE_SIZE - (pa & (PAGE_SIZE-1));
            memset(va + (pa & (PAGE_SIZE-1)), 0, chunksz);
            munmap(va, PAGE_SIZE);
        }
    }

    return 0;
}

/* xc_hvm_build
 *
 * Create a domain for a virtualized Linux, using files/filenames
 *
 */

int xc_hvm_build(int xc_handle,
                 uint32_t domid,
                 int memsize,
                 const char *image_name,
                 unsigned int vcpus,
                 unsigned int pae,
                 unsigned int acpi,
                 unsigned int apic,
                 unsigned int store_evtchn,
                 unsigned long *store_mfn)
{
    char *image;
    int  sts;
    unsigned long image_size;

    if ( (image_name == NULL) ||
         ((image = xc_read_image(image_name, &image_size)) == NULL) )
        return -1;

    sts = xc_hvm_build_internal(xc_handle, domid, memsize,
                                image, image_size,
                                vcpus, pae, acpi, apic,
                                store_evtchn, store_mfn);

    free(image);

    return sts;
}

/* xc_hvm_build_mem
 *
 * Create a domain for a virtualized Linux, using buffers
 *
 */

int xc_hvm_build_mem(int xc_handle,
                     uint32_t domid,
                     int memsize,
                     const char *image_buffer,
                     unsigned long image_size,
                     unsigned int vcpus,
                     unsigned int pae,
                     unsigned int acpi,
                     unsigned int apic,
                     unsigned int store_evtchn,
                     unsigned long *store_mfn)
{
    int           sts;
    unsigned long img_len;
    char         *img;

    /* Validate that there is a kernel buffer */

    if ( (image_buffer == NULL) || (image_size == 0) )
    {
        ERROR("kernel image buffer not present");
        return -1;
    }

    img = xc_inflate_buffer(image_buffer, image_size, &img_len);
    if (img == NULL)
    {
        ERROR("unable to inflate ram disk buffer");
        return -1;
    }

    sts = xc_hvm_build_internal(xc_handle, domid, memsize,
                                img, img_len,
                                vcpus, pae, acpi, apic,
                                store_evtchn, store_mfn);

    /* xc_inflate_buffer may return the original buffer pointer (for
       for already inflated buffers), so exercise some care in freeing */

    if ( (img != NULL) && (img != image_buffer) )
        free(img);

    return sts;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
