v='print *v'
args=$(./cmds2 "print attributes.size(); $v; c; $v; c; $v; c; $v; c; $v; c")
#args=$(eval echo $args)
IFS=$'\n' arr=( $(xargs -n1 <<<"$args") ) # https://stackoverflow.com/questions/37372225/convert-a-string-into-an-array-with-bash-honoring-quotes-for-grouping
#exit
#batch=--batch
# https://stackoverflow.com/questions/8657648/how-to-have-gdb-exit-if-program-succeeds-break-if-program-crashes
sudo runuser -u my_qemu_upstairs_pc_sim -- gdb $batch -x init.txt -ex 'b _close' -ex 'run' ${arr[@]} -ex 'fin' -ex 'print buf' -ex 'print rec' -ex 'print attributes' --args ./a.out /dev/loop3
