#! /bin/bash
#if [ -a /dev/strp/vm ]
#then
#banner Remove_VM!
lvremove -f /dev/vm/vm
#fi

banner Create_VM!
lvcreate -i 4 -I 64 -l 100%FREE -n vm vm
#lvcreate -i 5 -I 64 -l 100%FREE -n vm vm
#lvcreate -l 100%FREE -n vm vm
#lvcreate --type raid5 -i 3 -I 64 -l 100%FREE -n vm vm
#pvcreate /dev/sdb1 /dev/sdc1
#vgcreate strp /dev/sdb1 /dev/sdc1
#mirroring
#lvcreate -m 1 --type raid1 -l 50%VG --nosync -n vm1 vm1
#echo -e '0 62531248 volumemanager /dev/sdb1 0'\\n'62531248 62531248 volumemanager /dev/sdc1 0' | sudo dmsetup create tvm
sleep 1
mkfs.ext4 /dev/vm/vm
sleep 1
mount -o discard /dev/vm/vm /mnt/tvm
#mount /dev/vm/vm /mnt/tvm
chown -R kbs:kbs /mnt/tvm/
