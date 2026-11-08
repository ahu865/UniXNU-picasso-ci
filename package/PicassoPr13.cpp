// SPDX-License-Identifier: BSD-2-Clause-Patent
// PR13 board probes use the iOS16 display handoff as their late startup point.
// Keep the shared BootProgress UI disabled so it cannot replace iOS16 input.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>

extern "C" {
void native_adreno620_set_embedded_firmware(void);
void native_adreno_probe(void);
void native_wlan_probe(void);
void sheng_cpufreq_probe(void);
void sheng_cpufreq_poll(void);
void picasso_pr13_start(void);
// Read-only GPT reconnaissance of the adsp_a/adsp_b firmware partitions (NativeUfs). Retried while
// the UFS host is still coming up; off unless sheng-adsp=<n> is set.
int native_ufs_adsp_probe(void) __attribute__((weak_import));
// WLAN firmware is supplied by the generated wlan_embedded_firmware.c
// (scripts/build-wlan-picasso-firmware.py); if it were absent, the probe's
// weak-import references would resolve to null and stage 3 reports it.
volatile uint32_t native_legacy_gpu_fallbacks, native_legacy_gpu_last_status;
volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
void native_legacy_swap_stage(unsigned stage) { (void)stage; }
void native_legacy_gpu_fallback_observed(int status) {
    __atomic_add_fetch(&native_legacy_gpu_fallbacks, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&native_legacy_gpu_last_status, uint32_t(status), __ATOMIC_RELAXED);
}
}

static void probes(void *, wait_result_t) {
    sheng_cpufreq_probe();
    native_adreno620_set_embedded_firmware();
    native_adreno_probe();
    native_wlan_probe();
    bool reported = false;
    unsigned adspAttempts = 0;
    for (;;) {
        sheng_cpufreq_poll();
        // The UFS host is ready long before the scheduler gets here, but keep the retry bounded in
        // case the probe thread races the bring-up on a cold boot.
        if (native_ufs_adsp_probe && adspAttempts < 10) {
            ++adspAttempts;
            const int adsp = native_ufs_adsp_probe();
            if (adsp >= 0) adspAttempts = 10;  // off, done, or refused: stop asking
        }
        const ml_topology_info_t *topology = ml_get_topology_info();
        if (!reported && topology) {
            const unsigned en = ml_get_cpu_number_type(CLUSTER_TYPE_E, true, true);
            const unsigned pn = ml_get_cpu_number_type(CLUSTER_TYPE_P, true, true);
            if (en + pn >= topology->num_cpus) {
                reported = true;
                IOLog("PICASSO_CPUFREQ topo online cpus=%u clusters=%u types=%x en=%u pn=%u cl-e=%u cl-p=%u\n",
                    topology->num_cpus, topology->num_clusters, topology->cluster_types, en, pn,
                    ml_get_cluster_number_type(CLUSTER_TYPE_E), ml_get_cluster_number_type(CLUSTER_TYPE_P));
            }
        }
        IOSleep(1000);
    }
}

extern "C" void picasso_pr13_start(void) {
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread = nullptr;
    const kern_return_t status = kernel_thread_start(probes, nullptr, &thread);
    if (status == KERN_SUCCESS) thread_deallocate(thread);
    else {
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
        IOLog("PICASSO_PR13 worker failed status=%x\n", status);
    }
}
