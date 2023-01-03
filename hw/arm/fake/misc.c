
#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/block/flash.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "sysemu/runstate.h"
#include "trace.h"

#define TYPE_FAKE_MISC_IP "fake.misc"
OBJECT_DECLARE_SIMPLE_TYPE(fake_misc_ip, FAKE_MISC_IP)

struct fake_misc_ip {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion mem;
    VMChangeStateEntry *vmstate;

    BlockBackend *blk;
    uint16_t cfg;
    bool ro;

    uint8_t reg_cmd;
    uint8_t reg_status;
};

static const VMStateDescription vmstate_misc = {
    .name = "fake_misc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(reg_cmd, fake_misc_ip),
        VMSTATE_UINT8(reg_status, fake_misc_ip),
        VMSTATE_END_OF_LIST()
    }
};


static MemTxResult misc_mem_read_with_attrs(void *opaque, hwaddr addr, uint64_t *value,
                                            unsigned len, MemTxAttrs attrs)
{
    *value = 0xdeaddead;
    return MEMTX_OK;
}

static MemTxResult misc_mem_write_with_attrs(void *opaque, hwaddr addr, uint64_t value,
                                             unsigned len, MemTxAttrs attrs)
{
    switch (addr) {
    case 0: // reboot
        if (0x9070dead == value) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        break;
    default:
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps misc_ops = {
    .read_with_attrs = misc_mem_read_with_attrs,
    .write_with_attrs = misc_mem_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void fake_misc_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    fake_misc_ip *pfl = FAKE_MISC_IP(dev);

    memory_region_init_rom_device(
        &pfl->mem, OBJECT(dev),
        &misc_ops,
        pfl,
        "fake-misc", 0x1000, errp);
    if (*errp) {
        return;
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &pfl->mem);
}

static void fake_misc_system_reset(DeviceState *dev)
{
    fake_misc_ip *pfl = FAKE_MISC_IP(dev);

    pfl->reg_cmd = 0x00;
    pfl->reg_status = 0x80;
    memory_region_rom_device_set_romd(&pfl->mem, true);
}

static Property fake_misc_properties[] = {
    DEFINE_PROP_DRIVE("drive", fake_misc_ip, blk),
    DEFINE_PROP_UINT16("cfg", fake_misc_ip, cfg, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void fake_misc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = fake_misc_system_reset;
    dc->realize = fake_misc_realize;
    device_class_set_props(dc, fake_misc_properties);
    dc->vmsd = &vmstate_misc;
}

static const TypeInfo fake_misc_ip_info = {
    .name           = TYPE_FAKE_MISC_IP,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(fake_misc_ip),
    .class_init     = fake_misc_class_init,
};

static void FAKE_MISC_IP_register_types(void)
{
    type_register_static(&fake_misc_ip_info);
}

type_init(FAKE_MISC_IP_register_types)
