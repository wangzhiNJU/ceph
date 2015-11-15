// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <set>
#include <map>
#include <string>
#include <errno.h>

#include "lmdb.h"

using std::string;
#include "common/perf_counters.h"
#include "common/debug.h"
#include "KeyValueDB.h"
#include "LMDBStore.h"

int LMDBStore::init(string option_str)
{
  options.map_size = g_conf->lmdb_map_size;
  options.max_readers = g_conf->lmdb_max_readers;
  options.noreadahead = g_conf->lmdb_noreadahead;
  options.writemap = g_conf->lmdb_writemap;
  options.nomeminit = g_conf->lmdb_nomeminit;
  return 0;
}

int LMDBStore::do_open(ostream &out, bool create_if_missing)
{
  MDB_txn *txn = NULL;
  int rc;
  unsigned flags = 0;

  if (options.map_size > 0)
    mdb_env_set_mapsize(env.get(), options.map_size);
  if (options.max_readers > 0)
    mdb_env_set_maxreaders(env.get(), options.max_readers);
  if (options.noreadahead)
    flags |= MDB_NORDAHEAD;
  if (options.writemap)
    flags |= MDB_WRITEMAP;
  if (options.nomeminit)
    flags |= MDB_NOMEMINIT;
  if (create_if_missing)
    flags |= MDB_CREATE;
  flags |= MDB_NOTLS;
  flags |= MDB_NOMETASYNC;
  flags |= MDB_NOSYNC;

  rc = mdb_env_open(env.get(), path.c_str(), flags, 0644);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_env_close(env.get());
    return -1;
  }

  rc = mdb_txn_begin(env.get(), NULL, 0, &txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(txn);
    mdb_env_close(env.get());
    return -1;
  }

  rc = mdb_dbi_open(txn, NULL, 0, &dbi);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(txn);
    mdb_env_close(env.get());
    return -1;
  }
  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(txn);
    mdb_dbi_close(env.get(), dbi);
    mdb_env_close(env.get());
    return -1;
  }
  rc = mdb_env_sync(env.get(), 1);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    return -1;
  }

  PerfCountersBuilder plb(g_ceph_context, "lmdb", l_lmdb_first, l_lmdb_last);
  plb.add_u64_counter(l_lmdb_gets, "lmdb_get");
  plb.add_u64_counter(l_lmdb_txns, "lmdb_transaction");
  plb.add_time_avg(l_lmdb_get_latency, "lmdb_get_latency", "Get Latency");
  plb.add_time_avg(l_lmdb_submit_latency, "lmdb_submit_latency", "Submit Latency");
  plb.add_time_avg(l_lmdb_submit_sync_latency, "lmdb_submit_sync_latency", "Submit Sync Latency");
  logger = plb.create_perf_counters();
  cct->get_perfcounters_collection()->add(logger);
  return 0;
}

int LMDBStore::_test_init(const string& dir)
{
  MDB_env *_env;
  MDB_txn *_txn;
  MDB_dbi _dbi;
  int rc;
  string k = "key_test";
  string v = "value_test";
  MDB_val key,value;
  key.mv_size = k.size();
  key.mv_data = (void *)(k.data());
  value.mv_size = v.size();
  value.mv_data = (void *)(v.data());
  rc = mdb_env_create(&_env);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    return -1;
  }
  rc = mdb_env_open(_env, dir.c_str(), MDB_FIXEDMAP, 0644);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_env_close(_env);
    return -EIO;
  }
  rc = mdb_txn_begin(_env, NULL, 0, &_txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(_txn);
    mdb_env_close(_env);
    return -EIO;
  }
  rc = mdb_dbi_open(_txn, NULL, 0, &_dbi);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(_txn);
    mdb_env_close(_env);
    return -EIO;
  }
  rc = mdb_put(_txn, _dbi, &key, &value, MDB_NOOVERWRITE);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(_txn);
    mdb_dbi_close(_env, _dbi);
    mdb_env_close(_env);
    return -EIO;
  }
  rc = mdb_del(_txn, _dbi, &key, NULL);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(_txn);
    mdb_dbi_close(_env, _dbi);
    mdb_env_close(_env);
    return -EIO;
  }
  rc = mdb_txn_commit(_txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(_txn);
    mdb_dbi_close(_env, _dbi);
    mdb_env_close(_env);
    return -EIO;
   }
  mdb_dbi_close(_env, _dbi);
  mdb_env_close(_env);
  return (rc == 0) ? 0 : -EIO;
} 

LMDBStore::LMDBStore(CephContext *c, const string &path) :
  cct(c),
  logger(NULL),
  path(path),
  dbi(0),
  options()
{
  MDB_env *_env;
  int rc = mdb_env_create(&_env);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << dendl;
  }
  env.reset(_env, mdb_env_close);
}

LMDBStore::~LMDBStore()
{
  close();
  mdb_dbi_close(env.get(), dbi);
  if (env.get()) {
    env.reset();
  }
  delete logger;
}

void LMDBStore::close()
{
  if (logger)
    cct->get_perfcounters_collection()->remove(logger);
}

int LMDBStore::submit_transaction(KeyValueDB::Transaction t)
{ 
  int rc;
  utime_t start = ceph_clock_now(g_ceph_context);
  LMDBTransactionImpl * _t =
    static_cast<LMDBTransactionImpl *>(t.get());
  rc = mdb_txn_commit(_t->txn);
  utime_t lat = ceph_clock_now(g_ceph_context) - start;
  logger->inc(l_lmdb_txns);
  logger->tinc(l_lmdb_submit_latency, lat);
  return (rc == 0) ? 0 : -1;
}

int LMDBStore::submit_transaction_sync(KeyValueDB::Transaction t)
{
  int rc;
  utime_t start = ceph_clock_now(g_ceph_context);
  LMDBTransactionImpl * _t =
    static_cast<LMDBTransactionImpl *>(t.get());
  MDB_env *_env = mdb_txn_env(_t->txn);
  rc = mdb_txn_commit(_t->txn);
  rc = mdb_env_sync(_env, 1);
  utime_t lat = ceph_clock_now(g_ceph_context) - start;
  logger->inc(l_lmdb_txns);
  logger->tinc(l_lmdb_submit_sync_latency, lat);
  return (rc == 0) ? 0 : -1;
}

LMDBStore::LMDBTransactionImpl::LMDBTransactionImpl(LMDBStore *_db)
{
  int rc;
  db = _db;
  dbi = db->dbi;
  rc = mdb_txn_begin(db->env.get(), NULL, 0, &txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(txn);
    db = NULL;
    dbi = 0;
  }
  rc = mdb_dbi_open(txn, NULL, 0, &dbi);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    mdb_txn_abort(txn);
  }
}

void LMDBStore::LMDBTransactionImpl::set(
  const string &prefix,
  const string &key,
  const bufferlist &to_set_bl)
{
  buffers.push_back(to_set_bl);
  bufferlist &bl = *(buffers.rbegin());
  string kk = combine_strings(prefix, key);
  keys.push_back(kk);
  MDB_val k, v;
  k.mv_size = keys.rbegin()->size();
  k.mv_data = (void *)keys.rbegin()->data();
  v.mv_size = bl.length();
  v.mv_data = (void *)bl.c_str();
  mdb_put(txn, dbi, &k, &v, 0);
}

void LMDBStore::LMDBTransactionImpl::rmkey(const string &prefix,
					         const string &key)
{
  string kk = combine_strings(prefix, key);
  keys.push_back(kk);
  MDB_val k;
  k.mv_size = keys.rbegin()->size();
  k.mv_data = (void *)keys.rbegin()->data();
  mdb_del(txn, dbi, &k, NULL);
}

void LMDBStore::LMDBTransactionImpl::rmkeys_by_prefix(const string &prefix)
{ 
  KeyValueDB::Iterator it = db->get_iterator(prefix);
  for (it->seek_to_first();
       it->valid();
        it->next()) {
    string key = combine_strings(prefix, it->key());
    keys.push_back(key);
    MDB_val k;
    k.mv_size = keys.rbegin()->size();
    k.mv_data = (void *)keys.rbegin()->data();
    mdb_del(txn, dbi, &k, NULL);
  }
}

int LMDBStore::get(
    const string &prefix,
    const std::set<string> &keys,
    std::map<string, bufferlist> *out)
{
  utime_t start = ceph_clock_now(g_ceph_context);
  KeyValueDB::Iterator it = get_iterator(prefix);
  for (std::set<string>::const_iterator i = keys.begin();
       i != keys.end();
       ++i) {
    it->lower_bound(*i);
    if (it->valid() && it->key() == *i) {
      out->insert(make_pair(*i, it->value()));
     } else if (!it->valid())
      break;
  }
  utime_t lat = ceph_clock_now(g_ceph_context) - start;
  logger->inc(l_lmdb_gets);
  logger->tinc(l_lmdb_get_latency, lat);
  return 0;
} 

string LMDBStore::combine_strings(const string &prefix, const string &value)
{
  string out = prefix;
  out.push_back(0);
  out.append(value);
  return out;
}

bufferlist LMDBStore::to_bufferlist(string &in)
{
  bufferlist bl;
  bl.append(bufferptr(in.data(), in.size()));
  return bl;
}

int LMDBStore::split_key(string &in, string *prefix, string *key)
{
  size_t prefix_len = 0;
  const char* in_data = in.c_str();

  // Find separator inside Slice
  char* separator = (char*) memchr((const void*)in_data, 1, in.size());
  if (separator == NULL)
     return -EINVAL;
  prefix_len = size_t(separator - in_data);
  if (prefix_len >= in.size())
    return -EINVAL;

  // Fetch prefix and/or key directly from Slice
  if (prefix)
    *prefix = string(in_data, prefix_len);
  if (key)
    *key = string(separator+1, in.size()-prefix_len-1);
  return 0;
}

bool LMDBStore::check_omap_dir(string &omap_dir)
{ 
  int rc = _test_init(omap_dir); 
  return (rc == 0) ? true : false;
}
LMDBStore::LMDBWholeSpaceIteratorImpl::LMDBWholeSpaceIteratorImpl(LMDBStore *store)
{
  int rc = 1;
  MDB_env *env = store->env.get();
  MDB_dbi dbi = store->dbi;
  rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  rc = mdb_dbi_open(txn, NULL, 0, &dbi);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  rc = mdb_cursor_open(txn, dbi, &cursor);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  invalid = false;
}
LMDBStore::LMDBWholeSpaceIteratorImpl::~LMDBWholeSpaceIteratorImpl()
{
  if (cursor) {
    mdb_cursor_close(cursor);
    cursor = NULL;
  }
  if (txn) { 
    mdb_txn_commit(txn);
    txn = NULL;
  }
}
int LMDBStore::LMDBWholeSpaceIteratorImpl::seek_to_first()
{
  invalid = false;
  int rc = mdb_cursor_get(cursor, NULL, NULL, MDB_FIRST);
  return (rc == 0) ? 0 : -1;
}
int LMDBStore::LMDBWholeSpaceIteratorImpl::seek_to_first(const string &prefix)
{
  int rc;
  MDB_val key;
  invalid = false;
  if (prefix.size() == 0) {
    rc = mdb_cursor_get(cursor, &key, NULL, MDB_FIRST);
  } else {
    key.mv_size = prefix.size();
    key.mv_data = (void *)prefix.data();
    rc = mdb_cursor_get(cursor, &key, NULL, MDB_SET_RANGE);
  }
  return (rc == 0) ? 0 : -1;
}
int LMDBStore::LMDBWholeSpaceIteratorImpl::seek_to_last()
{
  invalid = false;
  int rc = mdb_cursor_get(cursor, NULL, NULL, MDB_LAST);
  return (rc == 0) ? 0 : -1;
}
int LMDBStore::LMDBWholeSpaceIteratorImpl::seek_to_last(const string &prefix)
{
  int rc;
  invalid = false;
  MDB_val key;
  string limit = past_prefix(prefix);
  if (prefix.size() == 0) {
    invalid = true;
    rc = 0;
  } else {
    key.mv_size = limit.size();
    key.mv_data = (void *)limit.data();
    rc = mdb_cursor_get(cursor, &key, NULL, MDB_SET_RANGE);
    if (rc != 0) {
      rc = mdb_cursor_get(cursor, NULL, NULL, MDB_LAST);
    } else {
      rc = mdb_cursor_get(cursor, NULL, NULL, MDB_PREV);
      if (rc != 0) {
        invalid = true;
        rc = 0;
      }
    }
  }
  return (rc == 0) ? 0 : 1;
}
int LMDBStore::LMDBWholeSpaceIteratorImpl::upper_bound(const string &prefix, const string &after)
{
  int rc;
  invalid = false;
  rc = lower_bound(prefix, after);
  if (valid()) {
    pair<string,string> key = raw_key();
    if (key.first == prefix && key.second == after)
      rc = next();
  }
  return (rc == 0) ? 0 : 1;
}

int LMDBStore::LMDBWholeSpaceIteratorImpl::lower_bound(const string &prefix, const string &to)
{
  int rc;
  invalid = false;
  MDB_val key;
  string bound = combine_strings(prefix, to);
  key.mv_size = bound.size();
  key.mv_data = (void *)bound.data();
  rc = mdb_cursor_get(cursor, &key, NULL, MDB_SET_RANGE);
  if (rc != 0) {
    invalid = true;
    rc = mdb_cursor_get(cursor, NULL, NULL, MDB_LAST);
  }
  return (rc == 0) ? 0 : 1;
}

bool LMDBStore::LMDBWholeSpaceIteratorImpl::valid()
{
  if (invalid)
    return false;
  int rc = mdb_cursor_get(cursor, NULL, NULL, MDB_GET_CURRENT);
  return (rc == 0) ? true : false;
}

int LMDBStore::LMDBWholeSpaceIteratorImpl::next()
{
  int rc = 1;
  if (valid())
    rc = mdb_cursor_get(cursor, NULL, NULL, MDB_NEXT);
  if (rc != 0) {
    invalid = true;
  }
  return 0;
}

int LMDBStore::LMDBWholeSpaceIteratorImpl::prev()
{
  int rc = 1;
  if (valid())
    rc = mdb_cursor_get(cursor, NULL, NULL, MDB_PREV);
  if (rc != 0) {
    invalid = true;
  }
  return 0;
}

string LMDBStore::LMDBWholeSpaceIteratorImpl::key()
{
  string key, out_key;
  MDB_val k, v;
  int rc = mdb_cursor_get(cursor, &k, &v, MDB_GET_CURRENT);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  key = string((const char *)k.mv_data, k.mv_size);
  split_key(key, 0, &out_key);
  return out_key;
}

pair<string,string> LMDBStore::LMDBWholeSpaceIteratorImpl::raw_key()
{
  string prefix, key, out;
  MDB_val k, v;
  int rc = mdb_cursor_get(cursor, &k, &v, MDB_GET_CURRENT);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  out = string((const char *)k.mv_data, k.mv_size);
  split_key(out, &prefix, &key);
  return make_pair(prefix, key);
}

bool LMDBStore::LMDBWholeSpaceIteratorImpl::raw_key_is_prefixed(const string &prefix) {
  MDB_val k, v;

  int rc = mdb_cursor_get(cursor, &k, &v, MDB_GET_CURRENT);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
    return false;
  }

  const char* data = (const char*)k.mv_data;
  if ((k.mv_size > prefix.length()) && (data[prefix.length()] == '\1')) {
    return memcmp(data, prefix.c_str(), prefix.length()) == 0;
  } else {
    return false;
  }
}

bufferlist LMDBStore::LMDBWholeSpaceIteratorImpl::value()
{
  MDB_val k, v;
  string value;
  int rc = mdb_cursor_get(cursor, &k, &v, MDB_GET_CURRENT);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  value = string((const char *)v.mv_data, v.mv_size);
  return to_bufferlist(value);
}

int LMDBStore::LMDBWholeSpaceIteratorImpl::status()
{
  int rc = mdb_cursor_get(cursor, NULL, NULL, MDB_GET_CURRENT);
  if (rc != 0) {
    derr << __FILE__ << ":" << __LINE__ << " " << mdb_strerror(rc) << dendl;
  }
  return (rc == 0) ? 0 : -1;
}

string LMDBStore::past_prefix(const string &prefix)
{
  string limit = prefix;
  limit.push_back(1);
  return limit;
}


LMDBStore::WholeSpaceIterator LMDBStore::_get_iterator()
{
  return std::shared_ptr<KeyValueDB::WholeSpaceIteratorImpl>(
    new LMDBWholeSpaceIteratorImpl(this)
  );
}
