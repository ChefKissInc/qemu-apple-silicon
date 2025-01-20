#include "hw/arm/apple-silicon/boot.h"
#include "hw/arm/apple-silicon/mem.h"
#include "hw/arm/apple-silicon/xnu_pf.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"

#define NOP 0xd503201f
#define RET 0xd65f03c0
#define RETAB 0xd65f0fff
#define PACIBSP 0xd503237f

static uint32_t *find_next_insn(uint32_t *from, uint32_t num, uint32_t insn,
                                uint32_t mask)
{
    while (num) {
        if ((*from & mask) == (insn & mask)) {
            return from;
        }
        from++;
        num--;
    }

    /* not found */
    return NULL;
}

static uint32_t *find_prev_insn(uint32_t *from, uint32_t num, uint32_t insn,
                                uint32_t mask)
{
    while (num) {
        if ((*from & mask) == (insn & mask)) {
            return from;
        }
        from--;
        num--;
    }

    /* not found */
    return NULL;
}

static bool kpf_apfs_rootauth(ApplePfPatch *patch, uint32_t *opcode_stream)
{
    opcode_stream[0] = NOP;
    opcode_stream[1] = 0x52800000; /* mov w0, 0 */

    puts("KPF: found handle_eval_rootauth");
    return true;
}

static bool kpf_apfs_vfsop_mount(ApplePfPatch *patch, uint32_t *opcode_stream)
{
    opcode_stream[0] = 0x52800000; /* mov w0, 0 */
    puts("KPF: found apfs_vfsop_mount");
    return true;
}

static void kpf_apfs_patches(ApplePfPatchset *patchset)
{
    /*
     * This patch bypass root authentication
     * address for kernelcache.research.iphone12b of 15.0 (19A5261w)
     * handle_eval_rootauth:
     * 086040f9       ldr x8, [x0, 0xc0]
     * 08e14039       ldrb w8, [x8, 0x38]
     * 68002837       tbnz w8, 5, 0xfffffff0095ade9c <- find this
     * 000a8052       mov w0, 0x50 -> mov w0, 0
     * c0035fd6       ret
     * d5ccfd17       b  _authapfs_seal_is_broken_full
     */
    uint64_t matches[] = {
        0x37280068, // tbnz w8, 5, 0xc
        0x52800a00, // mov w0, 0x50
        0xd65f03c0 // ret
    };
    uint64_t masks[] = { 0xffffffff, 0xffffffff, 0xffffffff };

    /* apfs_vfsop_mount:
     * This patch allows mount -urw /
     * vfs_flags() & MNT_ROOTFS
     * 0xfffffff009046d50      e0147037       tbnz w0, 0xe, 0xfffffff009046fec
     * 0xfffffff009046d54      e83b40b9       ldr w8, [sp, 0x38]  ; 5
     * 0xfffffff009046d58      08791f12       and w8, w8, 0xfffffffe
     * 0xfffffff009046d5c      e83b00b9       str w8, [sp, 0x38]
     */
    uint64_t matches2[] = {
        0x37700000, // tbnz w0, 0xe, *
        0xb94003a0, // ldr x*, [x29/sp, *]
        0x121f7800, // and w*, w*, 0xfffffffe
        0xb90003a0, // str x*, [x29/sp, *]
    };

    uint64_t masks2[] = {
        0xfff8001f,
        0xfffe03a0,
        0xfffffc00,
        0xffc003a0,
    };

    xnu_pf_maskmatch(patchset, "handle_eval_rootauth", matches, masks,
                     sizeof(masks) / sizeof(uint64_t), false,
                     (void *)kpf_apfs_rootauth);

    xnu_pf_maskmatch(patchset, "apfs_vfsop_mount", matches2, masks2,
                     sizeof(masks2) / sizeof(uint64_t), false,
                     (void *)kpf_apfs_vfsop_mount);
}

static bool kpf_amfi_callback(ApplePfPatch *patch, uint32_t *opcode_stream)
{
    /* possibly AMFI patch
     * this is here to patch out the trustcache checks
     *  so that AMFI thinks that everything is in trustcache
     * there are two different versions of the trustcache function
     *  lookup_in_trust_cache_module has cdhash in x1
     *  lookup_in_static_trust_cache has cdhash in x0
     * The former one requires [x2] = 2 and [x3] = 0
     * both of them are patched to return 1
     */
    if (((opcode_stream[-1] & 0xFF000000) != 0x91000000) &&
        ((opcode_stream[-2] & 0xFF000000) != 0x91000000)) {
        /* add x*
         * ldrb (optional and only on iOS 15.5b4+)
         */
        return false;
    }
    bool found_something = false;
    /* find ldrb w*, [x*, 0xb] */
    uint32_t *ldrb = find_next_insn(opcode_stream, 256, 0x39402c00, 0xfffffc00);
    uint32_t cdhash_param = extract32(*ldrb, 5, 5);
    uint32_t *frame = NULL;
    uint32_t *start = opcode_stream;
    bool pac = false;

    /* Most reliable marker of a stack frame seems to be "add x29, sp, 0x...".
     */
    frame = find_prev_insn(opcode_stream, 10, 0x910003fd, 0xff8003ff);
    if (!frame) {
        puts("kpf_amfi_callback: Found AMFI (Leaf)");
    } else {
        puts("kpf_amfi_callback: Found AMFI (Routine)");
        start = find_prev_insn(frame, 10, 0xa9a003e0, 0xffe003e0);
        if (!start) {
            start = find_prev_insn(frame, 10, 0xd10003ff, 0xff8003ff);
        }
        if (!start) {
            puts("kpf_amfi_callback: failed to find start");
            return false;
        }
    }

    pac = find_prev_insn(start, 5, PACIBSP, 0xffffffff) != NULL;
    switch (cdhash_param) {
    case 0: {
        /* ADRP x8, * */
        uint32_t *adrp = find_prev_insn(start, 10, 0x90000008, 0x9f00001f);
        if (adrp) {
            start = adrp;
        }
        fprintf(stderr,
                "%s: Found lookup_in_static_trust_cache @ 0x%" PRIx64 "\n",
                __func__, ptov_static((hwaddr)start));
        /* XXX: allows amfid to do its work
         * this also allows amfid impersonation
         */
        *(start++) = 0x52802020; /* MOV W0, 0x101 */
        *(start++) = (pac ? RETAB : RET);
        found_something = true;
        break;
    }
    case 1:
        fprintf(stderr,
                "%s: Found lookup_in_trust_cache_module @ 0x%" PRIx64 "\n",
                __func__, ptov_static((hwaddr)start));
        *(start++) = 0x52800040; /* mov w0, 2 */
        *(start++) = 0x39000040; /* strb w0, [x2] */
        /* XXX: allows amfid to do its work
         * this also allows amfid impersonation
         */
        *(start++) = 0x52800020; /* mov w0, 1 */
        *(start++) = 0x39000060; /* strb w0, [x3] */
        *(start++) = 0x52800020; /* MOV W0, 1 */
        *(start++) = (pac ? RETAB : RET);
        found_something = true;
        break;
    default:
        puts("kpf_amfi_callback: Found unexpected prototype");
        break;
    }
    if (!found_something) {
        puts("kpf_amfi_callback: failed to patch anything");
    }
    return found_something;
}

static void kpf_amfi_patch(ApplePfPatchset *xnu_text_exec_patchset)
{
    /* This patch leads to AMFI believing that everything is in trustcache
     * this is done by searching for the sequence below (example from an iPhone
     * 7, 13.3): 0xfffffff0072382b0      29610091       add x9, x9, 0x18
     * 0xfffffff0072382b4      ca028052       movz w10, 0x16
     * 0xfffffff0072382b8      0bfd41d3       lsr x11, x8, 1
     * 0xfffffff0072382bc      6c250a9b       madd x12, x11, x10, x9
     * then the callback checks if this is just a leaf instead of a full
     * routinue if it isn't a leaf then it finds the start of the routine if
     * cdhash is in x1 then [x2] = 2 and [x3] = 0 Finally patches them to return
     * true To find the patch in r2 use: /x
     * 0000009100028052000000d30000009b:000000FF00FFFFFF000000FF000000FF
     */
    uint64_t matches[] = {
        0x52800200, // mov w*, 0x16
        0xd3000000, // lsr *
        0x9b000000 // madd *
    };
    uint64_t masks[] = { 0xFFFFFF00, 0xFF000000, 0xFF000000 };
    xnu_pf_maskmatch(xnu_text_exec_patchset, "amfi_patch", matches, masks,
                     sizeof(matches) / sizeof(uint64_t), true,
                     (void *)kpf_amfi_callback);
}

static bool kpf_found_trustcache = false;

static bool kpf_trustcache_callback(ApplePfPatch *patch,
                                    uint32_t *opcode_stream)
{
    if (kpf_found_trustcache) {
        return false;
    }
    uint32_t *start = find_prev_insn(opcode_stream, 100, PACIBSP, 0xffffffff);

    if (!start) {
        return false;
    }
    kpf_found_trustcache = true;
    fprintf(stderr,
            "%s: Found pmap_lookup_in_static_trust_cache_internal "
            "@ 0x%" PRIx64 "\n",
            __func__, ptov_static((hwaddr)start));
    *(start++) = 0x52802020; /* MOV W0, 0x101 */
    *(start++) = RET;
    return true;
}

static void kpf_trustcache_patch(ApplePfPatchset *patchset)
{
    // This patch leads to AMFI believing that everything is in trustcache.
    uint64_t matches[] = {
        0xd29dcfc0, // mov w*, 0xee7e
    };
    uint64_t masks[] = {
        0xffffffc0,
    };
    xnu_pf_maskmatch(patchset, "trustcache16", matches, masks,
                     sizeof(matches) / sizeof(uint64_t), true,
                     (void *)kpf_trustcache_callback);
}

static bool kpf_amfi_sha1(ApplePfPatch *patch, uint32_t *opcode_stream)
{
    uint32_t *cmp = find_next_insn(opcode_stream, 0x10, 0x7100081f,
                                   0xFFFFFFFF); /* cmp w0, 2 */
    if (!cmp) {
        puts("kpf_amfi_sha1: failed to find cmp");
        return false;
    }
    puts("KPF: Found AMFI hashtype check");
    xnu_pf_disable_patch(patch);
    *cmp = 0x6b00001f; /* cmp w0, w0 */
    return true;
}

static void kpf_amfi_kext_patches(ApplePfPatchset *patchset)
{
    /* this patch allows us to run binaries with SHA1 signatures
     * this is done by searching for the sequence below
     *  and then finding the cmp w0, 2 (hashtype) and turning that into a cmp
     * w0, w0 Example from i7 13.3: 0xfffffff005f36b30      2201d036       tbz
     * w2, 0x1a, 0xfffffff005f36b54 0xfffffff005f36b34      f30305aa       mov
     * x19, x5 0xfffffff005f36b38      f40304aa       mov x20, x4
     * 0xfffffff005f36b3c      f50303aa       mov x21, x3
     * 0xfffffff005f36b40      f60300aa       mov x22, x0
     * 0xfffffff005f36b44      e00301aa       mov x0, x1
     * 0xfffffff005f36b48      a1010094       bl sym.stub._csblob_get_hashtype
     * 0xfffffff005f36b4c      1f080071       cmp w0, 2
     * 0xfffffff005f36b50      61000054       b.ne 0xfffffff005f36b5c
     * to find this in r2 run (make sure to check if the address is aligned):
     * /x 0200d036:1f00f8ff
     */
    uint64_t i_matches[] = {
        0x36d00002, // tbz w2, 0x1a, *
    };
    uint64_t i_masks[] = {
        0xfff8001f,
    };
    xnu_pf_maskmatch(patchset, "amfi_sha1", i_matches, i_masks,
                     sizeof(i_matches) / sizeof(uint64_t), true,
                     (void *)kpf_amfi_sha1);
}

bool kpf_has_done_mac_mount;
static bool kpf_mac_mount_callback(ApplePfPatch *patch, uint32_t *opcode_stream)
{
    puts("KPF: Found mac_mount");
    uint32_t *mac_mount = &opcode_stream[0];
    /* search for tbnz w*, 5, *
     * and nop it (enable MNT_UNION mounts)
     */
    uint32_t *mac_mount_1 =
        find_prev_insn(mac_mount, 0x40, 0x37280000, 0xfffe0000);

    if (!mac_mount_1) {
        mac_mount_1 = find_next_insn(mac_mount, 0x40, 0x37280000, 0xfffe0000);
    }
    if (!mac_mount_1) {
        kpf_has_done_mac_mount = false;
        puts("kpf_mac_mount_callback: failed to find NOP point");
        return false;
    }

    mac_mount_1[0] = NOP;
    /* search for ldrb w8, [x*, 0x71] */
    mac_mount_1 = find_prev_insn(mac_mount, 0x40, 0x3941c408, 0xfffffc1f);
    if (!mac_mount_1) {
        mac_mount_1 = find_next_insn(mac_mount, 0x40, 0x3941c408, 0xfffffc1f);
    }
    if (!mac_mount_1) {
        kpf_has_done_mac_mount = false;
        puts("kpf_mac_mount_callback: failed to find xzr point");
        return false;
    }

    /* replace with a mov x8, xzr */
    /* this will bypass the (vp->v_mount->mnt_flag & MNT_ROOTFS) check */
    mac_mount_1[0] = 0xaa1f03e8;
    kpf_has_done_mac_mount = true;
    xnu_pf_disable_patch(patch);

    puts("KPF: Found mac_mount");
    return true;
}

static void kpf_mac_mount_patch(ApplePfPatchset *xnu_text_exec_patchset)
{
    /*
     * This patch makes sure that we can remount the rootfs and that we can
     * UNION mount we first search for a pretty unique instruction movz/orr w9,
     * 0x1ffe then we search for a tbnz w*, 5, * (0x20 is MNT_UNION) and nop it
     * After that we search for a ldrb w8, [x8, 0x71] and replace it with a movz
     * x8, 0 at 0x70 there are the flags and MNT_ROOTFS is 0x00004000 -> 0x4000
     * >> 8 -> 0x40 -> bit 6 -> the check is right below that way we can also
     * perform operations on the rootfs
     */
    uint64_t matches[] = {
        0x321f2fe9, /* orr w9, wzr, 0x1ffe */
    };
    uint64_t masks[] = {
        0xFFFFFFFF,
    };

    xnu_pf_maskmatch(xnu_text_exec_patchset, "mac_mount_patch1", matches, masks,
                     sizeof(matches) / sizeof(uint64_t), false,
                     (void *)kpf_mac_mount_callback);
    matches[0] = 0x5283ffc9; /* movz w9, 0x1ffe */
    xnu_pf_maskmatch(xnu_text_exec_patchset, "mac_mount_patch2", matches, masks,
                     sizeof(matches) / sizeof(uint64_t), false,
                     (void *)kpf_mac_mount_callback);
}

void xnu_kpf(void)
{
    MachoHeader64 *hdr = xnu_header;
    ApplePfPatchset *xnu_text_exec_patchset =
        xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT);
    g_autofree ApplePfRange *text_exec_range = xnu_pf_get_actual_text_exec(hdr);
    ApplePfPatchset *xnu_ppl_text_patchset =
        xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT);
    g_autofree ApplePfRange *ppltext_exec_range =
        xnu_pf_section(hdr, "__PPLTEXT", "__text");

    ApplePfPatchset *apfs_patchset;
    MachoHeader64 *apfs_header;
    g_autofree ApplePfRange *apfs_text_exec_range = NULL;

    MachoHeader64 *amfi_header;
    ApplePfPatchset *amfi_patchset;
    g_autofree ApplePfRange *amfi_text_exec_range = NULL;


    apfs_patchset = xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT);
    apfs_header = xnu_pf_get_kext_header(hdr, "com.apple.filesystems.apfs");
    apfs_text_exec_range = xnu_pf_section(apfs_header, "__TEXT_EXEC", "__text");

    kpf_apfs_patches(apfs_patchset);
    xnu_pf_apply(apfs_text_exec_range, apfs_patchset);
    xnu_pf_patchset_destroy(apfs_patchset);

    amfi_patchset = xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT);
    amfi_header = xnu_pf_get_kext_header(
        hdr, "com.apple.driver.AppleMobileFileIntegrity");
    amfi_text_exec_range = xnu_pf_section(amfi_header, "__TEXT_EXEC", "__text");
    kpf_amfi_kext_patches(amfi_patchset);
    xnu_pf_apply(amfi_text_exec_range, amfi_patchset);
    xnu_pf_patchset_destroy(amfi_patchset);

    kpf_amfi_patch(xnu_text_exec_patchset);
    kpf_mac_mount_patch(xnu_text_exec_patchset);
    xnu_pf_apply(text_exec_range, xnu_text_exec_patchset);
    xnu_pf_patchset_destroy(xnu_text_exec_patchset);

    kpf_amfi_patch(xnu_ppl_text_patchset);
    kpf_trustcache_patch(xnu_ppl_text_patchset);
    if (ppltext_exec_range) {
        xnu_pf_apply(ppltext_exec_range, xnu_ppl_text_patchset);
    } else {
        info_report("warning: failed to find __PPLTEXT");
    }
    xnu_pf_patchset_destroy(xnu_ppl_text_patchset);
}
