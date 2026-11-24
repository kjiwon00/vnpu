# NPU 장치 상태

계약(레지스터, 큐, 명령 형식, 상태 코드)은 `DESIGN.md`와 `driver/npu_hw.h`에 있다.
README 로드맵의 장치 쪽 작업은 끝났다. 남은 일은 드라이버다.

## 구현된 것

| 파일 | 내용 |
|---|---|
| `ctrl.c` | PCIe + FLR, BAR0 레지스터, BAR2 장치 메모리, BAR4 MSI-X, EN/RDY/CFS, 리셋, 속성 |
| `queue.c` | doorbell, SQ/CQ, 요청 풀, CQE 게시와 CQ full 처리, MSI-X 통지, admin 명령 |
| `fw.c` | FW_LOAD: 헤더와 CRC 검사, FW_STS |
| `engine.c` | SUBMIT_JOB: 복사, 전체 검증, DMA/GEMM/RELU/REQUANT, 지연 모델 |

확인: `tests/npu_qtest.py` (게스트 없이 qtest 프로토콜로 59개 항목).
장치를 고치면 이 시험을 다시 돌린다.

펌웨어 이미지: `tools/mkfw.py npu_fw.bin`

## 설계에서 뺀 것

장치를 작게 유지하려고 뺐다. 드라이버 공부에 필요해지면 그때 넣는다.

- **DELETE_SQ / DELETE_CQ**: 큐는 리셋으로만 없앤다.
- **연산 스레드**: 연산은 메인 스레드에서 바로 끝내고, 걸리는 시간은 타이머로 흉내 낸다.
- **장애 주입 중 "완료 지연"**: `job-ns`를 크게 주면 같은 효과가 난다.

## 아직 하지 않은 것

- [ ] 실제 게스트 커널에서의 확인. 지금까지는 qtest로만 봤다. 드라이버 probe를 만들면서
      MSI-X 인터럽트가 실제로 전달되는지, FLR(`echo 1 > .../reset`)이 되는지 확인한다
- [x] `scripts/run.sh`를 `-M q35` + `-device pcie-root-port,id=rp -device npu,bus=rp,id=npu0`로
      고정 (FLR은 root port 아래에서만 동작, `id`가 있어야 qom-set 경로가 생긴다)
- [ ] (선택) QEMU 트리의 정식 qtest(`tests/qtest/npu-test.c`)와 `docs/specs/npu.rst`

## 디버깅

```
-trace 'npu_*'          장치의 모든 이벤트 (doorbell, SQE, CQE, irq, job, fw)
-d guest_errors         드라이버가 잘못 쓴 레지스터, 무시된 doorbell
```
