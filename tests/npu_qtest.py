#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""드라이버 없이 QEMU NPU 장치를 시험한다.

게스트를 부팅하지 않고 qtest 프로토콜로 레지스터와 게스트 메모리를 직접 읽고 써서
드라이버가 할 일을 흉내 낸다. 장치를 고친 뒤 계약(DESIGN.md)이 깨지지 않았는지 확인하는 용도다.

    ./npu_qtest.py                     # $QEMU_BUILD(기본 ~/workspace/qemu/build)의 QEMU를 쓴다
    ./npu_qtest.py --qemu <qemu-system-x86_64 경로> --bios <pc-bios 경로>
    ./npu_qtest.py --trace             # 장치 trace를 stderr로 본다
"""
import argparse
import json
import os
import random
import socket
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from mkfw import make_image     # noqa: E402

# ---- npu_hw.h와 같은 값 ----
REG_ID, REG_VERSION, REG_CAP, REG_CTRL, REG_CSTS, REG_FW_STS = \
    0x00, 0x04, 0x08, 0x10, 0x14, 0x1C
REG_ASQ_LO, REG_ASQ_HI, REG_ACQ_LO, REG_ACQ_HI, REG_AQA = \
    0x20, 0x24, 0x28, 0x2C, 0x30
CSTS_RDY, CSTS_CFS = 1, 2
OP_GET_PARAM, OP_FW_LOAD, OP_CREATE_CQ, OP_CREATE_SQ, OP_SUBMIT = \
    0x01, 0x02, 0x03, 0x04, 0x80
SC_OK, SC_INVALID_OPCODE, SC_INVALID_FIELD, SC_INVALID_QID, \
    SC_INVALID_STATE, SC_FW_INVALID, SC_DMA_ERROR = range(7)
SC_JOB_INVALID_HDR, SC_JOB_INVALID_CMD, SC_JOB_OUT_OF_RANGE = 0x20, 0x21, 0x22
CMD_NOP, CMD_H2D, CMD_D2H, CMD_GEMM, CMD_RELU, CMD_REQUANT = \
    0x01, 0x02, 0x03, 0x10, 0x11, 0x12
JOB_MAGIC = 0x424F4A4E

# ---- 이 시험이 정한 주소 ----
BAR0, BAR2, BAR4 = 0xFE000000, 0xE0000000, 0xFE010000
ASQ, ACQ, JCQ, JSQ = 0x100000, 0x101000, 0x102000, 0x103000
FW_BUF, JOB_BUF, DATA = 0x110000, 0x120000, 0x130000


class Qtest:
    def __init__(self, qemu, bios, trace):
        self.qmp_path = os.path.join(tempfile.mkdtemp(), "qmp")
        args = [qemu, "-L", bios, "-M", "q35", "-m", "64", "-display", "none",
                "-machine", "accel=qtest", "-qtest", "stdio",
                "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
                "-device", "npu,id=npu0"]
        if trace:
            args += ["-trace", "npu_*", "-d", "guest_errors"]
        self.p = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=None if trace else subprocess.DEVNULL, text=True)
        self.qmp = None

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        while True:
            reply = self.p.stdout.readline()
            if not reply:
                raise RuntimeError("QEMU exited (crash?) during: " + line)
            if reply.startswith(("OK", "FAIL", "ERR")):
                assert reply.startswith("OK"), (line, reply)
                return reply.split()[1:]

    def readl(self, addr):
        return int(self.cmd(f"readl {addr:#x}")[0], 16)

    def writel(self, addr, val):
        self.cmd(f"writel {addr:#x} {val:#x}")

    def read(self, addr, size):
        return bytes.fromhex(self.cmd(f"read {addr:#x} {size:#x}")[0][2:])

    def write(self, addr, data):
        self.cmd(f"write {addr:#x} {len(data):#x} 0x{data.hex()}")

    def clock_step(self, ns):
        self.cmd(f"clock_step {ns}")

    def qom_set(self, prop, value):
        if not self.qmp:
            self.qmp = socket.socket(socket.AF_UNIX)
            self.qmp.connect(self.qmp_path)
            self.qmp_f = self.qmp.makefile("rw")
            self.qmp_f.readline()
            self._qmp({"execute": "qmp_capabilities"})
        self._qmp({"execute": "qom-set", "arguments": {
            "path": "/machine/peripheral/npu0", "property": prop,
            "value": value}})

    def _qmp(self, obj):
        self.qmp_f.write(json.dumps(obj) + "\n")
        self.qmp_f.flush()
        while True:
            reply = json.loads(self.qmp_f.readline())
            if "return" in reply or "error" in reply:
                assert "return" in reply, reply
                return

    def close(self):
        self.p.stdin.close()
        self.p.terminate()
        self.p.wait()


class Npu:
    """드라이버가 할 일: PCI 설정, 큐 관리, 명령 제출과 완료 읽기."""

    def __init__(self, qt):
        self.qt = qt
        self.dev = next(d for d in range(32)
                        if self.cfg_r(d, 0) == 0x20261234)
        self.cfg_w(self.dev, 0x10, BAR0)
        self.cfg_w(self.dev, 0x18, BAR2)
        self.cfg_w(self.dev, 0x1C, 0)
        self.cfg_w(self.dev, 0x20, BAR4)
        self.cfg_w(self.dev, 0x04, self.cfg_r(self.dev, 0x04) | 0x6)
        self.msix_enable()
        self.q = {}
        self.cid = 0

    def cfg_r(self, dev, reg):
        self.qt.cmd(f"outl 0xcf8 {0x80000000 | dev << 11 | reg:#x}")
        return int(self.qt.cmd("inl 0xcfc")[0], 16)

    def cfg_w(self, dev, reg, val):
        self.qt.cmd(f"outl 0xcf8 {0x80000000 | dev << 11 | reg:#x}")
        self.qt.cmd(f"outl 0xcfc {val:#x}")

    def msix_enable(self):
        """MSI-X를 켠다. 벡터는 마스크된 채로 두어 인터럽트가 PBA의 pending 비트로 남게 한다."""
        ptr = self.cfg_r(self.dev, 0x34) & 0xFF
        while ptr:
            hdr = self.cfg_r(self.dev, ptr)
            if hdr & 0xFF == 0x11:
                self.cfg_w(self.dev, ptr, hdr | 0x80000000)
                return
            ptr = hdr >> 8 & 0xFF
        raise RuntimeError("no MSI-X capability")

    def irq_pending(self, vector):
        return bool(self.qt.readl(BAR4 + 0x800) >> vector & 1)

    def r(self, off):
        return self.qt.readl(BAR0 + off)

    def w(self, off, val):
        self.qt.writel(BAR0 + off, val)

    def add_queue(self, qid, sq, cq, sq_n, cq_n):
        self.qt.write(sq, bytes(64 * sq_n))
        self.qt.write(cq, bytes(16 * cq_n))
        self.q[qid] = dict(sq=sq, cq=cq, sq_n=sq_n, cq_n=cq_n,
                           tail=0, head=0, phase=1)

    def enable(self, asq_n=8, acq_n=8):
        self.q = {}
        self.add_queue(0, ASQ, ACQ, asq_n, acq_n)
        self.w(REG_ASQ_LO, ASQ)
        self.w(REG_ASQ_HI, 0)
        self.w(REG_ACQ_LO, ACQ)
        self.w(REG_ACQ_HI, 0)
        self.w(REG_AQA, acq_n << 16 | asq_n)
        self.w(REG_CTRL, 1)
        return self.r(REG_CSTS)

    def disable(self):
        self.w(REG_CTRL, 0)
        return self.r(REG_CSTS)

    def submit(self, qid, opcode, *cdw):
        q = self.q[qid]
        self.cid += 1
        sqe = struct.pack("<BBH15I", opcode, 0, self.cid,
                          *(list(cdw) + [0] * (15 - len(cdw))))
        self.qt.write(q["sq"] + 64 * q["tail"], sqe)
        q["tail"] = (q["tail"] + 1) % q["sq_n"]
        self.w(0x1000 * (qid + 1), q["tail"])
        return self.cid

    def poll(self, qid, ack=True):
        """새 CQE가 있으면 (status, result, dw1, cid)를, 없으면 None을 돌려준다."""
        q = self.q[qid]
        self.qt.clock_step(1)       # 예약된 BH가 돌게 한다
        raw = self.qt.read(q["cq"] + 16 * q["head"], 16)
        result, dw1, _sq_head, sq_id, cid, status = \
            struct.unpack("<IIHHHH", raw)
        if status & 1 != q["phase"]:
            return None
        assert sq_id == qid
        q["head"] += 1
        if q["head"] == q["cq_n"]:
            q["head"] = 0
            q["phase"] ^= 1
        if ack:
            self.ack(qid)
        return status >> 1, result, dw1, cid

    def ack(self, qid):
        self.w(0x1000 * (qid + 1) + 4, self.q[qid]["head"])

    def admin(self, opcode, *cdw):
        cid = self.submit(0, opcode, *cdw)
        cqe = self.poll(0)
        assert cqe and cqe[3] == cid, cqe
        return cqe[:3]

    def load_fw(self, image):
        self.qt.write(FW_BUF, image)
        return self.admin(OP_FW_LOAD, FW_BUF, 0, len(image))

    def job(self, cmds, sg=()):
        """job 버퍼를 만들어 큐 1에 제출하고 cid를 돌려준다."""
        buf = struct.pack("<IHH8x", JOB_MAGIC, len(cmds), len(sg))
        buf += b"".join(cmds)
        buf += b"".join(struct.pack("<QII", a, n, 0) for a, n in sg)
        self.qt.write(JOB_BUF, buf)
        return self.submit(1, OP_SUBMIT, JOB_BUF, 0, len(buf))

    def job_wait(self, cmds, sg=(), step=10**9):
        cid = self.job(cmds, sg)
        self.qt.clock_step(step)
        cqe = self.poll(1)
        assert cqe and cqe[3] == cid, cqe
        return cqe[:2]


def cmd(opcode, flags=0, *args):
    return struct.pack("<HH7I", opcode, flags,
                       *(list(args) + [0] * (7 - len(args))))


def u32(v):
    return v & 0xFFFFFFFF


# ---- 비트 단위로 비교할 CPU 참조 구현 (DESIGN.md 8.2절) ----

def ref_gemm(a, b, bias, m, k, n):
    out = []
    for i in range(m):
        for j in range(n):
            acc = bias[j] + sum(a[i * k + p] * b[p * n + j] for p in range(k))
            out.append((acc + 2**31) % 2**32 - 2**31)
    return out


def ref_requant(src, mult, shift, zp):
    out = []
    for v in src:
        t = v * mult
        if shift:
            t = (t + (1 << (shift - 1))) >> shift
        out.append(max(-128, min(127, t + zp)))
    return out


checks = 0


def check(name, got, want):
    global checks
    checks += 1
    if got != want:
        raise AssertionError(f"{name}: got {got!r}, want {want!r}")
    print(f"  ok  {name}")


def run(qt):
    npu = Npu(qt)

    print("레지스터")
    check("ID", npu.r(REG_ID), 0x3155504E)
    check("VERSION", npu.r(REG_VERSION), 0x00010000)
    check("CAP (큐 4개, 최대 1024칸)", npu.r(REG_CAP), 1024 << 16 | 4)

    print("EN/RDY/CFS")
    npu.w(REG_AQA, 0)
    npu.w(REG_CTRL, 1)
    check("admin 큐 설정 없이 EN=1 -> CFS", npu.r(REG_CSTS), CSTS_CFS)
    check("EN=0 -> CFS 해제", npu.disable(), 0)
    npu.w(REG_ASQ_LO, ASQ + 0x10)
    npu.w(REG_ACQ_LO, ACQ)
    npu.w(REG_AQA, 8 << 16 | 8)
    npu.w(REG_CTRL, 1)
    check("정렬 안 된 ASQ -> CFS", npu.r(REG_CSTS), CSTS_CFS)
    npu.disable()
    check("정상 설정 -> RDY", npu.enable(), CSTS_RDY)
    npu.w(REG_AQA, 0x00400040)
    check("EN=1일 때 AQA 쓰기는 무시", npu.r(REG_AQA), 8 << 16 | 8)

    print("admin 명령과 펌웨어")
    check("GET_PARAM DEVMEM_SIZE", npu.admin(OP_GET_PARAM, 2),
          (SC_OK, 64 << 20, 0))
    check("admin CQE가 MSI-X 벡터 0을 올림", npu.irq_pending(0), True)
    check("GET_PARAM 모르는 id", npu.admin(OP_GET_PARAM, 99)[0],
          SC_INVALID_FIELD)
    check("모르는 opcode", npu.admin(0x7F)[0], SC_INVALID_OPCODE)
    check("펌웨어 전 CREATE_CQ", npu.admin(OP_CREATE_CQ, 1, 8, JCQ, 0, 1)[0],
          SC_INVALID_STATE)
    good = make_image(0x00010002, payload=bytes(range(256)) * 16)
    bad = bytearray(good)
    bad[100] ^= 0xFF
    check("FW_LOAD 크기 오류", npu.admin(OP_FW_LOAD, FW_BUF, 0, 16)[0],
          SC_INVALID_FIELD)
    check("FW_LOAD CRC 오류", npu.load_fw(bytes(bad))[0], SC_FW_INVALID)
    check("FW_STS = ERROR", npu.r(REG_FW_STS), 3)
    check("FW_LOAD hw_major 오류",
          npu.load_fw(make_image(hw_major=2))[0], SC_FW_INVALID)
    check("FW_LOAD 성공 (result = 버전)", npu.load_fw(good)[:2],
          (SC_OK, 0x00010002))
    check("FW_STS = READY", npu.r(REG_FW_STS), 2)
    check("GET_PARAM FW_VERSION", npu.admin(OP_GET_PARAM, 1)[1], 0x00010002)
    check("READY 뒤 FW_LOAD", npu.load_fw(good)[0], SC_INVALID_STATE)

    print("큐 생성")
    check("CQ 없이 CREATE_SQ", npu.admin(OP_CREATE_SQ, 1, 8, JSQ, 0)[0],
          SC_INVALID_STATE)
    check("qid 0", npu.admin(OP_CREATE_CQ, 0, 8, JCQ, 0, 1)[0],
          SC_INVALID_QID)
    check("qid 5 (큐는 4개)", npu.admin(OP_CREATE_CQ, 5, 8, JCQ, 0, 1)[0],
          SC_INVALID_QID)
    check("엔트리 수 1", npu.admin(OP_CREATE_CQ, 1, 1, JCQ, 0, 1)[0],
          SC_INVALID_FIELD)
    check("정렬 안 된 주소", npu.admin(OP_CREATE_CQ, 1, 8, JCQ + 8, 0, 1)[0],
          SC_INVALID_FIELD)
    npu.add_queue(1, JSQ, JCQ, 8, 8)
    check("CREATE_CQ 1", npu.admin(OP_CREATE_CQ, 1, 8, JCQ, 0, 1)[0], SC_OK)
    check("CREATE_CQ 1 중복", npu.admin(OP_CREATE_CQ, 1, 8, JCQ, 0, 1)[0],
          SC_INVALID_STATE)
    check("CREATE_SQ 1", npu.admin(OP_CREATE_SQ, 1, 8, JSQ, 0)[0], SC_OK)

    print("job: MLP 한 층 (H2D -> GEMM -> RELU -> REQUANT -> D2H)")
    rnd = random.Random(1)
    m, k, n = 3, 5, 4
    a = [rnd.randint(-128, 127) for _ in range(m * k)]
    b = [rnd.randint(-128, 127) for _ in range(k * n)]
    bias = [rnd.randint(-100000, 100000) for _ in range(n)]
    mult, shift, zp = 1234567, 20, -3
    blob = struct.pack(f"<{m*k}b{k*n}b", *a, *b).ljust(36, b"\0") + \
        struct.pack(f"<{n}i", *bias)
    qt.write(DATA, blob)
    qt.write(DATA + 0x1000, bytes(64))
    A, B, BIAS, C, OUT = 0, 15, 36, 0x100, 0x200
    # 입력은 조각 2개로 나눠 보낸다(SG). 출력은 조각 1개.
    sg = [(DATA, 20), (DATA + 20, len(blob) - 20), (DATA + 0x1000, m * n)]
    cmds = [
        cmd(CMD_H2D, 0, 0, 2, 0),
        cmd(CMD_GEMM, 1, A, B, BIAS, C, m, k, n),
        cmd(CMD_RELU, 1, C, C, m * n),
        cmd(CMD_REQUANT, 0, C, OUT, m * n, u32(mult), shift, u32(zp)),
        cmd(CMD_D2H, 0, 2, 1, OUT),
    ]
    cid = npu.job(cmds, sg)
    check("지연 시간이 지나기 전에는 CQE 없음", npu.poll(1), None)
    qt.clock_step(10**9)
    check("시간이 지나면 CQE", npu.poll(1), (SC_OK, 0, 0, cid))
    check("job CQE가 MSI-X 벡터 1을 올림", npu.irq_pending(1), True)
    want_c = [max(v, 0) for v in ref_gemm(a, b, bias, m, k, n)]
    want_out = ref_requant(want_c, mult, shift, zp)
    got_c = list(struct.unpack(f"<{m*n}i", qt.read(BAR2 + C, 4 * m * n)))
    check("장치 메모리(BAR2)의 GEMM+RELU 결과", got_c, want_c)
    got_out = list(struct.unpack(f"<{m*n}b", qt.read(DATA + 0x1000, m * n)))
    check("D2H로 받은 REQUANT 결과", got_out, want_out)
    check("결과가 전부 0은 아님", any(got_out), True)

    print("job 검증")
    check("NOP", npu.job_wait([cmd(CMD_NOP)]), (SC_OK, 0))
    check("범위 밖 GEMM (result = 명령 번호 1)",
          npu.job_wait([cmd(CMD_NOP),
                        cmd(CMD_GEMM, 0, 0, 0, 0, (64 << 20) - 4, 1, 1, 2)]),
          (SC_JOB_OUT_OF_RANGE, 1))
    check("C가 A와 겹치는 GEMM",
          npu.job_wait([cmd(CMD_GEMM, 0, 0, 16, 0, 0, 2, 2, 2)]),
          (SC_JOB_OUT_OF_RANGE, 0))
    check("MAC 상한 초과",
          npu.job_wait([cmd(CMD_GEMM, 0, 0, 0, 0, 0, 2**16, 2**16, 1)]),
          (SC_JOB_INVALID_CMD, 0))
    check("모르는 명령", npu.job_wait([cmd(0x7F)]), (SC_JOB_INVALID_CMD, 0))
    check("없는 SG 조각", npu.job_wait([cmd(CMD_H2D, 0, 0, 1, 0)]),
          (SC_JOB_OUT_OF_RANGE, 0))
    check("검증 실패한 job은 앞 명령도 실행하지 않음",
          (npu.job_wait([cmd(CMD_RELU, 1, C, 0x300, m * n), cmd(0x7F)]),
           qt.read(BAR2 + 0x300, 4 * m * n)),
          ((SC_JOB_INVALID_CMD, 1), bytes(4 * m * n)))
    qt.write(JOB_BUF, bytes(48))
    npu.submit(1, OP_SUBMIT, JOB_BUF, 0, 48)
    check("magic 틀린 job", npu.poll(1)[0], SC_JOB_INVALID_HDR)
    npu.submit(1, 0x01)
    check("job 큐에 admin opcode", npu.poll(1)[0], SC_INVALID_OPCODE)
    check("게스트 RAM 밖을 가리키는 DMA (실행 중 실패)",
          npu.job_wait([cmd(CMD_NOP), cmd(CMD_H2D, 0, 0, 1, 0)],
                       [(0xC0000000, 16)])[1], 1)

    print("CQ가 가득 찼을 때")
    for _ in range(9):
        npu.submit(1, OP_SUBMIT, 0, 0, 0)       # 크기 0: 바로 INVALID_FIELD
        qt.clock_step(1)
    got = 0
    while npu.poll(1, ack=False):
        got += 1
    check("8칸 CQ에는 7개까지만 쓰임", got, 7)
    npu.ack(1)
    while npu.poll(1):
        got += 1
    check("head doorbell 뒤 나머지가 옴 (버려지지 않음)", got, 9)

    print("장애 주입")
    qt.qom_set("inject-cqe-drop", 1)
    npu.job([cmd(CMD_NOP)])
    qt.clock_step(10**9)
    check("CQE drop: 완료가 오지 않음", npu.poll(1), None)
    check("다음 job은 정상", npu.job_wait([cmd(CMD_NOP)]), (SC_OK, 0))
    qt.qom_set("inject-cfs", True)
    check("inject-cfs -> CSTS = RDY|CFS", npu.r(REG_CSTS),
          CSTS_RDY | CSTS_CFS)
    npu.job([cmd(CMD_NOP)])
    qt.clock_step(10**9)
    check("CFS 동안은 처리하지 않음", npu.poll(1), None)

    print("리셋")
    check("EN=0 -> CSTS 0", npu.disable(), 0)
    check("FW_STS = NONE", npu.r(REG_FW_STS), 0)
    check("AQA = 0", npu.r(REG_AQA), 0)
    check("다시 enable", npu.enable(), CSTS_RDY)
    check("리셋 뒤 펌웨어 없음", npu.admin(OP_GET_PARAM, 1)[1], 0)
    npu.load_fw(good)
    npu.add_queue(1, JSQ, JCQ, 8, 8)
    npu.admin(OP_CREATE_CQ, 1, 8, JCQ, 0, 0)
    npu.admin(OP_CREATE_SQ, 1, 8, JSQ, 0)
    npu.job([cmd(CMD_NOP)])
    qt.clock_step(1)
    check("job 실행 중 리셋", npu.disable(), 0)
    qt.clock_step(10**9)
    check("리셋된 job의 CQE는 오지 않음",
          qt.read(JCQ, 16), bytes(16))
    check("장치 메모리 내용은 남아 있음",
          list(struct.unpack(f"<{m*n}i", qt.read(BAR2 + C, 4 * m * n))),
          want_c)


def main():
    build = os.path.expanduser(os.environ.get("QEMU_BUILD", "~/workspace/qemu/build"))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--qemu", default=f"{build}/qemu-system-x86_64")
    ap.add_argument("--bios", default=f"{build}/../pc-bios")
    ap.add_argument("--trace", action="store_true")
    args = ap.parse_args()

    qt = Qtest(args.qemu, args.bios, args.trace)
    try:
        run(qt)
    finally:
        qt.close()
    print(f"\n{checks}개 확인 모두 통과")


if __name__ == "__main__":
    main()
