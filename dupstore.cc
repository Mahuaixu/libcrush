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

#include <iostream>
#include "ebofs/Ebofs.h"
#include "os/FileStore.h"

#include <ext/hash_map>
using __gnu_cxx::hash_map;

int dupstore(ObjectStore* src, ObjectStore* dst)
{
  if (src->mount() < 0) return 1;
  if (dst->mkfs() < 0) return 1;
  if (dst->mount() < 0) return 1;

  // objects
  hash_map<pobject_t, coll_t> did_object;

  // collections
  vector<coll_t> collections;
  src->list_collections(collections);
  int num = collections.size();
  cout << num << " collections" << std::endl;
  int i = 1;
  for (vector<coll_t>::iterator p = collections.begin();
       p != collections.end();
       ++p) {
    cout << "collection " << i++ << "/" << num << " " << hex << *p << dec << std::endl;
    dst->create_collection(*p, 0);
    map<string,bufferptr> attrs;
    src->collection_getattrs(*p, attrs);
    dst->collection_setattrs(*p, attrs);

    vector<pobject_t> o;
    src->collection_list(*p, o);
    int numo = o.size();
    int j = 1;
    for (vector<pobject_t>::iterator q = o.begin(); q != o.end(); q++) {
      if (did_object.count(*q))
	dst->collection_add(*p, did_object[*q], *q, 0);
      else {
	bufferlist bl;
	src->read(*p, *q, 0, 0, bl);
	cout << "object " << j++ << "/" << numo << " " << *q << " = " << bl.length() << " bytes" << std::endl;
	dst->write(*p, *q, 0, bl.length(), bl, 0);
	map<string,bufferptr> attrs;
	src->getattrs(*p, *q, attrs);
	dst->setattrs(*p, *q, attrs);
	did_object[*q] = *p;
      }
    }
  }
  
  src->umount();
  dst->umount();  
  return 0;
}

void usage()
{
  cerr << "usage: dup.ebofs (ebofs|fakestore) src (ebofs|fakestore) dst" << std::endl;
  exit(0);
}

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  parse_config_options(args);

  // args
  if (args.size() != 4) 
    usage();

  ObjectStore *src = 0, *dst = 0;

  if (strcmp(args[0], "ebofs") == 0) 
    src = new Ebofs(args[1]);
  else if (strcmp(args[0], "filestore") == 0) 
    src = new FileStore(args[1]);
  else usage();

  if (strcmp(args[2], "ebofs") == 0) 
    dst = new Ebofs(args[3]);
  else if (strcmp(args[2], "filestore") == 0) 
    dst = new FileStore(args[3]);
  else usage();

  return dupstore(src, dst);
}
