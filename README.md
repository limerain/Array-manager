# Array-manager
Array manager is gaurantee maximum bandwidth of SSD Array(or set of SSDs)

#한글/Korean
--v0.1--
Array-manager(가칭)은 다중 SSD환경을 고려하여 SSD들의 최대 대역폭을 보장해주는 관리 기법입니다.
v0.1은 기본적인 동작을 구현하였으며 아직 버그가 있습니다.

리눅스의 커널 버전에 크게 구애받지 않으며, dm-stripe.c 코드를
/drivers/md/dm-stripe.c와 교체해주시면 됩니다.
커널 컴파일 한 후, LVM2 명령을 이용해서 생성해주시면 됩니다.

아직은 LVM2의 소스 수정을 통해 Array manager만의 생성 명령을 만들지 못했습니다.
따라서 소스 코드명도 dm-stripe.c이며, 기존의 Stripe LVM을 생성하듯 만들어주시면 됩니다.
다음은 예시입니다.
pvcreate /dev/sdb1
pvcreate /dev/sdb2
pvcreate /dev/sdc1
pvcreate /dev/sdc2
vgcreate Arraymanager /dev/sdb1 /dev/sdb2 /dev/sdc1 /dev/sdc2
lvcreate -l 4 -L 40G -n Arraymanager am

물리적으로 다른 SSD를 요구하며 2개 이상 요구되지만, 3개 이상이 최적입니다.
2개일 때는 위와 같이 파티션을 반씩 나눠서 생성해주세요.

v0.1의 알려진 버그 : 
전체 Array 용량이 크면 에러가 발생합니다.
재현율이 100%가 아닌 General Protection Fault 에러와 함께 실패합니다.

#영어/English
