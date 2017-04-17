cd /usr/src/linux-4.3.3
make -j8 && make modules && make modules_install && make install
cd /home/kbs/Documents/tn40xx-0.3.6.12.2
make install
#cp /boot/grub/.grub.cfg /boot/grub/grub.cfg
