# scullpg
Variable size chunk of memory that implements page-sized blocks.

## Testing
    make

    sudo insmod scullpg.ko

    sudo chmod g+rw /dev/scull_pg

    sudo chmod o+rw /dev/scull_pg

    echo -n "BienVenue" > /dev/scull_pg

    dd bs=20 count=5 if=/dev/scull_pg of=~/scullpgout.txt

    vim /home/lym/scullpgout.txt
