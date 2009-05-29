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

#include "include/librados.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, const char **argv) 
{
  if (rados_initialize(argc, argv)) {
    printf("error initializing\n");
    exit(1);
  }

  time_t tm;
  char buf[128], buf2[128];

  time(&tm);
  snprintf(buf, 128, "%s", ctime(&tm));

  struct ceph_object oid;
  memset(&oid, 0, sizeof(oid));
  oid.ino = 0x2010;

  rados_pool_t pool;
  int r = rados_open_pool("data", &pool);
  printf("open pool result = %d, pool = %d\n", r, pool);

  rados_write(pool, &oid, 0, buf, strlen(buf) + 1);
  rados_exec(pool, &oid, "test", "foo", buf, strlen(buf) + 1, buf, 128);
  printf("exec result=%s\n", buf);
  int size = rados_read(pool, &oid, 0, buf2, 128);

  rados_close_pool(pool);

  printf("read result=%s\n", buf2);
  printf("size=%d\n", size);

  rados_deinitialize();

  return 0;
}
