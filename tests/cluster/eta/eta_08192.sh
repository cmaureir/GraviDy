#!/bin/bash
#PBS -q gpum
#PBS -N eta_8192
#PBS -l walltime=999:00:00


file=07-nbody-p8192_m1.in
etas=(0.001 0.003 0.005 0.007 0.01 0.03 0.05 0.07 0.1)

cd /user/c/cmaureir/GraviDy/

for e in "${etas[@]}"
do
    (time gpu/./gravidy -i input/$file -t 1 -e $e)
done

