#blktrace -d /dev/vm/vm -o - | blkparse -i - > swan.log
blktrace -d /dev/vm/vm &
blktrace -a issue -d /dev/sda1 &
blktrace -a issue -d /dev/sdc1 &
blktrace -a issue -d /dev/sdd1 &
#blktrace -a issue -d /dev/sde1 &
#blktrace -a issue -d /dev/sdf1 &
