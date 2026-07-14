#!/bin/bash

DEV=$1
if [ -z "$DEV" ]; then
    echo "Usage: $0 <nvme device>"
    exit 1
fi

DEV_PATH="/dev/$DEV"

sudo ./scripts/setup.sh reset

PCI_ALLOWED=$(readlink -f /sys/block/$DEV | awk -F'/' '{print $6}')

echo "Resetting device $DEV_PATH with PCI BDF $PCI_ALLOWED"

sudo umount /mnt/nvme &>/dev/null

sudo mkfs -t ext4 -E lazy_itable_init=0,lazy_journal_init=0 -F $DEV_PATH
# sudo mkfs -t f2fs -l f2fs -f $DEV_PATH
# sudo mkfs.xfs -f $DEV_PATH

sudo mkdir -p /mnt/nvme
sudo mount $DEV_PATH /mnt/nvme
sudo rm -r /mnt/nvme/test
sudo umount /mnt/nvme

sleep 1

sudo NRHUGE=48 HUGENODE=0 HUGEPGSZ=1048576 PCI_ALLOWED=$PCI_ALLOWED ./scripts/setup.sh
sudo NRHUGE=48 HUGENODE=1 HUGEPGSZ=1048576 PCI_ALLOWED=$PCI_ALLOWED ./scripts/setup.sh

sudo rm -f /mnt/huge_1GB/lkl*

