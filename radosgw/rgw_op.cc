
#include <errno.h>
#include <stdlib.h>

#include <sstream>

#include "include/base64.h"

#include "rgw_access.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_acl.h"
#include "rgw_user.h"

using namespace std;

static int parse_range(const char *range, off_t ofs, off_t end)
{
  int r = -ERANGE;
  string s(range);
  int pos = s.find("bytes=");
  string ofs_str;
  string end_str;

  if (pos < 0)
    goto done;

  s = s.substr(pos + 6); /* size of("bytes=")  */
  pos = s.find('-');
  if (pos < 0)
    goto done;

  ofs_str = s.substr(0, pos);
  end_str = s.substr(pos + 1);
  ofs = atoll(ofs_str.c_str());
  end = atoll(end_str.c_str());

  if (end < ofs)
    goto done;

  r = 0;
done:
  return r;
}

void get_request_metadata(struct req_state *s, map<nstring, bufferlist>& attrs)
{
  map<string, string>::iterator iter;
  for (iter = s->x_amz_map.begin(); iter != s->x_amz_map.end(); ++iter) {
    string name = iter->first;
#define X_AMZ_META "x-amz-meta"
    if (name.find(X_AMZ_META) == 0) {
      cerr << "x>> " << iter->first << ":" << iter->second << std::endl;
      string& val = iter->second;
      bufferlist bl;
      bl.append(val.c_str(), val.size() + 1);
      string attr_name = S3_ATTR_PREFIX;
      attr_name.append(name);
      attrs[attr_name.c_str()] = bl;
    }
  }
}

int read_acls(S3AccessControlPolicy *policy, string& bucket, string& object)
{
  bufferlist bl;
  int ret = 0;

  if (bucket.size()) {
    ret = s3store->get_attr(bucket, object,
                       S3_ATTR_ACL, bl);

    if (ret >= 0) {
      bufferlist::iterator iter = bl.begin();
      policy->decode(iter);
      policy->to_xml(cerr);
    }
  }

  return ret;
}

int read_acls(struct req_state *s, bool only_bucket)
{
  int ret = 0;
  string obj_str;

  if (!s->acl) {
     s->acl = new S3AccessControlPolicy;
     if (!s->acl)
       return -ENOMEM;
  }

  /* we're passed only_bucket = true when we specifically need the bucket's
     acls, that happens on write operations */
  if (!only_bucket)
    obj_str = s->object_str;

  ret = read_acls(s->acl, s->bucket_str, obj_str);

  return ret;
}

void S3GetObj::execute()
{
  if (!verify_permission(s, S3_PERM_READ)) {
    ret = -EACCES;
    goto done;
  }

  ret = get_params();
  if (ret < 0)
    goto done;

  init_common();

  len = s3store->get_obj(s->bucket_str, s->object_str, &data, ofs, end, &attrs,
                         mod_ptr, unmod_ptr, if_match, if_nomatch, get_data, &err);
  if (len < 0)
    ret = len;

done:
  send_response();
}

int S3GetObj::init_common()
{
  if (range_str) {
    int r = parse_range(range_str, ofs, end);
    if (r < 0)
      return r;
  }
  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0)
      return -EINVAL;
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0)
      return -EINVAL;
    unmod_ptr = &unmod_time;
  }

  return 0;
}

void S3ListBuckets::execute()
{
  ret = s3_get_user_buckets(s->user.user_id, buckets);
  if (ret < 0) {
    /* hmm.. something wrong here.. the user was authenticated, so it
       should exist, just try to recreate */
    cerr << "WARNING: failed on s3_get_user_buckets uid=" << s->user.user_id << std::endl;
    s3_put_user_buckets(s->user.user_id, buckets);
    ret = 0;
  }

  send_response();
}

void S3ListBucket::execute()
{
  if (!verify_permission(s, S3_PERM_READ)) {
    ret = -EACCES;
    goto done;
  }

  prefix = s->args.get("prefix");
  marker = s->args.get("marker");
  max_keys = s->args.get("max-keys");
 if (!max_keys.empty()) {
    max = atoi(max_keys.c_str());
  } else {
    max = -1;
  }
  delimiter = s->args.get("delimiter");
  ret = s3store->list_objects(s->user.user_id, s->bucket_str, max, prefix, delimiter, marker, objs, common_prefixes);
done:
  send_response();
}

void S3CreateBucket::execute()
{
  S3AccessControlPolicy policy;
  map<nstring, bufferlist> attrs;
  bufferlist aclbl;

  bool pol_ret = policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!pol_ret) {
    ret = -EINVAL;
    goto done;
  }
  policy.encode(aclbl);

  attrs[S3_ATTR_ACL] = aclbl;

  ret = s3store->create_bucket(s->user.user_id, s->bucket_str, attrs);

  if (ret == 0) {
    S3UserBuckets buckets;

    int r = s3_get_user_buckets(s->user.user_id, buckets);
    S3ObjEnt new_bucket;

    switch (r) {
    case 0:
    case -ENOENT:
    case -ENODATA:
      new_bucket.name = s->bucket_str;
      new_bucket.size = 0;
      time(&new_bucket.mtime);
      buckets.add(new_bucket);
      ret = s3_put_user_buckets(s->user.user_id, buckets);
      break;
    default:
      cerr << "s3_get_user_buckets returned " << ret << std::endl;
      break;
    }
  }
done:
  send_response();
}

void S3DeleteBucket::execute()
{
  ret = -EINVAL;

  if (!verify_permission(s, S3_PERM_WRITE)) {
    abort_early(s, -EACCES);
    return;
  }

  if (s->bucket) {
    ret = s3store->delete_bucket(s->user.user_id, s->bucket_str);

    if (ret == 0) {
      S3UserBuckets buckets;

      int r = s3_get_user_buckets(s->user.user_id, buckets);

      if (r == 0 || r == -ENOENT) {
        buckets.remove(s->bucket_str);
        ret = s3_put_user_buckets(s->user.user_id, buckets);
      }
    }
  }

  send_response();
}

void S3PutObj::execute()
{
  ret = -EINVAL;
  struct s3_err err;
  if (!s->object) {
    goto done;
  } else {
    ret = get_params();
    if (ret < 0)
      goto done;

    S3AccessControlPolicy policy;

    if (!verify_permission(s, S3_PERM_WRITE)) {
      ret = -EACCES;
      goto done;
    }

    bool ret = policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
    if (!ret) {
       err.code = "InvalidArgument";
       ret = -EINVAL;
       goto done;
    }

    char supplied_md5_bin[MD5_DIGEST_LENGTH + 1];
    char supplied_md5[MD5_DIGEST_LENGTH * 2 + 1];
    char calc_md5[MD5_DIGEST_LENGTH * 2 + 1];
    MD5_CTX c;
    unsigned char m[MD5_DIGEST_LENGTH];

    if (supplied_md5_b64) {
      cerr << "supplied_md5_b64=" << supplied_md5_b64 << std::endl;
      int ret = decode_base64(supplied_md5_b64, strlen(supplied_md5_b64),
                                 supplied_md5_bin, sizeof(supplied_md5_bin));
      cerr << "decode_base64 ret=" << ret << std::endl;
      if (ret != MD5_DIGEST_LENGTH) {
        err.code = "InvalidDigest";
        ret = -EINVAL;
        goto done;
      }

      buf_to_hex((const unsigned char *)supplied_md5_bin, MD5_DIGEST_LENGTH, supplied_md5);
      cerr << "supplied_md5=" << supplied_md5 << std::endl;
    }

    MD5_Init(&c);
    MD5_Update(&c, data, (unsigned long)len);
    MD5_Final(m, &c);

    buf_to_hex(m, MD5_DIGEST_LENGTH, calc_md5);

    if (supplied_md5_b64 && strcmp(calc_md5, supplied_md5)) {
       err.code = "BadDigest";
       ret = -EINVAL;
       goto done;
    }
    bufferlist aclbl;
    policy.encode(aclbl);

    string md5_str(calc_md5);
    map<nstring, bufferlist> attrs;
    bufferlist bl;
    bl.append(md5_str.c_str(), md5_str.size() + 1);
    attrs[S3_ATTR_ETAG] = bl;
    attrs[S3_ATTR_ACL] = aclbl;

    if (s->content_type) {
      bl.clear();
      bl.append(s->content_type, strlen(s->content_type) + 1);
      attrs[S3_ATTR_CONTENT_TYPE] = bl;
    }

    get_request_metadata(s, attrs);

    ret = s3store->put_obj(s->user.user_id, s->bucket_str, s->object_str, data, len, NULL, attrs);
  }
done:
  free(data);
  send_response();
}

void S3DeleteObj::execute()
{
  ret = -EINVAL;
  if (s->object) {
    ret = s3store->delete_obj(s->user.user_id, s->bucket_str, s->object_str);
  }

  send_response();
}

static bool parse_copy_source(const char *src, string& bucket, string& object)
{
  string url_src(src);
  string dec_src;

  url_decode(url_src, dec_src);
  src = dec_src.c_str();

  cerr << "decoded src=" << src << std::endl;

  if (*src == '/') ++src;

  string str(src);

  int pos = str.find("/");
  if (pos <= 0)
    return false;

  bucket = str.substr(0, pos);
  object = str.substr(pos + 1);

  if (object.size() == 0)
    return false;

  return true;
}

int S3CopyObj::init_common()
{
  struct s3_err err;
  S3AccessControlPolicy dest_policy;
  bool ret;
  bufferlist aclbl;
  map<nstring, bufferlist> attrs;
  bufferlist bl;
  S3AccessControlPolicy src_policy;
  string empty_str;
  time_t mod_time;
  time_t unmod_time;
  time_t *mod_ptr = NULL;
  time_t *unmod_ptr = NULL;

  if (!verify_permission(s, S3_PERM_WRITE)) {
    ret = -EACCES;
    return ret;
  }

  ret = dest_policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!ret) {
     err.code = "InvalidArgument";
     ret = -EINVAL;
     return ret;
  }

  ret = parse_copy_source(s->copy_source, src_bucket, src_object);
  if (!ret) {
     err.code = "InvalidArgument";
     ret = -EINVAL;
     return ret;
  }
  /* just checking the bucket's permission */
  ret = read_acls(&src_policy, src_bucket, empty_str);
  if (ret < 0)
    return ret;

  if (!verify_permission(&src_policy, s->user.user_id, S3_PERM_READ)) {
    ret = -EACCES;
    return ret;
  }

  dest_policy.encode(aclbl);

  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    unmod_ptr = &unmod_time;
  }

  attrs[S3_ATTR_ACL] = aclbl;
  get_request_metadata(s, attrs);

  return 0;
}

void S3CopyObj::execute()
{
  ret = get_params();
  if (ret < 0)
    goto done;

  if (init_common() < 0)
    goto done;

  ret = s3store->copy_obj(s->user.user_id,
                        s->bucket_str, s->object_str,
                        src_bucket, src_object,
                        &mtime,
                        mod_ptr,
                        unmod_ptr,
                        if_match,
                        if_nomatch,
                        attrs, &err);

done:
  send_response();
}

void S3GetACLs::execute()
{
  if (!verify_permission(s, S3_PERM_READ_ACP)) {
    abort_early(s, -EACCES);
    return;
  }

  int ret = read_acls(s);

  if (ret < 0) {
    /* FIXME */
  }

  stringstream ss;
  s->acl->to_xml(ss);
  acls = ss.str(); 

  send_response();
}

static int rebuild_policy(S3AccessControlPolicy& src, S3AccessControlPolicy& dest)
{
  ACLOwner *owner = (ACLOwner *)src.find_first("Owner");
  if (!owner)
    return -EINVAL;

  S3UserInfo owner_info;
  if (s3_get_user_info(owner->get_id(), owner_info) < 0) {
    cerr << "owner info does not exist" << std::endl;
    return -EINVAL;
  }
  ACLOwner& new_owner = dest.get_owner();
  new_owner.set_id(owner->get_id());
  new_owner.set_name(owner_info.display_name);

  S3AccessControlList& src_acl = src.get_acl();
  S3AccessControlList& acl = dest.get_acl();

  XMLObjIter iter = src_acl.find("Grant");
  ACLGrant *src_grant = (ACLGrant *)iter.get_next();
  while (src_grant) {
    ACLGranteeType& type = src_grant->get_type();
    ACLGrant new_grant;
    bool grant_ok = false;
    string id;
    switch (type.get_type()) {
    case ACL_TYPE_EMAIL_USER:
      {
        string email = src_grant->get_id();
        cerr << "grant user email=" << email << std::endl;
        if (s3_get_uid_by_email(email, id) < 0) {
          cerr << "grant user email not found or other error" << std::endl;
          break;
        }
      }
    case ACL_TYPE_CANON_USER:
      {
        if (type.get_type() == ACL_TYPE_CANON_USER)
          id = src_grant->get_id();
    
        S3UserInfo grant_user;
        if (s3_get_user_info(id, grant_user) < 0) {
          cerr << "grant user does not exist:" << id << std::endl;
        } else {
          ACLPermission& perm = src_grant->get_permission();
          new_grant.set_canon(id, grant_user.display_name, perm.get_permissions());
          grant_ok = true;
          cerr << "new grant: " << new_grant.get_id() << ":" << grant_user.display_name << std::endl;
        }
      }
      break;
    case ACL_TYPE_GROUP:
      {
        string group = src_grant->get_id();
        if (group.compare(S3_URI_ALL_USERS) == 0 ||
            group.compare(S3_URI_AUTH_USERS) == 0) {
          new_grant = *src_grant;
          grant_ok = true;
          cerr << "new grant: " << new_grant.get_id() << std::endl;
        }
      }
    default:
      /* FIXME: implement email based grant */
      break;
    }
    if (grant_ok) {
      acl.add_grant(&new_grant);
    }
    src_grant = (ACLGrant *)iter.get_next();
  }

  return 0; 
}

void S3PutACLs::execute()
{
  bufferlist bl;

  char *data = NULL;
  S3AccessControlPolicy *policy;
  S3XMLParser parser;
  S3AccessControlPolicy new_policy;

  if (!verify_permission(s, S3_PERM_WRITE_ACP)) {
    ret = -EACCES;
    goto done;
  }

  ret = 0;

  if (!parser.init()) {
    ret = -EINVAL;
    goto done;
  }

  if (!s->acl) {
     s->acl = new S3AccessControlPolicy;
     if (!s->acl) {
       ret = -ENOMEM;
       goto done;
     }
  }

  if (get_params() < 0)
    goto done;

  cerr << "read data=" << data << " len=" << len << std::endl;

  if (!parser.parse(data, len, 1)) {
    ret = -EACCES;
    goto done;
  }
  policy = (S3AccessControlPolicy *)parser.find_first("AccessControlPolicy");
  if (!policy) {
    ret = -EINVAL;
    goto done;
  }
  policy->to_xml(cerr);
  cerr << std::endl;

  ret = rebuild_policy(*policy, new_policy);
  if (ret < 0)
    goto done;

  cerr << "new_policy: ";
  new_policy.to_xml(cerr);
  cerr << std::endl;

  /* FIXME: make some checks around checks and fix policy */

  new_policy.encode(bl);
  ret = s3store->set_attr(s->bucket_str, s->object_str,
                       S3_ATTR_ACL, bl);

done:
  free(data);

  send_response();
  return;
}


void S3Handler::init_state(struct req_state *s, struct fcgx_state *fcgx)
{
  this->s = s;

  char *p;
  for (int i=0; (p = fcgx->envp[i]); ++i) {
    cerr << p << std::endl;
  }
  s->fcgx = fcgx;
  s->content_started = false;
  s->indent = 0;
  s->err_exist = false;
  memset(&s->err, 0, sizeof(s->err));
  if (s->acl) {
     delete s->acl;
     s->acl = new S3AccessControlPolicy;
  }
  s->canned_acl.clear();

  provider_init_state();
}

int S3Handler::do_read_permissions(bool only_bucket)
{
  int ret = read_acls(s, only_bucket);

  if (ret < 0)
    cerr << "read_permissions on " << s->bucket_str << ":" <<s->object_str << " only_bucket=" << only_bucket << " ret=" << ret << std::endl;

  return ret;
}

