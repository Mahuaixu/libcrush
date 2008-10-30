#!/bin/sh

killall cmon cmds crun

for host in `cd dev/hosts ; ls`
do
 ssh root@cosd$host killall cosd \; cd $HOME/ceph/src/dev/hosts/$host \; for f in \* \; do umount $HOME/ceph/src/devm/osd\$f \; done \; rmmod btrfs
done