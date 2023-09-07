sudo rmmod max_falk_kmod
sudo insmod max_falk_kmod.ko
sudo chmod 666 /dev/AVM-Task
echo "Hallo das ist ein Test" > /dev/AVM-Task
echo "Linux Kernel Module sind cool" > /dev/AVM-Task
