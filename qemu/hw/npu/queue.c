/*
 * NPU: doorbells, queues, admin commands, completions and interrupts
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 계약은 DESIGN.md의 3.2절(doorbell), 5절(큐), 6절(admin 명령).
 *
 * 흐름
 *   게스트가 doorbell을 씀 -> npu_queue_doorbell() -> sq->bh 또는 cq->bh 예약
 *   npu_sq_process()    SQE를 꺼내 요청(NpuRequest)을 만들고 admin/job 처리로 넘긴다
 *   npu_req_complete()  실행이 끝난 요청을 cq->done_list에 넣고 cq->bh 예약
 *   npu_cq_post()       CQ에 자리가 있는 만큼 CQE를 쓰고 인터럽트를 올린다
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/pci/msix.h"
#include "npu.h"
#include "trace.h"

/* ---- ring 상태 ---- */

/* 장치가 아직 꺼내지 않은 SQE가 없으면 true */
static bool npu_sq_empty(NpuSQueue *sq)
{
    return sq->head == sq->tail;
}

/* CQE를 쓸 칸이 없으면 true */
static bool npu_cq_full(NpuCQueue *cq)
{
    return (cq->tail + 1) % cq->size == cq->head;
}

/*
 * 큐 설정이 맞는지: 엔트리 수 2 ~ NPU_MAX_QUEUE_ENTRIES, 주소는 4KiB 정렬.
 * admin 큐(EN 0->1)와 CREATE_SQ/CQ가 같이 쓴다.
 */
static bool npu_queue_params_valid(uint32_t entries, uint64_t dma_addr)
{
    if (entries < NPU_MIN_QUEUE_ENTRIES || entries > NPU_MAX_QUEUE_ENTRIES) {
        return false;
    }
    return QEMU_IS_ALIGNED(dma_addr, NPU_PAGE_SIZE);
}

/* cdw[first..14]가 모두 0이면 true. 명령이 쓰지 않는 칸은 0이어야 한다 */
bool npu_cdw_is_zero(const NpuSqe *sqe, unsigned first)
{
    unsigned i;

    for (i = first; i < ARRAY_SIZE(sqe->cdw); i++) {
        if (sqe->cdw[i]) {
            return false;
        }
    }
    return true;
}

/* ---- 완료와 인터럽트 ---- */

/* CQ의 IEN이 켜져 있고 MSI-X가 켜져 있으면 벡터(= cqid)를 올린다 */
static void npu_cq_irq(NpuCQueue *cq)
{
    PCIDevice *pdev = PCI_DEVICE(cq->state);

    if (!cq->irq_enabled || !msix_enabled(pdev)) {
        return;
    }
    trace_npu_cq_irq(cq->cqid);
    msix_notify(pdev, cq->cqid);
}

/*
 * 요청 하나를 CQE로 만들어 CQ의 tail 칸에 쓰고 tail과 phase를 갱신한다.
 * 게스트 메모리에 쓰지 못하면 false (부른 쪽이 npu_ctrl_fatal()).
 */
static bool npu_cqe_write(NpuCQueue *cq, NpuRequest *req)
{
    PCIDevice *pdev = PCI_DEVICE(cq->state);
    hwaddr addr = cq->dma_addr + (hwaddr)cq->tail * sizeof(NpuCqe);
    NpuCqe cqe = {
        .result  = cpu_to_le32(req->result),
        .dw1     = cpu_to_le32(req->dw1),
        .sq_head = cpu_to_le16(req->sq->head),
        .sq_id   = cpu_to_le16(req->sq->sqid),
        .cid     = cpu_to_le16(req->cid),
        .status  = cpu_to_le16((req->status << NPU_CQE_SC_SHIFT) | cq->phase),
    };

    if (pci_dma_write(pdev, addr, &cqe, sizeof(cqe)) != MEMTX_OK) {
        return false;
    }
    trace_npu_cqe(cq->cqid, cq->tail, req->cid, req->status, req->result);

    /* 쓰기가 성공한 뒤에만 전진한다. 마지막 칸을 썼으면 처음으로 돌아가고 phase를 뒤집는다 */
    cq->tail++;
    if (cq->tail == cq->size) {
        cq->tail = 0;
        cq->phase = !cq->phase;
    }
    return true;
}

/*
 * cq->bh. done_list의 요청을 CQ에 자리가 있는 만큼 CQE로 쓰고, 쓴 요청은 free_list로 돌려놓는다.
 * 하나라도 썼으면 인터럽트를 올린다.
 */
static void npu_cq_post(void *opaque)
{
    NpuCQueue *cq = opaque;
    NpuState *n = cq->state;
    NpuRequest *req, *next;
    bool posted = false;

    /* 리셋됐거나 CFS면 게스트 메모리를 건드리지 않는다 */
    if (!npu_dma_allowed(n)) {
        return;
    }

    QTAILQ_FOREACH_SAFE(req, &cq->done_list, entry, next) {
        NpuSQueue *sq = req->sq;

        if (npu_cq_full(cq)) {
            break;      /* 남은 요청은 CQ head doorbell이 오면 다시 시도한다 */
        }
        if (!npu_cqe_write(cq, req)) {
            npu_ctrl_fatal(n);
            return;
        }
        QTAILQ_REMOVE(&cq->done_list, req, entry);
        QTAILQ_INSERT_TAIL(&sq->free_list, req, entry);
        posted = true;

        /* 빈 요청이 생겼으니 이 SQ가 다음 SQE를 꺼낼 수 있다 */
        qemu_bh_schedule(sq->bh);
    }

    /* CQE를 여러 개 썼어도 인터럽트는 한 번이면 된다 */
    if (posted) {
        npu_cq_irq(cq);
    }
}

/*
 * 요청의 실행이 끝났다. 상태 코드를 기록하고 sq->cur를 비운 뒤 cq->done_list에 넣고
 * cq->bh를 예약한다. 같은 SQ의 다음 명령을 꺼내도록 sq->bh도 예약한다.
 * result와 dw1은 부르기 전에 req에 채워 둔다.
 */
void npu_req_complete(NpuRequest *req, uint16_t sc)
{
    NpuSQueue *sq = req->sq;
    NpuState *n = sq->state;
    NpuCQueue *cq = n->cq[sq->sqid];

    assert(sq->cur == req);
    assert(sc != NPU_SC_NO_COMPLETE);

    req->status = sc;
    sq->cur = NULL;

    if (sq->sqid != NPU_ADMIN_QID && n->inject_cqe_drop > 0) {
        /* 장애 주입: 실행은 했지만 CQE를 보내지 않는다. 드라이버는 timeout으로 알아챈다 */
        n->inject_cqe_drop--;
        trace_npu_cqe_drop(sq->sqid, req->cid);
        QTAILQ_INSERT_TAIL(&sq->free_list, req, entry);
    } else {
        QTAILQ_INSERT_TAIL(&cq->done_list, req, entry);
        qemu_bh_schedule(cq->bh);
    }
    qemu_bh_schedule(sq->bh);
}

/* ---- 큐 생성과 해제 ---- */

static void npu_sq_process(void *opaque);

/* CQ를 만들어 n->cq[cqid]에 등록한다. phase는 1에서 시작한다 */
static void npu_cq_create(NpuState *n, uint16_t cqid, uint64_t dma_addr,
                          uint32_t size, bool irq_enabled)
{
    NpuCQueue *cq = g_new0(NpuCQueue, 1);

    cq->state = n;
    cq->cqid = cqid;
    cq->phase = 1;
    cq->irq_enabled = irq_enabled;
    cq->size = size;
    cq->dma_addr = dma_addr;
    QTAILQ_INIT(&cq->done_list);
    cq->bh = qemu_bh_new_guarded(npu_cq_post, cq,
                                 &DEVICE(n)->mem_reentrancy_guard);
    n->cq[cqid] = cq;
    trace_npu_queue_create("cq", cqid, dma_addr, size);
}

/* SQ를 만들어 n->sq[sqid]에 등록한다. 요청은 큐 크기만큼 미리 잡는다 */
static void npu_sq_create(NpuState *n, uint16_t sqid, uint64_t dma_addr,
                          uint32_t size)
{
    NpuSQueue *sq = g_new0(NpuSQueue, 1);
    uint32_t i;

    sq->state = n;
    sq->sqid = sqid;
    sq->size = size;
    sq->dma_addr = dma_addr;
    sq->reqs = g_new0(NpuRequest, size);
    QTAILQ_INIT(&sq->free_list);
    for (i = 0; i < size; i++) {
        sq->reqs[i].sq = sq;
        QTAILQ_INSERT_TAIL(&sq->free_list, &sq->reqs[i], entry);
    }
    sq->bh = qemu_bh_new_guarded(npu_sq_process, sq,
                                 &DEVICE(n)->mem_reentrancy_guard);
    n->sq[sqid] = sq;
    trace_npu_queue_create("sq", sqid, dma_addr, size);
}

/* 실행 중인 job을 멈추고 SQ를 없앤다. 예약된 bh도 취소된다 */
static void npu_sq_free(NpuSQueue *sq)
{
    if (sq->cur) {
        npu_job_cancel(sq->cur);
    }
    sq->state->sq[sq->sqid] = NULL;
    qemu_bh_delete(sq->bh);
    g_free(sq->reqs);
    g_free(sq);
}

/* CQ를 없앤다. done_list의 요청은 SQ의 것이므로 여기서 해제하지 않는다 */
static void npu_cq_free(NpuCQueue *cq)
{
    cq->state->cq[cq->cqid] = NULL;
    qemu_bh_delete(cq->bh);
    g_free(cq);
}

/* ---- admin 명령 (6절) ---- */

/*
 * 처리 함수는 NPU_SC_*를 돌려주고, CQE에 실을 값은 req->result, req->dw1에 채운다.
 */

static uint64_t npu_sqe_addr(const NpuSqe *sqe, unsigned lo)
{
    return le32_to_cpu(sqe->cdw[lo]) |
           ((uint64_t)le32_to_cpu(sqe->cdw[lo + 1]) << 32);
}

/* GET_PARAM: cdw0 = NPU_PARAM_*. 값의 하위/상위 32비트를 result/dw1에 */
static uint16_t npu_admin_get_param(NpuState *n, NpuRequest *req,
                                    const NpuSqe *sqe)
{
    uint64_t val;

    if (!npu_cdw_is_zero(sqe, 1)) {
        return NPU_SC_INVALID_FIELD;
    }
    switch (le32_to_cpu(sqe->cdw[0])) {
    case NPU_PARAM_FW_VERSION:
        val = n->fw_version;
        break;
    case NPU_PARAM_DEVMEM_SIZE:
        val = n->devmem_size;
        break;
    default:
        return NPU_SC_INVALID_FIELD;
    }
    req->result = val;
    req->dw1 = val >> 32;
    return NPU_SC_SUCCESS;
}

/* CREATE_CQ와 CREATE_SQ가 같이 쓰는 검사. 통과하면 qid, 엔트리 수, 주소를 돌려준다 */
static uint16_t npu_admin_queue_args(NpuState *n, const NpuSqe *sqe,
                                     unsigned first_unused, uint32_t *qid,
                                     uint32_t *entries, uint64_t *addr)
{
    if (!npu_fw_ready(n)) {
        return NPU_SC_INVALID_STATE;
    }
    if (!npu_cdw_is_zero(sqe, first_unused)) {
        return NPU_SC_INVALID_FIELD;
    }
    *qid = le32_to_cpu(sqe->cdw[0]);
    *entries = le32_to_cpu(sqe->cdw[1]);
    *addr = npu_sqe_addr(sqe, 2);

    if (*qid == NPU_ADMIN_QID || *qid > n->num_queues) {
        return NPU_SC_INVALID_QID;
    }
    if (!npu_queue_params_valid(*entries, *addr)) {
        return NPU_SC_INVALID_FIELD;
    }
    return NPU_SC_SUCCESS;
}

/* CREATE_CQ: cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소, cdw4 = NPU_CQ_F_* */
static uint16_t npu_admin_create_cq(NpuState *n, const NpuSqe *sqe)
{
    uint32_t qid, entries, flags;
    uint64_t addr;
    uint16_t sc;

    sc = npu_admin_queue_args(n, sqe, 5, &qid, &entries, &addr);
    if (sc != NPU_SC_SUCCESS) {
        return sc;
    }
    flags = le32_to_cpu(sqe->cdw[4]);
    if (flags & ~NPU_CQ_F_IEN) {
        return NPU_SC_INVALID_FIELD;
    }
    if (n->cq[qid]) {
        return NPU_SC_INVALID_STATE;
    }
    npu_cq_create(n, qid, addr, entries, flags & NPU_CQ_F_IEN);
    return NPU_SC_SUCCESS;
}

/* CREATE_SQ: cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소. 같은 번호의 CQ가 먼저 있어야 한다 */
static uint16_t npu_admin_create_sq(NpuState *n, const NpuSqe *sqe)
{
    uint32_t qid, entries;
    uint64_t addr;
    uint16_t sc;

    sc = npu_admin_queue_args(n, sqe, 4, &qid, &entries, &addr);
    if (sc != NPU_SC_SUCCESS) {
        return sc;
    }
    if (!n->cq[qid] || n->sq[qid]) {
        return NPU_SC_INVALID_STATE;
    }
    npu_sq_create(n, qid, addr, entries);
    return NPU_SC_SUCCESS;
}

static uint16_t npu_admin_cmd(NpuState *n, NpuRequest *req, const NpuSqe *sqe)
{
    switch (sqe->opcode) {
    case NPU_ADMIN_OP_GET_PARAM:
        return npu_admin_get_param(n, req, sqe);
    case NPU_ADMIN_OP_FW_LOAD:
        return npu_fw_load(n, req, sqe);
    case NPU_ADMIN_OP_CREATE_CQ:
        return npu_admin_create_cq(n, sqe);
    case NPU_ADMIN_OP_CREATE_SQ:
        return npu_admin_create_sq(n, sqe);
    default:
        return NPU_SC_INVALID_OPCODE;
    }
}

/* ---- SQ 처리 ---- */

/*
 * SQ의 head 칸에서 SQE 하나를 읽어 오고 head를 올린다.
 * 게스트 메모리를 읽지 못하면 false (부른 쪽이 npu_ctrl_fatal()).
 */
static bool npu_sq_fetch(NpuSQueue *sq, NpuSqe *sqe)
{
    PCIDevice *pdev = PCI_DEVICE(sq->state);
    hwaddr addr = sq->dma_addr + (hwaddr)sq->head * sizeof(NpuSqe);

    if (pci_dma_read(pdev, addr, sqe, sizeof(*sqe)) != MEMTX_OK) {
        return false;
    }
    sq->head = (sq->head + 1) % sq->size;
    return true;
}

/* SQE 하나를 큐 종류에 맞는 처리 함수로 넘긴다 */
static uint16_t npu_sq_dispatch(NpuSQueue *sq, NpuRequest *req,
                                const NpuSqe *sqe)
{
    NpuState *n = sq->state;

    if (sqe->flags) {
        return NPU_SC_INVALID_FIELD;
    }
    if (sq->sqid == NPU_ADMIN_QID) {
        return npu_admin_cmd(n, req, sqe);
    }
    if (sqe->opcode == NPU_JOB_OP_SUBMIT) {
        return npu_job_submit(n, req, sqe);
    }
    return NPU_SC_INVALID_OPCODE;
}

/*
 * sq->bh. 꺼낼 수 있는 동안 SQE를 꺼내 요청을 만들고 처리한다. 꺼낼 수 있는 조건은
 * DMA 허용, SQ가 비지 않음, 실행 중인 요청 없음(한 번에 하나), 빈 요청 있음.
 * 늦게 끝나는 명령(job)은 NPU_SC_NO_COMPLETE를 돌려주고, 끝나면 npu_req_complete()가
 * 이 bh를 다시 예약한다.
 */
static void npu_sq_process(void *opaque)
{
    NpuSQueue *sq = opaque;
    NpuState *n = sq->state;
    NpuRequest *req;
    NpuSqe sqe;
    uint16_t sc;

    while (npu_dma_allowed(n) && !npu_sq_empty(sq) && !sq->cur &&
           !QTAILQ_EMPTY(&sq->free_list)) {
        if (!npu_sq_fetch(sq, &sqe)) {
            npu_ctrl_fatal(n);
            return;
        }

        req = QTAILQ_FIRST(&sq->free_list);
        QTAILQ_REMOVE(&sq->free_list, req, entry);
        req->cid = le16_to_cpu(sqe.cid);
        req->status = NPU_SC_SUCCESS;
        req->result = 0;
        req->dw1 = 0;
        req->job = NULL;
        sq->cur = req;
        trace_npu_sqe(sq->sqid, req->cid, sqe.opcode);

        sc = npu_sq_dispatch(sq, req, &sqe);
        if (sc != NPU_SC_NO_COMPLETE) {
            npu_req_complete(req, sc);
        }
    }
}

/* ---- doorbell (3.2절) ---- */

/*
 * BAR0의 doorbell 영역(addr >= NPU_DB_BASE)에 쓰기가 왔다.
 * 없는 큐, 예약 페이지, 정의되지 않은 오프셋, 큐 크기 이상의 값은 무시한다.
 * RDY=0이거나 CFS=1일 때도 무시한다.
 */
void npu_queue_doorbell(NpuState *n, hwaddr addr, uint32_t val)
{
    uint32_t qid = (addr - NPU_DB_BASE) / NPU_DB_STRIDE;
    uint32_t off = addr % NPU_DB_STRIDE;
    NpuSQueue *sq;
    NpuCQueue *cq;

    if (!npu_dma_allowed(n)) {
        return;
    }
    if (qid >= NPU_NUM_QIDS) {
        return;     /* 예약 페이지 */
    }

    switch (off) {
    case NPU_DB_SQ_TAIL:
        sq = n->sq[qid];
        if (!sq || val >= sq->size) {
            break;
        }
        trace_npu_doorbell("sq tail", qid, val);
        sq->tail = val;
        qemu_bh_schedule(sq->bh);
        return;
    case NPU_DB_CQ_HEAD:
        cq = n->cq[qid];
        if (!cq || val >= cq->size) {
            break;
        }
        trace_npu_doorbell("cq head", qid, val);
        cq->head = val;
        /* 자리가 생겼다. 밀린 완료가 있으면 이어서 쓴다 */
        if (!QTAILQ_EMPTY(&cq->done_list)) {
            qemu_bh_schedule(cq->bh);
        }
        return;
    default:
        return;     /* doorbell 페이지의 나머지 오프셋 */
    }

    qemu_log_mask(LOG_GUEST_ERROR, "npu: doorbell write 0x%x to queue %u "
                  "offset 0x%x ignored (no such queue or value too large)\n",
                  val, qid, off);
}

/* ---- 시작과 정지 (4.1절, 4.3절) ---- */

/*
 * EN 0->1. n->aqa, n->asq, n->acq를 검사하고 admin SQ/CQ를 만든다.
 * 설정이 틀리면 아무것도 만들지 않고 false (부른 쪽이 RDY 대신 CFS를 세운다).
 */
bool npu_queue_start(NpuState *n)
{
    uint32_t asqs = (n->aqa >> NPU_AQA_ASQS_SHIFT) & NPU_AQA_SIZE_MASK;
    uint32_t acqs = (n->aqa >> NPU_AQA_ACQS_SHIFT) & NPU_AQA_SIZE_MASK;

    if (!npu_queue_params_valid(asqs, n->asq) ||
        !npu_queue_params_valid(acqs, n->acq)) {
        return false;
    }
    npu_cq_create(n, NPU_ADMIN_QID, n->acq, acqs, true);
    npu_sq_create(n, NPU_ADMIN_QID, n->asq, asqs);
    return true;
}

/*
 * EN 1->0, FLR, 시스템 리셋, exit. 실행 중인 job을 멈추고 admin 큐를 포함한 모든 SQ/CQ를 없앤다.
 * CQE는 보내지 않는다. 돌아온 뒤에는 예약된 bh도 타이머도 게스트 메모리 접근도 없다.
 * 큐가 하나도 없는 상태에서 불려도 된다.
 */
void npu_queue_stop(NpuState *n)
{
    unsigned qid;

    for (qid = 0; qid < NPU_NUM_QIDS; qid++) {
        if (n->sq[qid]) {
            npu_sq_free(n->sq[qid]);
        }
    }
    for (qid = 0; qid < NPU_NUM_QIDS; qid++) {
        if (n->cq[qid]) {
            npu_cq_free(n->cq[qid]);
        }
    }
}
