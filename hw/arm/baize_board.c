#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"

#include "fake_soc.h"

struct BzMachineState {
    MachineState parent;
    FakeSocState soc;
};
#define TYPE_BZ_MACHINE MACHINE_TYPE_NAME("baize")
OBJECT_DECLARE_SIMPLE_TYPE(BzMachineState, BZ_MACHINE)

static struct arm_boot_info bz_board_binfo = {
    .ram_size = 4 * GiB,
    .loader_start = 0,
    .smp_loader_start = 0
};

static void bz_init(MachineState *machine)
{
    BzMachineState *s = g_new(BzMachineState, 1);

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_FAKE_SOC);

    s->soc.norflash_file = machine->firmware;

    sysbus_realize(SYS_BUS_DEVICE(&s->soc), NULL);

    // power on board
    arm_load_kernel(ARM_CPU(first_cpu), machine, &bz_board_binfo);
}

static void bz_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "bai-ze board";
    mc->init = bz_init;
    //mc->min_cpus = 1;
    //mc->max_cpus = 64;
    //mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a57");
    mc->default_cpus = 29;
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
