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

enum {
    FAKE_NOR_FLASH,
    FAKE_MEM,
    FAKE_CPU_ITF,
    FAKE_GIC_DIST,
    FAKE_GIC_REDIST,
    FAKE_SMMU,
    FAKE_UART,
    FAKE_GPIO,
    FAKE_SECURE_MEM
};

static const MemMapEntry fake_memmap[] = {
    /* 512M boot ROM */
    [FAKE_NOR_FLASH] =          { 0x00000000, 0x08000000 },
    [FAKE_SECURE_MEM] =         { 0x08000000, 0x08000000 },
    [FAKE_CPU_ITF] =            { 0x10000000, 0x00010000 },
    [FAKE_GIC_DIST] =           { 0x10010000, 0x00010000 },
    [FAKE_GIC_REDIST] =         { 0x10020000, 0x04000000 }, //GICV3_REDIST_SIZE
    [FAKE_SMMU] =               { 0x14000000, 0x00020000 },
    [FAKE_UART] =               { 0x20000000, 0x00001000 },
    [FAKE_GPIO] =               { 0x20001000, 0x00001000 },
    [FAKE_MEM] =                { 0x30000000ULL, 0x40000000ULL },
};

static struct arm_boot_info bz_board_binfo = {
    .ram_size = 4 * GiB,
    .loader_start     = 0,
    .smp_loader_start = 0,
    .gic_cpu_if_addr = fake_memmap[FAKE_CPU_ITF].base,
};

#define NUM_IRQS 256

struct BzMachineState {
    MachineState parent;
    struct arm_boot_info bootinfo;
    int smp_cpus;
    void *fdt;
    int fdt_size;
    int psci_conduit;
    DeviceState *gic;
};
#define TYPE_BZ_MACHINE MACHINE_TYPE_NAME("baize")
OBJECT_DECLARE_SIMPLE_TYPE(BzMachineState, BZ_MACHINE)

static void create_gic(BzMachineState *bzms)
{
    unsigned int smp_cpus = 2;
    SysBusDevice *gicbusdev;
    int i;

    bzms->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(bzms->gic, "revision", 3);
    qdev_prop_set_uint32(bzms->gic, "num-cpu", smp_cpus);
    /* Note that the num-irq property counts both internal and external interrupts;
       there are always 32 of the former (mandated by GIC spec). */
    qdev_prop_set_uint32(bzms->gic, "num-irq", NUM_IRQS + 32);
    qdev_prop_set_bit(bzms->gic, "has-security-extensions", true);
    qdev_prop_set_uint32(bzms->gic, "len-redist-region-count", 1);
    qdev_prop_set_uint32(bzms->gic, "redist-region-count[0]", smp_cpus);

    gicbusdev = SYS_BUS_DEVICE(bzms->gic);
    sysbus_realize_and_unref(gicbusdev, NULL);
    sysbus_mmio_map(gicbusdev, 0, fake_memmap[FAKE_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, fake_memmap[FAKE_GIC_REDIST].base);

    /*
     * Wire the outputs from each CPU's generic timer and the GICv3
     * maintenance interrupt signal to the appropriate GIC PPI inputs,
     * and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;
        int irq;
        /*
         * Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs used for this board.
         */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(bzms->gic,
                                                   ppibase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(bzms->gic, ppibase
                                                     + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(bzms->gic, ppibase
                                                     + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

static const int fake_irqmap[] = {
    [FAKE_UART] = 8,
    [FAKE_GPIO] = 9,
};

static void create_uart(const BzMachineState *bzms, int uart, MemoryRegion *mem, Chardev *chr)
{
    hwaddr base = fake_memmap[uart].base;
    int irq = fake_irqmap[uart];
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(bzms->gic, irq));
}

static void bz_init(MachineState *machine)
{
    BzMachineState *bzms = BZ_MACHINE(machine);

    // cpu
    Object *cpu0 = object_new(ARM_CPU_TYPE_NAME("cortex-a57"));
    Object *cpu1 = object_new(ARM_CPU_TYPE_NAME("cortex-a57"));
    qdev_realize(DEVICE(cpu0), NULL, NULL);
    qdev_realize(DEVICE(cpu1), NULL, NULL);
    object_property_set_bool(cpu0, "has_el3", true, NULL);
    object_property_set_bool(cpu1, "has_el3", true, NULL);

    // memory
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *system_mem = get_system_memory();
    memory_region_init_ram(sram, NULL, "bz.sram", fake_memmap[FAKE_MEM].size, NULL);
    memory_region_add_subregion(system_mem, fake_memmap[FAKE_MEM].base, sram);

    MemoryRegion *secram = g_new(MemoryRegion, 1);
    memory_region_init_ram(secram, NULL, "fake.secure-ram", fake_memmap[FAKE_SECURE_MEM].size, NULL);
    memory_region_add_subregion(system_mem, fake_memmap[FAKE_SECURE_MEM].base, secram);

    object_property_set_link(cpu0, "memory", OBJECT(system_mem), NULL);
    object_property_set_link(cpu1, "memory", OBJECT(system_mem), NULL);

    // interrupt
    create_gic(bzms);

    // rom
    MemoryRegion *norflash = g_new(MemoryRegion, 1);
    memory_region_init_ram(norflash, NULL, "nor.flash", fake_memmap[FAKE_NOR_FLASH].size, NULL);
    memory_region_set_readonly(norflash, true);
    memory_region_add_subregion(system_mem, fake_memmap[FAKE_NOR_FLASH].base, norflash);
    char *fname;
    int image_size;
    fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!fname) {
        error_report("Could not find ROM image '%s'", machine->firmware);
        exit(1);
    }
    image_size = load_image_mr(fname, norflash);
    g_free(fname);
    if (image_size < 0) {
        error_report("Could not load ROM image '%s'", machine->firmware);
        exit(1);
    }

    object_property_set_link(cpu0, "secure-memory", OBJECT(norflash), NULL);

    // peripheral
    //pl011_luminary_create(fake_memmap[FAKE_UART].base, qdev_get_gpio_in(gic, fake_irqmap[FAKE_UART]), serial_hd(0));
    create_uart(bzms, FAKE_UART, system_mem, serial_hd(0));
    // boot
    arm_load_kernel(ARM_CPU(first_cpu), machine, &bz_board_binfo);
}

static void bz_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "bai-ze board";
    mc->init = bz_init;
    mc->max_cpus = 2;
    mc->min_cpus = 1;
    mc->default_cpus = 2;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a57");
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo bz_type = {
    .name = MACHINE_TYPE_NAME("baize"),
    .parent = TYPE_MACHINE,
    .class_init = bz_class_init,
};

static void bz_machines_init(void)
{
    type_register_static(&bz_type);
}

type_init(bz_machines_init)
