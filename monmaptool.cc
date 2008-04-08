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
#include <errno.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"

/*

./monmaptool -f .ceph_monmap
./monmaptool -f .ceph_monmap --create --clobber --add 1.2.3.4:12345
./monmaptool -f .ceph_monmap --add 1.2.3.4:12345
./monmaptool -f .ceph_monmap --rm 1.2.3.4:12345

 */

void usage(const char *me)
{
  cout << me << " usage: [--print] [--create [--clobber]] [--add 1.2.3.4:567] [--rm 1.2.3.4:567] <mapfilename>" << std::endl;
  exit(1);
}

void printmap(const char *me, MonMap *m)
{
  cout << me << ": monmap: epoch " << m->epoch << std::endl
       << me << ": monmap: fsid " << m->fsid << std::endl;
  for (unsigned i=0; i<m->mon_inst.size(); i++)
    cout << me << ": monmap:  " //<< "mon" << i << " " 
	 << m->mon_inst[i] << std::endl;
}

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);

  const char *me = argv[0];

  const char *fn = 0;
  bool print = false;
  bool create = false;
  bool clobber = false;
  bool modified = false;
  list<entity_addr_t> add, rm;

  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i], "--print") == 0)
      print = true;
    else if (strcmp(args[i], "--create") == 0) 
      create = true;
    else if (strcmp(args[i], "--clobber") == 0) 
      clobber = true;
    else if (strcmp(args[i], "--add") == 0 ||
	     strcmp(args[i], "--rm") == 0) {
      if (++i >= args.size()) usage(me);
      entity_addr_t addr;
      if (!parse_ip_port(args[i], addr)) {
	cerr << me << ": invalid ip:port '" << args[i] << "'" << std::endl;
	return -1;
      }
      //inst.name = entity_name_t::MON(monmap.size());
      if (strcmp(args[i-1], "--add") == 0)
	add.push_back(addr);
      else 
	rm.push_back(addr);
      modified = true;
    } else if (!fn)
      fn = args[i];
    else 
      usage(me);
  }
  if (!fn)
    usage(me);
  
  MonMap monmap;

  cout << me << ": monmap file " << fn << std::endl;

  int r = 0;
  if (!(create && clobber))
    r = monmap.read(fn);

  if (!create && r < 0) {
    cerr << me << ": couldn't open " << fn << ": " << strerror(errno) << std::endl;
    return -1;
  }    
  else if (create && !clobber && r == 0) {
    cerr << me << ": " << fn << " exists, --clobber to overwrite" << std::endl;
    return -1;
  }

  if (create) {
    monmap.generate_fsid();
    cout << me << ": generated fsid " << monmap.fsid << std::endl;
    modified++;
  }

  for (list<entity_addr_t>::iterator p = add.begin(); p != add.end(); p++)
    monmap.add(*p);
  for (list<entity_addr_t>::iterator p = rm.begin(); p != rm.end(); p++) {
    cout << me << ": removing " << *p << std::endl;
    if (!monmap.remove(*p)) {
      cerr << me << ": map does not contain " << *p << std::endl;
      usage(me);
    }
  }

  if (!print && !modified)
    usage(me);

  if (modified)
    monmap.epoch++;

  if (print) 
    printmap(me, &monmap);

  if (modified) {
    // write it out
    cout << me << ": writing epoch " << monmap.epoch
	 << " to " << fn
	 << " (" << monmap.size() << " monitors)" 
	 << std::endl;
    int r = monmap.write(fn);
    assert(r >= 0);
  }
  

  return 0;
}
