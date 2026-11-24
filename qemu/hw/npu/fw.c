/*
 * NPU: firmware image validation and READY handshake
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 계약은 DESIGN.md의 4절(상태 머신)과 7절(펌웨어 이미지).
 *
 * 실행되는 펌웨어 코드는 없다. 헤더와 CRC만 본다. 목적은 드라이버의
 * request_firmware() -> 업로드 -> READY 확인 흐름을 만드는 것이다.
 * 검사는 FW_LOAD 명령 한 번 안에서 끝나므로 게스트는 LOADING을 볼 수 없다.
 */
#include "qemu/osdep.h"
#include <zlib.h>   /* crc32() */
#include "qemu/bswap.h"
#include "npu.h"
#include "trace.h"

static void npu_fw_set_sts(NpuState *n, uint32_t sts)
{
    n->fw_sts = sts;
    trace_npu_fw_sts(sts);
}

/* 헤더가 맞으면 true. hdr의 값은 little-endian 그대로다 */
static bool npu_fw_hdr_valid(const struct npu_fw_hdr *hdr, uint32_t size)
{
    unsigned i;

    if (le32_to_cpu(hdr->magic) != NPU_FW_MAGIC ||
        le32_to_cpu(hdr->hw_major) != NPU_VERSION_MAJOR(NPU_VERSION_VALUE) ||
        le32_to_cpu(hdr->image_size) != size) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(hdr->rsvd); i++) {
        if (hdr->rsvd[i]) {
            return false;
        }
    }
    return true;
}

/* 헤더 뒤 payload의 CRC-32. payload가 0바이트여도 된다 */
static uint32_t npu_fw_payload_crc(const uint8_t *image, uint32_t size)
{
    return crc32(0, image + sizeof(struct npu_fw_hdr),
                 size - sizeof(struct npu_fw_hdr));
}

/*
 * FW_LOAD: cdw0/1 = 이미지 주소, cdw2 = 크기.
 *
 *   크기가 헤더보다 작거나 NPU_FW_MAX_SIZE 초과   INVALID_FIELD, FW_STS 그대로
 *   이미 READY                                   INVALID_STATE, FW_STS 그대로
 *   이미지를 읽지 못함                           DMA_ERROR,     FW_STS = ERROR
 *   헤더나 CRC가 틀림                            FW_INVALID,    FW_STS = ERROR
 *   성공                                        SUCCESS,       FW_STS = READY
 */
uint16_t npu_fw_load(NpuState *n, NpuRequest *req, const NpuSqe *sqe)
{
    uint64_t addr = le32_to_cpu(sqe->cdw[0]) |
                    ((uint64_t)le32_to_cpu(sqe->cdw[1]) << 32);
    uint32_t size = le32_to_cpu(sqe->cdw[2]);
    const struct npu_fw_hdr *hdr;
    g_autofree uint8_t *image = NULL;
    uint16_t sc;

    if (!npu_cdw_is_zero(sqe, 3) ||
        size < sizeof(*hdr) || size > NPU_FW_MAX_SIZE) {
        return NPU_SC_INVALID_FIELD;
    }
    if (n->fw_sts == NPU_FW_STS_LOADING || n->fw_sts == NPU_FW_STS_READY) {
        return NPU_SC_INVALID_STATE;
    }

    npu_fw_set_sts(n, NPU_FW_STS_LOADING);

    image = g_malloc(size);
    hdr = (const struct npu_fw_hdr *)image;

    if (pci_dma_read(PCI_DEVICE(n), addr, image, size) != MEMTX_OK) {
        sc = NPU_SC_DMA_ERROR;
    } else if (!npu_fw_hdr_valid(hdr, size) ||
               npu_fw_payload_crc(image, size) !=
               le32_to_cpu(hdr->payload_crc32)) {
        sc = NPU_SC_FW_INVALID;
    } else {
        sc = NPU_SC_SUCCESS;
    }

    if (sc != NPU_SC_SUCCESS) {
        npu_fw_set_sts(n, NPU_FW_STS_ERROR);
        return sc;
    }

    n->fw_version = le32_to_cpu(hdr->fw_version);
    req->result = n->fw_version;
    npu_fw_set_sts(n, NPU_FW_STS_READY);
    return NPU_SC_SUCCESS;
}

/* 펌웨어가 READY면 true. READY 전에는 GET_PARAM, FW_LOAD 외의 admin 명령을 받지 않는다 */
bool npu_fw_ready(NpuState *n)
{
    return n->fw_sts == NPU_FW_STS_READY;
}

/* EN 1->0, FLR, 시스템 리셋: 펌웨어가 사라진다 */
void npu_fw_reset(NpuState *n)
{
    n->fw_sts = NPU_FW_STS_NONE;
    n->fw_version = 0;
}
