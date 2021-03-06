#!/usr/bin/env bash

v='print *v; print/x *v'
noArgs="$1"
if [ "$noArgs" != "1" ]; then
    #extras="$v; c; $v; c; $v; c; $v; c; fin;"
    extras2="fin;"
    VOL_PTR="(VolumeInformation*)volume_information_pair.first.ac._M_payload"
    args=$(./cmds2 "b _close; run; $extras2 print attributes.size(); $extras print buf; print rec; print attributes; print *$VOL_PTR; print/x *$VOL_PTR")
fi
#args=$(eval echo $args)
IFS=$'\n' arr=( $(xargs -n1 <<<"$args") ) # https://stackoverflow.com/questions/37372225/convert-a-string-into-an-array-with-bash-honoring-quotes-for-grouping
#exit
#batch=--batch
# https://stackoverflow.com/questions/8657648/how-to-have-gdb-exit-if-program-succeeds-break-if-program-crashes
sudo runuser -u my_qemu_upstairs_pc_sim -- gdb $batch -x init.txt ${arr[@]} --args $(eval echo $(cat run.sh | tail -n 1) | xargs -n 1)
