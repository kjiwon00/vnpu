# NPU 설계 문서

QEMU 가상 NPU 장치(`qemu/hw/npu`)와 Linux DRM accel 드라이버 사이의 계약을 정한다.
README의 로드맵 0단계 산출물이다.

- 상수와 구조체의 원본은 `npu_hw.h`다. 이 문서와 다르면 `npu_hw.h`가 맞다.
- 이 문서는 "무엇을 약속하는가"만 다룬다. 장치 구현 진행 상황은 `qemu/hw/npu/TODO.md`에 있다.
- 장치는 이 문서대로 구현돼 있다. `tests/npu_qtest.py`가 드라이버 없이 계약을 확인한다.

## 1. 구성

```
사용자 공간   libnpu ── ioctl(/dev/accel/accel0) ──┐
커널          DRM accel 드라이버 (GEM, drm_sched)  ◄┘
                 │  MMIO(BAR0)   DMA(큐, job 버퍼, SG)   mmap(BAR2)
장치          QEMU NPU: 컨트롤러 ─ 큐 ─ 펌웨어 검증 ─ 연산 엔진 ─ 장치 메모리
```

원칙은 세 가지다.

1. **사용자 공간은 DMA 주소와 장치 메모리 오프셋을 직접 다루지 않는다.** 사용자 공간은 BO handle과
   그 안의 오프셋만 넘기고, 드라이버가 검증한 뒤 장치 형식으로 바꾼다. 장치는 받은 주소를 그대로
   믿기 때문에, 이 변환이 없으면 사용자 공간이 장치를 시켜 커널 메모리를 읽고 쓸 수 있다.
2. **RDY=0이면 장치는 게스트 메모리에 접근하지 않는다.** 드라이버는 RDY=0을 확인한 뒤에만 큐와
   버퍼 메모리를 해제한다.
3. **모든 리셋은 장치를 같은 초기 상태로 되돌린다.** 복구 경로와 probe 경로가 같아진다.

## 2. PCI

| 항목 | 값 |
|---|---|
| Vendor / Device ID | 0x1234 / 0x2026 |
| Class | 0x1200 (processing accelerator) |
| 종류 | PCIe endpoint, FLR 지원. `-M q35`의 PCIe root port 아래에 둔다 |
| 인터럽트 | MSI-X만. 벡터 9개(벡터 번호 = qid). INTx는 없다 |

| BAR | 크기 | 종류 | 내용 |
|---|---|---|---|
| 0 | 64KiB | 32비트 | 레지스터 + doorbell |
| 2 (2~3) | 기본 64MiB | 64비트 prefetchable | 장치 메모리 |
| 4 | 4KiB | 32비트 | MSI-X 테이블 + PBA |

- **장치 메모리 크기는 BAR2의 크기다.** 드라이버는 `pci_resource_len(pdev, 2)`로 읽는다.
  `devmem-size` 속성으로 1MiB ~ 1GiB 사이의 2의 거듭제곱을 줄 수 있다.
  최대가 1GiB라서 장치 메모리 오프셋은 32비트로 충분하다.
- 장치 메모리는 CPU가 BAR2로 직접 읽고 쓸 수도 있고, job의 DMA 명령으로 옮길 수도 있다.
- MSI-X를 전용 BAR에 둔 이유: doorbell 페이지와 같은 BAR에 두면 doorbell을 사용자 공간에
  매핑하는 구조로 확장할 때 테이블이 같이 노출될 수 있다.

## 3. BAR0

4KiB 페이지 16개다. 큐 수와 상관없이 크기가 고정이라 드라이버는 doorbell 위치를 계산만 하면 된다.

| 페이지 | 오프셋 | 내용 |
|---|---|---|
| 0 | 0x0000 | 레지스터 |
| 1 | 0x1000 | admin 큐(qid 0) doorbell |
| 2 ~ 9 | 0x2000 ~ | job 큐(qid 1 ~ 8) doorbell |
| 10 ~ 15 | 0xA000 ~ | 예약. 읽으면 0, 쓰면 무시 |

모든 접근은 4바이트다. 정의되지 않은 오프셋은 읽으면 0, 쓰면 무시된다.

### 3.1 레지스터

| 오프셋 | 이름 | 접근 | 내용 |
|---|---|---|---|
| 0x00 | ID | RO | 0x3155504E ("NPU1") |
| 0x04 | VERSION | RO | major << 16 \| minor. 지금은 1.0 |
| 0x08 | CAP | RO | [7:0] job 큐 수(NQ), [31:16] 큐 최대 엔트리 수(MQES = 1024) |
| 0x10 | CTRL | RW | bit 0 EN |
| 0x14 | CSTS | RO | bit 0 RDY, bit 1 CFS |
| 0x1C | FW_STS | RO | 0 NONE, 1 LOADING, 2 READY, 3 ERROR |
| 0x20 / 0x24 | ASQ_LO / ASQ_HI | RW | admin SQ의 DMA 주소 |
| 0x28 / 0x2C | ACQ_LO / ACQ_HI | RW | admin CQ의 DMA 주소 |
| 0x30 | AQA | RW | [11:0] admin SQ 엔트리 수, [27:16] admin CQ 엔트리 수 |

- 64비트 주소는 32비트 레지스터 두 개(LO, HI)로 나눠 쓴다. 드라이버는 `lo_hi_writeq()`를 쓴다.
- ASQ/ACQ/AQA는 EN=0일 때만 쓸 수 있다. EN=1일 때 쓰면 무시된다. 장치는 EN 0→1 순간에만
  이 값을 읽으므로 절반만 써진 주소가 쓰일 일이 없다.
- AQA의 엔트리 수는 "개수 − 1"이 아니라 실제 개수다(2 ~ 1024).

### 3.2 doorbell

큐 `qid`의 doorbell 페이지는 `0x1000 × (qid + 1)`에 있다.

| 페이지 안 오프셋 | 이름 | 내용 |
|---|---|---|
| 0x0 | SQ tail | 드라이버가 다음에 쓸 SQ 칸 번호 |
| 0x4 | CQ head | 드라이버가 다음에 읽을 CQ 칸 번호 |

- 없는 큐의 doorbell이나 큐 크기 이상의 값은 무시된다(장치 로그에만 남는다).
- 큐마다 페이지를 따로 준 이유: 4KiB가 MMU의 최소 보호 단위라서, 나중에 큐 하나의 doorbell만
  사용자 공간에 매핑할 수 있다. 이번 구현에서는 커널(drm_sched)만 doorbell을 쓴다.

## 4. 상태 머신

컨트롤러 상태(CSTS)와 펌웨어 상태(FW_STS)는 별개다. RDY가 먼저 서고 펌웨어는 그 다음에 올린다.
펌웨어를 올리려면 DMA가 필요하고, DMA는 RDY=1일 때만 허용되기 때문이다.

```
EN=0                      RDY=0, CFS=0, FW=NONE. 장치는 DMA하지 않는다
  │ EN 0→1
  ├─ AQA/ASQ/ACQ가 잘못됨 ──→ CFS=1 (RDY는 0 그대로)
  ▼
RDY=1, FW=NONE            admin 큐만 동작. GET_PARAM, FW_LOAD만 받는다
  │ FW_LOAD
  ▼
FW=LOADING ──실패──→ FW=ERROR   (FW_LOAD를 다시 보낼 수 있다)
  │ 성공
  ▼
FW=READY                  CREATE_CQ/SQ와 job 제출을 받는다

어느 상태에서든 치명적 오류 → CFS=1
어느 상태에서든 EN 1→0, FLR, 시스템 리셋 → 맨 위 상태
```

### 4.1 EN과 RDY

- **EN 0→1**: 장치는 AQA(엔트리 수 2 ~ 1024)와 ASQ/ACQ(4KiB 정렬)를 검사한다.
  맞으면 admin 큐를 만들고 RDY=1, 틀리면 CFS=1.
- **EN 1→0**: 컨트롤러 리셋이다. 장치는 새 명령을 받지 않고, 실행 중인 작업을 멈추고,
  모든 큐를 없앤 다음 RDY=0으로 만든다. 이때 진행 중이던 명령의 CQE는 **오지 않는다**.
  드라이버가 자기 쪽 대기 목록을 직접 실패 처리해야 한다.
- RDY는 EN을 쓴 즉시 바뀐다고 보장하지 않는다. 드라이버는 양쪽 전환 모두 timeout을 두고
  CSTS를 polling한다.

### 4.2 CFS

CFS=1이 되는 경우:

- EN 0→1에서 admin 큐 설정이 잘못됨
- 큐 메모리 자체에 접근 실패(SQE 읽기, CQE 쓰기)
- 장애 주입

CFS=1인 동안 장치는 큐를 처리하지 않고, DMA도 인터럽트도 없고, doorbell은 무시한다.
레지스터는 읽을 수 있다. CFS를 알리는 인터럽트는 없다. 드라이버는 job timeout에서 CSTS를 읽어
알아낸다. CFS는 리셋(EN 1→0, FLR, 시스템 리셋)으로만 지워진다.

### 4.3 리셋

EN 1→0, FLR, 시스템 리셋은 모두 같은 결과를 낸다.

- CTRL, CSTS, FW_STS, ASQ, ACQ, AQA가 0이 된다.
- 모든 큐가 사라진다. 펌웨어도 사라진다(FW=NONE).
- 장치 메모리의 내용은 **보존을 보장하지 않는다**. 드라이버는 리셋 뒤 내용이 사라졌다고 본다.
- FLR과 시스템 리셋은 PCI config(MSI-X 설정 포함)도 초기화한다.

### 4.4 드라이버가 따르는 순서

```
probe / 복구
 1. (probe만) BAR 매핑, ID와 VERSION 확인
 2. EN=0을 쓰고 RDY=0을 기다린다
 3. CAP을 읽는다. admin SQ/CQ 메모리를 0으로 채워 준비하고 AQA, ASQ, ACQ를 쓴다
 4. EN=1을 쓰고 RDY=1 또는 CFS=1을 기다린다
 5. request_firmware() → FW_LOAD → FW=READY 확인
 6. job 큐마다 CREATE_CQ → CREATE_SQ

remove / shutdown / reset_prepare
 1. EN=0을 쓰고 RDY=0을 기다린다
 2. 그 다음에 큐와 버퍼 메모리를 해제한다
```

job timeout 복구는 "대기 중인 job을 모두 실패 처리 → 위 2~6번"이다.

## 5. 큐

SQ(제출)와 CQ(완료)는 게스트 메모리에 있는 ring이다. 드라이버가 `dma_alloc_coherent()`로
잡아 주소를 장치에 알려 준다.

- qid 0은 admin 큐, qid 1 ~ CAP.NQ는 job 큐다.
- **SQ n의 완료는 항상 CQ n으로 가고, CQ n의 인터럽트는 MSI-X 벡터 n이다.**
- ring 메모리는 4KiB 정렬이어야 한다. 엔트리 수는 2 ~ 1024다.
- SQ ring은 64바이트 × 엔트리 수, CQ ring은 16바이트 × 엔트리 수다.

### 5.1 SQE (64바이트)

| 오프셋 | 필드 | 내용 |
|---|---|---|
| 0 | opcode (u8) | 0x00 ~ 0x7F admin 큐 전용, 0x80 ~ 0xFF job 큐 전용 |
| 1 | flags (u8) | 예약, 0 |
| 2 | cid (u16) | 드라이버가 정한 명령 ID. 장치는 CQE에 그대로 돌려준다 |
| 4 | cdw[15] (u32 × 15) | 명령별 필드. 쓰지 않는 칸은 0 |

### 5.2 CQE (16바이트)

| 오프셋 | 필드 | 내용 |
|---|---|---|
| 0 | result (u32) | 명령별 결과 |
| 4 | dw1 (u32) | 명령별 결과(64비트 값의 상위) |
| 8 | sq_head (u16) | 장치가 SQ를 어디까지 읽었나 |
| 10 | sq_id (u16) | 이 명령이 들어온 SQ |
| 12 | cid (u16) | SQE의 cid |
| 14 | status (u16) | bit 0 = phase, bit 15:1 = 상태 코드 |

### 5.3 규칙

- **제출**: 드라이버가 SQ의 tail 칸에 SQE를 쓰고 tail을 1 올려 SQ tail doorbell에 쓴다.
  `(tail + 1) % size == head`면 가득 찬 것이다. head는 CQE의 `sq_head`로 알 수 있다.
- **완료**: 장치가 CQ의 tail 칸에 CQE를 쓴다. phase 비트는 첫 바퀴에 1이고 ring을 한 바퀴 돌 때마다
  뒤집힌다. 드라이버는 CQ 메모리를 0으로 채워서 넘기고, head 칸의 phase가 기대값과 같으면
  새 CQE로 본다. 읽은 뒤 CQ head doorbell에 새 head를 쓴다.
- **CQ가 가득 차면** 장치는 완료를 쥐고 있다가 드라이버가 CQ head doorbell을 쓰면 이어서 쓴다.
  완료는 버려지지 않는다.
- **인터럽트**: CQE를 쓴 뒤 그 CQ의 IEN이 켜져 있으면 벡터 n을 올린다. 인터럽트 한 번에 CQE가
  여러 개일 수 있으므로 드라이버는 phase가 맞는 동안 계속 읽는다. admin CQ는 IEN이 항상 켜져 있다.
- **순서**: 한 SQ의 명령은 제출한 순서대로 하나씩 실행되고, CQE도 그 순서로 온다.
  서로 다른 SQ 사이에는 순서가 없다.

## 6. admin 명령

FW가 READY가 되기 전에는 GET_PARAM과 FW_LOAD만 받는다. 나머지는 INVALID_STATE다.

| opcode | 이름 | cdw | CQE |
|---|---|---|---|
| 0x01 | GET_PARAM | cdw0 = param id | result/dw1 = 값의 하위/상위 32비트 |
| 0x02 | FW_LOAD | cdw0/1 = 이미지 주소 LO/HI, cdw2 = 크기 | result = 펌웨어 버전 |
| 0x03 | CREATE_CQ | cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소 LO/HI, cdw4 bit 0 = IEN | |
| 0x04 | CREATE_SQ | cdw0 = qid, cdw1 = 엔트리 수, cdw2/3 = 주소 LO/HI | |

**GET_PARAM**: param 1 = FW_VERSION(펌웨어가 없으면 0), 2 = DEVMEM_SIZE(바이트).
큐 수와 최대 엔트리 수는 CAP 레지스터에 있어서 넣지 않았다.

**CREATE_CQ / CREATE_SQ**

- CQ를 먼저 만든다. 같은 번호의 CQ가 없는데 SQ를 만들거나 이미 있는 큐를 만들면 INVALID_STATE다.
- qid가 0이거나 CAP.NQ보다 크면 INVALID_QID, 엔트리 수나 정렬이 틀리면 INVALID_FIELD다.
- IEN=0인 CQ는 CQE만 쓰고 인터럽트를 보내지 않는다(polling 벤치마크용).
- **큐를 지우는 명령은 없다.** 큐를 없애려면 리셋(EN 1→0)한다. 리셋은 모든 큐를 한꺼번에 없앤다.
  드라이버의 remove, 복구 경로가 모두 리셋을 거치므로 큐를 하나씩 지울 일이 없다.

## 7. 펌웨어 이미지

실제로 실행되는 코드는 없다. 장치는 헤더와 CRC만 검사한다. 목적은 드라이버의
`request_firmware()` → 업로드 → READY 대기 흐름을 만드는 것이다.

이미지는 헤더(32바이트) + payload이고, 연속된 DMA 버퍼 하나로 넘긴다. 최대 1MiB.

| 오프셋 | 필드 | 내용 |
|---|---|---|
| 0 | magic | 0x5746504E ("NPFW") |
| 4 | fw_version | major << 16 \| minor |
| 8 | hw_major | 필요한 장치 major 버전. VERSION 레지스터의 major와 같아야 한다 |
| 12 | image_size | 헤더를 포함한 전체 크기. FW_LOAD의 cdw2와 같아야 한다 |
| 16 | payload_crc32 | 헤더 뒤 payload의 CRC-32 (zlib `crc32()`와 같은 값) |
| 20 | rsvd[3] | 0 |

**FW_LOAD의 결과**

| 경우 | 상태 코드 | FW_STS |
|---|---|---|
| 성공 | SUCCESS | READY |
| 크기가 32 미만이거나 1MiB 초과 | INVALID_FIELD | 그대로 |
| 이미 LOADING이거나 READY | INVALID_STATE | 그대로 |
| 이미지를 읽지 못함 | DMA_ERROR | ERROR |
| magic, hw_major, image_size, CRC, rsvd 오류 | FW_INVALID | ERROR |

READY가 된 펌웨어를 바꾸려면 리셋해야 한다. ERROR에서는 FW_LOAD를 다시 보낼 수 있다.

## 8. job

job 큐가 받는 명령은 SUBMIT_JOB(0x80) 하나다. job 하나는 "명령 목록"이고, 장치는 목록을 순서대로
실행한 뒤 CQE 하나를 보낸다.

| SQE 필드 | 내용 |
|---|---|
| opcode | 0x80 |
| cdw0/1 | job 버퍼의 DMA 주소 LO/HI |
| cdw2 | job 버퍼 크기(바이트). 최대 64KiB |

### 8.1 job 버퍼

드라이버가 만드는 연속된 DMA 버퍼 하나다.

```
struct npu_job_hdr   (16바이트)   magic "NJOB", num_cmds(1 이상), num_sg
struct npu_job_cmd   cmds[num_cmds]   (각 32바이트)
struct npu_sg        sg[num_sg]       (각 16바이트: addr u64, len u32, rsvd u32)
```

- 크기는 정확히 `16 + 32 × num_cmds + 16 × num_sg`여야 한다.
- `sg[]`는 호스트 메모리 조각의 목록이다. shmem BO는 물리적으로 연속이 아니어서 조각 목록이
  필요하다. DMA 명령은 `sg[]` 안의 구간을 `(sg_idx, sg_cnt)`로 가리킨다.
- job이 스스로 완결된다(주소, 명령, 조각 목록이 한 버퍼에 있다). 장치는 버퍼를 한 번에 복사해 온 뒤
  복사본만 본다. 그래서 검증한 뒤에 게스트가 내용을 바꾸는 문제가 생기지 않는다.

### 8.2 명령

모든 명령은 32바이트이고 `opcode`(u16), `flags`(u16)로 시작한다. `*_off`는 장치 메모리 안의
바이트 오프셋이다. int32 배열의 오프셋은 4의 배수여야 한다.

| opcode | 이름 | 인자 | 동작 |
|---|---|---|---|
| 0x01 | NOP | 없음 | 아무것도 하지 않는다. 큐 경로 시험용 |
| 0x02 | DMA_H2D | sg_idx, sg_cnt, dev_off | 호스트 메모리 → 장치 메모리 |
| 0x03 | DMA_D2H | sg_idx, sg_cnt, dev_off | 장치 메모리 → 호스트 메모리 |
| 0x10 | GEMM | a_off, b_off, bias_off, c_off, m, k, n | C = A × B (+ bias) |
| 0x11 | RELU | src_off, dst_off, count | dst[i] = max(src[i], 0) |
| 0x12 | REQUANT | src_off, dst_off, count, mult, shift, zp | int32 → int8 |

**DMA_H2D / DMA_D2H**: 가리킨 SG 조각을 순서대로 이어 붙인 것이 호스트 쪽 버퍼다.
전송 길이는 조각 길이의 합이다.

**GEMM**: 행 우선(row-major) 배치다.

```
A: int8  [m][k]     B: int8 [k][n]     bias: int32 [n]     C: int32 [m][n]

C[i][j] = (BIAS 플래그 ? bias[j] : 0) + Σp A[i][p] × B[p][j]
```

- 덧셈은 32비트 2의 보수 wraparound다.
- m, k, n은 1 이상이고 `m × k × n ≤ 2^30`이다.
- C는 A, B, bias와 겹치면 안 된다.
- MLP 한 층에서는 A = 입력(batch × 입력 수), B = 가중치, bias = 편향이다.

**RELU**: 원소 타입은 int8이고, `NPU_RELU_F_I32` 플래그가 있으면 int32다.
제자리 연산(dst_off == src_off)은 되고, 그 밖의 겹침은 안 된다.

**REQUANT**: int32 원소를 int8로 줄인다. `mult`와 `zp`는 int32, `shift`는 0 ~ 31이다.

```
t = (int64)src[i] × mult
shift > 0이면  t = (t + (1 << (shift − 1))) >> shift      (산술 시프트)
dst[i] = clamp(t + zp, −128, 127)
```

제자리 연산(dst_off == src_off)은 되고, 그 밖의 겹침은 안 된다.

GEMM, RELU, REQUANT의 정의는 사용자 공간의 CPU 참조 구현과 비트 단위로 같은 결과가 나오도록
모든 반올림과 overflow 동작을 정해 둔 것이다.

### 8.3 검증과 실패

1. 장치가 job 버퍼를 복사해 온다. 실패하면 DMA_ERROR.
2. **실행 전에 job 전체를 검증한다.** 헤더, 모든 명령의 opcode/flags/인자, 장치 메모리 범위,
   SG 구간, 겹침을 본다. 하나라도 틀리면 아무 명령도 실행하지 않는다.
3. 명령을 순서대로 실행한다. 실행 중 실패할 수 있는 것은 DMA 명령뿐이다(DMA_ERROR).
   실패하면 거기서 멈추고 남은 명령은 실행하지 않는다. 앞 명령이 장치 메모리에 쓴 내용은 남는다.
4. CQE를 보낸다. 실패했으면 `result` = 실패한 명령의 번호(0부터)다.

### 8.4 동시 실행

- 한 큐에서는 job이 하나씩 순서대로 실행된다.
- 서로 다른 큐의 job은 동시에 실행될 수 있다.
- 장치 메모리는 모든 큐가 공유하고 장치는 구역을 나누지 않는다. 같은 구역을 쓰는 job의 순서는
  드라이버가 책임진다.

## 9. 상태 코드

CQE의 `status`는 `(코드 << 1) | phase`다.

| 코드 | 이름 | 언제 |
|---|---|---|
| 0x00 | SUCCESS | |
| 0x01 | INVALID_OPCODE | 모르는 opcode, 큐 종류에 맞지 않는 opcode |
| 0x02 | INVALID_FIELD | 범위 밖의 값, 정렬 오류, 모르는 param id, 0이 아닌 예약 필드 |
| 0x03 | INVALID_QID | qid가 0이거나 CAP.NQ보다 큼 |
| 0x04 | INVALID_STATE | FW가 READY가 아님, 큐가 이미 있음, CQ 없이 SQ 생성, READY 뒤 FW_LOAD |
| 0x05 | FW_INVALID | 펌웨어 헤더 또는 CRC 오류 |
| 0x06 | DMA_ERROR | 게스트 메모리 접근 실패 |
| 0x20 | JOB_INVALID_HDR | job 헤더의 magic, 개수, 크기 오류 |
| 0x21 | JOB_INVALID_CMD | 모르는 job 명령, 잘못된 flags나 인자 |
| 0x22 | JOB_OUT_OF_RANGE | 장치 메모리나 SG 범위 밖, 금지된 겹침 |

리셋으로 사라진 명령에는 CQE가 오지 않으므로 상태 코드도 없다(4.1절).

## 10. 장치 내부 동작 (계약이 아님)

드라이버가 의존하면 안 되는 부분이다.

**만들 때 정하는 속성** (`-device npu,num-queues=2,devmem-size=128M`)

| 속성 | 기본값 | 내용 |
|---|---|---|
| `num-queues` | 4 | job 큐 수(1 ~ 8). CAP.NQ로 보인다 |
| `devmem-size` | 64M | 장치 메모리 크기(1MiB ~ 1GiB, 2의 거듭제곱) |

**실행 중에 바꾸는 속성** (QEMU 모니터에서 `qom-set /machine/peripheral/<id> <속성> <값>`.
`-device npu,id=npu0`처럼 id를 줘야 경로가 생긴다)

| 속성 | 기본값 | 내용 |
|---|---|---|
| `job-ns` | 10000 | 지연 모델: job 하나의 고정 비용(ns) |
| `mac-ns` | 1 | 지연 모델: GEMM MAC 하나의 비용(ns) |
| `inject-cqe-drop` | 0 | 장애 주입: 다음 job CQE를 이 개수만큼 버린다. job은 실행되지만 완료가 오지 않는다 |
| `inject-cfs` | | 장애 주입: true를 쓰면 CFS=1이 된다 |

- **지연 모델**: job의 연산은 제출 즉시 끝나고, CQE만 `job-ns + MAC 수 × mac-ns` 뒤에 온다.
  지연이 0이면 비동기 제출, batching, polling의 효과를 잴 수 없다.
- **장애 주입**: `inject-cqe-drop`은 job timeout을, `inject-cfs`는 치명적 오류 복구를 시험한다.
- 연산은 QEMU의 메인 스레드에서 실행된다. 큰 GEMM은 그동안 게스트 전체를 멈춘다.

## 11. ioctl 목록

`/dev/accel/accel0`의 드라이버 ioctl이다. 구조체 필드 배치는 각 단계에서 uapi 헤더를 쓸 때 정한다.

| ioctl | 입력 → 출력 | 단계 |
|---|---|---|
| `DRM_IOCTL_NPU_GET_PARAM` | param → 값(u64). 장치 버전, 펌웨어 버전, 큐 수, 장치 메모리 크기 | 2 |
| `DRM_IOCTL_NPU_BO_CREATE` | 크기, 종류(HOST 또는 DEVICE) → handle | 3 |
| `DRM_IOCTL_NPU_BO_INFO` | handle → mmap offset, 크기, 종류 | 3 |
| `DRM_IOCTL_NPU_SUBMIT` | 명령 배열(포인터, 개수) → 없음. 5단계에서 out syncobj handle 추가 | 4 |
| `DRM_IOCTL_NPU_BO_WAIT` | handle, timeout → 그 BO를 쓰는 job이 모두 끝날 때까지 대기 | 4 |

- **HOST BO**는 shmem 메모리, **DEVICE BO**는 장치 메모리(BAR2)의 한 구역이다.
  둘 다 `mmap()`으로 사용자 공간에 매핑할 수 있다.
- **SUBMIT의 명령**은 8.2절과 같은 종류지만 주소 대신 `(BO handle, BO 안의 오프셋)`을 쓴다.
  드라이버가 범위를 검증하고, DEVICE BO는 장치 메모리 오프셋으로, HOST BO는 SG 목록으로 바꿔
  job 버퍼를 만든다.
- **BO_WAIT**는 BO의 `dma_resv`에 달린 fence를 기다린다. ivpu, ethosu, rocket과 같은 방식이다.
- 그 밖에 DRM 코어 ioctl을 그대로 쓴다: `GEM_CLOSE`, 5단계부터 `SYNCOBJ_CREATE/WAIT/DESTROY`.

## 12. 아직 정하지 않은 것

장치 계약에는 영향이 없고, 해당 단계에서 정한다.

| 항목 | 단계 |
|---|---|
| uapi 구조체의 필드 배치 | 2 ~ 4 |
| job 실패(상태 코드, 실패한 명령 번호)를 사용자 공간에 돌려주는 방법 | 4 |
| 어느 파일의 job을 어느 큐로 보낼지 | 4 |
