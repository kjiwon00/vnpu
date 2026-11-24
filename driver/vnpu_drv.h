/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vnpu 드라이버 내부 헤더. 드라이버 파일끼리 공유하는 구조체와 함수 선언.
 * 장치와의 계약(레지스터, 큐, 명령 형식)은 npu_hw.h에 있다.
 */
#ifndef VNPU_DRV_H
#define VNPU_DRV_H

#include <linux/io.h>
#include <linux/types.h>
#include <drm/drm_device.h>

#include "npu_hw.h"

#define DRIVER_NAME		"vnpu"
#define DRIVER_DESC		"Virtual NPU (QEMU) accel driver"

/* CSTS 전환을 기다리는 최대 시간 (DESIGN.md 4.1: 양쪽 전환 모두 timeout을 둔다) */
#define VNPU_CSTS_TIMEOUT_MS	1000

struct vnpu_device {
	struct drm_device drm;		/* 첫 멤버: devm_drm_dev_alloc()이 이 구조체째로 할당한다 */

	void __iomem *regs;		/* BAR0: 레지스터 + doorbell */
	resource_size_t devmem_start;	/* BAR2: 장치 메모리 (단계 3에서 사용) */
	resource_size_t devmem_size;

	u32 version;			/* NPU_REG_VERSION */
	u32 cap;			/* NPU_REG_CAP */

	/* 단계 2 이후: admin 큐, 펌웨어 상태, job 큐 배열 ... */
};

static inline struct vnpu_device *to_vnpu(struct drm_device *drm)
{
	return container_of(drm, struct vnpu_device, drm);
}

/* BAR0 접근. 장치는 4바이트 접근만 받는다 */
static inline u32 vnpu_rd32(struct vnpu_device *vdev, u32 reg)
{
	return readl(vdev->regs + reg);
}

static inline void vnpu_wr32(struct vnpu_device *vdev, u32 reg, u32 val)
{
	writel(val, vdev->regs + reg);
}

/* ---- 하드웨어 제어 (vnpu_drv.c, 커지면 vnpu_hw.c로 옮긴다) ---- */
int vnpu_hw_check_id(struct vnpu_device *vdev);
int vnpu_hw_wait_csts(struct vnpu_device *vdev, u32 mask, u32 val,
		      unsigned int timeout_ms);
int vnpu_hw_disable(struct vnpu_device *vdev);
int vnpu_hw_enable(struct vnpu_device *vdev);

#endif /* VNPU_DRV_H */
