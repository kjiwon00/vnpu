/*
 * NPU: learning PCIe accelerator device - PCI function, BAR0, controller state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 계약은 DESIGN.md의 2절(PCI), 3절(BAR0), 4절(상태 머신).
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/msix.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "npu.h"
#include "trace.h"

/* 장치가 게스트 메모리에 접근해도 되는지: RDY=1이고 CFS=0일 때만 (DESIGN.md 4절) */
bool npu_dma_allowed(NpuState *n)
{
    return (n->csts & (NPU_CSTS_RDY | NPU_CSTS_CFS)) == NPU_CSTS_RDY;
}

/* 치명적 오류. 이후 큐 처리, DMA, 인터럽트가 멈춘다. 리셋으로만 풀린다 (DESIGN.md 4.2절) */
void npu_ctrl_fatal(NpuState *n)
{
    n->csts |= NPU_CSTS_CFS;
    trace_npu_ctrl_fatal();
}

/*
 * 컨트롤러를 초기 상태로 되돌린다. EN 1->0, FLR, 시스템 리셋이 모두 여기로 온다 (4.3절).
 * 큐를 먼저 멈춘 뒤에 RDY를 내린다: RDY=0을 본 드라이버는 큐 메모리를 해제해도 된다.
 * PCI config와 MSI-X는 PCI 코어가 리셋한다. 장치 메모리의 내용은 건드리지 않는다.
 */
static void npu_ctrl_reset(NpuState *n)
{
    npu_queue_stop(n);
    npu_fw_reset(n);

    n->ctrl = 0;
    n->csts = 0;
    n->asq = 0;
    n->acq = 0;
    n->aqa = 0;
}

/* EN 0->1: admin 큐 설정이 맞으면 RDY, 틀리면 CFS (4.1절) */
static void npu_ctrl_enable(NpuState *n)
{
    n->ctrl |= NPU_CTRL_EN;

    if (!npu_queue_start(n)) {
        qemu_log_mask(LOG_GUEST_ERROR, "npu: invalid admin queue setup "
                      "(aqa 0x%x asq 0x%" PRIx64 " acq 0x%" PRIx64 ")\n",
                      n->aqa, n->asq, n->acq);
        npu_ctrl_fatal(n);
        return;
    }
    n->csts |= NPU_CSTS_RDY;
    trace_npu_ctrl_enable();
}

/*
 * ASQ/ACQ는 64비트 값을 32비트씩 나눠 받는다. 쓰기마다 해당 절반만 바꾸고 나머지는 둔다.
 * 범위와 정렬은 여기서 보지 않고 EN 0->1에서 한꺼번에 검사한다 (절반만 쓴 상태는 정상이므로)
 */
static void npu_write_admin_queue_reg(NpuState *n, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case NPU_REG_ASQ_LO:
        n->asq = deposit64(n->asq, 0, 32, val);
        break;
    case NPU_REG_ASQ_HI:
        n->asq = deposit64(n->asq, 32, 32, val);
        break;
    case NPU_REG_ACQ_LO:
        n->acq = deposit64(n->acq, 0, 32, val);
        break;
    case NPU_REG_ACQ_HI:
        n->acq = deposit64(n->acq, 32, 32, val);
        break;
    case NPU_REG_AQA:
        /* 정의된 필드만 남긴다. 예약 비트는 읽으면 0 */
        n->aqa = val & ((NPU_AQA_SIZE_MASK << NPU_AQA_ASQS_SHIFT) |
                        (NPU_AQA_SIZE_MASK << NPU_AQA_ACQS_SHIFT));
        break;
    default:
        g_assert_not_reached();
    }
}

static uint64_t npu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NpuState *n = opaque;
    uint64_t val = 0;

    switch (addr) {
    case NPU_REG_ID:
        val = NPU_ID_VALUE;
        break;
    case NPU_REG_VERSION:
        val = NPU_VERSION_VALUE;
        break;
    case NPU_REG_CAP:
        val = (n->num_queues << NPU_CAP_NQ_SHIFT) |
              (NPU_MAX_QUEUE_ENTRIES << NPU_CAP_MQES_SHIFT);
        break;
    case NPU_REG_CTRL:
        val = n->ctrl;
        break;
    case NPU_REG_CSTS:
        val = n->csts;
        break;
    case NPU_REG_FW_STS:
        val = n->fw_sts;
        break;
    case NPU_REG_ASQ_LO:
        val = extract64(n->asq, 0, 32);
        break;
    case NPU_REG_ASQ_HI:
        val = extract64(n->asq, 32, 32);
        break;
    case NPU_REG_ACQ_LO:
        val = extract64(n->acq, 0, 32);
        break;
    case NPU_REG_ACQ_HI:
        val = extract64(n->acq, 32, 32);
        break;
    case NPU_REG_AQA:
        val = n->aqa;
        break;
    default:
        /* doorbell(쓰기 전용), 예약 페이지, 정의되지 않은 오프셋은 0으로 읽힌다 */
        break;
    }
    trace_npu_mmio_read(addr, val, size);
    return val;
}

static void npu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NpuState *n = opaque;

    trace_npu_mmio_write(addr, val, size);

    if (addr >= NPU_DB_BASE) {
        npu_queue_doorbell(n, addr, val);
        return;
    }

    switch (addr) {
    case NPU_REG_CTRL:
        if ((val & NPU_CTRL_EN) && !(n->ctrl & NPU_CTRL_EN)) {
            npu_ctrl_enable(n);
        } else if (!(val & NPU_CTRL_EN) && (n->ctrl & NPU_CTRL_EN)) {
            npu_ctrl_reset(n);
            trace_npu_ctrl_disable();
        }
        break;
    case NPU_REG_ASQ_LO:
    case NPU_REG_ASQ_HI:
    case NPU_REG_ACQ_LO:
    case NPU_REG_ACQ_HI:
    case NPU_REG_AQA:
        /* 장치는 EN 0->1 순간에만 읽는다. 실행 중에 바뀌면 안 되므로 EN=1이면 무시 */
        if (n->ctrl & NPU_CTRL_EN) {
            qemu_log_mask(LOG_GUEST_ERROR, "npu: write to admin queue register "
                          "0x%" HWADDR_PRIx " while EN=1 ignored\n", addr);
            break;
        }
        npu_write_admin_queue_reg(n, addr, val);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "npu: write to read-only/unknown "
                      "register 0x%" HWADDR_PRIx " ignored\n", addr);
        break;
    }
}

static const MemoryRegionOps npu_mmio_ops = {
    .read = npu_mmio_read,
    .write = npu_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void npu_realize(PCIDevice *pdev, Error **errp)
{
    NpuState *n = NPU(pdev);
    unsigned i;

    if (n->num_queues < 1 || n->num_queues > NPU_MAX_QUEUES) {
        error_setg(errp, "npu: num-queues must be between 1 and %d",
                   NPU_MAX_QUEUES);
        return;
    }
    if (n->devmem_size < NPU_MIN_DEVMEM_SIZE ||
        n->devmem_size > NPU_DEVMEM_MAX_SIZE ||
        !is_power_of_2(n->devmem_size)) {
        error_setg(errp, "npu: devmem-size must be a power of two "
                   "between 1MiB and 1GiB");
        return;
    }

    /* BAR2: 장치 메모리. 게스트 CPU도 직접 읽고 쓸 수 있는 RAM이다 */
    if (!memory_region_init_ram(&n->devmem, OBJECT(n), "npu-devmem",
                                n->devmem_size, errp)) {
        return;
    }

    if (pcie_endpoint_cap_init(pdev, 0x80) < 0) {
        error_setg(errp, "npu: failed to add PCIe capability");
        return;
    }
    pcie_cap_flr_init(pdev);    /* FLR 지원 표시. 실제 리셋은 npu_write_config에서 */

    /* BAR4: MSI-X. 벡터 번호 = qid, admin 큐(0)까지 NPU_MSIX_VECTORS개 */
    if (msix_init_exclusive_bar(pdev, NPU_MSIX_VECTORS, NPU_BAR_MSIX,
                                errp) < 0) {
        pcie_cap_exit(pdev);
        return;
    }
    for (i = 0; i < NPU_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }

    /* BAR0: 레지스터 + doorbell */
    memory_region_init_io(&n->mmio, OBJECT(n), &npu_mmio_ops, n, "npu-bar0",
                          NPU_BAR0_SIZE);
    pci_register_bar(pdev, NPU_BAR_REG, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &n->mmio);
    pci_register_bar(pdev, NPU_BAR_DEVMEM,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &n->devmem);
}

/* FLR 비트(DEVCTL.BCR_FLR)가 써지면 pcie_cap_flr_write_config가 장치를 리셋한다 */
static void npu_write_config(PCIDevice *pdev, uint32_t addr, uint32_t val,
                             int len)
{
    pci_default_write_config(pdev, addr, val, len);
    pcie_cap_flr_write_config(pdev, addr, val, len);
}

static void npu_exit(PCIDevice *pdev)
{
    NpuState *n = NPU(pdev);

    npu_queue_stop(n);
    npu_fw_reset(n);
    msix_uninit_exclusive_bar(pdev);
    pcie_cap_exit(pdev);
}

/* 시스템 리셋(게스트 재부팅)과 FLR에서 불린다 */
static void npu_reset(DeviceState *dev)
{
    trace_npu_reset();
    npu_ctrl_reset(NPU(dev));
}

/* 장애 주입: qom-set <장치> inject-cfs true. 값은 저장하지 않고 CFS를 세우기만 한다 */
static void npu_set_inject_cfs(Object *obj, bool value, Error **errp)
{
    if (value) {
        npu_ctrl_fatal(NPU(obj));
    }
}

/* 실행 중에도 바꿀 수 있는 속성. qdev 속성(아래 npu_props)은 realize 뒤에 못 바꾼다 */
static void npu_instance_init(Object *obj)
{
    NpuState *n = NPU(obj);

    n->job_ns = NPU_DEFAULT_JOB_NS;
    n->mac_ns = NPU_DEFAULT_MAC_NS;

    object_property_add_uint64_ptr(obj, "job-ns", &n->job_ns,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "mac-ns", &n->mac_ns,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "inject-cqe-drop", &n->inject_cqe_drop,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "inject-cfs", NULL, npu_set_inject_cfs);
}

static const Property npu_props[] = {
    DEFINE_PROP_UINT32("num-queues", NpuState, num_queues, NPU_DEFAULT_QUEUES),
    DEFINE_PROP_SIZE("devmem-size", NpuState, devmem_size,
                     NPU_DEFAULT_DEVMEM_SIZE),
};

/* 큐와 실행 중인 job의 상태를 옮기는 코드가 없다. 마이그레이션을 막는다 */
static const VMStateDescription npu_vmstate = {
    .name = TYPE_NPU,
    .unmigratable = 1,
};

static void npu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = npu_realize;
    pc->exit = npu_exit;
    pc->config_write = npu_write_config;
    pc->vendor_id = PCI_VENDOR_ID_QEMU;
    pc->device_id = NPU_DEVICE_ID;
    pc->revision = 0x01;
    pc->class_id = NPU_CLASS_ACCEL;

    dc->desc = "Learning NPU (PCIe accelerator)";
    dc->vmsd = &npu_vmstate;
    device_class_set_props(dc, npu_props);
    device_class_set_legacy_reset(dc, npu_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo npu_types[] = {
    {
        .name          = TYPE_NPU,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(NpuState),
        .instance_init = npu_instance_init,
        .class_init    = npu_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(npu_types)
