// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"
#include "mon/MonClient.h"

#include "osd/OSD.h"
#include "ebofs/Ebofs.h"

#include "msg/SimpleMessenger.h"

#include "common/Timer.h"

void usage() 
{
  cerr << "usage: cosd <device> [-m monitor] [--mkfs_for_osd <nodeid>]" << std::endl;
  cerr << "   -d              daemonize" << std::endl;
  cerr << "   --debug_osd N   set debug level (e.g. 10)" << std::endl;
  cerr << "   --debug_ms N    set message debug level (e.g. 1)" << std::endl;
  cerr << "   --ebofs         use EBOFS for object storage (default)" << std::endl;
  cerr << "   --fakestore     store objects as files in directory <device>" << std::endl;
  exit(1);
}


int main(int argc, const char **argv) 
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);
  parse_config_options(args);

  if (g_conf.clock_tare) g_clock.tare();

  // osd specific args
  const char *dev = 0;
  int whoami = -1;
  bool mkfs = 0;
  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i],"--mkfs_for_osd") == 0) {
      mkfs = 1; 
      whoami = atoi(args[++i]);
    } else if (strcmp(args[i],"--dev") == 0) 
      dev = args[++i];
    else if (!dev)
      dev = args[i];
    else {
      cerr << "unrecognized arg " << args[i] << std::endl;
      usage();
    }
  }
  if (!dev) {
    cerr << "must specify device file" << std::endl;
    usage();
  }

  if (mkfs && whoami < 0) {
    cerr << "must specify '--osd #' where # is the osd number" << std::endl;
    usage();
  }

  // get monmap
  MonMap monmap;
  MonClient mc;
  if (mc.get_monmap(&monmap) < 0)
    return -1;

  if (mkfs) {
    int err = OSD::mkfs(dev, monmap.fsid, whoami);
    if (err < 0) {
      cerr << "error creating empty object store in " << dev << ": " << strerror(-err) << std::endl;
      exit(1);
    }
    cout << "created object store for osd" << whoami << " fsid " << monmap.fsid << " on " << dev << std::endl;
    exit(0);
  }

  if (whoami < 0) {
    whoami = OSD::peek_whoami(dev);
    if (whoami < 0) {
      cerr << "unable to determine OSD identity from superblock on " << dev << ": " << strerror(-whoami) << std::endl;
      exit(1);
    }
  }

  _dout_create_courtesy_output_symlink("osd", whoami);

  // start up network
  rank.bind();

  cout << "starting osd" << whoami
       << " at " << rank.get_rank_addr() 
       << " dev " << dev
       << std::endl;

  g_timer.shutdown();

  rank.start();

  rank.set_policy(entity_name_t::TYPE_MON, Rank::Policy::lossy_fast_fail());
  rank.set_policy(entity_name_t::TYPE_OSD, Rank::Policy::lossless());

  // make a _reasonable_ effort to send acks/replies to requests, but
  // don't get carried away, as the sender may go away and we won't
  // ever hear about it.
  rank.set_policy(entity_name_t::TYPE_MDS, Rank::Policy::lossy_fast_fail());
  rank.set_policy(entity_name_t::TYPE_CLIENT, Rank::Policy::lossy_fast_fail());

  // start osd
  Messenger *m = rank.register_entity(entity_name_t::OSD(whoami));
  assert(m);
  OSD *osd = new OSD(whoami, m, &monmap, dev);
  osd->init();

  rank.wait();

  // done
  delete osd;

  // cd on exit, so that gmon.out (if any) goes into a separate directory for each node.
  char s[20];
  sprintf(s, "gmon/%d", getpid());
  if (mkdir(s, 0755) == 0)
    chdir(s);

  return 0;
}

