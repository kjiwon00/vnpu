/*
 * NPU: learning PCIe accelerator device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NPU_NPU_H
#define HW_NPU_NPU_H

#include "hw/pci/pci_device.h"
#include "qemu/queue.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "npu_hw.h"     /* 드라이버와 공유하는 계약: 레지스터, 큐 엔트리, 명령 형식 */

#define TYPE_NPU "npu"
OBJECT_DECLARE_SIMPLE_TYPE(NpuState, NPU)

#define NPU_CLASS_ACCEL         0x1200          /* processing accelerator */

/* 속성 기본값과 범위 (장치 내부 설정. 드라이버는 CAP과 BAR 크기로 읽는다) */
#define NPU_DEFAULT_QUEUES      4               /* num-queues: 1 ~ NPU_MAX_QUEUES */
#define NPU_DEFAULT_DEVMEM_SIZE (64 * MiB)      /* devmem-size: 2의 거듭제곱 */
#define NPU_MIN_DEVMEM_SIZE     (1 * MiB)
#define NPU_DEFAULT_JOB_NS      10000           /* job-ns: job 하나의 고정 지연 */
#define NPU_DEFAULT_MAC_NS      1               /* mac-ns: GEMM MAC 하나의 지연 */

QEMU_BUILD_BUG_ON(NPU_VENDOR_ID != PCI_VENDOR_ID_QEMU);
QEMU_BUILD_BUG_ON(NPU_BAR0_SIZE != 64 * KiB);
QEMU_BUILD_BUG_ON(NPU_DEVMEM_MAX_SIZE != 1 * GiB);

/* 게스트 메모리의 큐 엔트리 (npu_hw.h). little-endian, 패딩 없음 */
typedef struct npu_sqe NpuSqe;
typedef struct npu_cqe NpuCqe;

typedef struct NpuSQueue NpuSQueue;
typedef struct NpuJob NpuJob;       /* engine.c 안에서만 내용을 안다 */

/*
 * SQ에서 꺼낸 명령 하나. 상태는 셋 중 하나다.
 *   sq->free_list   쓰이지 않음
 *   sq->cur         실행 중 (SQ마다 최대 하나)
 *   cq->done_list   실행이 끝났고 CQE 쓰기를 기다림
 */
typedef struct NpuRequest {
    NpuSQueue   *sq;
    uint16_t    cid;
    uint16_t    status;     /* NPU_SC_* */
    uint32_t    result;     /* CQE result */
    uint32_t    dw1;        /* CQE dw1 */
    NpuJob      *job;       /* job 큐의 요청만: 실행 중인 job */
    QTAILQ_ENTRY(NpuRequest) entry;
} NpuRequest;

/*
 * 장치 안에서만 쓰는 값. 명령 처리 함수가 이 값을 돌려주면 "CQE는 지금 보내지 않고
 * 나중에 npu_req_complete()로 보낸다"는 뜻이다. 상태 코드는 15비트라 실제 코드와 겹치지 않는다
 */
#define NPU_SC_NO_COMPLETE      0xffff

struct NpuSQueue {
    NpuState    *state;
    uint16_t    sqid;       /* 완료는 항상 같은 번호의 CQ로 간다 */
    uint32_t    head;       /* 장치가 다음에 읽을 칸 */
    uint32_t    tail;       /* 드라이버가 doorbell로 알려 준 끝 */
    uint32_t    size;
    uint64_t    dma_addr;
    QEMUBH      *bh;        /* doorbell을 받으면 명령 처리를 예약한다 */

    NpuRequest  *reqs;      /* size개를 미리 잡아 둔다: 게스트가 요청을 끝없이 쌓지 못한다 */
    QTAILQ_HEAD(, NpuRequest) free_list;
    NpuRequest  *cur;       /* 실행 중인 요청. 있으면 다음 SQE를 꺼내지 않는다 */
};

typedef struct NpuCQueue {
    NpuState    *state;
    uint16_t    cqid;       /* MSI-X 벡터 번호이기도 하다 */
    uint8_t     phase;      /* 1에서 시작, tail이 한 바퀴 돌 때마다 뒤집는다 */
    bool        irq_enabled;    /* CREATE_CQ의 NPU_CQ_F_IEN. admin CQ는 항상 켜짐 */
    uint32_t    head;       /* 드라이버가 doorbell로 알려 준 소비 위치 */
    uint32_t    tail;       /* 장치가 다음에 CQE를 쓸 칸 */
    uint32_t    size;
    uint64_t    dma_addr;
    QEMUBH      *bh;        /* CQE 쓰기와 인터럽트를 예약한다 */

    QTAILQ_HEAD(, NpuRequest) done_list;    /* CQ에 자리가 나면 CQE로 쓸 요청 */
} NpuCQueue;

struct NpuState {
    PCIDevice       parent_obj;
    MemoryRegion    mmio;       /* BAR0 */
    MemoryRegion    devmem;     /* BAR2: 장치 메모리 */

    uint32_t    ctrl;
    uint32_t    csts;
    uint32_t    fw_sts;     /* NPU_FW_STS_* */
    uint32_t    fw_version; /* READY가 된 펌웨어의 버전. 없으면 0 */
    uint32_t    aqa;
    uint64_t    asq;        /* ASQ_LO/HI를 deposit64()로 조립 */
    uint64_t    acq;        /* ACQ_LO/HI를 deposit64()로 조립 */

    /* 큐. 번호가 qid다. [0]은 admin, 없는 큐는 NULL */
    NpuSQueue   *sq[NPU_NUM_QIDS];
    NpuCQueue   *cq[NPU_NUM_QIDS];

    /* 만들 때 정하는 속성 (-device npu,num-queues=..,devmem-size=..) */
    uint32_t    num_queues; /* job 큐 수 (admin 제외), 1 ~ NPU_MAX_QUEUES */
    uint64_t    devmem_size;

    /* 실행 중에 qom-set으로 바꿀 수 있는 속성 */
    uint64_t    job_ns;             /* 지연 모델: job 하나의 고정 비용 */
    uint64_t    mac_ns;             /* 지연 모델: GEMM MAC 하나의 비용 */
    uint32_t    inject_cqe_drop;    /* 장애 주입: 다음 job CQE를 이 개수만큼 버린다 */
};

/* ---- ctrl.c ---- */
bool npu_dma_allowed(NpuState *n);      /* RDY=1이고 CFS=0일 때만 true */
void npu_ctrl_fatal(NpuState *n);       /* CFS=1로 만든다 (DESIGN.md 4.2절) */

/* ---- queue.c ---- */
bool npu_queue_start(NpuState *n);                      /* EN 0->1 */
void npu_queue_stop(NpuState *n);                       /* EN 1->0, 리셋, exit */
void npu_queue_doorbell(NpuState *n, hwaddr addr, uint32_t val);
void npu_req_complete(NpuRequest *req, uint16_t sc);    /* 실행이 끝난 요청의 CQE를 예약 */
bool npu_cdw_is_zero(const NpuSqe *sqe, unsigned first);    /* cdw[first..14]가 모두 0인가 */

/* ---- engine.c ---- */
uint16_t npu_job_submit(NpuState *n, NpuRequest *req, const NpuSqe *sqe);
void npu_job_cancel(NpuRequest *req);

/* ---- fw.c ---- */
uint16_t npu_fw_load(NpuState *n, NpuRequest *req, const NpuSqe *sqe);
bool npu_fw_ready(NpuState *n);
void npu_fw_reset(NpuState *n);                         /* 모든 리셋 */

#endif /* HW_NPU_NPU_H */
