// SPDX-License-Identifier: BSD-2-Clause-Patent
// TrustZone's peripheral authentication service (PAS), for the WLAN image this board already parses.
//
// Why this file exists: stage 3 could read the phone's own wlanmdsp.mbn, check its hashes and describe the
// WLFW handshake, but it stopped at "no SCM/TZ call in this tree". Everything the WCN3990 needs next —
// authenticate the image, put it where the subsystem expects it, let it out of reset — is one secure-monitor
// call away, and the call's shape is fixed by the SoC, not by the phone:
//
//   svc 0x02 (PIL), cmd 0x01 init_image     pas_id, physical address of the image's header
//   svc 0x02 (PIL), cmd 0x02 mem_setup      pas_id, physical address of the region, size
//   svc 0x02 (PIL), cmd 0x05 auth_and_reset pas_id
//   svc 0x02 (PIL), cmd 0x06 shutdown       pas_id
//
// Conventions (qcom_scm-smc.c): the function word is (svc << 8 | cmd) | SCM_64 | SIP, x1 is the argument
// descriptor, x2..x4 the arguments, x6 the session id a preempted call resumes with. A call that was
// preempted returns 1 in x0 and must be re-issued with x0 = 1.
//
// The one thing this board does not answer by itself is *which* pas id owns the WLAN. The device tree names
// mss=4, lpass=1, turing=18, venus=9, ipa=15, npu=23 and kgsl=13, and no wlan node at all: on this SoC the
// WLAN is a child of the modem subsystem, not a PAS subsystem of its own. init_image is the only call that
// can be made safely while that is unknown — it uploads the image's header and hands back a context, and it
// starts nothing — so the probe below asks every id in turn and reports which ones accept the image. A wrong
// id answers with an error; nothing is reset, and no memory is taken.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
#include "WlanBoard.h"

// Boot-monitor diagnostics, alongside the ones WlanProbe publishes.
volatile uint32_t native_wlan_pas_id = 0;         // the id that accepted the image, or 0
volatile uint32_t native_wlan_pas_tried = 0;      // how many ids were asked
volatile int32_t native_wlan_pas_error = 0;       // that id's init_image answer
volatile uint32_t native_wlan_pas_answers[24] = {0}; // per-id result, for the log and the boot report
}

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_TWO_VALUES          2
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

// One call, made interruptible the way Linux makes it: a preempted call says so in x0 and is issued again
// with x0 = 1, carrying the session id it returned.
static int32_t scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second,
    uint64_t third, uint64_t *answer) {
    uint64_t session = 0;
    function |= SCM_64 | SCM_SIP;
    printf("PICASSO_WLAN pas: asking TrustZone %s: smc %#llx args %#llx/%#llx/%#llx/%#llx\n", what, function,
        arguments, first, second, third);
    for (int i = 0; i < 100; ++i) {
        register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
        register uint64_t x1 __asm__("x1") = arguments;
        register uint64_t x2 __asm__("x2") = first;
        register uint64_t x3 __asm__("x3") = second;
        register uint64_t x4 __asm__("x4") = third;
        register uint64_t x5 __asm__("x5") = 0;
        register uint64_t x6 __asm__("x6") = session;
        register uint64_t x7 __asm__("x7") = 0;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                         : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        session = x6;
        if (x0 != SCM_INTERRUPTED) {
            if (answer) *answer = x1;
            printf("PICASSO_WLAN pas: %s -> %d (answer %#llx)\n", what, (int32_t)x0, x1);
            return (int32_t)x0;
        }
    }
    printf("PICASSO_WLAN pas: %s keeps saying it was interrupted\n", what);
    return -1;
}

// The image's header, in memory TrustZone can read: header + hash table, i.e. the first page of the MBN the
// stage-3 parser already validated. A page of physically contiguous memory is enough for every PIL header
// this board's images carry (40-byte header, 12 bytes per hash entry, at most a few hundred entries).
struct PasPage {
    void *virt;
    uint64_t phys;
};

// Keep one page for the lifetime of the kernel. IOFreeContiguous() reaches
// IOGeneralMemoryDescriptor::complete() -> upl_commit_range() ->
// lck_rw_lock_exclusive() on a non-sleepable lock; from this boot-time probe
// thread that panics with "Taking non-sleepable RW lock with preemption
// enabled" (symbolized on boot-vangogh-ios12: wlan_pas_probe +0xfc). The PAS
// probe runs at most twice per boot and needs only 4 KiB, so retaining the
// page is strictly safer than resetting the phone 46 seconds into every boot.
static void *gPasPageVirt = nullptr;
static uint64_t gPasPagePhys = 0;

static PasPage pasMetadata(const void *mbn, size_t size, size_t keep) {
    PasPage page = {nullptr, 0};
    if (keep > size) keep = size;
    if (keep > PAGE_SIZE) keep = PAGE_SIZE;
    if (!gPasPageVirt) {
        IOPhysicalAddress phys = 0;
        gPasPageVirt = IOMallocContiguous(PAGE_SIZE, PAGE_SIZE, &phys);
        gPasPagePhys = phys;
    }
    if (!gPasPageVirt) return page;
    memset(gPasPageVirt, 0, PAGE_SIZE);
    memcpy(gPasPageVirt, mbn, keep);
    page.virt = gPasPageVirt;
    page.phys = gPasPagePhys;
    return page;
}

static void pasMetadataFree(PasPage &page) {
    // Deliberately does not call IOFreeContiguous(): see the comment above.
    page.virt = nullptr;
    page.phys = 0;
}

// Ask each candidate id to take the image's header. init_image uploads nothing to the subsystem and starts
// nothing: a wrong id answers with an error and leaves the phone as it was.
extern "C" int wlan_pas_probe(const void *mbn, size_t size, unsigned first, unsigned last) {
    if (!mbn || !size || first < 1) return -1;
    if (last > 23) last = 23;
    PasPage meta = pasMetadata(mbn, size, PAGE_SIZE);
    if (!meta.virt) {
        printf("PICASSO_WLAN pas: no page for the image's header\n");
        return -1;
    }
    const uint64_t phys = meta.phys;
    printf("PICASSO_WLAN pas: header at phys %#llx, %u bytes; ids %u..%u\n", (unsigned long long)phys,
        (unsigned)PAGE_SIZE, first, last);
    unsigned found = 0;
    int32_t lastError = 0;
    for (unsigned id = first; id <= last; ++id) {
        // 13 is the GPU's zap shader and 1/4 are the audio DSP and the modem: asking them is harmless (this
        // call starts nothing) but there is nothing to learn from them either, so they are skipped.
        if (id == 13 || id == 1 || id == 4) {
            native_wlan_pas_answers[id] = 0xffffffffu;
            printf("PICASSO_WLAN pas: id %u skipped (named by the device tree as another subsystem)\n", id);
            continue;
        }
        uint64_t answer = ~0ull;
        const int32_t rc = scm("init_image", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, id, phys, 0, &answer);
        native_wlan_pas_answers[id] = rc == 0 ? (uint32_t)answer : 0x80000000u | (uint32_t)(-rc & 0x7fffffffu);
        ++native_wlan_pas_tried;
        if (rc == 0) {
            ++found;
            if (!native_wlan_pas_id) {
                native_wlan_pas_id = id;
                native_wlan_pas_error = (int32_t)answer;
            }
            printf("PICASSO_WLAN pas: id %u ACCEPTS the wlan image (answer %#llx)\n", id, answer);
        } else {
            lastError = rc;
        }
    }
    pasMetadataFree(meta);
    if (!found) printf("PICASSO_WLAN pas: no id accepted the image; last error %d\n", lastError);
    printf("PICASSO_WLAN pas: probe done, tried=%u accepted=%u id=%u\n", native_wlan_pas_tried, found,
        native_wlan_pas_id);
    return found ? 0 : -1;
}

// The load itself, for an id the probe accepted: tell TrustZone where the image will sit, then let it check
// the signature and take the subsystem out of reset. Kept separate from the probe so that a boot can ask the
// first question without answering the second.
extern "C" int wlan_pas_load(const void *mbn, size_t size, unsigned id, uint64_t region, uint64_t regionSize) {
    if (!mbn || !size || !id || !region || !regionSize) return -1;
    PasPage meta = pasMetadata(mbn, size, PAGE_SIZE);
    if (!meta.virt) return -1;
    const uint64_t phys = meta.phys;
    uint64_t answer = ~0ull;
    int32_t rc = scm("init_image", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, id, phys, 0, &answer);
    if (rc == 0) rc = scm("mem_setup", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, id, region, regionSize, &answer);
    if (rc == 0) rc = scm("auth_and_reset", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, id, 0, 0, &answer);
    pasMetadataFree(meta);
    printf("PICASSO_WLAN pas: load id=%u region=%#llx/%llu -> rc=%d answer=%#llx\n", id,
        (unsigned long long)region, (unsigned long long)regionSize, rc, answer);
    return rc;
}
