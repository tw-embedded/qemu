#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/block/flash.h"
#include "hw/vfio/vfio-amd-xgbe.h"
#include "hw/loader.h"
#include "hw/platform-bus.h"
#include "hw/irq.h"
#include "kvm_arm.h"
#include "hw/mem/memory-device.h"
#include "hw/virtio/virtio-iommu.h"
#include "hw/char/pl011.h"

#include "fake_soc.h"

enum {
    FAKE_ROM,
    FAKE_NOR_FLASH,
    FAKE_MEM,
    FAKE_CPU_ITF,
    FAKE_GIC_DIST,
    FAKE_GIC_REDIST,
    FAKE_SMMU,
    FAKE_UART,
    FAKE_GPIO,
    FAKE_VIRTIO,
    FAKE_RTC,
    FAKE_MISC,
    FAKE_SECURE_MEM
};

static const MemMapEntry fake_memmap[] = {
    [FAKE_ROM] =        { 0x00000000, 0x04000000 }, // 64M boot ROM
    [FAKE_NOR_FLASH] =  { 0x04000000, 0x01000000 }, // 16M nor flash
    [FAKE_SECURE_MEM] = { 0x08000000, 0x08000000 },
    [FAKE_CPU_ITF] =    { 0x10000000, 0x00010000 },
    [FAKE_GIC_DIST] =   { 0x10010000, 0x00010000 },
    [FAKE_GIC_REDIST] = { 0x10020000, 0x04000000 }, // GICV3_REDIST_SIZE
    [FAKE_SMMU] =       { 0x14000000, 0x00020000 },
    [FAKE_UART] =       { 0x20000000, 0x00001000 },
    [FAKE_GPIO] =       { 0x20001000, 0x00001000 },
    [FAKE_VIRTIO] =     { 0x20002000, 0x00000200 }, // size * NUM_VIRTIO_TRANSPORTS
    [FAKE_RTC] =        { 0x20003000, 0x00001000 },
    [FAKE_MISC] =       { 0x20004000, 0x00001000 },
    [FAKE_MEM] =        { 0x30000000ULL, 0x40000000ULL },
};

static const int fake_irqmap[] = {
    [FAKE_UART] = 0x10,
    [FAKE_GPIO] = 0x11,
    [FAKE_VIRTIO] = 0x12, /* + NUM_VIRTIO_TRANSPORTS */
    [FAKE_RTC] =  0x1a
};

#define NUM_IRQS 256

#define ARCH_GIC_MAINT_IRQ  9

#define ARCH_TIMER_S_EL1_IRQ  10
#define ARCH_TIMER_NS_EL1_IRQ 11
#define ARCH_TIMER_VIRT_IRQ   12
#define ARCH_TIMER_NS_EL2_IRQ 13

#define VIRTUAL_PMU_IRQ 7

static void create_gic(FakeSocState *fss)
{
    SysBusDevice *gicbusdev;
    int i;

    fss->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(fss->gic, "revision", 3);
    qdev_prop_set_uint32(fss->gic, "num-cpu", fss->smp_cpus);
    /* Note that the num-irq property counts both internal and external interrupts;
       there are always 32 of the former (mandated by GIC spec). */
    qdev_prop_set_uint32(fss->gic, "num-irq", NUM_IRQS + 32);
    qdev_prop_set_bit(fss->gic, "has-security-extensions", true);
    qdev_prop_set_uint32(fss->gic, "len-redist-region-count", 1);
    qdev_prop_set_uint32(fss->gic, "redist-region-count[0]", fss->smp_cpus);

    gicbusdev = SYS_BUS_DEVICE(fss->gic);
    sysbus_realize_and_unref(gicbusdev, NULL);
    sysbus_mmio_map(gicbusdev, 0, fake_memmap[FAKE_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, fake_memmap[FAKE_GIC_REDIST].base);

    /* Wire the outputs from each CPU's generic timer and the GICv3 maintenance interrupt signal to the appropriate GIC PPI inputs,
       and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs. */
    for (i = 0; i < fss->smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;
        int irq;
        /* Mapping from the output timer irq lines from the CPU to the GIC PPI inputs used for this board. */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq, qdev_get_gpio_in(fss->gic, ppibase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0, qdev_get_gpio_in(fss->gic, ppibase + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0, qdev_get_gpio_in(fss->gic, ppibase + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + fss->smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * fss->smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * fss->smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

static void create_uart(FakeSocState *fss, int uart, MemoryRegion *mem, Chardev *chr)
{
    hwaddr base = fake_memmap[uart].base;
    int irq = fake_irqmap[uart];
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(fss->gic, irq));
}

#define FAKE_FLASH_SECTOR_SIZE (256 * KiB)

static void create_pflash(FakeSocState *fss, int pflash, MemoryRegion *mem, DriveInfo *dev_info)
{
#define PFLASH_NAME "fake.pflash"
    /* Create a single flash device.  We use the same parameters as the flash devices on the Versatile Express board. */
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
    qdev_prop_set_uint64(dev, "sector-length", FAKE_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", PFLASH_NAME);
    object_property_add_child(OBJECT(fss), PFLASH_NAME, OBJECT(dev));
    object_property_add_alias(OBJECT(fss), "pflash", OBJECT(dev), "drive");
    fss->flash = PFLASH_CFI01(dev);

    pflash_cfi01_legacy_drive(fss->flash, dev_info);

    /* map flash */
    assert(QEMU_IS_ALIGNED(fake_memmap[pflash].size, FAKE_FLASH_SECTOR_SIZE));
    assert(fake_memmap[pflash].size / FAKE_FLASH_SECTOR_SIZE <= UINT32_MAX);
    qdev_prop_set_uint32(dev, "num-blocks", fake_memmap[pflash].size / FAKE_FLASH_SECTOR_SIZE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(mem, fake_memmap[pflash].base, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

#define FAKE_VIRTIO_TRANSPORTS_NUM 8

static void create_virtio(FakeSocState *fss)
{
    int i;
    hwaddr size = fake_memmap[FAKE_VIRTIO].size;

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
    for (i = 0; i < FAKE_VIRTIO_TRANSPORTS_NUM; i++) {
        int irq = fake_irqmap[FAKE_VIRTIO] + i;
        hwaddr base = fake_memmap[FAKE_VIRTIO].base + i * size;
        sysbus_create_simple("virtio-mmio", base, qdev_get_gpio_in(fss->gic, irq));
    }
}

static void create_rtc(FakeSocState *fss)
{
    sysbus_create_simple("pl031", fake_memmap[FAKE_RTC].base, qdev_get_gpio_in(fss->gic, fake_irqmap[FAKE_RTC]));
}

static void fake_realize(DeviceState *socdev, Error **errp)
{
    FakeSocState *s = FAKE_SOC(socdev);

    // cpus
    Object *cpu0 = object_new(ARM_CPU_TYPE_NAME("cortex-a57"));
    Object *cpu1 = object_new(ARM_CPU_TYPE_NAME("cortex-a57"));
    qdev_realize(DEVICE(cpu0), NULL, NULL);
    qdev_realize(DEVICE(cpu1), NULL, NULL);
    object_property_set_bool(cpu0, "has_el3", true, NULL);
    object_property_set_bool(cpu1, "has_el3", true, NULL);
    s->smp_cpus = 2;

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
    create_gic(s);

    // rom
    MemoryRegion *bootrom = g_new(MemoryRegion, 1);
    memory_region_init_ram(bootrom, NULL, "boot.flash", fake_memmap[FAKE_ROM].size, NULL);
    memory_region_set_readonly(bootrom, true);
    memory_region_add_subregion(system_mem, fake_memmap[FAKE_ROM].base, bootrom);
    char *fname;
    int image_size;
    fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->rom_file);
    if (!fname) {
        error_report("Could not find ROM image '%s'", s->rom_file);
        exit(1);
    }
    image_size = load_image_mr(fname, bootrom);
    g_free(fname);
    if (image_size < 0) {
        error_report("Could not load ROM image '%s'", s->rom_file);
        exit(1);
    }
    object_property_set_link(cpu0, "secure-memory", OBJECT(bootrom), NULL);

    // peripheral
#define FAKE_SERIAL_INDEX 0
#define FAKE_PFLASH_INDEX 0
    create_uart(s, FAKE_UART, system_mem, serial_hd(FAKE_SERIAL_INDEX)); //pl011_luminary_create(fake_memmap[FAKE_UART].base, qdev_get_gpio_in(gic, fake_irqmap[FAKE_UART]), serial_hd(0));
    create_pflash(s, FAKE_NOR_FLASH, system_mem, drive_get(IF_PFLASH, 0, FAKE_PFLASH_INDEX)); /* Map legacy -drive if=pflash to machine properties */
    create_virtio(s);
    create_rtc(s);

    // others
    sysbus_create_simple("fake.misc", fake_memmap[FAKE_MISC].base, NULL);
}

static void fake_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = fake_realize;
}

static void fake_init(Object *obj)
{
}

static const TypeInfo fake_type = {
    .name = TYPE_FAKE_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FakeSocState),
    .instance_init = fake_init,
    .class_init = fake_class_init,
};

static void fake_soc_init(void)
{
    type_register_static(&fake_type);
}
type_init(fake_soc_init)
