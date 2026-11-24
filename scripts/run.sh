#!/bin/sh
# QEMU 게스트를 띄운다. 레포 디렉터리가 게스트의 /vnpu로 마운트된다 (9p, mount_tag=vnpu).
#   게스트에서: insmod /vnpu/driver/vnpu.ko
#
# 경로는 환경 변수로 바꿀 수 있다.
#   QEMU_BUILD  QEMU 빌드 디렉터리   (기본 ~/workspace/qemu/build)
#   KERNEL      게스트 커널 bzImage   (기본 ~/workspace/linux/arch/x86/boot/bzImage)
#   INITRD      initramfs            (기본 ~/workspace/rootfs.cpio.gz)
# 추가 인자는 QEMU에 그대로 넘긴다. 예: scripts/run.sh -trace 'npu_*'
set -e
VNPU=$(cd "$(dirname "$0")/.." && pwd)
QEMU_BUILD=${QEMU_BUILD:-$HOME/workspace/qemu/build}
KERNEL=${KERNEL:-$HOME/workspace/linux/arch/x86/boot/bzImage}
INITRD=${INITRD:-$HOME/workspace/rootfs.cpio.gz}

exec "$QEMU_BUILD/qemu-system-x86_64" -L "$QEMU_BUILD/../pc-bios" \
  -M q35 -m 1G -nographic \
  -accel kvm -accel tcg \
  -kernel "$KERNEL" -initrd "$INITRD" \
  -append "console=ttyS0 nokaslr" \
  -virtfs local,path="$VNPU",mount_tag=vnpu,security_model=none \
  -device pcie-root-port,id=rp \
  -device npu,bus=rp,id=npu0 \
  "$@"
