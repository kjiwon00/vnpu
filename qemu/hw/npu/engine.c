/*
 * NPU: job execution: validation, DMA, GEMM, elementwise ops, latency model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 계약은 DESIGN.md의 8절(job)과 10절(지연 모델).
 *
 * 흐름
 *   npu_job_submit()    job 버퍼를 복사 -> 전체 검증 -> 실행 -> 지연 타이머 예약
 *   npu_job_done()      타이머가 만료되면 npu_req_complete()로 CQE를 예약
 *   npu_job_cancel()    리셋이 실행 중인 job을 멈춘다
 *
 * 연산은 제출 시점에 BQL 안에서 바로 끝낸다. 걸리는 "시간"은 타이머로 흉내 낸다.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "npu.h"
#include "trace.h"

/*
 * 게스트에서 복사해 온 job 하나. 복사한 뒤에는 게스트 메모리의 job 버퍼를 다시 읽지 않으므로
 * 검증한 뒤에 게스트가 내용을 바꿔도 영향이 없다. buf 안의 값은 little-endian 그대로다.
 */
struct NpuJob {
    NpuState    *state;
    NpuRequest  *req;

    uint8_t     *buf;       /* job 버퍼 복사본 */
    uint32_t    num_cmds;
    uint32_t    num_sg;
    const struct npu_job_cmd *cmds;     /* buf 안을 가리킨다 */
    const struct npu_sg *sg;            /* buf 안을 가리킨다 */

    uint64_t    macs;       /* 검증하면서 센 GEMM MAC 수. 지연 모델에 쓴다 */
    uint16_t    status;     /* 실행 결과 NPU_SC_* */
    uint32_t    bad_cmd;    /* 실패한 명령 번호 */
    QEMUTimer   *timer;     /* 만료되면 npu_job_done() */
};

/* ---- job 버퍼 (8.1절) ---- */

static void npu_job_free(NpuJob *job)
{
    if (job->timer) {
        timer_free(job->timer);
    }
    job->req->job = NULL;
    g_free(job->buf);
    g_free(job);
}

/*
 * 게스트 메모리에서 job 버퍼를 통째로 복사하고 헤더를 읽어 job의 필드를 채운다.
 * size는 부른 쪽이 헤더 크기 이상, NPU_JOB_MAX_SIZE 이하인지 확인했다.
 */
static uint16_t npu_job_read(NpuJob *job, uint64_t addr, uint32_t size)
{
    const struct npu_job_hdr *hdr;
    uint64_t expect;

    job->buf = g_malloc(size);
    if (pci_dma_read(PCI_DEVICE(job->state), addr, job->buf, size) !=
        MEMTX_OK) {
        return NPU_SC_DMA_ERROR;
    }

    hdr = (const struct npu_job_hdr *)job->buf;
    job->num_cmds = le16_to_cpu(hdr->num_cmds);
    job->num_sg = le16_to_cpu(hdr->num_sg);
    expect = sizeof(*hdr) +
             (uint64_t)job->num_cmds * sizeof(struct npu_job_cmd) +
             (uint64_t)job->num_sg * sizeof(struct npu_sg);

    if (le32_to_cpu(hdr->magic) != NPU_JOB_MAGIC || job->num_cmds == 0 ||
        hdr->rsvd[0] || hdr->rsvd[1] || expect != size) {
        return NPU_SC_JOB_INVALID_HDR;
    }

    job->cmds = (const struct npu_job_cmd *)(hdr + 1);
    job->sg = (const struct npu_sg *)(job->cmds + job->num_cmds);
    return NPU_SC_SUCCESS;
}

/* ---- 검증 도우미 ---- */

/* [off, off + len)이 장치 메모리 안에 있으면 true. 64비트로 계산해서 overflow가 없다 */
static bool npu_devmem_range_ok(NpuState *n, uint64_t off, uint64_t len)
{
    return off <= n->devmem_size && len <= n->devmem_size - off;
}

/* 두 구간이 한 바이트라도 겹치면 true */
static bool npu_ranges_overlap(uint64_t a_off, uint64_t a_len,
                               uint64_t b_off, uint64_t b_len)
{
    return a_off < b_off + b_len && b_off < a_off + a_len;
}

/*
 * sg[sg_idx .. sg_idx + sg_cnt) 조각 길이의 합을 *total에 넣는다.
 * 구간이 num_sg를 벗어나거나, 길이가 0인 조각이나 0이 아닌 예약 필드가 있으면 false.
 */
static bool npu_sg_total(const NpuJob *job, uint32_t sg_idx, uint32_t sg_cnt,
                         uint64_t *total)
{
    uint64_t sum = 0;
    uint32_t i;

    if (sg_cnt == 0 || sg_idx > job->num_sg ||
        sg_cnt > job->num_sg - sg_idx) {
        return false;
    }
    for (i = sg_idx; i < sg_idx + sg_cnt; i++) {
        if (job->sg[i].len == 0 || job->sg[i].rsvd) {
            return false;
        }
        sum += le32_to_cpu(job->sg[i].len);
    }
    *total = sum;
    return true;
}

/* ---- 명령별 검증 (8.2절) ---- */

/*
 * 돌려주는 값은 NPU_SC_SUCCESS, JOB_INVALID_CMD(flags, 예약 필드, 인자 값, 정렬),
 * JOB_OUT_OF_RANGE(장치 메모리나 SG 범위, 금지된 겹침) 중 하나.
 */

static uint16_t npu_check_nop(const struct npu_job_cmd *c)
{
    unsigned i;

    if (c->flags) {
        return NPU_SC_JOB_INVALID_CMD;
    }
    for (i = 0; i < ARRAY_SIZE(c->arg); i++) {
        if (c->arg[i]) {
            return NPU_SC_JOB_INVALID_CMD;
        }
    }
    return NPU_SC_SUCCESS;
}

static uint16_t npu_check_dma(const NpuJob *job, const struct npu_cmd_dma *c)
{
    uint64_t total;

    if (c->flags || c->rsvd[0] || c->rsvd[1] || c->rsvd[2] || c->rsvd[3]) {
        return NPU_SC_JOB_INVALID_CMD;
    }
    if (!npu_sg_total(job, le32_to_cpu(c->sg_idx), le32_to_cpu(c->sg_cnt),
                      &total) ||
        !npu_devmem_range_ok(job->state, le32_to_cpu(c->dev_off), total)) {
        return NPU_SC_JOB_OUT_OF_RANGE;
    }
    return NPU_SC_SUCCESS;
}

static uint16_t npu_check_gemm(const NpuJob *job, const struct npu_cmd_gemm *c,
                               uint64_t *macs)
{
    NpuState *n = job->state;
    uint16_t flags = le16_to_cpu(c->flags);
    bool has_bias = flags & NPU_GEMM_F_BIAS;
    uint64_t a_off = le32_to_cpu(c->a_off);
    uint64_t b_off = le32_to_cpu(c->b_off);
    uint64_t bias_off = le32_to_cpu(c->bias_off);
    uint64_t c_off = le32_to_cpu(c->c_off);
    uint64_t m = le32_to_cpu(c->m);
    uint64_t k = le32_to_cpu(c->k);
    uint64_t nn = le32_to_cpu(c->n);
    uint64_t a_len, b_len, bias_len, c_len;

    if ((flags & ~NPU_GEMM_F_BIAS) || m == 0 || k == 0 || nn == 0 ||
        (!has_bias && bias_off != 0) ||
        c_off % 4 || (has_bias && bias_off % 4)) {
        return NPU_SC_JOB_INVALID_CMD;
    }
    /* 각각 32비트라 두 개의 곱은 64비트에 들어간다. 단계별로 상한을 확인한다 */
    if (m * k > NPU_GEMM_MAX_MACS || m * k * nn > NPU_GEMM_MAX_MACS) {
        return NPU_SC_JOB_INVALID_CMD;
    }

    a_len = m * k;
    b_len = k * nn;
    c_len = m * nn * 4;
    bias_len = nn * 4;

    if (!npu_devmem_range_ok(n, a_off, a_len) ||
        !npu_devmem_range_ok(n, b_off, b_len) ||
        !npu_devmem_range_ok(n, c_off, c_len) ||
        (has_bias && !npu_devmem_range_ok(n, bias_off, bias_len))) {
        return NPU_SC_JOB_OUT_OF_RANGE;
    }
    if (npu_ranges_overlap(c_off, c_len, a_off, a_len) ||
        npu_ranges_overlap(c_off, c_len, b_off, b_len) ||
        (has_bias && npu_ranges_overlap(c_off, c_len, bias_off, bias_len))) {
        return NPU_SC_JOB_OUT_OF_RANGE;
    }

    *macs = m * k * nn;
    return NPU_SC_SUCCESS;
}

/* RELU와 REQUANT가 같이 쓰는 범위 검사: 제자리(같은 오프셋)는 되고 그 밖의 겹침은 안 된다 */
static uint16_t npu_check_src_dst(NpuState *n, uint64_t src_off,
                                  uint64_t src_len, uint64_t dst_off,
                                  uint64_t dst_len)
{
    if (!npu_devmem_range_ok(n, src_off, src_len) ||
        !npu_devmem_range_ok(n, dst_off, dst_len)) {
        return NPU_SC_JOB_OUT_OF_RANGE;
    }
    if (src_off != dst_off &&
        npu_ranges_overlap(src_off, src_len, dst_off, dst_len)) {
        return NPU_SC_JOB_OUT_OF_RANGE;
    }
    return NPU_SC_SUCCESS;
}

static uint16_t npu_check_relu(const NpuJob *job, const struct npu_cmd_relu *c)
{
    uint16_t flags = le16_to_cpu(c->flags);
    uint64_t esz = (flags & NPU_RELU_F_I32) ? 4 : 1;
    uint64_t src_off = le32_to_cpu(c->src_off);
    uint64_t dst_off = le32_to_cpu(c->dst_off);
    uint64_t count = le32_to_cpu(c->count);

    if ((flags & ~NPU_RELU_F_I32) || count == 0 ||
        c->rsvd[0] || c->rsvd[1] || c->rsvd[2] || c->rsvd[3] ||
        src_off % esz || dst_off % esz) {
        return NPU_SC_JOB_INVALID_CMD;
    }
    return npu_check_src_dst(job->state, src_off, count * esz,
                             dst_off, count * esz);
}

static uint16_t npu_check_requant(const NpuJob *job,
                                  const struct npu_cmd_requant *c)
{
    uint64_t src_off = le32_to_cpu(c->src_off);
    uint64_t dst_off = le32_to_cpu(c->dst_off);
    uint64_t count = le32_to_cpu(c->count);

    if (c->flags || c->rsvd || count == 0 || src_off % 4 ||
        le32_to_cpu(c->shift) > NPU_REQUANT_MAX_SHIFT) {
        return NPU_SC_JOB_INVALID_CMD;
    }
    return npu_check_src_dst(job->state, src_off, count * 4, dst_off, count);
}

/*
 * 실행 전에 모든 명령을 검증한다. 하나라도 틀리면 그 번호를 job->bad_cmd에 넣고 상태 코드를
 * 돌려준다. 통과하면 job->macs에 MAC 수의 합이 들어 있다.
 */
static uint16_t npu_job_validate(NpuJob *job)
{
    uint32_t i;

    job->macs = 0;
    for (i = 0; i < job->num_cmds; i++) {
        const struct npu_job_cmd *c = &job->cmds[i];
        uint64_t macs = 0;
        uint16_t sc;

        switch (le16_to_cpu(c->opcode)) {
        case NPU_CMD_NOP:
            sc = npu_check_nop(c);
            break;
        case NPU_CMD_DMA_H2D:
        case NPU_CMD_DMA_D2H:
            sc = npu_check_dma(job, (const struct npu_cmd_dma *)c);
            break;
        case NPU_CMD_GEMM:
            sc = npu_check_gemm(job, (const struct npu_cmd_gemm *)c, &macs);
            break;
        case NPU_CMD_RELU:
            sc = npu_check_relu(job, (const struct npu_cmd_relu *)c);
            break;
        case NPU_CMD_REQUANT:
            sc = npu_check_requant(job, (const struct npu_cmd_requant *)c);
            break;
        default:
            sc = NPU_SC_JOB_INVALID_CMD;
            break;
        }
        if (sc != NPU_SC_SUCCESS) {
            job->bad_cmd = i;
            return sc;
        }
        job->macs += macs;
    }
    return NPU_SC_SUCCESS;
}

/* ---- 연산 (8.2절) ---- */

/*
 * 검증을 통과한 명령만 받는다. 여기서는 범위를 다시 검사하지 않는다.
 * mem은 장치 메모리의 시작 주소다. 다중 바이트 값은 little-endian으로 읽고 쓴다.
 */

/* SG 조각을 순서대로 옮긴다. to_dev가 true면 호스트 -> 장치 */
static uint16_t npu_exec_dma(NpuJob *job, uint8_t *mem,
                             const struct npu_cmd_dma *c, bool to_dev)
{
    PCIDevice *pdev = PCI_DEVICE(job->state);
    uint32_t sg_idx = le32_to_cpu(c->sg_idx);
    uint32_t sg_cnt = le32_to_cpu(c->sg_cnt);
    uint8_t *p = mem + le32_to_cpu(c->dev_off);
    uint32_t i;

    for (i = sg_idx; i < sg_idx + sg_cnt; i++) {
        uint64_t addr = le64_to_cpu(job->sg[i].addr);
        uint32_t len = le32_to_cpu(job->sg[i].len);
        MemTxResult res;

        if (to_dev) {
            res = pci_dma_read(pdev, addr, p, len);
        } else {
            res = pci_dma_write(pdev, addr, p, len);
        }
        if (res != MEMTX_OK) {
            return NPU_SC_DMA_ERROR;
        }
        p += len;
    }
    return NPU_SC_SUCCESS;
}

/* C[i][j] = (bias[j]) + sum_p A[i][p] * B[p][j]. 덧셈은 32비트 wraparound */
static void npu_exec_gemm(uint8_t *mem, const struct npu_cmd_gemm *c)
{
    bool has_bias = le16_to_cpu(c->flags) & NPU_GEMM_F_BIAS;
    const int8_t *a = (const int8_t *)mem + le32_to_cpu(c->a_off);
    const int8_t *b = (const int8_t *)mem + le32_to_cpu(c->b_off);
    const uint8_t *bias = mem + le32_to_cpu(c->bias_off);
    uint8_t *out = mem + le32_to_cpu(c->c_off);
    uint32_t m = le32_to_cpu(c->m);
    uint32_t k = le32_to_cpu(c->k);
    uint32_t n = le32_to_cpu(c->n);
    uint32_t i, j, p;

    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++) {
            /* 부호 없는 덧셈이라 넘쳐도 정의된 동작(wraparound)이다 */
            uint32_t acc = has_bias ? ldl_le_p(bias + 4 * j) : 0;

            for (p = 0; p < k; p++) {
                acc += (uint32_t)((int32_t)a[(size_t)i * k + p] *
                                  (int32_t)b[(size_t)p * n + j]);
            }
            stl_le_p(out + 4 * ((size_t)i * n + j), acc);
        }
    }
}

/* dst[i] = max(src[i], 0). 원소는 int8, NPU_RELU_F_I32면 int32 */
static void npu_exec_relu(uint8_t *mem, const struct npu_cmd_relu *c)
{
    bool is_i32 = le16_to_cpu(c->flags) & NPU_RELU_F_I32;
    const uint8_t *src = mem + le32_to_cpu(c->src_off);
    uint8_t *dst = mem + le32_to_cpu(c->dst_off);
    uint32_t count = le32_to_cpu(c->count);
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (is_i32) {
            int32_t v = ldl_le_p(src + 4 * (size_t)i);

            stl_le_p(dst + 4 * (size_t)i, MAX(v, 0));
        } else {
            int8_t v = src[i];

            dst[i] = MAX(v, 0);
        }
    }
}

/*
 * int32 -> int8.
 *   t = (int64)src[i] * mult
 *   shift > 0이면 t = (t + (1 << (shift - 1))) >> shift   (산술 시프트)
 *   dst[i] = clamp(t + zp, -128, 127)
 * 제자리일 때도 dst[i]를 쓰기 전에 src[i]를 읽으므로 앞에서부터 처리하면 된다.
 */
static void npu_exec_requant(uint8_t *mem, const struct npu_cmd_requant *c)
{
    const uint8_t *src = mem + le32_to_cpu(c->src_off);
    uint8_t *dst = mem + le32_to_cpu(c->dst_off);
    uint32_t count = le32_to_cpu(c->count);
    int64_t mult = (int32_t)le32_to_cpu(c->mult);
    int64_t zp = (int32_t)le32_to_cpu(c->zp);
    uint32_t shift = le32_to_cpu(c->shift);
    uint32_t i;

    for (i = 0; i < count; i++) {
        int64_t t = (int64_t)(int32_t)ldl_le_p(src + 4 * (size_t)i) * mult;

        if (shift) {
            t = (t + (1LL << (shift - 1))) >> shift;
        }
        t += zp;
        dst[i] = (int8_t)MIN(MAX(t, -128), 127);
    }
}

/*
 * 명령을 순서대로 실행한다. 실패하면 거기서 멈추고 그 번호를 job->bad_cmd에 넣는다.
 * 앞 명령이 장치 메모리에 쓴 내용은 되돌리지 않는다. 결과는 job->status에 넣는다.
 */
static void npu_job_run(NpuJob *job)
{
    NpuState *n = job->state;
    uint8_t *mem = memory_region_get_ram_ptr(&n->devmem);
    uint32_t i;

    job->status = NPU_SC_SUCCESS;
    for (i = 0; i < job->num_cmds; i++) {
        const struct npu_job_cmd *c = &job->cmds[i];
        uint16_t sc = NPU_SC_SUCCESS;

        switch (le16_to_cpu(c->opcode)) {
        case NPU_CMD_NOP:
            break;
        case NPU_CMD_DMA_H2D:
            sc = npu_exec_dma(job, mem, (const struct npu_cmd_dma *)c, true);
            break;
        case NPU_CMD_DMA_D2H:
            sc = npu_exec_dma(job, mem, (const struct npu_cmd_dma *)c, false);
            break;
        case NPU_CMD_GEMM:
            npu_exec_gemm(mem, (const struct npu_cmd_gemm *)c);
            break;
        case NPU_CMD_RELU:
            npu_exec_relu(mem, (const struct npu_cmd_relu *)c);
            break;
        case NPU_CMD_REQUANT:
            npu_exec_requant(mem, (const struct npu_cmd_requant *)c);
            break;
        default:
            g_assert_not_reached();     /* 검증에서 걸렀다 */
        }
        if (sc != NPU_SC_SUCCESS) {
            job->status = sc;
            job->bad_cmd = i;
            break;
        }
    }

    /* 호스트 포인터로 직접 썼으니 RAM이 바뀌었다고 알린다 */
    memory_region_set_dirty(&n->devmem, 0, n->devmem_size);
}

/* ---- 지연 모델과 완료 (10절) ---- */

/* 이 job의 CQE를 얼마나 늦출지(ns): 고정 비용 + MAC 수 x 계수 */
static uint64_t npu_job_delay_ns(const NpuJob *job)
{
    return job->state->job_ns + job->macs * job->state->mac_ns;
}

/* 타이머 콜백: job이 "끝났다". 결과를 요청에 옮기고 CQE를 예약한다 */
static void npu_job_done(void *opaque)
{
    NpuJob *job = opaque;
    NpuRequest *req = job->req;
    uint16_t sc = job->status;

    if (sc != NPU_SC_SUCCESS) {
        req->result = job->bad_cmd;
    }
    trace_npu_job_done(req->sq->sqid, req->cid, sc);
    npu_job_free(job);
    npu_req_complete(req, sc);
}

/* ---- queue.c가 부르는 함수 ---- */

/*
 * SUBMIT_JOB: cdw0/1 = job 버퍼 주소, cdw2 = 크기.
 * 복사 -> 검증 -> 실행 -> 타이머 예약까지 하고 NPU_SC_NO_COMPLETE를 돌려준다.
 * 복사나 검증이 실패하면 아무것도 실행하지 않고 그 상태 코드를 바로 돌려준다
 * (req->result = 실패한 명령 번호).
 */
uint16_t npu_job_submit(NpuState *n, NpuRequest *req, const NpuSqe *sqe)
{
    uint64_t addr = le32_to_cpu(sqe->cdw[0]) |
                    ((uint64_t)le32_to_cpu(sqe->cdw[1]) << 32);
    uint32_t size = le32_to_cpu(sqe->cdw[2]);
    NpuJob *job;
    uint16_t sc;

    if (!npu_cdw_is_zero(sqe, 3) ||
        size < sizeof(struct npu_job_hdr) || size > NPU_JOB_MAX_SIZE) {
        return NPU_SC_INVALID_FIELD;
    }

    job = g_new0(NpuJob, 1);
    job->state = n;
    job->req = req;
    req->job = job;

    sc = npu_job_read(job, addr, size);
    if (sc == NPU_SC_SUCCESS) {
        sc = npu_job_validate(job);
    }
    if (sc != NPU_SC_SUCCESS) {
        req->result = job->bad_cmd;
        npu_job_free(job);
        return sc;
    }

    npu_job_run(job);
    trace_npu_job_run(req->sq->sqid, req->cid, job->num_cmds, job->macs,
                      npu_job_delay_ns(job));

    job->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, npu_job_done, job);
    timer_mod(job->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              npu_job_delay_ns(job));
    return NPU_SC_NO_COMPLETE;
}

/*
 * 실행 중인 job(req->job)을 멈추고 해제한다. 타이머를 지워서 npu_job_done()이 불리지 않게 한다.
 * 요청을 완료시키지는 않는다. 리셋에서 불리며 CQE 없이 끝난다.
 */
void npu_job_cancel(NpuRequest *req)
{
    if (req->job) {
        npu_job_free(req->job);
    }
}
