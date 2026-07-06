/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pvsched PCI device: the VMM side of the paravirt-scheduling discovery
 * transport (see Linux uapi/linux/pvsched_pci.h for the contract).
 *
 * BAR0 is a control region whose register 0 is the doorbell; BAR1 backs the
 * one-page {apic_id, gpa} entry table. A doorbell write of N (re)publishes
 * the full set: for each of the first N table entries the device translates
 * the APIC id to the vCPU thread tid and the GPA to a host virtual address,
 * and registers {tid, hva} with the host pvsched framework via /dev/pvsched.
 * Previously registered vCPUs are unregistered first (full-republish
 * semantics), so a doorbell write of 0 tears everything down. The MMIO trap
 * is synchronous and the framework writes each page's status before the
 * ioctl returns, so the guest reads its statuses when the doorbell write
 * retires.
 *
 * Lives in hw/i386 (not hw/misc) because the APIC-id lookup needs X86CPU.
 * Targets QEMU v9.2.x. QEMU must run with CAP_SYS_NICE and access to
 * /dev/pvsched (the ioctls require both).
 *
 * Usage: -device pvsched
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "qom/object.h"
#include "cpu.h"		/* X86CPU; target-specific build */

#include <sys/ioctl.h>

/* Mirror of the Linux pvsched uapi (stable contract; kept self-contained). */
#define PVSCHED_PCI_VENDOR_ID	0x1b36
#define PVSCHED_PCI_DEVICE_ID	0x11f0
#define PVSCHED_PCI_TABLE_SIZE	4096
#define PVSCHED_PCI_REG_DOORBELL	0x00
#define PVSCHED_PCI_REG_POLICY_VERSION	0x04
#define PVSCHED_PCI_REG_POLICY_NAME	0x08
#define PVSCHED_PCI_REG_KICK		0x28
#define PVSCHED_NAME_MAX	32
#define PVSCHED_VCPU_STRIDE	4096

struct pvsched_pci_entry {
    uint64_t apic_id;
    uint64_t gpa;
};
#define PVSCHED_PCI_MAX_VCPUS \
    (PVSCHED_PCI_TABLE_SIZE / sizeof(struct pvsched_pci_entry))

struct pvsched_reg_vcpu {
    uint32_t tid;
    uint32_t pad;
    uint64_t shm_hva;
};
#define PVSCHED_IOC_MAGIC		0xB7
#define PVSCHED_IOC_REGISTER_VCPU	_IOW(PVSCHED_IOC_MAGIC, 1, struct pvsched_reg_vcpu)
#define PVSCHED_IOC_UNREGISTER_VCPU	_IOW(PVSCHED_IOC_MAGIC, 2, uint32_t)

#define TYPE_PVSCHED "pvsched"
OBJECT_DECLARE_SIMPLE_TYPE(PVSchedState, PVSCHED)

typedef struct PVSchedVcpuReg {
    uint32_t tid;
    void *hva;
    hwaddr maplen;
    bool registered;
} PVSchedVcpuReg;

struct PVSchedState {
    PCIDevice pdev;
    MemoryRegion ctrl;
    MemoryRegion table_mr;
    uint8_t table[PVSCHED_PCI_TABLE_SIZE];
    PVSchedVcpuReg reg[PVSCHED_PCI_MAX_VCPUS];
    unsigned int nr_reg;
    int fd;			/* /dev/pvsched */

    /* The advertised policy (device properties; the management plane's
     * choice -- the policy itself is loaded host-side, not by the VMM). */
    char *policy;
    uint32_t policy_version;
    uint8_t policy_buf[PVSCHED_NAME_MAX];
};

/* APIC id -> vCPU thread tid; -1 if no such vCPU. */
static int pvsched_apicid_to_tid(uint64_t apic_id)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        if (X86_CPU(cs)->apic_id == apic_id) {
            return cs->thread_id;
        }
    }
    return -1;
}

static void pvsched_unregister_all(PVSchedState *s)
{
    unsigned int i;

    for (i = 0; i < s->nr_reg; i++) {
        PVSchedVcpuReg *r = &s->reg[i];

        if (r->registered &&
            ioctl(s->fd, PVSCHED_IOC_UNREGISTER_VCPU, &r->tid) < 0) {
            warn_report("pvsched: unregister tid %u: %s", r->tid,
                        strerror(errno));
        }
        if (r->hva) {
            address_space_unmap(&address_space_memory, r->hva, r->maplen,
                                true, r->maplen);
        }
        memset(r, 0, sizeof(*r));
    }
    s->nr_reg = 0;
}

static void pvsched_doorbell(PVSchedState *s, uint32_t n)
{
    const struct pvsched_pci_entry *table = (const void *)s->table;
    unsigned int i;

    /* Full-republish semantics: drop the old set, register the new one. */
    pvsched_unregister_all(s);

    n = MIN(n, PVSCHED_PCI_MAX_VCPUS);
    for (i = 0; i < n; i++) {
        uint64_t apic_id = table[i].apic_id;
        uint64_t gpa = table[i].gpa;
        PVSchedVcpuReg *r = &s->reg[s->nr_reg];
        struct pvsched_reg_vcpu reg;
        hwaddr len = PVSCHED_VCPU_STRIDE;
        int tid;

        tid = pvsched_apicid_to_tid(apic_id);
        if (tid < 0) {
            warn_report("pvsched: entry %u: no vCPU with APIC id %" PRIu64,
                        i, apic_id);
            continue;
        }

        r->hva = address_space_map(&address_space_memory, gpa, &len, true,
                                   MEMTXATTRS_UNSPECIFIED);
        if (!r->hva || len < PVSCHED_VCPU_STRIDE) {
            warn_report("pvsched: entry %u: cannot map GPA 0x%" PRIx64, i,
                        gpa);
            if (r->hva) {
                address_space_unmap(&address_space_memory, r->hva, len, false,
                                    0);
                r->hva = NULL;
            }
            continue;
        }
        r->maplen = len;
        r->tid = tid;

        /*
         * Synchronous: the framework matches the policy and writes the
         * page's status before this returns, i.e. before the guest's
         * doorbell write retires.
         */
        reg.tid = tid;
        reg.pad = 0;
        reg.shm_hva = (uintptr_t)r->hva;
        if (ioctl(s->fd, PVSCHED_IOC_REGISTER_VCPU, &reg) < 0) {
            /*
             * -ENOENT is expected, not a failure: no host policy matched the
             * guest's request, so this vCPU simply runs unparavirtualized. The
             * guest reads the precise reason from its own page status; we are a
             * courier and never dereference the page. Anything else is a real
             * error (bad handle, no such thread, already bound -- a VMM bug).
             */
            if (errno == ENOENT) {
                info_report("pvsched: tid %d: no matching host policy; "
                            "vCPU runs unparavirtualized", tid);
            } else {
                warn_report("pvsched: register tid %d: %s", tid,
                            strerror(errno));
            }
            address_space_unmap(&address_space_memory, r->hva, r->maplen,
                                true, r->maplen);
            memset(r, 0, sizeof(*r));
            continue;
        }
        r->registered = true;
        s->nr_reg++;
    }
}

static uint64_t pvsched_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    PVSchedState *s = opaque;
    uint64_t val = 0;

    if (addr == PVSCHED_PCI_REG_POLICY_VERSION) {
        return s->policy_version;
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

    if (addr == PVSCHED_PCI_REG_DOORBELL && size == 4) {
        pvsched_doorbell(s, val);
    }
    /*
     * PVSCHED_PCI_REG_KICK needs no handling: the MMIO trap itself is the
     * event (the host policy samples guest_area at the resulting vmexit).
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

    if (addr + size <= sizeof(s->table)) {
        memcpy(&val, &s->table[addr], size);
    }
    return val;
}

static void pvsched_table_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    PVSchedState *s = opaque;

    if (addr + size <= sizeof(s->table)) {
        memcpy(&s->table[addr], &val, size);
    }
}

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

    if (!s->policy) {
        s->policy = g_strdup("demo");
    }
    if (strlen(s->policy) >= PVSCHED_NAME_MAX) {
        error_setg(errp, "pvsched: policy name longer than %d",
                   PVSCHED_NAME_MAX - 1);
        return;
    }
    memset(s->policy_buf, 0, sizeof(s->policy_buf));
    strcpy((char *)s->policy_buf, s->policy);

    s->fd = open("/dev/pvsched", O_RDWR);
    if (s->fd < 0) {
        error_setg_errno(errp, errno,
                         "pvsched: cannot open /dev/pvsched "
                         "(is pvsched.ko loaded?)");
        return;
    }

    memory_region_init_io(&s->ctrl, OBJECT(s), &pvsched_ctrl_ops, s,
                          "pvsched-ctrl", 4096);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ctrl);

    memory_region_init_io(&s->table_mr, OBJECT(s), &pvsched_table_ops, s,
                          "pvsched-table", PVSCHED_PCI_TABLE_SIZE);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->table_mr);
}

static void pvsched_exit(PCIDevice *pdev)
{
    PVSchedState *s = PVSCHED(pdev);

    pvsched_unregister_all(s);
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static const Property pvsched_properties[] = {
    DEFINE_PROP_STRING("policy", PVSchedState, policy),
    DEFINE_PROP_UINT32("policy-version", PVSchedState, policy_version, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void pvsched_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    device_class_set_props(dc, pvsched_properties);

    k->realize = pvsched_realize;
    k->exit = pvsched_exit;
    k->vendor_id = PVSCHED_PCI_VENDOR_ID;
    k->device_id = PVSCHED_PCI_DEVICE_ID;
    k->revision = 0;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "pvsched paravirt-scheduling transport";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pvsched_info = {
    .name = TYPE_PVSCHED,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PVSchedState),
    .class_init = pvsched_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pvsched_register_types(void)
{
    type_register_static(&pvsched_info);
}
type_init(pvsched_register_types);
