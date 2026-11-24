/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NPU 하드웨어 인터페이스: QEMU 장치(qemu/hw/npu)와 Linux 드라이버가 공유하는 계약.
 *
 * 원본은 이 파일 하나다. qemu/hw/npu/npu_hw.h는 이 파일을 가리키는 심볼릭 링크다.
 * 양쪽이 각자의 헤더 뒤에 include할 수 있도록 여기서는 아무것도 include하지 않는다.
 * 각 항목의 의미와 근거는 DESIGN.md에 있다.
 *
 * 공통 규칙
 *  - 게스트 메모리와 장치 메모리의 모든 다중 바이트 값은 little-endian이다.
 *  - 예약 필드(rsvd)와 정의되지 않은 flags 비트는 0이어야 한다. 장치는 0이 아니면 거부한다.
 */
#ifndef NPU_HW_H
#define NPU_HW_H

/* 필드 타입: 커널에서는 장치가 보는 값을 little-endian 타입으로 표시한다 */
#ifdef __KERNEL__
#define NPU_U8      __u8
#define NPU_U16     __le16
#define NPU_U32     __le32
#define NPU_U64     __le64
#else
#define NPU_U8      uint8_t
#define NPU_U16     uint16_t
#define NPU_U32     uint32_t
#define NPU_U64     uint64_t
#endif

/* ---- PCI ---- */
#define NPU_VENDOR_ID           0x1234          /* PCI_VENDOR_ID_QEMU */
#define NPU_DEVICE_ID           0x2026

#define NPU_BAR_REG             0       /* 64KiB, 32비트: 레지스터 + doorbell */
#define NPU_BAR_DEVMEM          2       /* 64비트 prefetchable: 장치 메모리. BAR 크기 = 메모리 크기 */
#define NPU_BAR_MSIX            4       /* 4KiB, 32비트: MSI-X 테이블 + PBA */

#define NPU_PAGE_SIZE           0x1000
#define NPU_DEVMEM_MAX_SIZE     0x40000000u     /* 1GiB. 장치 메모리 오프셋은 32비트로 충분하다 */

/*
 * ---- BAR0 (4KiB 페이지 16개, 큐 수와 상관없이 크기 고정) ----
 *   페이지 0       0x0000  레지스터
 *   페이지 1       0x1000  admin 큐(qid 0) doorbell
 *   페이지 2~9     0x2000  job 큐(qid 1~8) doorbell
 *   페이지 10~15           예약: 읽으면 0, 쓰면 무시
 *
 * 모든 접근은 4바이트. 64비트 주소(ASQ/ACQ)는 LO, HI 순서로 두 번 쓴다.
 * ASQ/ACQ/AQA는 EN=0일 때만 쓸 수 있고(EN=1이면 무시), 장치는 EN 0->1 순간에만 읽는다.
 */
#define NPU_BAR0_SIZE           0x10000
#define NPU_REG_ID              0x00    /* RO: NPU_ID_VALUE */
#define NPU_REG_VERSION         0x04    /* RO: major << 16 | minor */
#define NPU_REG_CAP             0x08    /* RO: NPU_CAP_* */
#define NPU_REG_CTRL            0x10    /* RW: NPU_CTRL_* */
#define NPU_REG_CSTS            0x14    /* RO: NPU_CSTS_* */
#define NPU_REG_FW_STS          0x1C    /* RO: NPU_FW_STS_* */
#define NPU_REG_ASQ_LO          0x20    /* RW: admin SQ 주소 하위 32비트 */
#define NPU_REG_ASQ_HI          0x24    /* RW: admin SQ 주소 상위 32비트 */
#define NPU_REG_ACQ_LO          0x28    /* RW: admin CQ 주소 하위 32비트 */
#define NPU_REG_ACQ_HI          0x2C    /* RW: admin CQ 주소 상위 32비트 */
#define NPU_REG_AQA             0x30    /* RW: NPU_AQA_* */

#define NPU_ID_VALUE            0x3155504e      /* "NPU1" */
#define NPU_VERSION_VALUE       0x00010000      /* 1.0 */
#define NPU_VERSION_MAJOR(v)    ((v) >> 16)

#define NPU_CAP_NQ_SHIFT        0               /* [7:0]   job 큐 수 (admin 제외) */
#define NPU_CAP_NQ_MASK         0xffu
#define NPU_CAP_MQES_SHIFT      16              /* [31:16] 큐 하나의 최대 엔트리 수 */
#define NPU_CAP_MQES_MASK       0xffffu

#define NPU_CTRL_EN             (1u << 0)       /* 0->1: 시작, 1->0: 컨트롤러 리셋 */
#define NPU_CSTS_RDY            (1u << 0)       /* 0이면 장치는 게스트 메모리에 접근하지 않는다 */
#define NPU_CSTS_CFS            (1u << 1)       /* 치명적 오류. EN 1->0으로만 지워진다 */

/* RDY=1이 된 뒤 admin FW_LOAD로 NONE -> LOADING -> READY/ERROR. 모든 리셋은 NONE으로 되돌린다 */
#define NPU_FW_STS_NONE         0
#define NPU_FW_STS_LOADING      1
#define NPU_FW_STS_READY        2
#define NPU_FW_STS_ERROR        3

/* admin 큐 엔트리 수: "개수 - 1"이 아니라 실제 개수 (2 ~ NPU_MAX_QUEUE_ENTRIES) */
#define NPU_AQA_ASQS_SHIFT      0               /* [11:0]  admin SQ 엔트리 수 */
#define NPU_AQA_ACQS_SHIFT      16              /* [27:16] admin CQ 엔트리 수 */
#define NPU_AQA_SIZE_MASK       0xfffu

/*
 * ---- 큐 ----
 * qid 0 = admin, qid 1 ~ CAP.NQ = job 큐. SQ n의 완료는 항상 CQ n으로 가고,
 * CQ n의 인터럽트는 MSI-X 벡터 n이다. 큐 메모리(ring)는 4KiB 정렬이어야 한다.
 */
#define NPU_ADMIN_QID           0
#define NPU_MAX_QUEUES          8               /* job 큐 최대 수 (admin 제외) */
#define NPU_NUM_QIDS            (1 + NPU_MAX_QUEUES)
#define NPU_MIN_QUEUE_ENTRIES   2
#define NPU_MAX_QUEUE_ENTRIES   1024
#define NPU_MSIX_VECTORS        NPU_NUM_QIDS

/* doorbell: 큐마다 4KiB 페이지 하나. 잘못된 값(없는 큐, 크기 이상)은 무시된다 */
#define NPU_DB_BASE             0x1000
#define NPU_DB_STRIDE           0x1000
#define NPU_DB_OFFSET(qid)      (NPU_DB_BASE + (qid) * NPU_DB_STRIDE)
#define NPU_DB_SQ_TAIL          0x0             /* WO: 드라이버가 다음에 쓸 SQ 칸 */
#define NPU_DB_CQ_HEAD          0x4             /* WO: 드라이버가 다음에 읽을 CQ 칸 */

struct npu_sqe {
    NPU_U8      opcode;     /* NPU_ADMIN_OP_* 또는 NPU_JOB_OP_* */
    NPU_U8      flags;      /* 예약, 0 */
    NPU_U16     cid;        /* 드라이버가 정한 명령 ID. CQE에 그대로 돌려준다 */
    NPU_U32     cdw[15];    /* 명령별 필드 */
};

struct npu_cqe {
    NPU_U32     result;     /* 명령별 결과 */
    NPU_U32     dw1;        /* 명령별 결과 (64비트 값의 상위) */
    NPU_U16     sq_head;    /* 장치가 SQ를 어디까지 읽었나 */
    NPU_U16     sq_id;
    NPU_U16     cid;
    NPU_U16     status;     /* bit 0 = phase, bit 15:1 = NPU_SC_* */
};

#define NPU_CQE_PHASE           0x1
#define NPU_CQE_SC_SHIFT        1

/* 상태 코드 */
#define NPU_SC_SUCCESS          0x00
#define NPU_SC_INVALID_OPCODE   0x01    /* 모르는 opcode, 큐 종류에 맞지 않는 opcode */
#define NPU_SC_INVALID_FIELD    0x02    /* 범위 밖의 값, 정렬 오류, 0이 아닌 예약 필드 */
#define NPU_SC_INVALID_QID      0x03    /* qid가 0이거나 CAP.NQ보다 큼 */
#define NPU_SC_INVALID_STATE    0x04    /* FW가 READY가 아님, 큐가 이미 있음/없음, 순서 오류 */
#define NPU_SC_FW_INVALID       0x05    /* 펌웨어 헤더 또는 CRC 오류 */
#define NPU_SC_DMA_ERROR        0x06    /* 게스트 메모리 접근 실패 */
#define NPU_SC_JOB_INVALID_HDR  0x20    /* job 헤더 magic/개수/크기 오류 */
#define NPU_SC_JOB_INVALID_CMD  0x21    /* 모르는 job 명령, 잘못된 flags/인자. result = 명령 번호 */
#define NPU_SC_JOB_OUT_OF_RANGE 0x22    /* 장치 메모리/SG 범위 밖, 금지된 겹침. result = 명령 번호 */

/*
 * ---- admin 명령 (qid 0, opcode 0x00 ~ 0x7F) ----
 * FW가 READY가 되기 전에는 GET_PARAM과 FW_LOAD만 받는다.
 *
 * GET_PARAM   cdw0 = NPU_PARAM_*                      result/dw1 = 값 하위/상위
 * FW_LOAD     cdw0/1 = 이미지 주소 LO/HI, cdw2 = 크기    result = 펌웨어 버전
 * CREATE_CQ   cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소 LO/HI, cdw4 = NPU_CQ_F_*
 * CREATE_SQ   cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소 LO/HI
 *
 * CQ를 먼저 만든다. 큐를 지우는 명령은 없다. 리셋(EN 1->0)이 모든 큐를 없앤다.
 */
#define NPU_ADMIN_OP_GET_PARAM  0x01
#define NPU_ADMIN_OP_FW_LOAD    0x02
#define NPU_ADMIN_OP_CREATE_CQ  0x03
#define NPU_ADMIN_OP_CREATE_SQ  0x04

#define NPU_PARAM_FW_VERSION    1       /* 펌웨어가 없으면 0 */
#define NPU_PARAM_DEVMEM_SIZE   2       /* 바이트 단위, 64비트 */

#define NPU_CQ_F_IEN            (1u << 0)       /* 0이면 CQE만 쓰고 인터럽트는 보내지 않는다 */

/*
 * ---- 펌웨어 이미지 ----
 * 이미지 = 헤더 + payload. 한 덩어리의 연속된 DMA 버퍼로 넘긴다.
 */
#define NPU_FW_MAGIC            0x5746504e      /* "NPFW" */
#define NPU_FW_MAX_SIZE         0x100000        /* 1MiB, 헤더 포함 */

struct npu_fw_hdr {
    NPU_U32     magic;          /* NPU_FW_MAGIC */
    NPU_U32     fw_version;     /* major << 16 | minor */
    NPU_U32     hw_major;       /* 필요한 장치 major 버전. NPU_REG_VERSION의 major와 같아야 한다 */
    NPU_U32     image_size;     /* 헤더를 포함한 전체 크기. FW_LOAD의 cdw2와 같아야 한다 */
    NPU_U32     payload_crc32;  /* 헤더 뒤 payload의 CRC-32 (IEEE 802.3, zlib crc32) */
    NPU_U32     rsvd[3];
};

/*
 * ---- job 큐 명령 (qid 1 이상, opcode 0x80 ~ 0xFF) ----
 *
 * SUBMIT_JOB  cdw0/1 = job 버퍼 주소 LO/HI, cdw2 = job 버퍼 크기
 *             실패하면 CQE result = 실패한 명령 번호
 *
 * job 버퍼(한 덩어리의 연속된 DMA 버퍼):
 *   struct npu_job_hdr
 *   struct npu_job_cmd  cmds[num_cmds]
 *   struct npu_sg       sg[num_sg]
 * 크기는 정확히 sizeof(hdr) + 32 * num_cmds + 16 * num_sg 여야 한다.
 */
#define NPU_JOB_OP_SUBMIT       0x80

#define NPU_JOB_MAGIC           0x424f4a4e      /* "NJOB" */
#define NPU_JOB_MAX_SIZE        0x10000         /* 64KiB */

struct npu_job_hdr {
    NPU_U32     magic;          /* NPU_JOB_MAGIC */
    NPU_U16     num_cmds;       /* 1 이상 */
    NPU_U16     num_sg;         /* DMA 명령이 없으면 0 */
    NPU_U32     rsvd[2];
};

/* 호스트 메모리 조각. DMA 명령이 sg[] 안의 구간(sg_idx, sg_cnt)으로 가리킨다 */
struct npu_sg {
    NPU_U64     addr;           /* DMA 주소 */
    NPU_U32     len;            /* 바이트 수, 1 이상 */
    NPU_U32     rsvd;
};

/* job 명령: 모두 32바이트. *_off는 장치 메모리 안의 바이트 오프셋 */
#define NPU_CMD_NOP             0x01
#define NPU_CMD_DMA_H2D         0x02    /* 호스트 메모리 -> 장치 메모리 */
#define NPU_CMD_DMA_D2H         0x03    /* 장치 메모리 -> 호스트 메모리 */
#define NPU_CMD_GEMM            0x10    /* C(int32) = A(int8) x B(int8) [+ bias(int32)] */
#define NPU_CMD_RELU            0x11    /* dst[i] = max(src[i], 0) */
#define NPU_CMD_REQUANT         0x12    /* int32 -> int8 재양자화 */

struct npu_job_cmd {            /* 공통 모양. NOP은 arg가 모두 0 */
    NPU_U16     opcode;
    NPU_U16     flags;
    NPU_U32     arg[7];
};

struct npu_cmd_dma {            /* 전송 길이 = 가리킨 SG 조각 길이의 합 */
    NPU_U16     opcode;
    NPU_U16     flags;
    NPU_U32     sg_idx;
    NPU_U32     sg_cnt;         /* 1 이상 */
    NPU_U32     dev_off;
    NPU_U32     rsvd[4];
};

#define NPU_GEMM_F_BIAS         (1u << 0)       /* 0이면 bias_off는 0이어야 한다 */
#define NPU_GEMM_MAX_MACS       (1u << 30)      /* m * k * n 상한 */

struct npu_cmd_gemm {           /* 행 우선(row-major). int32 배열 오프셋은 4의 배수 */
    NPU_U16     opcode;
    NPU_U16     flags;          /* NPU_GEMM_F_* */
    NPU_U32     a_off;          /* int8  A[m][k] */
    NPU_U32     b_off;          /* int8  B[k][n] */
    NPU_U32     bias_off;       /* int32 bias[n]: 모든 행에 더한다 */
    NPU_U32     c_off;          /* int32 C[m][n]: A, B, bias와 겹치면 안 된다 */
    NPU_U32     m;
    NPU_U32     k;
    NPU_U32     n;
};

#define NPU_RELU_F_I32          (1u << 0)       /* 원소가 int32 (0이면 int8) */

struct npu_cmd_relu {           /* dst_off == src_off(제자리)는 허용, 그 밖의 겹침은 금지 */
    NPU_U16     opcode;
    NPU_U16     flags;          /* NPU_RELU_F_* */
    NPU_U32     src_off;
    NPU_U32     dst_off;
    NPU_U32     count;          /* 원소 수, 1 이상 */
    NPU_U32     rsvd[4];
};

#define NPU_REQUANT_MAX_SHIFT   31

/*
 * t = (int64)src[i] * mult
 * if (shift) t = (t + (1 << (shift - 1))) >> shift     (산술 시프트)
 * dst[i] = clamp(t + zp, -128, 127)
 */
struct npu_cmd_requant {        /* dst_off == src_off(제자리)는 허용, 그 밖의 겹침은 금지 */
    NPU_U16     opcode;
    NPU_U16     flags;
    NPU_U32     src_off;        /* int32 src[count] */
    NPU_U32     dst_off;        /* int8  dst[count] */
    NPU_U32     count;          /* 원소 수, 1 이상 */
    NPU_U32     mult;           /* int32 */
    NPU_U32     shift;          /* 0 ~ NPU_REQUANT_MAX_SHIFT */
    NPU_U32     zp;             /* int32 zero point */
    NPU_U32     rsvd;
};

_Static_assert(sizeof(struct npu_sqe) == 64, "sqe must be 64 bytes");
_Static_assert(sizeof(struct npu_cqe) == 16, "cqe must be 16 bytes");
_Static_assert(sizeof(struct npu_fw_hdr) == 32, "fw header must be 32 bytes");
_Static_assert(sizeof(struct npu_job_hdr) == 16, "job header must be 16 bytes");
_Static_assert(sizeof(struct npu_sg) == 16, "sg entry must be 16 bytes");
_Static_assert(sizeof(struct npu_job_cmd) == 32, "job command must be 32 bytes");
_Static_assert(sizeof(struct npu_cmd_dma) == 32, "dma command must be 32 bytes");
_Static_assert(sizeof(struct npu_cmd_gemm) == 32, "gemm command must be 32 bytes");
_Static_assert(sizeof(struct npu_cmd_relu) == 32, "relu command must be 32 bytes");
_Static_assert(sizeof(struct npu_cmd_requant) == 32, "requant command must be 32 bytes");
_Static_assert(NPU_DB_OFFSET(NPU_NUM_QIDS) <= NPU_BAR0_SIZE, "doorbells must fit in BAR0");
_Static_assert(NPU_MAX_QUEUES <= NPU_CAP_NQ_MASK, "queue count must fit in CAP.NQ");
_Static_assert(NPU_MAX_QUEUE_ENTRIES <= NPU_AQA_SIZE_MASK, "queue size must fit in AQA");

#endif /* NPU_HW_H */
