#!/bin/sh

# run me from the root of a _linux_ git tree, and pass ceph tree root.
cephtree=$1
echo ceph tree at $cephtree.
test -d .git || exit 0
test -e include/linux/mm.h || exit 0
test -e $cephtree/src/kernel/super.h || exit 0

# copy into the tree
mkdir fs/ceph
mkdir fs/ceph/crush
cp $cephtree/src/kernel/Makefile fs/ceph
cp $cephtree/src/kernel/*.[ch] fs/ceph
cp $cephtree/src/kernel/crush/*.[ch] fs/ceph/crush
cp $cephtree/src/kernel/ceph.txt Documentation/filesystems
git apply $cephtree/src/kernel/kbuild.patch

# build the patch sequence
git branch -D series_start
git branch series_start

git add Documentation/filesystems/ceph.txt
git commit -F - <<EOF
ceph: documentation

Mount options, syntax.

EOF

git add fs/ceph/ceph_fs.h
git commit -F - <<EOF
ceph: on-wire types

This header describes the types used to exchange messages between the
Ceph client and various servers.  All types are little-endian and
packed.

Additionally, we define a few magic values to identify the current
version of the protocol(s) in use, so that discrepancies to be
detected on mount.

EOF

git add fs/ceph/types.h
git add fs/ceph/super.h
git commit -F - <<EOF
ceph: client types

We first define constants, types, and prototypes for the kernel client
proper.

A few subsystems are defined separately later: the MDS, OSD, and
monitor clients, and the messaging layer.

EOF

git add fs/ceph/super.c
git commit -F - <<EOF
ceph: super.c

Mount option parsing, client setup and teardown, and a few odds and
ends (e.g., statfs).

EOF


git add fs/ceph/inode.c
git commit -F - <<EOF
ceph: inode operations

Inode cache and inode operations.  We also include routines to
incorporate metadata structures returned by the MDS into the client
cache, and some helpers to deal with file capabilities and metadata
leases.

EOF

git add fs/ceph/dir.c
git commit -F - <<EOF
ceph: directory operations

Directory operations, including lookup, are defined here.  We take
advantage of lookup intents when possible.  For the most part, we just
need to build the proper requests for the metadata server(s) and
pass things off to the mds_client.  

The results of most operations are normally incorporated into the
client's cache when the reply is parsed by ceph_fill_trace().
However, if the MDS replies without a trace (e.g., when retrying an
update after an MDS failure recovery), some operation-specific cleanup
may be needed.

EOF

git add fs/ceph/file.c
git commit -F - <<EOF
ceph: file operations

File open and close operations, and read and write methods that ensure
we have obtained the proper capabilities from the MDS cluster before
performing IO on a file.  We take references on held capabilities for
the duration of the read/write to avoid prematurely releasing them
back to the MDS.

EOF

git add fs/ceph/addr.c
git commit -F - <<EOF
ceph: address space operations

The ceph address space methods are concerned primarily with managing
the dirty page accounting in the inode, which (among other things)
must keep track of which snapshot context each page was dirtied in,
and ensure that dirty data is written out to the OSDs in snapshort
order.

A writepage() on a page that is not currently writeable due to
snapshot writeback ordering (presumably called from kswapd) is
ignored.

EOF

git add fs/ceph/mds_client.h
git add fs/ceph/mds_client.c
git add fs/ceph/mdsmap.h
git add fs/ceph/mdsmap.c
git commit -F - <<EOF
ceph: MDS client

The MDS client is responsible for submitting requests to the MDS
cluster and parsing the response.  We decide which MDS to submit each
request to based on cached information about the current partition of
the directory hierarchy across the cluster.  A stateful session is
opened with each MDS before we submit requests to it.  If a MDS fails
and/or recovers, we resubmit (potentially) affected requests as
needed.

EOF

git add fs/ceph/osd_client.h
git add fs/ceph/osd_client.c
git add fs/ceph/osdmap.h
git add fs/ceph/osdmap.c
git commit -F - <<EOF
ceph: OSD client

The OSD client is responsible for reading and writing data from/to the
object storage pool.  This includes determining where objects are
stored in the cluster, and ensuring that requests are retried or
redirected in the event of a node failure or data migration.

EOF

git add fs/ceph/crush/crush.h
git add fs/ceph/crush/crush.c
git add fs/ceph/crush/mapper.h
git add fs/ceph/crush/mapper.c
git add fs/ceph/crush/hash.h
git commit -F - <<EOF
ceph: CRUSH mapping algorithm

CRUSH is a fancy hash function designed to map inputs onto a dynamic
hierarchy of devices while minimizing the extent to which inputs are
remapped when the devices are added or removed.  It includes some
features that are specifically useful for storage, most notably the
ability to map each input onto a set of N devices that are separated
across administrator-defined failure domains.  CRUSH is used to
distribute data across the cluster of Ceph storage nodes.

More information about CRUSH can be found in this paper:

    http://www.ssrc.ucsc.edu/Papers/weil-sc06.pdf

EOF

git add fs/ceph/mon_client.h
git add fs/ceph/mon_client.c
git commit -F - <<EOF
ceph: monitor client

The monitor cluster is responsible for managing cluster membership
and state.  The monitor client handles what minimal interaction
the Ceph client has with it.

EOF

git add fs/ceph/caps.c
git commit -F - <<EOF
ceph: capability management

The Ceph metadata servers control client access to data by issuing
capabilities granting clients permission to read and/or write to OSDs
(storage nodes).  Each capability consists of a set of bits indicating
which operations are allowed.

EOF

git add fs/ceph/snap.c
git commit -F - <<EOF
ceph: snapshot management

Ceph snapshots rely on client cooperation in determining which
operations apply to which snapshots, and appropriately flushing
snapshotted data and metadata back to the OSD and MDS clusters.
Because snapshots apply to subtrees of the file hierarchy and can be
created at any time, there is a fair bit of bookkeeping required to
make this work.

EOF

git add fs/ceph/decode.h
git add fs/ceph/messenger.h
git add fs/ceph/messenger.c
git commit -F - <<EOF
ceph: messenger library

A generic message passing library is used to communicate with all
other components in the Ceph file system.  The messenger library
provides ordered, reliable delivery of messages between two nodes in
the system, or notifies the higher layer when it is unable to do so.

EOF

git add fs/ceph/export.c
git commit -F - <<EOF
ceph: nfs re-export support

Basic NFS re-export support is included.  This mostly works.  However,
Ceph's MDS design precludes the ability to generate a (small)
filehandle that will be valid forever, so this is of limited utility.

EOF

git add fs/ceph/ioctl.h
git add fs/ceph/ioctl.c
git commit -F - <<EOF
ceph: ioctls

A few Ceph ioctls for getting and setting file layout (striping)
parameters.

EOF

git add fs/ceph/ceph_debug.h
git add fs/ceph/ceph_tools.h
git add fs/ceph/ceph_tools.c
git add fs/ceph/proc.c
git commit -F - <<EOF
ceph: debugging

Some debugging infrastructure, including the ability to adjust the
level of debug output on a per-file basis.  A memory leak check tool
can also be enabled via .config.

EOF

git add fs/ceph/Makefile
git commit -F - <<EOF fs/Kconfig fs/Makefile fs/ceph/Makefile
ceph: Kconfig, Makefile

Kconfig options and Makefile.

EOF


# build the patch files
mkdir out
rm out/*
git-format-patch -s -o out -n series_start..HEAD

cp 0000 out/0000
echo --- >> out/0000
git diff --stat series_start >> out/0000