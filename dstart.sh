#!/bin/bash

let new=0
let debug=0

while [ $# -ge 1 ]; do
        case $1 in
                -d | --debug )
                debug=1
		;;
                --new | -n )
                new=1
        esac
        shift
done


if [ $debug -eq 0 ]; then
	CMON_ARGS="--debug_mon 10 --debug_ms 1"
	COSD_ARGS=""
	CMDS_ARGS=""
else
	echo "** going verbose **"
	CMON_ARGS="--lockdep 1 --debug_mon 20 --debug_ms 1 --debug_paxos 20"
	COSD_ARGS="--lockdep 1 --debug_osd 20 --debug_journal 20 --debug_ms 1" # --debug_journal 20 --debug_osd 20 --debug_filestore 20 --debug_ebofs 20
	CMDS_ARGS="--lockdep 1 --mds_cache_size 500 --mds_log_max_segments 2 --debug_ms 1 --debug_mds 20 --mds_thrash_fragments 0 --mds_thrash_exports 0"
fi


./dstop.sh
rm -f core*

test -d out || mkdir out
rm -f out/*
test -d gmon && ssh cosd0 rm -rf ceph/src/gmon/*

# mkmonfs
if [ $new -eq 1 ]; then
    # figure machine's ip
    HOSTNAME=`hostname`
    IP=`host $HOSTNAME | grep $HOSTNAME | cut -d ' ' -f 4`

    echo hostname $HOSTNAME
    echo "ip $IP"
    if [ `echo $IP | grep '^127\\.'` ]
    then
	echo
	echo "WARNING: hostname resolves to loopback; remote hosts will not be able to"
	echo "  connect.  either adjust /etc/hosts, or edit this script to use your"
	echo "  machine's real IP."
	echo
    fi
    
    # build a fresh fs monmap, mon fs
    ./monmaptool --create --clobber --add $IP:12345 --print .ceph_monmap
    ./mkmonfs --clobber mondata/mon0 --mon 0 --monmap .ceph_monmap
fi

# monitor
./cmon -d mondata/mon0 $CMON_ARGS

if [ $new -eq 1 ]; then
    # build and inject an initial osd map
    ./osdmaptool --clobber --createsimple .ceph_monmap 32 --num_dom 4 .ceph_osdmap

    # use custom crush map to separate data from metadata
    ./crushtool -c cm.txt -o cm
    ./osdmaptool --clobber --import-crush cm .ceph_osdmap

    ./cmonctl osd setmap -i .ceph_osdmap
fi


# osds
for host in `cd dev/hosts ; ls`
do
 ssh root@cosd$host killall cosd

 test -d devm && ssh root@cosd$host modprobe crc32c \; insmod $HOME/src/btrfs-unstable/fs/btrfs/btrfs.ko

 for osd in `cd dev/hosts/$host ; ls`
 do
   dev="dev/hosts/$host/$osd"
   echo "---- host $host osd $osd dev $dev ----"
   devm="$dev"

   # btrfs?
   if [ -d devm ]; then
       devm="devm/osd$osd"
       echo "---- dev mount $devm ----"
       test -d $devm || mkdir -p $devm
       if [ $new -eq 1 ]; then
	   ssh root@cosd$host cd $HOME/ceph/src \; umount $devm \; \
	       $HOME/src/btrfs-progs-unstable/mkfs.btrfs $dev \; \
	       mount -t btrfs $dev $devm
       else
	   ssh root@cosd$host cd $HOME/ceph/src \; mount $dev $devm
       fi
   fi

   if [ $new -eq 1 ]; then
       ssh root@cosd$host cd $HOME/ceph/src \; ./cosd --mkfs_for_osd $osd $devm # --osd_auto_weight 1
   fi
   ssh root@cosd$host cd $HOME/ceph/src \; ulimit -c unlimited \; LD_PRELOAD=./gprof-helper.so ./cosd $devm -d $COSD_ARGS

 done
done

# mds
./cmds -d $CMDS_ARGS


