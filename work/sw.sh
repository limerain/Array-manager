seekwatcher -t dm-0.blktrace.0 -o swan.png
#blkparse -i dm-0 -o swan.log

seekwatcher -t sdb1.blktrace.0 -o sdb.png

seekwatcher -t sda1.blktrace.0 -o sda.png
#blkparse -i sda1 -o sda.log

seekwatcher -t sdc1.blktrace.0 -o sdc.png
#blkparse -i sdc1 -o sdc.log

seekwatcher -t sdd1.blktrace.0 -o sdd.png
#blkparse -i sdd1 -o sdd.log

#seekwatcher -t sde1.blktrace.0 -o sde.png
#blkparse -i sde1 -o sde.log

#seekwatcher -t sdf1.blktrace.0 -o sdf.png
#blkparse -i sdf1 -o sdf.log

mv *.png *.log ~kbs/
rm *.blktrace.*
