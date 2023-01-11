
#ifndef FAKE_SOC_H
#define FAKE_SOC_H

struct FakeSocState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    char *rom_file; // image file
    unsigned int smp_cpus; // 2
    DeviceState *gic; // gic v3
    PFlashCFI01 *flash; // NV configuration of uboot or uefi
};

#define TYPE_FAKE_SOC "fake_soc"
OBJECT_DECLARE_SIMPLE_TYPE(FakeSocState, FAKE_SOC)

#endif

