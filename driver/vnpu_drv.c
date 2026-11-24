// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnpu: QEMU 가상 NPU(hw/npu)용 DRM accel 드라이버.
 *
 * 이 파일: 모듈/PCI 등록, DRM 장치 등록, probe/remove, 컨트롤러 enable/disable.
 * 계약: npu_hw.h, DESIGN.md (특히 4.4절 "드라이버가 따르는 순서").
 */

#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>	/* DEFINE_DRM_ACCEL_FOPS가 drm_gem_mmap을 쓴다 */
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>

#include "vnpu_drv.h"

/* ------------------------------------------------------------------ */
/* 하드웨어 제어                                                       */
/* ------------------------------------------------------------------ */

int vnpu_hw_check_id(struct vnpu_device *vdev)
{
	u32 id = vnpu_rd32(vdev, NPU_REG_ID);

	if (id != NPU_ID_VALUE) {
		drm_err(&vdev->drm, "unknown device id 0x%08x\n", id);
		return -ENODEV;
	}

	vdev->version = vnpu_rd32(vdev, NPU_REG_VERSION);
	if (NPU_VERSION_MAJOR(vdev->version) !=
	    NPU_VERSION_MAJOR(NPU_VERSION_VALUE)) {
		drm_err(&vdev->drm, "unsupported version %u.%u\n",
			NPU_VERSION_MAJOR(vdev->version),
			vdev->version & 0xffff);
		return -ENODEV;
	}

	vdev->cap = vnpu_rd32(vdev, NPU_REG_CAP);
	return 0;
}

/*
 * (CSTS & mask) == val 이 될 때까지 polling한다.
 * timeout_ms 안에 안 되면 -ETIMEDOUT.
 * RDY=1을 기다리는 중에 CFS가 서면 기다려도 소용없으니 바로 -EIO.
 * RDY=0을 기다릴 때(disable)는 CFS가 서 있어도 리셋이 지워 주므로 계속 기다린다.
 *
 * 잠들 수 있다(usleep). 프로세스 문맥에서만 부른다.
 */
int vnpu_hw_wait_csts(struct vnpu_device *vdev, u32 mask, u32 val,
		      unsigned int timeout_ms)
{
	const bool fail_on_cfs = val & NPU_CSTS_RDY;
	u32 csts;
	int ret;

	ret = readl_poll_timeout(vdev->regs + NPU_REG_CSTS, csts,
				 (csts & mask) == val ||
				 (fail_on_cfs && (csts & NPU_CSTS_CFS)),
				 100, timeout_ms * USEC_PER_MSEC);
	if (ret) {
		drm_err(&vdev->drm, "CSTS timeout: csts 0x%x, want 0x%x/0x%x\n",
			csts, val, mask);
		return ret;
	}

	if ((csts & mask) != val) {
		drm_err(&vdev->drm, "controller fatal status (CSTS 0x%x)\n", csts);
		return -EIO;
	}

	return 0;
}

/*
 * 컨트롤러 리셋: EN=0을 쓰고 RDY=0을 기다린다.
 * 이 함수가 성공한 뒤에만 큐와 버퍼 메모리를 해제할 수 있다 (DESIGN.md 원칙 2).
 */
int vnpu_hw_disable(struct vnpu_device *vdev)
{
	vnpu_wr32(vdev, NPU_REG_CTRL, 0);

	return vnpu_hw_wait_csts(vdev, NPU_CSTS_RDY, 0, VNPU_CSTS_TIMEOUT_MS);
}

/*
 * 컨트롤러 시작: EN=1을 쓰고 RDY=1 또는 CFS=1을 기다린다.
 * 그 전에 AQA/ASQ/ACQ가 써져 있어야 한다. 아니면 장치는 CFS를 세운다.
 * (admin 큐는 다음 단계에서 vnpu_queue.c가 준비한다)
 */
int vnpu_hw_enable(struct vnpu_device *vdev)
{
	vnpu_wr32(vdev, NPU_REG_CTRL, NPU_CTRL_EN);

	return vnpu_hw_wait_csts(vdev, NPU_CSTS_RDY, NPU_CSTS_RDY,
				 VNPU_CSTS_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* DRM                                                                 */
/* ------------------------------------------------------------------ */

/* DRM_IOCTL_NPU_* 는 단계 2(GET_PARAM)부터 uapi 헤더를 만들고 여기 추가한다 */

DEFINE_DRM_ACCEL_FOPS(vnpu_fops);

static const struct drm_driver vnpu_drm_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL,	/* 단계 3: | DRIVER_GEM */
	.fops			= &vnpu_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= 0,
	.minor			= 1,
};

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */

/*
 * DESIGN.md 4.4 probe 순서 중 이번 단계 범위:
 *   - vnpu_device 할당 (devm_drm_dev_alloc)
 *   - PCI enable, bus master, DMA 마스크
 *   - BAR0 매핑, BAR2 위치/크기 기록
 *   - ID/VERSION 확인, CAP 읽기
 *   - EN=0 → RDY=0 확인
 *   - drm_dev_register → /dev/accel/accel0
 * 다음 단계: MSI-X 벡터, admin 큐, EN=1, 펌웨어 로딩, job 큐 생성
 */
static int vnpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vnpu_device *vdev;
	int ret;

	vdev = devm_drm_dev_alloc(&pdev->dev, &vnpu_drm_driver,
								struct vnpu_device, drm);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);
	pci_set_drvdata(pdev, vdev);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	vdev->regs = pcim_iomap_region(pdev, NPU_BAR_REG, DRIVER_NAME);
	if (IS_ERR(vdev->regs))
		return PTR_ERR(vdev->regs);
	vdev->devmem_start = pci_resource_start(pdev, NPU_BAR_DEVMEM);
	vdev->devmem_size = pci_resource_len(pdev, NPU_BAR_DEVMEM);

	ret = vnpu_hw_check_id(vdev);
	if (ret) 
		return ret;
	ret = vnpu_hw_disable(vdev);
	if (ret)
		return ret;
	// ret = vnpu_hw_enable(vdev);
	// if (ret)
	// 	return ret;

	ret = drm_dev_register(&vdev->drm, 0);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register DRM device: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * 역순 정리:
 *   - drm_dev_unplug (새 ioctl 차단, 진행 중인 것 기다림)
 *   - EN=0 → RDY=0 확인
 *   - 그 다음에 메모리 해제 (devm 자원은 자동)
 */
static void vnpu_remove(struct pci_dev *pdev)
{
	/* TODO */
	struct vnpu_device *vdev = pci_get_drvdata(pdev);

	drm_dev_unplug(&vdev->drm);   /* 새 ioctl 차단 + 사용자 공간에서 장치 숨김 */
	vnpu_hw_disable(vdev);        /* RDY=0 → 장치가 DMA 멈춤 */
	/* BAR 매핑, PCI enable, vdev 해제는 devm/pcim이 자동으로 */
}

static const struct pci_device_id vnpu_pci_ids[] = {
	{ PCI_DEVICE(NPU_VENDOR_ID, NPU_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, vnpu_pci_ids);

static struct pci_driver vnpu_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= vnpu_pci_ids,
	.probe		= vnpu_probe,
	.remove		= vnpu_remove,
	/* 단계 7: .shutdown, .err_handler(reset_prepare/reset_done) */
};
module_pci_driver(vnpu_pci_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
