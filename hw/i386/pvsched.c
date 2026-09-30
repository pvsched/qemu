/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pvsched PCI device: the VMM side of the paravirtualized-scheduling
 * discovery transport (see Linux uapi/linux/pvsched_pci.h for the contract).
 *
 * BAR0 holds the control registers; BAR1 backs the one-page {apic_id, gpa}
 * entry table.  A doorbell write of N republishes the full set:
 *
 *   1. The whole table is validated first: N in range, every APIC id a
 *      distinct, running vCPU, every GPA a distinct, page-aligned page of
 *      ordinary writable guest RAM.  An invalid table changes nothing and
 *      reports EINVAL (or EFAULT for a non-RAM page) in the RESULT register.
 *   2. Every attached vCPU is detached (DETACH_SHM) and its RAM reference
 *      dropped.
 *   3. Each entry's vCPU thread is registered once as a pvsched runner
 *      (CREATE_RUNNER) and its page is attached (ATTACH_SHM).
 *
 * A doorbell write of 0 therefore detaches everything.  ATTACH_SHM is
 * synchronous: the host writes each page's negotiation status before the
 * ioctl returns, so the guest reads its statuses, and the RESULT register,
 * when the doorbell write retires.  A rejected negotiation (EPROTO) leaves
 * that vCPU unattached with the reason in its page; any other failure is
 * reported in RESULT, since the host could not write that page.  The device
 * never reads the pages itself.
 *
 * Each attached page keeps a reference on its RAM region until the host has
 * detached it.  Device reset and unrealize detach everything, so a rebooted
 * guest never leaves the host writing to memory it has reused.  Runners live
 * as long as the /dev/pvsched session, which the device holds open.  The
 * host pins the pages, so the device blocks migration.
 *
 * Lives in hw/i386 because the entry table names vCPUs by x86 APIC id.
 * QEMU needs CAP_SYS_NICE and access to /dev/pvsched.
 *
 * Usage: -device pvsched[,policy=default,policy-version=1]
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "qom/object.h"
#include "hw/core/cpu.h"

#include <sys/ioctl.h>

/*
 * Mirror of the Linux pvsched UAPI (<linux/pvsched.h> control ABI version 1
 * and <linux/pvsched_pci.h>), kept self-contained.
 */
#define PVSCHED_PCI_VENDOR_ID           0x1b36
#define PVSCHED_PCI_DEVICE_ID           0x11f0
#define PVSCHED_PCI_TABLE_SIZE          4096
#define PVSCHED_PCI_REG_DOORBELL        0x00
#define PVSCHED_PCI_REG_POLICY_VERSION  0x04
#define PVSCHED_PCI_REG_POLICY_NAME     0x08
#define PVSCHED_PCI_REG_KICK            0x28
#define PVSCHED_PCI_REG_RESULT          0x2c
#define PVSCHED_NAME_MAX                32
#define PVSCHED_VCPU_STRIDE             4096
#define PVSCHED_CONTROL_VERSION         1
#define PVSCHED_DEFAULT_POLICY_NAME     "default"
#define PVSCHED_DEFAULT_POLICY_VERSION  1

struct pvsched_pci_entry {
    uint64_t apic_id;
    uint64_t gpa;
};
#define PVSCHED_PCI_MAX_VCPUS \
    (PVSCHED_PCI_TABLE_SIZE / sizeof(struct pvsched_pci_entry))

struct pvsched_info {
    uint32_t control_version;
    uint32_t flags;
    uint32_t max_runners_per_session;
    uint32_t max_runners_global;
    uint32_t max_sessions_global;
    uint32_t max_shm_pages_per_session;
    uint32_t max_shm_pages_global;
    uint32_t reserved;
};

struct pvsched_create_runner {
    uint32_t flags;
    int32_t tid;
    uint64_t runner_id;
    uint64_t reserved[2];
};

struct pvsched_attach_shm {
    uint64_t runner_id;
    uint64_t user_addr;
    uint64_t size;
    uint32_t flags;
    uint32_t negotiation_status;
};

struct pvsched_detach_shm {
    uint64_t runner_id;
    uint64_t reserved[3];
};

#define PVSCHED_IOCTL_TYPE      0xb9
#define PVSCHED_GET_INFO \
    _IOR(PVSCHED_IOCTL_TYPE, 0, struct pvsched_info)
#define PVSCHED_CREATE_RUNNER \
    _IOWR(PVSCHED_IOCTL_TYPE, 1, struct pvsched_create_runner)
#define PVSCHED_ATTACH_SHM \
    _IOWR(PVSCHED_IOCTL_TYPE, 3, struct pvsched_attach_shm)
#define PVSCHED_DETACH_SHM \
    _IOW(PVSCHED_IOCTL_TYPE, 4, struct pvsched_detach_shm)

#define TYPE_PVSCHED "pvsched"
OBJECT_DECLARE_SIMPLE_TYPE(PVSchedState, PVSCHED)

/* A vCPU thread registered as a pvsched runner in this session. */
typedef struct PVSchedRunner {
    int tid;
    uint64_t runner_id;
} PVSchedRunner;

/* One validated table entry: its vCPU thread and referenced RAM page. */
typedef struct PVSchedPage {
    int tid;
    MemoryRegion *mr;           /* referenced until detached */
    void *hva;
    uint64_t runner_id;         /* valid once attached */
} PVSchedPage;

struct PVSchedState {
    PCIDevice pdev;
    MemoryRegion ctrl;
    MemoryRegion table_mr;
    uint8_t table[PVSCHED_PCI_TABLE_SIZE];
    PVSchedRunner runners[PVSCHED_PCI_MAX_VCPUS];
    unsigned int nr_runners;
    PVSchedPage attached[PVSCHED_PCI_MAX_VCPUS];
    unsigned int nr_attached;
    uint32_t result;            /* RESULT register: 0 or a positive errno */
    int fd;                     /* /dev/pvsched session while realized */
    Error *migration_blocker;
    EventNotifier kick;         /* KICK register ioeventfd */

    /*
     * The advertised policy (device properties).  The policy itself runs in
     * the host; the management plane chooses which one to advertise.
     */
    char *policy;
    uint32_t policy_version;
    uint8_t policy_buf[PVSCHED_NAME_MAX];
};

/*
 * APIC id -> thread id of a running vCPU; -1 if there is none.  An x86 CPU's
 * arch id is its 32-bit APIC id, so an id that does not fit matches nothing.
 */
static int pvsched_apicid_to_tid(uint64_t apic_id)
{
    CPUState *cs = cpu_by_arch_id(apic_id);

    return cs && cs->created && cs->thread_id > 0 ? cs->thread_id : -1;
}

/* The runner ID of vCPU thread @tid, registering it on first use. */
static int pvsched_runner_id(PVSchedState *s, int tid, uint64_t *runner_id)
{
    struct pvsched_create_runner create = { .tid = tid };
    unsigned int i;

    for (i = 0; i < s->nr_runners; i++) {
        if (s->runners[i].tid == tid) {
            *runner_id = s->runners[i].runner_id;
            return 0;
        }
    }
    if (s->nr_runners == ARRAY_SIZE(s->runners)) {
        return -ENOSPC;
    }
    if (ioctl(s->fd, PVSCHED_CREATE_RUNNER, &create) < 0) {
        return -errno;
    }
    s->runners[s->nr_runners].tid = tid;
    s->runners[s->nr_runners].runner_id = create.runner_id;
    s->nr_runners++;
    *runner_id = create.runner_id;
    return 0;
}

/*
 * Resolve @gpa to one whole, writable page of ordinary guest RAM and take a
 * reference on its region.  Never an MMIO bounce buffer, ROM or RAM device.
 */
static int pvsched_ram_page(uint64_t gpa, MemoryRegion **mrp, void **hvap)
{
    MemoryRegionSection section;
    MemoryRegion *mr;

    if (gpa & (PVSCHED_VCPU_STRIDE - 1)) {
        return -EINVAL;
    }
    section = memory_region_find(get_system_memory(), gpa,
                                 PVSCHED_VCPU_STRIDE);
    mr = section.mr;
    if (!mr) {
        return -EFAULT;
    }
    if (int128_get64(section.size) != PVSCHED_VCPU_STRIDE ||
        !memory_region_is_ram(mr) || memory_region_is_ram_device(mr) ||
        section.readonly) {
        memory_region_unref(mr);
        return -EFAULT;
    }
    *mrp = mr;
    *hvap = (uint8_t *)memory_region_get_ram_ptr(mr) +
            section.offset_within_region;
    return 0;
}

static void pvsched_put_pages(PVSchedPage *pages, unsigned int nr)
{
    unsigned int i;

    for (i = 0; i < nr; i++) {
        memory_region_unref(pages[i].mr);
    }
}

/*
 * Detach every attached page; the host no longer touches it on return.  A
 * page whose DETACH failed stays attached, with its RAM reference, and is
 * retried by the next doorbell or reset; session close detaches it last.
 * Returns 0 or the first failure as a negative errno.
 */
static int pvsched_detach_all(PVSchedState *s)
{
    unsigned int i, kept = 0;
    int first_error = 0;

    for (i = 0; i < s->nr_attached; i++) {
        PVSchedPage *p = &s->attached[i];
        struct pvsched_detach_shm detach = { .runner_id = p->runner_id };

        if (ioctl(s->fd, PVSCHED_DETACH_SHM, &detach) < 0) {
            error_report("pvsched: detach runner %" PRIu64 ": %s",
                         p->runner_id, strerror(errno));
            first_error = first_error ?: -errno;
            s->attached[kept++] = *p;
            continue;
        }
        memory_region_unref(p->mr);
    }
    s->nr_attached = kept;
    return first_error;
}

/*
 * Validate the first @n table entries into @pages, taking a RAM reference on
 * each.  On failure nothing is referenced and a negative errno is returned.
 */
static int pvsched_validate(PVSchedState *s, uint32_t n, PVSchedPage *pages)
{
    const struct pvsched_pci_entry *table = (const void *)s->table;
    unsigned int i, j;
    int ret;

    if (n > PVSCHED_PCI_MAX_VCPUS) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        uint64_t apic_id = table[i].apic_id;
        uint64_t gpa = table[i].gpa;

        pages[i].tid = pvsched_apicid_to_tid(apic_id);
        ret = pages[i].tid < 0 ? -EINVAL :
              pvsched_ram_page(gpa, &pages[i].mr, &pages[i].hva);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR, "pvsched: entry %u (APIC id %"
                          PRIu64 ", GPA 0x%" PRIx64 "): %s\n", i, apic_id,
                          gpa, strerror(-ret));
            pvsched_put_pages(pages, i);
            return ret;
        }
        for (j = 0; j < i; j++) {
            if (pages[j].tid == pages[i].tid ||
                pages[j].hva == pages[i].hva) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pvsched: entry %u repeats a vCPU or page\n", i);
                pvsched_put_pages(pages, i + 1);
                return -EINVAL;
            }
        }
    }
    return 0;
}

static void pvsched_forget_runner(PVSchedState *s, int tid)
{
    unsigned int i;

    for (i = 0; i < s->nr_runners; i++) {
        if (s->runners[i].tid == tid) {
            s->runners[i] = s->runners[--s->nr_runners];
            return;
        }
    }
}

static int pvsched_attach_page(PVSchedState *s, PVSchedPage *p,
                               struct pvsched_attach_shm *attach)
{
    int ret;

    memset(attach, 0, sizeof(*attach));
    ret = pvsched_runner_id(s, p->tid, &p->runner_id);
    if (ret) {
        return ret;
    }
    attach->runner_id = p->runner_id;
    attach->user_addr = (uintptr_t)p->hva;
    attach->size = PVSCHED_VCPU_STRIDE;
    return ioctl(s->fd, PVSCHED_ATTACH_SHM, attach) < 0 ? -errno : 0;
}

/* Republish the first @n table entries; 0 or a negative errno for RESULT. */
static int pvsched_doorbell(PVSchedState *s, uint32_t n)
{
    g_autofree PVSchedPage *pages = g_new0(PVSchedPage,
                                           PVSCHED_PCI_MAX_VCPUS);
    int ret, first_error = 0;
    unsigned int i;

    /* An invalid table changes nothing. */
    ret = pvsched_validate(s, n, pages);
    if (ret) {
        return ret;
    }

    ret = pvsched_detach_all(s);
    if (ret) {
        /* Never attach over a page the host may still write. */
        pvsched_put_pages(pages, n);
        return ret;
    }
    for (i = 0; i < n; i++) {
        PVSchedPage *p = &pages[i];
        struct pvsched_attach_shm attach;

        ret = pvsched_attach_page(s, p, &attach);
        if (ret == -ESRCH) {
            /*
             * The cached runner's thread is gone, e.g. an unplugged vCPU
             * whose thread id a new vCPU reuses: register the live thread.
             * The old runner keeps its session slot until the session
             * closes, so each replacement uses one more of the per-session
             * runner limit.
             */
            pvsched_forget_runner(s, p->tid);
            ret = pvsched_attach_page(s, p, &attach);
        }
        if (!ret) {
            s->attached[s->nr_attached++] = *p;
            continue;
        }
        memory_region_unref(p->mr);
        /*
         * EPROTO is a completed negotiation the host rejected, and the
         * guest reads the reason from its own page.  Any other failure
         * left the page unwritten, so report it in RESULT.
         */
        if (ret == -EPROTO) {
            qemu_log_mask(LOG_GUEST_ERROR, "pvsched: vCPU thread %d: "
                          "negotiation rejected (status %u)\n", p->tid,
                          attach.negotiation_status);
            continue;
        }
        error_report("pvsched: vCPU thread %d: attach: %s", p->tid,
                     strerror(-ret));
        first_error = first_error ?: ret;
    }
    return first_error;
}

static uint64_t pvsched_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    PVSchedState *s = opaque;
    uint64_t val = 0;

    if (addr == PVSCHED_PCI_REG_POLICY_VERSION) {
        return s->policy_version;
    }
    if (addr == PVSCHED_PCI_REG_RESULT) {
        return s->result;
    }
    if (addr >= PVSCHED_PCI_REG_POLICY_NAME &&
        addr + size <= PVSCHED_PCI_REG_POLICY_NAME + PVSCHED_NAME_MAX) {
        memcpy(&val, &s->policy_buf[addr - PVSCHED_PCI_REG_POLICY_NAME],
               size);
    }
    return val;
}

static void pvsched_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    PVSchedState *s = opaque;

    if (addr == PVSCHED_PCI_REG_DOORBELL) {
        s->result = -pvsched_doorbell(s, val);
    }
    /*
     * PVSCHED_PCI_REG_KICK is normally consumed by its ioeventfd.  Either
     * way the exit itself is the event: the host re-evaluates the vCPU on
     * its way back into the guest.
     */
}

static const MemoryRegionOps pvsched_ctrl_ops = {
    .read = pvsched_ctrl_read,
    .write = pvsched_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t pvsched_table_read(void *opaque, hwaddr addr, unsigned size)
{
    PVSchedState *s = opaque;
    uint64_t val = 0;

    memcpy(&val, &s->table[addr], size);
    return val;
}

static void pvsched_table_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    PVSchedState *s = opaque;

    memcpy(&s->table[addr], &val, size);
}

/* The core admits only aligned 4- or 8-byte accesses inside the table. */
static const MemoryRegionOps pvsched_table_ops = {
    .read = pvsched_table_read,
    .write = pvsched_table_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void pvsched_realize(PCIDevice *pdev, Error **errp)
{
    PVSchedState *s = PVSCHED(pdev);
    struct pvsched_info info;
    int ret;

    if (!s->policy) {
        s->policy = g_strdup(PVSCHED_DEFAULT_POLICY_NAME);
    }
    if (!s->policy[0] || strlen(s->policy) >= PVSCHED_NAME_MAX) {
        error_setg(errp, "pvsched: policy name must be 1..%d characters",
                   PVSCHED_NAME_MAX - 1);
        return;
    }
    strpadcpy((char *)s->policy_buf, sizeof(s->policy_buf), s->policy, 0);

    ret = event_notifier_init(&s->kick, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "pvsched: cannot create kick eventfd");
        return;
    }
    s->fd = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
    if (s->fd < 0) {
        error_setg_errno(errp, errno,
                         "pvsched: cannot open /dev/pvsched "
                         "(is pvsched loaded?)");
        goto err_kick;
    }
    if (ioctl(s->fd, PVSCHED_GET_INFO, &info) < 0 ||
        info.control_version != PVSCHED_CONTROL_VERSION) {
        error_setg(errp, "pvsched: unsupported host control interface");
        goto err_close;
    }

    /* The host pins guest pages for this process; they cannot migrate. */
    error_setg(&s->migration_blocker,
               "pvsched: guest pages are pinned by the host");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        goto err_close;
    }

    memory_region_init_io(&s->ctrl, OBJECT(s), &pvsched_ctrl_ops, s,
                          "pvsched-ctrl", 4096);
    /*
     * With KVM the kick then completes inside the kernel, without a return
     * to QEMU or the BQL.  Nothing reads the eventfd: the exit itself is
     * the event.  A full counter is harmless: KVM's ioeventfd saturates it
     * and event_notifier_set() ignores EAGAIN.
     */
    memory_region_add_eventfd(&s->ctrl, PVSCHED_PCI_REG_KICK, 4, false, 0,
                              &s->kick);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ctrl);

    memory_region_init_io(&s->table_mr, OBJECT(s), &pvsched_table_ops, s,
                          "pvsched-table", PVSCHED_PCI_TABLE_SIZE);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->table_mr);
    return;

err_close:
    close(s->fd);
err_kick:
    event_notifier_cleanup(&s->kick);
}

static void pvsched_exit(PCIDevice *pdev)
{
    PVSchedState *s = PVSCHED(pdev);

    /*
     * Closing the session detaches every page, including any a failed
     * DETACH left, and waits for the host's writers; only then drop the
     * RAM references.
     */
    close(s->fd);
    pvsched_put_pages(s->attached, s->nr_attached);
    s->nr_attached = 0;
    memory_region_del_eventfd(&s->ctrl, PVSCHED_PCI_REG_KICK, 4, false, 0,
                              &s->kick);
    event_notifier_cleanup(&s->kick);
    migrate_del_blocker(&s->migration_blocker);
}

/* A reset guest may reuse its pages for anything; stop the host first. */
static void pvsched_reset(DeviceState *dev)
{
    PVSchedState *s = PVSCHED(dev);

    s->result = -pvsched_detach_all(s);
    memset(s->table, 0, sizeof(s->table));
}

static const Property pvsched_properties[] = {
    DEFINE_PROP_STRING("policy", PVSchedState, policy),
    DEFINE_PROP_UINT32("policy-version", PVSchedState, policy_version,
                       PVSCHED_DEFAULT_POLICY_VERSION),
};

static void pvsched_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    device_class_set_props(dc, pvsched_properties);
    device_class_set_legacy_reset(dc, pvsched_reset);

    k->realize = pvsched_realize;
    k->exit = pvsched_exit;
    k->vendor_id = PVSCHED_PCI_VENDOR_ID;
    k->device_id = PVSCHED_PCI_DEVICE_ID;
    k->revision = 0;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "pvsched paravirtualized-scheduling transport";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pvsched_info = {
    .name = TYPE_PVSCHED,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PVSchedState),
    .class_init = pvsched_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pvsched_register_types(void)
{
    type_register_static(&pvsched_info);
}
type_init(pvsched_register_types);
