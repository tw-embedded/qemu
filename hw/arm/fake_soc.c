#if 0
#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/option.h"
#include "monitor/qdev.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/arm/boot.h"
#include "hw/arm/primecell.h"
#include "hw/arm/virt.h"
#include "hw/block/flash.h"
#include "hw/vfio/vfio-calxeda-xgmac.h"
#include "hw/vfio/vfio-amd-xgbe.h"
#include "hw/display/ramfb.h"
#include "net/net.h"
#include "sysemu/device_tree.h"
#include "sysemu/numa.h"
#include "sysemu/runstate.h"
#include "sysemu/tpm.h"
#include "sysemu/kvm.h"
#include "sysemu/hvf.h"
#include "hw/loader.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/pci-host/gpex.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/core/sysbus-fdt.h"
#include "hw/platform-bus.h"
#include "hw/qdev-properties.h"
#include "hw/arm/fdt.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/irq.h"
#include "kvm_arm.h"
#include "hw/firmware/smbios.h"
#include "qapi/visitor.h"
#include "qapi/qapi-visit-common.h"
#include "standard-headers/linux/input.h"
#include "hw/arm/smmuv3.h"
#include "hw/acpi/acpi.h"
#include "target/arm/internals.h"
#include "hw/mem/memory-device.h"
#include "hw/mem/pc-dimm.h"
#include "hw/mem/nvdimm.h"
#include "hw/acpi/generic_event_device.h"
#include "hw/virtio/virtio-mem-pci.h"
#include "hw/virtio/virtio-iommu.h"
#include "hw/char/pl011.h"
#include "qemu/guest-random.h"

struct FakeMachineState {
    MachineState parent;
    Notifier machine_done;
    DeviceState *platform_bus_dev;
    FWCfgState *fw_cfg;
    PFlashCFI01 *flash[2];
    bool secure;
    bool highmem;
    bool highmem_ecam;
    bool highmem_mmio;
    bool highmem_redists;
    bool its;
    bool tcg_its;
    bool virt;
    bool ras;
    bool mte;
    bool dtb_randomness;
    OnOffAuto acpi;
    VirtGICType gic_version;
    VirtIOMMUType iommu;
    bool default_bus_bypass_iommu;
    VirtMSIControllerType msi_controller;
    uint16_t virtio_iommu_bdf;
    struct arm_boot_info bootinfo;
    MemMapEntry *memmap;
    char *pciehb_nodename;
    const int *irqmap;
    int fdt_size;
    uint32_t clock_phandle;
    uint32_t gic_phandle;
    uint32_t msi_phandle;
    uint32_t iommu_phandle;
    int psci_conduit;
    hwaddr highest_gpa;
    DeviceState *gic;
    DeviceState *acpi_dev;
    Notifier powerdown_notifier;
    PCIBus *bus;
    char *oem_id;
    char *oem_table_id;
};
#define TYPE_FAKE_MACHINE MACHINE_TYPE_NAME("fake")
OBJECT_DECLARE_SIMPLE_TYPE(FakeMachineState, FAKE_MACHINE)

/* Number of external interrupt lines to configure the GIC with */
#define NUM_IRQS 256

#define PLATFORM_BUS_NUM_IRQS 64

/* Legacy RAM limit in GB (< version 4.0) */
#define LEGACY_RAMLIMIT_GB 255
#define LEGACY_RAMLIMIT_BYTES (LEGACY_RAMLIMIT_GB * GiB)

/* Addresses and sizes of our components.
 * 0..128MB is space for a flash device so we can run bootrom code such as UEFI.
 * 128MB..256MB is used for miscellaneous device I/O.
 * 256MB..1GB is reserved for possible future PCI support (ie where the
 * PCI memory window will go if we add a PCI host controller).
 * 1GB and up is RAM (which may happily spill over into the
 * high memory region beyond 4GB).
 * This represents a compromise between how much RAM can be given to
 * a 32 bit VM and leaving space for expansion and in particular for PCI.
 * Note that devices should generally be placed at multiples of 0x10000,
 * to accommodate guests using 64K pages.
 */
static const MemMapEntry base_memmap[] = {
    /* Space up to 0x8000000 is reserved for a boot ROM */
    [VIRT_FLASH] =              {          0, 0x08000000 },
    [VIRT_CPUPERIPHS] =         { 0x08000000, 0x00020000 },
    /* GIC distributor and CPU interfaces sit inside the CPU peripheral space */
    [VIRT_GIC_DIST] =           { 0x08000000, 0x00010000 },
    [VIRT_GIC_CPU] =            { 0x08010000, 0x00010000 },
    [VIRT_GIC_V2M] =            { 0x08020000, 0x00001000 },
    [VIRT_GIC_HYP] =            { 0x08030000, 0x00010000 },
    [VIRT_GIC_VCPU] =           { 0x08040000, 0x00010000 },
    /* The space in between here is reserved for GICv3 CPU/vCPU/HYP */
    [VIRT_GIC_ITS] =            { 0x08080000, 0x00020000 },
    /* This redistributor space allows up to 2*64kB*123 CPUs */
    [VIRT_GIC_REDIST] =         { 0x080A0000, 0x00F60000 },
    [VIRT_UART] =               { 0x09000000, 0x00001000 },
    [VIRT_RTC] =                { 0x09010000, 0x00001000 },
    [VIRT_FW_CFG] =             { 0x09020000, 0x00000018 },
    [VIRT_GPIO] =               { 0x09030000, 0x00001000 },
    [VIRT_SECURE_UART] =        { 0x09040000, 0x00001000 },
    [VIRT_SMMU] =               { 0x09050000, 0x00020000 },
    [VIRT_PCDIMM_ACPI] =        { 0x09070000, MEMORY_HOTPLUG_IO_LEN },
    [VIRT_ACPI_GED] =           { 0x09080000, ACPI_GED_EVT_SEL_LEN },
    [VIRT_NVDIMM_ACPI] =        { 0x09090000, NVDIMM_ACPI_IO_LEN},
    [VIRT_PVTIME] =             { 0x090a0000, 0x00010000 },
    [VIRT_SECURE_GPIO] =        { 0x090b0000, 0x00001000 },
    [VIRT_MMIO] =               { 0x0a000000, 0x00000200 },
    /* ...repeating for a total of NUM_VIRTIO_TRANSPORTS, each of that size */
    [VIRT_PLATFORM_BUS] =       { 0x0c000000, 0x02000000 },
    [VIRT_SECURE_MEM] =         { 0x0e000000, 0x01000000 },
    [VIRT_PCIE_MMIO] =          { 0x10000000, 0x2eff0000 },
    [VIRT_PCIE_PIO] =           { 0x3eff0000, 0x00010000 },
    [VIRT_PCIE_ECAM] =          { 0x3f000000, 0x01000000 },
    /* Actual RAM size depends on initial RAM and device memory settings */
    [VIRT_MEM] =                { GiB, LEGACY_RAMLIMIT_BYTES },
};

/*
 * Highmem IO Regions: This memory map is floating, located after the RAM.
 * Each MemMapEntry base (GPA) will be dynamically computed, depending on the
 * top of the RAM, so that its base get the same alignment as the size,
 * ie. a 512GiB entry will be aligned on a 512GiB boundary. If there is
 * less than 256GiB of RAM, the floating area starts at the 256GiB mark.
 * Note the extended_memmap is sized so that it eventually also includes the
 * base_memmap entries (VIRT_HIGH_GIC_REDIST2 index is greater than the last
 * index of base_memmap).
 */
static MemMapEntry extended_memmap[] = {
    /* Additional 64 MB redist region (can contain up to 512 redistributors) */
    [VIRT_HIGH_GIC_REDIST2] =   { 0x0, 64 * MiB },
    [VIRT_HIGH_PCIE_ECAM] =     { 0x0, 256 * MiB },
    /* Second PCIe window */
    [VIRT_HIGH_PCIE_MMIO] =     { 0x0, 512 * GiB },
};

static const char *valid_cpus[] = {
    ARM_CPU_TYPE_NAME("cortex-a7"),
    ARM_CPU_TYPE_NAME("cortex-a15"),
    ARM_CPU_TYPE_NAME("cortex-a53"),
    ARM_CPU_TYPE_NAME("cortex-a57"),
    ARM_CPU_TYPE_NAME("cortex-a72"),
    ARM_CPU_TYPE_NAME("cortex-a76"),
    ARM_CPU_TYPE_NAME("a64fx"),
    ARM_CPU_TYPE_NAME("neoverse-n1"),
    ARM_CPU_TYPE_NAME("host"),
    ARM_CPU_TYPE_NAME("max"),
};

static bool cpu_type_valid(const char *cpu)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(valid_cpus); i++) {
        if (strcmp(cpu, valid_cpus[i]) == 0) {
            return true;
        }
    }
    return false;
}

static inline DeviceState *create_acpi_ged(FakeMachineState *vms)
{
    DeviceState *dev;
    MachineState *ms = MACHINE(vms);
    int irq = vms->irqmap[VIRT_ACPI_GED];
    uint32_t event = ACPI_GED_PWR_DOWN_EVT;

    if (ms->ram_slots) {
        event |= ACPI_GED_MEM_HOTPLUG_EVT;
    }

    if (ms->nvdimms_state->is_enabled) {
        event |= ACPI_GED_NVDIMM_HOTPLUG_EVT;
    }

    dev = qdev_new(TYPE_ACPI_GED);
    qdev_prop_set_uint32(dev, "ged-event", event);

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VIRT_ACPI_GED].base);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, vms->memmap[VIRT_PCDIMM_ACPI].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(vms->gic, irq));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    return dev;
}

static void create_its(FakeMachineState *vms)
{
    const char *itsclass = its_class_name();
    DeviceState *dev;

    if (!strcmp(itsclass, "arm-gicv3-its")) {
        if (!vms->tcg_its) {
            itsclass = NULL;
        }
    }

    if (!itsclass) {
        /* Do nothing if not supported */
        return;
    }

    dev = qdev_new(itsclass);

    object_property_set_link(OBJECT(dev), "parent-gicv3", OBJECT(vms->gic),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VIRT_GIC_ITS].base);

    vms->msi_controller = VIRT_MSI_CTRL_ITS;
}

static void create_v2m(FakeMachineState *vms)
{
    int i;
    int irq = vms->irqmap[VIRT_GIC_V2M];
    DeviceState *dev;

    dev = qdev_new("arm-gicv2m");
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VIRT_GIC_V2M].base);
    qdev_prop_set_uint32(dev, "base-spi", irq);
    qdev_prop_set_uint32(dev, "num-spi", NUM_GICV2M_SPIS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    for (i = 0; i < NUM_GICV2M_SPIS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(vms->gic, irq + i));
    }

    vms->msi_controller = VIRT_MSI_CTRL_GICV2M;
}

/* Return number of redistributors that fit in the specified region */
static uint32_t fake_redist_capacity(FakeMachineState *vms, int region)
{
    uint32_t redist_size;

    if (vms->gic_version == VIRT_GIC_VERSION_3) {
        redist_size = GICV3_REDIST_SIZE;
    } else {
        redist_size = GICV4_REDIST_SIZE;
    }
    return vms->memmap[region].size / redist_size;
}

/* Return the number of used redistributor regions  */
static inline int fake_gicv3_redist_region_count(FakeMachineState *vms)
{
    uint32_t redist0_capacity = fake_redist_capacity(vms, VIRT_GIC_REDIST);

    assert(vms->gic_version != VIRT_GIC_VERSION_2);

    return (MACHINE(vms)->smp.cpus > redist0_capacity &&
            vms->highmem_redists) ? 2 : 1;
}

static void create_gic(FakeMachineState *vms, MemoryRegion *mem)
{
    MachineState *ms = MACHINE(vms);
    /* We create a standalone GIC */
    SysBusDevice *gicbusdev;
    const char *gictype;
    int i;
    unsigned int smp_cpus = ms->smp.cpus;
    uint32_t nb_redist_regions = 0;
    int revision;

    if (vms->gic_version == VIRT_GIC_VERSION_2) {
        gictype = gic_class_name();
    } else {
        gictype = gicv3_class_name();
    }

    switch (vms->gic_version) {
    case VIRT_GIC_VERSION_2:
        revision = 2;
        break;
    case VIRT_GIC_VERSION_3:
        revision = 3;
        break;
    case VIRT_GIC_VERSION_4:
        revision = 4;
        break;
    default:
        g_assert_not_reached();
    }
    vms->gic = qdev_new(gictype);
    qdev_prop_set_uint32(vms->gic, "revision", revision);
    qdev_prop_set_uint32(vms->gic, "num-cpu", smp_cpus);
    /* Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(vms->gic, "num-irq", NUM_IRQS + 32);
    if (!kvm_irqchip_in_kernel()) {
        qdev_prop_set_bit(vms->gic, "has-security-extensions", vms->secure);
    }

    if (vms->gic_version != VIRT_GIC_VERSION_2) {
        uint32_t redist0_capacity = fake_redist_capacity(vms, VIRT_GIC_REDIST);
        uint32_t redist0_count = MIN(smp_cpus, redist0_capacity);

        nb_redist_regions = fake_gicv3_redist_region_count(vms);

        qdev_prop_set_uint32(vms->gic, "len-redist-region-count",
                             nb_redist_regions);
        qdev_prop_set_uint32(vms->gic, "redist-region-count[0]", redist0_count);

        if (!kvm_irqchip_in_kernel()) {
            if (vms->tcg_its) {
                object_property_set_link(OBJECT(vms->gic), "sysmem",
                                         OBJECT(mem), &error_fatal);
                qdev_prop_set_bit(vms->gic, "has-lpi", true);
            }
        }

        if (nb_redist_regions == 2) {
            uint32_t redist1_capacity =
                fake_redist_capacity(vms, VIRT_HIGH_GIC_REDIST2);

            qdev_prop_set_uint32(vms->gic, "redist-region-count[1]",
                MIN(smp_cpus - redist0_count, redist1_capacity));
        }
    } else {
        if (!kvm_irqchip_in_kernel()) {
            qdev_prop_set_bit(vms->gic, "has-virtualization-extensions",
                              vms->virt);
        }
    }
    gicbusdev = SYS_BUS_DEVICE(vms->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, vms->memmap[VIRT_GIC_DIST].base);
    if (vms->gic_version != VIRT_GIC_VERSION_2) {
        sysbus_mmio_map(gicbusdev, 1, vms->memmap[VIRT_GIC_REDIST].base);
        if (nb_redist_regions == 2) {
            sysbus_mmio_map(gicbusdev, 2,
                            vms->memmap[VIRT_HIGH_GIC_REDIST2].base);
        }
    } else {
        sysbus_mmio_map(gicbusdev, 1, vms->memmap[VIRT_GIC_CPU].base);
        if (vms->virt) {
            sysbus_mmio_map(gicbusdev, 2, vms->memmap[VIRT_GIC_HYP].base);
            sysbus_mmio_map(gicbusdev, 3, vms->memmap[VIRT_GIC_VCPU].base);
        }
    }

    /* Wire the outputs from each CPU's generic timer and the GICv3
     * maintenance interrupt signal to the appropriate GIC PPI inputs,
     * and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;
        int irq;
        /* Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs we use for the virt board.
         */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(vms->gic,
                                                   ppibase + timer_irq[irq]));
        }

        if (vms->gic_version != VIRT_GIC_VERSION_2) {
            qemu_irq irq = qdev_get_gpio_in(vms->gic,
                                            ppibase + ARCH_GIC_MAINT_IRQ);
            qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt",
                                        0, irq);
        } else if (vms->virt) {
            qemu_irq irq = qdev_get_gpio_in(vms->gic,
                                            ppibase + ARCH_GIC_MAINT_IRQ);
            sysbus_connect_irq(gicbusdev, i + 4 * smp_cpus, irq);
        }

        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(vms->gic, ppibase
                                                     + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    if (vms->gic_version != VIRT_GIC_VERSION_2 && vms->its) {
        create_its(vms);
    } else if (vms->gic_version == VIRT_GIC_VERSION_2) {
        create_v2m(vms);
    }
}

static void create_uart(const FakeMachineState *vms, int uart,
                        MemoryRegion *mem, Chardev *chr)
{
    hwaddr base = vms->memmap[uart].base;
    int irq = vms->irqmap[uart];
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
}

static DeviceState *gpio_key_dev;
static void virt_powerdown_req(Notifier *n, void *opaque)
{
    FakeMachineState *s = container_of(n, FakeMachineState, powerdown_notifier);

    if (s->acpi_dev) {
        acpi_send_event(s->acpi_dev, ACPI_POWER_DOWN_STATUS);
    } else {
        /* use gpio Pin 3 for power button event */
        qemu_set_irq(qdev_get_gpio_in(gpio_key_dev, 0), 1);
    }
}

static void create_gpio_keys(DeviceState *pl061_dev)
{
    gpio_key_dev = sysbus_create_simple("gpio-key", -1,
                                        qdev_get_gpio_in(pl061_dev, 3));
}

#define SECURE_GPIO_POWEROFF 0
#define SECURE_GPIO_RESET    1

static void create_secure_gpio_pwr(DeviceState *pl061_dev)
{
    DeviceState *gpio_pwr_dev;

    /* gpio-pwr */
    gpio_pwr_dev = sysbus_create_simple("gpio-pwr", -1, NULL);

    /* connect secure pl061 to gpio-pwr */
    qdev_connect_gpio_out(pl061_dev, SECURE_GPIO_RESET,
                          qdev_get_gpio_in_named(gpio_pwr_dev, "reset", 0));
    qdev_connect_gpio_out(pl061_dev, SECURE_GPIO_POWEROFF,
                          qdev_get_gpio_in_named(gpio_pwr_dev, "shutdown", 0));
}

static void create_gpio_devices(const FakeMachineState *vms, int gpio,
                                MemoryRegion *mem)
{
    DeviceState *pl061_dev;
    hwaddr base = vms->memmap[gpio].base;
    int irq = vms->irqmap[gpio];
    SysBusDevice *s;

    pl061_dev = qdev_new("pl061");
    /* Pull lines down to 0 if not driven by the PL061 */
    qdev_prop_set_uint32(pl061_dev, "pullups", 0);
    qdev_prop_set_uint32(pl061_dev, "pulldowns", 0xff);
    s = SYS_BUS_DEVICE(pl061_dev);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));

    /* Child gpio devices */
    if (gpio == VIRT_GPIO) {
        create_gpio_keys(pl061_dev);
    } else {
        create_secure_gpio_pwr(pl061_dev);
    }
}

static void create_virtio_devices(const FakeMachineState *vms)
{
    int i;
    hwaddr size = vms->memmap[VIRT_MMIO].size;
    MachineState *ms = MACHINE(vms);

    /* We create the transports in forwards order. Since qbus_realize()
     * prepends (not appends) new child buses, the incrementing loop below will
     * create a list of virtio-mmio buses with decreasing base addresses.
     *
     * When a -device option is processed from the command line,
     * qbus_find_recursive() picks the next free virtio-mmio bus in forwards
     * order. The upshot is that -device options in increasing command line
     * order are mapped to virtio-mmio buses with decreasing base addresses.
     *
     * When this code was originally written, that arrangement ensured that the
     * guest Linux kernel would give the lowest "name" (/dev/vda, eth0, etc) to
     * the first -device on the command line. (The end-to-end order is a
     * function of this loop, qbus_realize(), qbus_find_recursive(), and the
     * guest kernel's name-to-address assignment strategy.)
     *
     * Meanwhile, the kernel's traversal seems to have been reversed; see eg.
     * the message, if not necessarily the code, of commit 70161ff336.
     * Therefore the loop now establishes the inverse of the original intent.
     *
     * Unfortunately, we can't counteract the kernel change by reversing the
     * loop; it would break existing command lines.
     *
     * In any case, the kernel makes no guarantee about the stability of
     * enumeration order of virtio devices (as demonstrated by it changing
     * between kernel versions). For reliable and stable identification
     * of disks users must use UUIDs or similar mechanisms.
     */
    for (i = 0; i < NUM_VIRTIO_TRANSPORTS; i++) {
        int irq = vms->irqmap[VIRT_MMIO] + i;
        hwaddr base = vms->memmap[VIRT_MMIO].base + i * size;

        sysbus_create_simple("virtio-mmio", base,
                             qdev_get_gpio_in(vms->gic, irq));
    }

    /* We add dtb nodes in reverse order so that they appear in the finished
     * device tree lowest address first.
     *
     * Note that this mapping is independent of the loop above. The previous
     * loop influences virtio device to virtio transport assignment, whereas
     * this loop controls how virtio transports are laid out in the dtb.
     */
    for (i = NUM_VIRTIO_TRANSPORTS - 1; i >= 0; i--) {
        char *nodename;
        int irq = vms->irqmap[VIRT_MMIO] + i;
        hwaddr base = vms->memmap[VIRT_MMIO].base + i * size;

        nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, base);
        qemu_fdt_add_subnode(ms->fdt, nodename);
        qemu_fdt_setprop_string(ms->fdt, nodename,
                                "compatible", "virtio,mmio");
        qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg",
                                     2, base, 2, size);
        qemu_fdt_setprop_cells(ms->fdt, nodename, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, irq,
                               GIC_FDT_IRQ_FLAGS_EDGE_LO_HI);
        qemu_fdt_setprop(ms->fdt, nodename, "dma-coherent", NULL, 0);
        g_free(nodename);
    }
}

static bool virt_firmware_init(FakeMachineState *vms,
                               MemoryRegion *sysmem,
                               MemoryRegion *secure_sysmem)
{
    int i;
    const char *bios_name;

    /* Map legacy -drive if=pflash to machine properties */
    for (i = 0; i < ARRAY_SIZE(vms->flash); i++) {
        pflash_cfi01_legacy_drive(vms->flash[i],
                                  drive_get(IF_PFLASH, 0, i));
    }

    bios_name = MACHINE(vms)->firmware;
    if (bios_name) {
        char *fname;
        MemoryRegion *mr;
        int image_size;

        /* Fall back to -bios */

        fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (!fname) {
            error_report("Could not find ROM image '%s'", bios_name);
            exit(1);
        }
        mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(vms->flash[0]), 0);
        image_size = load_image_mr(fname, mr);
        g_free(fname);
        if (image_size < 0) {
            error_report("Could not load ROM image '%s'", bios_name);
            exit(1);
        }
    }

    return bios_name;
}

static void create_platform_bus(FakeMachineState *vms)
{
    DeviceState *dev;
    SysBusDevice *s;
    int i;
    MemoryRegion *sysmem = get_system_memory();

    dev = qdev_new(TYPE_PLATFORM_BUS_DEVICE);
    dev->id = g_strdup(TYPE_PLATFORM_BUS_DEVICE);
    qdev_prop_set_uint32(dev, "num_irqs", PLATFORM_BUS_NUM_IRQS);
    qdev_prop_set_uint32(dev, "mmio_size", vms->memmap[VIRT_PLATFORM_BUS].size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    vms->platform_bus_dev = dev;

    s = SYS_BUS_DEVICE(dev);
    for (i = 0; i < PLATFORM_BUS_NUM_IRQS; i++) {
        int irq = vms->irqmap[VIRT_PLATFORM_BUS] + i;
        sysbus_connect_irq(s, i, qdev_get_gpio_in(vms->gic, irq));
    }

    memory_region_add_subregion(sysmem,
                                vms->memmap[VIRT_PLATFORM_BUS].base,
                                sysbus_mmio_get_region(s, 0));
}

static void create_tag_ram(MemoryRegion *tag_sysmem,
                           hwaddr base, hwaddr size,
                           const char *name)
{
    MemoryRegion *tagram = g_new(MemoryRegion, 1);

    memory_region_init_ram(tagram, NULL, name, size / 32, &error_fatal);
    memory_region_add_subregion(tag_sysmem, base / 32, tagram);
}

static void create_secure_ram(FakeMachineState *vms,
                              MemoryRegion *secure_sysmem,
                              MemoryRegion *secure_tag_sysmem)
{
    MemoryRegion *secram = g_new(MemoryRegion, 1);
    char *nodename;
    hwaddr base = vms->memmap[VIRT_SECURE_MEM].base;
    hwaddr size = vms->memmap[VIRT_SECURE_MEM].size;
    MachineState *ms = MACHINE(vms);

    memory_region_init_ram(secram, NULL, "virt.secure-ram", size,
                           &error_fatal);
    memory_region_add_subregion(secure_sysmem, base, secram);

    nodename = g_strdup_printf("/secram@%" PRIx64, base);
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename, "device_type", "memory");
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg", 2, base, 2, size);
    qemu_fdt_setprop_string(ms->fdt, nodename, "status", "disabled");
    qemu_fdt_setprop_string(ms->fdt, nodename, "secure-status", "okay");

    if (secure_tag_sysmem) {
        create_tag_ram(secure_tag_sysmem, base, size, "mach-virt.secure-tag");
    }

    g_free(nodename);
}

static void *machvirt_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    const FakeMachineState *board = container_of(binfo, FakeMachineState,
                                                 bootinfo);
    MachineState *ms = MACHINE(board);


    *fdt_size = board->fdt_size;
    return ms->fdt;
}

static
void FAKE_MACHINE_done(Notifier *notifier, void *data)
{
    FakeMachineState *vms = container_of(notifier, FakeMachineState,
                                         machine_done);
    MachineState *ms = MACHINE(vms);
    ARMCPU *cpu = ARM_CPU(first_cpu);
    struct arm_boot_info *info = &vms->bootinfo;
    AddressSpace *as = arm_boot_address_space(cpu, info);

    /*
     * If the user provided a dtb, we assume the dynamic sysbus nodes
     * already are integrated there. This corresponds to a use case where
     * the dynamic sysbus nodes are complex and their generation is not yet
     * supported. In that case the user can take charge of the guest dt
     * while qemu takes charge of the qom stuff.
     */
    if (info->dtb_filename == NULL) {
        platform_bus_add_all_fdt_nodes(ms->fdt, "/intc",
                                       vms->memmap[VIRT_PLATFORM_BUS].base,
                                       vms->memmap[VIRT_PLATFORM_BUS].size,
                                       vms->irqmap[VIRT_PLATFORM_BUS]);
    }
    if (arm_load_dtb(info->dtb_start, info, info->dtb_limit, as, ms) < 0) {
        exit(1);
    }

        fw_cfg_add_extra_pci_roots(vms->bus, vms->fw_cfg);
}

static uint64_t virt_cpu_mp_affinity(FakeMachineState *vms, int idx)
{
    uint8_t clustersz = ARM_DEFAULT_CPUS_PER_CLUSTER;

    return arm_cpu_mp_affinity(idx, clustersz);
}

static void virt_set_memmap(FakeMachineState *vms, int pa_bits)
{
    MachineState *ms = MACHINE(vms);
    hwaddr base, device_memory_base, device_memory_size, memtop;
    int i;

    vms->memmap = extended_memmap;

    for (i = 0; i < ARRAY_SIZE(base_memmap); i++) {
        vms->memmap[i] = base_memmap[i];
    }

    if (ms->ram_slots > ACPI_MAX_RAM_SLOTS) {
        error_report("unsupported number of memory slots: %"PRIu64,
                     ms->ram_slots);
        exit(EXIT_FAILURE);
    }

    /*
     * !highmem is exactly the same as limiting the PA space to 32bit,
     * irrespective of the underlying capabilities of the HW.
     */
    if (!vms->highmem) {
        pa_bits = 32;
    }

    /*
     * We compute the base of the high IO region depending on the
     * amount of initial and device memory. The device memory start/size
     * is aligned on 1GiB. We never put the high IO region below 256GiB
     * so that if maxram_size is < 255GiB we keep the legacy memory map.
     * The device region size assumes 1GiB page max alignment per slot.
     */
    device_memory_base =
        ROUND_UP(vms->memmap[VIRT_MEM].base + ms->ram_size, GiB);
    device_memory_size = ms->maxram_size - ms->ram_size + ms->ram_slots * GiB;

    /* Base address of the high IO region */
    memtop = base = device_memory_base + ROUND_UP(device_memory_size, GiB);
    if (memtop > BIT_ULL(pa_bits)) {
	    error_report("Addressing limited to %d bits, but memory exceeds it by %llu bytes\n",
			 pa_bits, memtop - BIT_ULL(pa_bits));
        exit(EXIT_FAILURE);
    }
    if (base < device_memory_base) {
        error_report("maxmem/slots too huge");
        exit(EXIT_FAILURE);
    }
    if (base < vms->memmap[VIRT_MEM].base + LEGACY_RAMLIMIT_BYTES) {
        base = vms->memmap[VIRT_MEM].base + LEGACY_RAMLIMIT_BYTES;
    }

    /* We know for sure that at least the memory fits in the PA space */
    vms->highest_gpa = memtop - 1;

    for (i = VIRT_LOWMEMMAP_LAST; i < ARRAY_SIZE(extended_memmap); i++) {
        hwaddr size = extended_memmap[i].size;
        bool fits;

        base = ROUND_UP(base, size);
        vms->memmap[i].base = base;
        vms->memmap[i].size = size;

        /*
         * Check each device to see if they fit in the PA space,
         * moving highest_gpa as we go.
         *
         * For each device that doesn't fit, disable it.
         */
        fits = (base + size) <= BIT_ULL(pa_bits);
        if (fits) {
            vms->highest_gpa = base + size - 1;
        }

        switch (i) {
        case VIRT_HIGH_GIC_REDIST2:
            vms->highmem_redists &= fits;
            break;
        case VIRT_HIGH_PCIE_ECAM:
            vms->highmem_ecam &= fits;
            break;
        case VIRT_HIGH_PCIE_MMIO:
            vms->highmem_mmio &= fits;
            break;
        }

        base += size;
    }

    if (device_memory_size > 0) {
        ms->device_memory = g_malloc0(sizeof(*ms->device_memory));
        ms->device_memory->base = device_memory_base;
        memory_region_init(&ms->device_memory->mr, OBJECT(vms),
                           "device-memory", device_memory_size);
    }
}

/*
 * finalize_gic_version - Determines the final gic_version
 * according to the gic-version property
 *
 * Default GIC type is v2
 */
static void finalize_gic_version(FakeMachineState *vms)
{
    unsigned int max_cpus = MACHINE(vms)->smp.max_cpus;

    if (kvm_enabled()) {
        int probe_bitmap;

        if (!kvm_irqchip_in_kernel()) {
            switch (vms->gic_version) {
            case VIRT_GIC_VERSION_HOST:
                warn_report(
                    "gic-version=host not relevant with kernel-irqchip=off "
                     "as only userspace GICv2 is supported. Using v2 ...");
                return;
            case VIRT_GIC_VERSION_MAX:
            case VIRT_GIC_VERSION_NOSEL:
                vms->gic_version = VIRT_GIC_VERSION_2;
                return;
            case VIRT_GIC_VERSION_2:
                return;
            case VIRT_GIC_VERSION_3:
                error_report(
                    "gic-version=3 is not supported with kernel-irqchip=off");
                exit(1);
            case VIRT_GIC_VERSION_4:
                error_report(
                    "gic-version=4 is not supported with kernel-irqchip=off");
                exit(1);
            }
        }

        probe_bitmap = kvm_arm_vgic_probe();
        if (!probe_bitmap) {
            error_report("Unable to determine GIC version supported by host");
            exit(1);
        }

        switch (vms->gic_version) {
        case VIRT_GIC_VERSION_HOST:
        case VIRT_GIC_VERSION_MAX:
            if (probe_bitmap & KVM_ARM_VGIC_V3) {
                vms->gic_version = VIRT_GIC_VERSION_3;
            } else {
                vms->gic_version = VIRT_GIC_VERSION_2;
            }
            return;
        case VIRT_GIC_VERSION_NOSEL:
            if ((probe_bitmap & KVM_ARM_VGIC_V2) && max_cpus <= GIC_NCPU) {
                vms->gic_version = VIRT_GIC_VERSION_2;
            } else if (probe_bitmap & KVM_ARM_VGIC_V3) {
                /*
                 * in case the host does not support v2 in-kernel emulation or
                 * the end-user requested more than 8 VCPUs we now default
                 * to v3. In any case defaulting to v2 would be broken.
                 */
                vms->gic_version = VIRT_GIC_VERSION_3;
            } else if (max_cpus > GIC_NCPU) {
                error_report("host only supports in-kernel GICv2 emulation "
                             "but more than 8 vcpus are requested");
                exit(1);
            }
            break;
        case VIRT_GIC_VERSION_2:
        case VIRT_GIC_VERSION_3:
            break;
        case VIRT_GIC_VERSION_4:
            error_report("gic-version=4 is not supported with KVM");
            exit(1);
        }

        /* Check chosen version is effectively supported by the host */
        if (vms->gic_version == VIRT_GIC_VERSION_2 &&
            !(probe_bitmap & KVM_ARM_VGIC_V2)) {
            error_report("host does not support in-kernel GICv2 emulation");
            exit(1);
        } else if (vms->gic_version == VIRT_GIC_VERSION_3 &&
                   !(probe_bitmap & KVM_ARM_VGIC_V3)) {
            error_report("host does not support in-kernel GICv3 emulation");
            exit(1);
        }
        return;
    }

    /* TCG mode */
    switch (vms->gic_version) {
    case VIRT_GIC_VERSION_NOSEL:
        vms->gic_version = VIRT_GIC_VERSION_2;
        break;
    case VIRT_GIC_VERSION_MAX:
        if (module_object_class_by_name("arm-gicv3")) {
            /* CONFIG_ARM_GICV3_TCG was set */
            if (vms->virt) {
                /* GICv4 only makes sense if CPU has EL2 */
                vms->gic_version = VIRT_GIC_VERSION_4;
            } else {
                vms->gic_version = VIRT_GIC_VERSION_3;
            }
        } else {
            vms->gic_version = VIRT_GIC_VERSION_2;
        }
        break;
    case VIRT_GIC_VERSION_HOST:
        error_report("gic-version=host requires KVM");
        exit(1);
    case VIRT_GIC_VERSION_4:
        if (!vms->virt) {
            error_report("gic-version=4 requires virtualization enabled");
            exit(1);
        }
        break;
    case VIRT_GIC_VERSION_2:
    case VIRT_GIC_VERSION_3:
        break;
    }
}

/*
 * virt_cpu_post_init() must be called after the CPUs have
 * been realized and the GIC has been created.
 */
static void virt_cpu_post_init(FakeMachineState *vms, MemoryRegion *sysmem)
{
    int max_cpus = MACHINE(vms)->smp.max_cpus;
    bool aarch64, pmu, steal_time;
    CPUState *cpu;

    aarch64 = object_property_get_bool(OBJECT(first_cpu), "aarch64", NULL);
    pmu = object_property_get_bool(OBJECT(first_cpu), "pmu", NULL);
    steal_time = object_property_get_bool(OBJECT(first_cpu),
                                          "kvm-steal-time", NULL);

    if (kvm_enabled()) {
        hwaddr pvtime_reg_base = vms->memmap[VIRT_PVTIME].base;
        hwaddr pvtime_reg_size = vms->memmap[VIRT_PVTIME].size;

        if (steal_time) {
            MemoryRegion *pvtime = g_new(MemoryRegion, 1);
            hwaddr pvtime_size = max_cpus * PVTIME_SIZE_PER_CPU;

            /* The memory region size must be a multiple of host page size. */
            pvtime_size = REAL_HOST_PAGE_ALIGN(pvtime_size);

            if (pvtime_size > pvtime_reg_size) {
                error_report("pvtime requires a %" HWADDR_PRId
                             " byte memory region for %d CPUs,"
                             " but only %" HWADDR_PRId " has been reserved",
                             pvtime_size, max_cpus, pvtime_reg_size);
                exit(1);
            }

            memory_region_init_ram(pvtime, NULL, "pvtime", pvtime_size, NULL);
            memory_region_add_subregion(sysmem, pvtime_reg_base, pvtime);
        }

        CPU_FOREACH(cpu) {
            if (pmu) {
                assert(arm_feature(&ARM_CPU(cpu)->env, ARM_FEATURE_PMU));
                if (kvm_irqchip_in_kernel()) {
                    kvm_arm_pmu_set_irq(cpu, PPI(VIRTUAL_PMU_IRQ));
                }
                kvm_arm_pmu_init(cpu);
            }
            if (steal_time) {
                kvm_arm_pvtime_init(cpu, pvtime_reg_base +
                                         cpu->cpu_index * PVTIME_SIZE_PER_CPU);
            }
        }
    } else {
        if (aarch64 && vms->highmem) {
            int requested_pa_size = 64 - clz64(vms->highest_gpa);
            int pamax = arm_pamax(ARM_CPU(first_cpu));

            if (pamax < requested_pa_size) {
                error_report("VCPU supports less PA bits (%d) than "
                             "requested by the memory map (%d)",
                             pamax, requested_pa_size);
                exit(1);
            }
        }
    }
}

static void machvirt_init(MachineState *machine)
{
    FakeMachineState *vms = FAKE_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *secure_sysmem = NULL;
    MemoryRegion *tag_sysmem = NULL;
    MemoryRegion *secure_tag_sysmem = NULL;
    int n, virt_max_cpus;
    bool firmware_loaded;
    bool aarch64 = true;
    unsigned int smp_cpus = machine->smp.cpus;
    unsigned int max_cpus = machine->smp.max_cpus;

    if (!cpu_type_valid(machine->cpu_type)) {
        error_report("mach-virt: CPU type %s not supported", machine->cpu_type);
        exit(1);
    }

    possible_cpus = mc->possible_cpu_arch_ids(machine);

    /*
     * In accelerated mode, the memory map is computed earlier in kvm_type()
     * to create a VM with the right number of IPA bits.
     */
    if (!vms->memmap) {
        Object *cpuobj;
        ARMCPU *armcpu;
        int pa_bits;

        /*
         * Instanciate a temporary CPU object to find out about what
         * we are about to deal with. Once this is done, get rid of
         * the object.
         */
        cpuobj = object_new(possible_cpus->cpus[0].type);
        armcpu = ARM_CPU(cpuobj);

        pa_bits = arm_pamax(armcpu);

        object_unref(cpuobj);

        virt_set_memmap(vms, pa_bits);
    }

    /* We can probe only here because during property set
     * KVM is not available yet
     */
    finalize_gic_version(vms);

    if (vms->secure) {
        /*
         * The Secure view of the world is the same as the NonSecure,
         * but with a few extra devices. Create it as a container region
         * containing the system memory at low priority; any secure-only
         * devices go in at higher priority and take precedence.
         */
        secure_sysmem = g_new(MemoryRegion, 1);
        memory_region_init(secure_sysmem, OBJECT(machine), "secure-memory",
                           UINT64_MAX);
        memory_region_add_subregion_overlap(secure_sysmem, 0, sysmem, -1);
    }

    firmware_loaded = virt_firmware_init(vms, sysmem,
                                         secure_sysmem ?: sysmem);

    /* If we have an EL3 boot ROM then the assumption is that it will
     * implement PSCI itself, so disable QEMU's internal implementation
     * so it doesn't get in the way. Instead of starting secondary
     * CPUs in PSCI powerdown state we will start them all running and
     * let the boot ROM sort them out.
     * The usual case is that we do use QEMU's PSCI implementation;
     * if the guest has EL2 then we will use SMC as the conduit,
     * and otherwise we will use HVC (for backwards compatibility and
     * because if we're using KVM then we must use HVC).
     */
    if (vms->secure && firmware_loaded) {
        vms->psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;
    } else if (vms->virt) {
        vms->psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    } else {
        vms->psci_conduit = QEMU_PSCI_CONDUIT_HVC;
    }

    /*
     * The maximum number of CPUs depends on the GIC version, or on how
     * many redistributors we can fit into the memory map (which in turn
     * depends on whether this is a GICv3 or v4).
     */
    if (vms->gic_version == VIRT_GIC_VERSION_2) {
        virt_max_cpus = GIC_NCPU;
    } else {
        virt_max_cpus = fake_redist_capacity(vms, VIRT_GIC_REDIST) +
            fake_redist_capacity(vms, VIRT_HIGH_GIC_REDIST2);
    }

    if (max_cpus > virt_max_cpus) {
        error_report("Number of SMP CPUs requested (%d) exceeds max CPUs "
                     "supported by machine 'mach-virt' (%d)",
                     max_cpus, virt_max_cpus);
        exit(1);
    }

    if (vms->secure && (kvm_enabled() || hvf_enabled())) {
        error_report("mach-virt: %s does not support providing "
                     "Security extensions (TrustZone) to the guest CPU",
                     kvm_enabled() ? "KVM" : "HVF");
        exit(1);
    }

    if (vms->virt && (kvm_enabled() || hvf_enabled())) {
        error_report("mach-virt: %s does not support providing "
                     "Virtualization extensions to the guest CPU",
                     kvm_enabled() ? "KVM" : "HVF");
        exit(1);
    }

    if (vms->mte && (kvm_enabled() || hvf_enabled())) {
        error_report("mach-virt: %s does not support providing "
                     "MTE to the guest CPU",
                     kvm_enabled() ? "KVM" : "HVF");
        exit(1);
    }

    assert(possible_cpus->len == max_cpus);
    for (n = 0; n < possible_cpus->len; n++) {
        Object *cpuobj;
        CPUState *cs;

        if (n >= smp_cpus) {
            break;
        }

        cpuobj = object_new(possible_cpus->cpus[n].type);
        object_property_set_int(cpuobj, "mp-affinity",
                                possible_cpus->cpus[n].arch_id, NULL);

        cs = CPU(cpuobj);
        cs->cpu_index = n;

        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpuobj),
                          &error_fatal);

        aarch64 &= object_property_get_bool(cpuobj, "aarch64", NULL);

        if (!vms->secure) {
            object_property_set_bool(cpuobj, "has_el3", false, NULL);
        }

        if (!vms->virt && object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", false, NULL);
        }

        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar",
                                    vms->memmap[VIRT_CPUPERIPHS].base,
                                    &error_abort);
        }

        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        if (vms->secure) {
            object_property_set_link(cpuobj, "secure-memory",
                                     OBJECT(secure_sysmem), &error_abort);
        }

        if (vms->mte) {
            /* Create the memory region only once, but link to all cpus. */
            if (!tag_sysmem) {
                /*
                 * The property exists only if MemTag is supported.
                 * If it is, we must allocate the ram to back that up.
                 */
                if (!object_property_find(cpuobj, "tag-memory")) {
                    error_report("MTE requested, but not supported "
                                 "by the guest CPU");
                    exit(1);
                }

                tag_sysmem = g_new(MemoryRegion, 1);
                memory_region_init(tag_sysmem, OBJECT(machine),
                                   "tag-memory", UINT64_MAX / 32);

                if (vms->secure) {
                    secure_tag_sysmem = g_new(MemoryRegion, 1);
                    memory_region_init(secure_tag_sysmem, OBJECT(machine),
                                       "secure-tag-memory", UINT64_MAX / 32);

                    /* As with ram, secure-tag takes precedence over tag.  */
                    memory_region_add_subregion_overlap(secure_tag_sysmem, 0,
                                                        tag_sysmem, -1);
                }
            }

            object_property_set_link(cpuobj, "tag-memory", OBJECT(tag_sysmem),
                                     &error_abort);
            if (vms->secure) {
                object_property_set_link(cpuobj, "secure-tag-memory",
                                         OBJECT(secure_tag_sysmem),
                                         &error_abort);
            }
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }

    memory_region_add_subregion(sysmem, vms->memmap[VIRT_MEM].base,
                                machine->ram);
    if (machine->device_memory) {
        memory_region_add_subregion(sysmem, machine->device_memory->base,
                                    &machine->device_memory->mr);
    }

    create_gic(vms, sysmem);

    virt_cpu_post_init(vms, sysmem);

    create_uart(vms, VIRT_UART, sysmem, serial_hd(0));

    if (vms->secure) {
        create_secure_ram(vms, secure_sysmem, secure_tag_sysmem);
        create_uart(vms, VIRT_SECURE_UART, secure_sysmem, serial_hd(1));
    }

    if (tag_sysmem) {
        create_tag_ram(tag_sysmem, vms->memmap[VIRT_MEM].base,
                       machine->ram_size, "mach-virt.tag");
    }

    vms->highmem_ecam &= (!firmware_loaded || aarch64);

    if (aarch64 && firmware_loaded) {
        vms->acpi_dev = create_acpi_ged(vms);
    } else {
        create_gpio_devices(vms, VIRT_GPIO, sysmem);
    }

    if (vms->secure) {
        create_gpio_devices(vms, VIRT_SECURE_GPIO, secure_sysmem);
    }

     /* connect powerdown request */
     vms->powerdown_notifier.notify = virt_powerdown_req;
     qemu_register_powerdown_notifier(&vms->powerdown_notifier);

    /* Create mmio transports, so the user can create virtio backends
     * (which will be automatically plugged in to the transports). If
     * no backend is created the transport will just sit harmlessly idle.
     */
    create_virtio_devices(vms);

    create_platform_bus(vms);

    if (machine->nvdimms_state->is_enabled) {
        const struct AcpiGenericAddress arm_virt_nvdimm_acpi_dsmio = {
            .space_id = AML_AS_SYSTEM_MEMORY,
            .address = vms->memmap[VIRT_NVDIMM_ACPI].base,
            .bit_width = NVDIMM_ACPI_IO_LEN << 3
        };

        nvdimm_init_acpi_state(machine->nvdimms_state, sysmem,
                               arm_virt_nvdimm_acpi_dsmio,
                               vms->fw_cfg, OBJECT(vms));
    }

    vms->bootinfo.ram_size = machine->ram_size;
    vms->bootinfo.board_id = -1;
    vms->bootinfo.loader_start = vms->memmap[VIRT_MEM].base;
    vms->bootinfo.get_dtb = machvirt_dtb;
    vms->bootinfo.skip_dtb_autoload = true;
    vms->bootinfo.firmware_loaded = firmware_loaded;
    vms->bootinfo.psci_conduit = vms->psci_conduit;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &vms->bootinfo);

    vms->machine_done.notify = FAKE_MACHINE_done;
    qemu_add_machine_init_done_notifier(&vms->machine_done);
}

static bool virt_get_secure(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->secure;
}

static void virt_set_secure(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->secure = value;
}

static bool virt_get_virt(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->virt;
}

static void virt_set_virt(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->virt = value;
}

static bool virt_get_highmem(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->highmem;
}

static void virt_set_highmem(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->highmem = value;
}

static bool virt_get_its(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->its;
}

static void virt_set_its(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->its = value;
}

static void virt_get_acpi(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);
    OnOffAuto acpi = vms->acpi;

    visit_type_OnOffAuto(v, name, &acpi, errp);
}

static void virt_set_acpi(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    visit_type_OnOffAuto(v, name, &vms->acpi, errp);
}

static bool virt_get_ras(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->ras;
}

static void virt_set_ras(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->ras = value;
}

static bool virt_get_mte(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->mte;
}

static void virt_set_mte(Object *obj, bool value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->mte = value;
}

static char *virt_get_gic_version(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);
    const char *val;

    switch (vms->gic_version) {
    case VIRT_GIC_VERSION_4:
        val = "4";
        break;
    case VIRT_GIC_VERSION_3:
        val = "3";
        break;
    default:
        val = "2";
        break;
    }
    return g_strdup(val);
}

static void virt_set_gic_version(Object *obj, const char *value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    if (!strcmp(value, "4")) {
        vms->gic_version = VIRT_GIC_VERSION_4;
    } else if (!strcmp(value, "3")) {
        vms->gic_version = VIRT_GIC_VERSION_3;
    } else if (!strcmp(value, "2")) {
        vms->gic_version = VIRT_GIC_VERSION_2;
    } else if (!strcmp(value, "host")) {
        vms->gic_version = VIRT_GIC_VERSION_HOST; /* Will probe later */
    } else if (!strcmp(value, "max")) {
        vms->gic_version = VIRT_GIC_VERSION_MAX; /* Will probe later */
    } else {
        error_setg(errp, "Invalid gic-version value");
        error_append_hint(errp, "Valid values are 3, 2, host, max.\n");
    }
}

static char *virt_get_iommu(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    switch (vms->iommu) {
    case VIRT_IOMMU_NONE:
        return g_strdup("none");
    case VIRT_IOMMU_SMMUV3:
        return g_strdup("smmuv3");
    default:
        g_assert_not_reached();
    }
}

static void virt_set_iommu(Object *obj, const char *value, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    if (!strcmp(value, "smmuv3")) {
        vms->iommu = VIRT_IOMMU_SMMUV3;
    } else if (!strcmp(value, "none")) {
        vms->iommu = VIRT_IOMMU_NONE;
    } else {
        error_setg(errp, "Invalid iommu value");
        error_append_hint(errp, "Valid values are none, smmuv3.\n");
    }
}

static bool virt_get_default_bus_bypass_iommu(Object *obj, Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    return vms->default_bus_bypass_iommu;
}

static void virt_set_default_bus_bypass_iommu(Object *obj, bool value,
                                              Error **errp)
{
    FakeMachineState *vms = FAKE_MACHINE(obj);

    vms->default_bus_bypass_iommu = value;
}

static CpuInstanceProperties
virt_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t virt_get_default_cpu_node_id(const MachineState *ms, int idx)
{
    int64_t socket_id = ms->possible_cpus->cpus[idx].props.socket_id;

    return socket_id % ms->numa_state->num_nodes;
}

static const CPUArchIdList *virt_possible_cpu_arch_ids(MachineState *ms)
{
    int n;
    unsigned int max_cpus = ms->smp.max_cpus;
    FakeMachineState *vms = FAKE_MACHINE(ms);
    MachineClass *mc = MACHINE_GET_CLASS(vms);

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id =
            virt_cpu_mp_affinity(vms, n);

        assert(!mc->smp_props.dies_supported);
        ms->possible_cpus->cpus[n].props.has_socket_id = true;
        ms->possible_cpus->cpus[n].props.socket_id =
            n / (ms->smp.clusters * ms->smp.cores * ms->smp.threads);
        ms->possible_cpus->cpus[n].props.has_cluster_id = true;
        ms->possible_cpus->cpus[n].props.cluster_id =
            (n / (ms->smp.cores * ms->smp.threads)) % ms->smp.clusters;
        ms->possible_cpus->cpus[n].props.has_core_id = true;
        ms->possible_cpus->cpus[n].props.core_id =
            (n / ms->smp.threads) % ms->smp.cores;
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id =
            n % ms->smp.threads;
    }
    return ms->possible_cpus;
}

static void baize_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
printf("s1\n");
    mc->init = machvirt_init;
    mc->desc = "bai-ze board";
    /* Start with max_cpus set to 512, which is the maximum supported by KVM.
     * The value may be reduced later when we have more information about the
     * configuration of the particular instance.
     */
    mc->max_cpus = 512;
    mc->pci_allow_0_address = true;
    /* We know we will never create a pre-ARMv7 CPU which needs 1K pages */
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = virt_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = virt_cpu_index_to_props;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->get_default_cpu_node_id = virt_get_default_cpu_node_id;
    mc->nvdimm_supported = true;
    mc->smp_props.clusters_supported = true;
    mc->auto_enable_numa_with_memhp = true;
    mc->auto_enable_numa_with_memdev = true;
    mc->default_ram_id = "mach-virt.ram";

    object_class_property_add(oc, "acpi", "OnOffAuto",
        virt_get_acpi, virt_set_acpi,
        NULL, NULL);
    object_class_property_set_description(oc, "acpi",
        "Enable ACPI");
    object_class_property_add_bool(oc, "secure", virt_get_secure,
                                   virt_set_secure);
    object_class_property_set_description(oc, "secure",
                                                "Set on/off to enable/disable the ARM "
                                                "Security Extensions (TrustZone)");

    object_class_property_add_bool(oc, "virtualization", virt_get_virt,
                                   virt_set_virt);
    object_class_property_set_description(oc, "virtualization",
                                          "Set on/off to enable/disable emulating a "
                                          "guest CPU which implements the ARM "
                                          "Virtualization Extensions");

    object_class_property_add_bool(oc, "highmem", virt_get_highmem,
                                   virt_set_highmem);
    object_class_property_set_description(oc, "highmem",
                                          "Set on/off to enable/disable using "
                                          "physical address space above 32 bits");

    object_class_property_add_str(oc, "gic-version", virt_get_gic_version,
                                  virt_set_gic_version);
    object_class_property_set_description(oc, "gic-version",
                                          "Set GIC version. "
                                          "Valid values are 2, 3, 4, host and max");

    object_class_property_add_str(oc, "iommu", virt_get_iommu, virt_set_iommu);
    object_class_property_set_description(oc, "iommu",
                                          "Set the IOMMU type. "
                                          "Valid values are none and smmuv3");

    object_class_property_add_bool(oc, "default-bus-bypass-iommu",
                                   virt_get_default_bus_bypass_iommu,
                                   virt_set_default_bus_bypass_iommu);
    object_class_property_set_description(oc, "default-bus-bypass-iommu",
                                          "Set on/off to enable/disable "
                                          "bypass_iommu for default root bus");

    object_class_property_add_bool(oc, "ras", virt_get_ras,
                                   virt_set_ras);
    object_class_property_set_description(oc, "ras",
                                          "Set on/off to enable/disable reporting host memory errors "
                                          "to a KVM guest using ACPI and guest external abort exceptions");

    object_class_property_add_bool(oc, "mte", virt_get_mte, virt_set_mte);
    object_class_property_set_description(oc, "mte",
                                          "Set on/off to enable/disable emulating a "
                                          "guest CPU which implements the ARM "
                                          "Memory Tagging Extension");
printf("s2\n");
    object_class_property_add_bool(oc, "its", virt_get_its,
                                   virt_set_its);
    object_class_property_set_description(oc, "its",
                                          "Set on/off to enable/disable "
                                          "ITS instantiation");
}

static const TypeInfo FAKE_MACHINE_info = {
    .name          = MACHINE_TYPE_NAME("baize"),
    .parent        = TYPE_MACHINE,
    .class_init    = baize_machine_class_init
};

static void baize_machine_init(void)
{
    type_register_static(&FAKE_MACHINE_info);
}
type_init(baize_machine_init);
#endif
