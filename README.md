# vnpu — QEMU 가상 NPU와 Linux DRM accel 드라이버

QEMU에 PCIe NPU 장치를 직접 설계·구현하고, 그 장치를 위한 Linux 커널 드라이버를
`drivers/accel/` 구조(DRM accel)로 만드는 프로젝트입니다.
목표는 작은 INT8 신경망(MNIST MLP) 추론을 사용자 공간 → 커널 → 장치까지 end-to-end로 돌리는 것입니다.

연산 성능보다 **드라이버 구조**에 초점을 둡니다. 장치와 드라이버 사이의 계약을 먼저 문서로 정하고,
장애 복구, 수명 관리, 동시성을 검증할 수 있도록 장치에 지연 모델과 장애 주입 기능을 넣었습니다.

```
사용자 공간   libnpu ── ioctl(/dev/accel/accel0) ──┐
커널          DRM accel 드라이버 (GEM, drm_sched)  ◄┘
                 │  MMIO(BAR0)   DMA(큐, job 버퍼, SG)   mmap(BAR2)
장치          QEMU NPU: 컨트롤러 ─ 큐 ─ 펌웨어 검증 ─ 연산 엔진 ─ 장치 메모리
```

## 진행 상황

| 단계 | 내용 | 상태 |
|---|---|---|
| 0 | 설계 문서: 레지스터 맵, 큐/job 형식, 상태 머신, ioctl 목록 ([DESIGN.md](DESIGN.md)) | 완료 |
| 1 | QEMU 장치: PCIe + FLR, CTRL/CSTS, 장치 메모리 BAR, SQ/CQ, MSI-X, 펌웨어 검증, 연산 엔진, 장애 주입 | 완료 (qtest 59개 통과) |
| 2 | 드라이버: accel 등록, 컨트롤러 리셋 handshake, admin 큐, 펌웨어 로딩, `GET_PARAM` | **진행 중** |
| 3 | GEM BO (호스트 shmem / 장치 메모리) + mmap | 예정 |
| 4 | `drm_sched` 기반 job 제출, GEMM/ReLU/DMA, MNIST 데모 | 예정 |
| 5 | `dma_fence` / `drm_syncobj` 비동기 제출, 벤치마크 | 예정 |
| 6 | job timeout → 리셋 복구, 장애 주입 테스트, unbind 안전성 (KASAN/lockdep) | 예정 |
| 7 | 자동 회귀 테스트, debugfs/tracepoint | 예정 |

**2단계에서 지금 되는 것:** PCI probe(`devm_drm_dev_alloc`, `pcim_*`), 64비트 DMA 마스크, BAR0 매핑과
BAR2 위치 기록, ID/VERSION 확인, EN=0 → RDY=0 리셋 handshake(timeout 포함), `/dev/accel/accel0` 등록,
remove 시 `drm_dev_unplug` → 컨트롤러 정지 순서.
**다음:** MSI-X 벡터, admin SQ/CQ, EN=1, `request_firmware()` → FW_LOAD, `DRM_IOCTL_NPU_GET_PARAM`.

## 설계에서 신경 쓴 점

자세한 내용은 [DESIGN.md](DESIGN.md)에 있습니다.

- **사용자 공간은 DMA 주소를 다루지 않는다.** ioctl은 `(BO handle, 오프셋)`만 받고, 드라이버가 범위를
  검증한 뒤 장치 주소와 SG 목록으로 바꿉니다. 장치는 받은 주소를 그대로 믿기 때문입니다.
- **RDY=0이면 장치는 게스트 메모리에 접근하지 않는다.** 드라이버는 RDY=0을 확인한 뒤에만 큐와 버퍼를
  해제합니다. remove, shutdown, 복구 경로가 모두 이 약속에 기댑니다.
- **모든 리셋(EN 1→0, FLR, 시스템 리셋)은 같은 초기 상태로 돌아간다.** 그래서 job timeout 복구 경로가
  probe 경로와 같아집니다. 큐를 하나씩 지우는 명령을 일부러 두지 않았습니다.
- **job은 스스로 완결되고, 실행 전에 전체를 검증한다.** 장치는 job 버퍼를 한 번에 복사해 온 뒤 복사본만
  보므로, 검증 후에 게스트가 내용을 바꾸는 문제(TOCTOU)가 생기지 않습니다. 하나라도 틀리면 아무 명령도
  실행하지 않습니다.
- **큐마다 4KiB doorbell 페이지, MSI-X는 전용 BAR.** 나중에 doorbell 하나만 사용자 공간에 매핑하는
  구조로 확장해도 다른 큐나 MSI-X 테이블이 노출되지 않습니다.
- **연산량에 비례하는 지연 모델.** 연산은 즉시 끝나지만 CQE는 `job-ns + MAC 수 × mac-ns` 뒤에 옵니다.
  지연이 0이면 비동기 제출, batching, polling의 효과를 측정할 수 없기 때문입니다.
- **장애 주입.** QOM 속성으로 CQE drop(job timeout 재현)과 CFS(치명적 오류)를 실행 중에 켤 수 있습니다.
- **GEMM/ReLU/REQUANT의 반올림과 overflow를 모두 정의했다.** 사용자 공간의 CPU 참조 구현과 비트 단위로
  같은 결과가 나오는지 검증하기 위해서입니다.

## 디렉터리 구조

```
DESIGN.md                장치-드라이버 계약 (레지스터, 큐, job, 상태 코드, ioctl)
driver/                  Linux 커널 모듈 (vnpu.ko)
  npu_hw.h               장치와 드라이버가 공유하는 계약 헤더 (원본)
  vnpu_drv.c / .h        PCI/DRM 등록, probe/remove, 컨트롤러 enable/disable
qemu/
  hw/npu/                QEMU 장치 모델
    ctrl.c               PCIe + FLR, BAR0 레지스터, BAR2 장치 메모리, BAR4 MSI-X, EN/RDY/CFS, 리셋
    queue.c              doorbell, SQ/CQ, CQ full 처리, MSI-X 통지, admin 명령
    fw.c                 FW_LOAD: 헤더와 CRC 검사
    engine.c             SUBMIT_JOB: 복사, 전체 검증, DMA/GEMM/RELU/REQUANT, 지연 모델
    npu_hw.h             → driver/npu_hw.h (symlink)
  0001-hw-npu-add-build-integration.patch   QEMU 빌드 시스템에 hw/npu 연결
tests/npu_qtest.py       드라이버 없이 qtest 프로토콜로 장치 계약을 확인 (59개 항목)
tools/mkfw.py            펌웨어 이미지 생성기
scripts/run.sh           게스트 실행 스크립트
```

## 빌드와 실행

개발 환경: Linux 7.2-rc5, QEMU 11.0.91 (upstream `300438ffbb`), x86_64.

### QEMU 장치

```sh
cd qemu-src                                   # upstream QEMU 트리
git apply /path/to/vnpu/qemu/0001-hw-npu-add-build-integration.patch
ln -s /path/to/vnpu/qemu/hw/npu hw/npu        # 장치 소스를 레포에서 바로 쓴다
./configure --target-list=x86_64-softmmu && make -C build
```

### 장치 시험 (게스트 불필요)

```sh
QEMU_BUILD=/path/to/qemu-src/build tests/npu_qtest.py
```

레지스터, EN/RDY/CFS 전환, admin 명령과 펌웨어 로딩, 큐 생성, MLP 한 층 job
(H2D → GEMM → RELU → REQUANT → D2H, CPU 참조 구현과 비교), job 검증, CQ full,
장애 주입, 리셋을 확인합니다.

### 드라이버

커널에 `CONFIG_DRM_ACCEL=y`가 필요합니다. 디버깅용으로 `CONFIG_KASAN`, `CONFIG_PROVE_LOCKING`을
켜고 개발하고 있습니다.

```sh
make -C driver KDIR=/path/to/linux
tools/mkfw.py npu_fw.bin                      # 펌웨어 이미지 (4단계부터 사용)
scripts/run.sh                                # 게스트 부팅. 레포가 /vnpu로 마운트된다
```

게스트에서:

```sh
insmod /vnpu/driver/vnpu.ko
ls /dev/accel/                                # accel0
```

장치 이벤트는 `scripts/run.sh -trace 'npu_*'`, 드라이버가 잘못 쓴 레지스터는 `-d guest_errors`로 봅니다.

## 참고한 코드

- Linux `drivers/accel/` — ivpu, amdxdna, ethosu, rocket (probe 흐름, 펌웨어 로딩, BO_WAIT 방식)
- QEMU `hw/nvme/ctrl.c` — CC.EN/CSTS.RDY handshake, 리셋, FLR

## 라이선스

GPL-2.0. 커널 드라이버는 `GPL-2.0-only`, QEMU 장치와 도구는 `GPL-2.0-or-later`입니다
(파일별 SPDX 헤더 참고).
