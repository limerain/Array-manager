#fuser -ck /mnt/tvm
umount /mnt/tvm
lvremove -f /dev/vm/vm
sleep 1
reboot
