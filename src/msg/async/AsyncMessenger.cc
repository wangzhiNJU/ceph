// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "acconfig.h"

#include <errno.h>
#include <iostream>
#include <fstream>

#include "AsyncMessenger.h"

#include "common/config.h"
#include "common/Timer.h"
#include "common/errno.h"
#include "auth/Crypto.h"
#include "include/Spinlock.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix _prefix(_dout, this)
static ostream& _prefix(std::ostream *_dout, AsyncMessenger *m) {
  return *_dout << "-- " << m->get_myaddr() << " ";
}

static ostream& _prefix(std::ostream *_dout, Processor *p) {
  return *_dout << " Processor -- ";
}



/*******************
 * Processor
 */

class C_listen_forward : public EventCallback {
  Worker *worker;
  entity_addr_t &addr;
  SocketOptions &options;
  ServerSocket *ss;
  int &ret;
  std::mutex lock;
  std::condition_variable cond;
  bool done = false;

 public:
  C_listen_forward(Worker *w, entity_addr_t &sa, SocketOptions opts,
                   ServerSocket *_ss, int &r)
      : worker(w), addr(sa), options(opts), ss(_ss), ret(r) {
  }
  void do_request(int id) {
    ret = worker->listen(addr, options, ss);
    std::unique_lock<std::mutex> l(lock);
    done = true;
    cond.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> l(lock);
    while (!done)
      cond.wait(l);
  }
};

int Processor::bind(const entity_addr_t &bind_addr, const set<int>& avoid_ports)
{
  const md_config_t *conf = msgr->cct->_conf;
  // bind to a socket
  ldout(msgr->cct, 10) << __func__ << dendl;

  int family;
  switch (bind_addr.get_family()) {
    case AF_INET:
    case AF_INET6:
      family = bind_addr.get_family();
      break;

    default:
      // bind_addr is empty
      family = conf->ms_bind_ipv6 ? AF_INET6 : AF_INET;
  }

  SocketOptions opts;
  opts.nodelay = msgr->cct->_conf->ms_tcp_nodelay;
  opts.rcbuf_size = msgr->cct->_conf->ms_tcp_rcvbuf;

  // use whatever user specified (if anything)
  entity_addr_t listen_addr = bind_addr;
  listen_addr.set_family(family);

  /* bind to port */
  int r = -1;

  for (int i = 0; i < conf->ms_bind_retry_count; i++) {
    if (i > 0) {
      lderr(msgr->cct) << __func__ << " was unable to bind. Trying again in "
                       << conf->ms_bind_retry_delay << " seconds " << dendl;
      sleep(conf->ms_bind_retry_delay);
    }

    if (listen_addr.get_port()) {
      C_listen_forward listen_forward(worker, listen_addr, opts, &listen_socket, r);
      worker->center.dispatch_event_external(&listen_forward);
      listen_forward.wait();
      if (r < 0) {
        lderr(msgr->cct) << __func__ << " unable to listen to " << listen_addr.ss_addr()
                         << ": " << cpp_strerror(r) << dendl;
        continue;
      }
    } else {
      // try a range of ports
      for (int port = msgr->cct->_conf->ms_bind_port_min; port <= msgr->cct->_conf->ms_bind_port_max; port++) {
        if (avoid_ports.count(port))
          continue;

        listen_addr.set_port(port);
        C_listen_forward listen_forward(worker, listen_addr, opts, &listen_socket, r);
        worker->center.dispatch_event_external(&listen_forward);
        listen_forward.wait();
        if (r == 0)
          break;
      }
      if (r < 0) {
        lderr(msgr->cct) << __func__ << " unable to bind to " << listen_addr.ss_addr()
                         << " on any port in range " << msgr->cct->_conf->ms_bind_port_min
                         << "-" << msgr->cct->_conf->ms_bind_port_max << ": "
                         << cpp_strerror(r) << dendl;
        r = -errno;
        listen_addr.set_port(0); // Clear port before retry, otherwise we shall fail again.
        continue;
      }
      ldout(msgr->cct, 10) << __func__ << " bound on random port " << listen_addr << dendl;
    }
    if (r == 0)
      break;
  }
  // It seems that binding completely failed, return with that exit status
  if (r < 0) {
    lderr(msgr->cct) << __func__ << " was unable to bind after " << conf->ms_bind_retry_count
                     << " attempts: " << cpp_strerror(errno) << dendl;
    return r;
  }

  ldout(msgr->cct, 10) << __func__ << " bound to " << listen_addr << dendl;

  msgr->set_myaddr(bind_addr);
  if (bind_addr != entity_addr_t())
    msgr->learned_addr(bind_addr);

  if (msgr->get_myaddr().get_port() == 0) {
    msgr->set_myaddr(listen_addr);
  }
  entity_addr_t addr = msgr->get_myaddr();
  addr.nonce = nonce;
  msgr->set_myaddr(addr);

  msgr->init_local_connection();

  ldout(msgr->cct,1) << __func__ << " bind my_inst.addr is " << msgr->get_myaddr() << dendl;
  return 0;
}

int Processor::rebind(const set<int>& avoid_ports)
{
  ldout(msgr->cct, 1) << __func__ << " rebind avoid " << avoid_ports << dendl;

  entity_addr_t addr = msgr->get_myaddr();
  set<int> new_avoid = avoid_ports;
  new_avoid.insert(addr.get_port());
  addr.set_port(0);

  // adjust the nonce; we want our entity_addr_t to be truly unique.
  nonce += 1000000;
  msgr->my_inst.addr.nonce = nonce;
  ldout(msgr->cct, 10) << __func__ << " new nonce " << nonce << " and inst " << msgr->my_inst << dendl;

  ldout(msgr->cct, 10) << __func__ << " will try " << addr << " and avoid ports " << new_avoid << dendl;
  return bind(addr, new_avoid);
}

int Processor::start()
{
  ldout(msgr->cct, 1) << __func__ << " " << dendl;

  // start thread
  if (listen_socket) {
    worker->center.submit_event([this]() {
      worker->center.create_file_event(listen_socket.fd(), EVENT_READABLE, listen_handler); });
  }

  return 0;
}

void Processor::accept()
{
  ldout(msgr->cct, 10) << __func__ << " listen_fd=" << listen_socket.fd() << dendl;
  int errors = 0;
  SocketOptions opts;
  opts.nodelay = msgr->cct->_conf->ms_tcp_nodelay;
  opts.rcbuf_size = msgr->cct->_conf->ms_tcp_rcvbuf;
  while (errors < 4) {
    entity_addr_t addr;
    ConnectedSocket cli_socket;
    int r = listen_socket.accept(&cli_socket, opts, &addr);
    if (r == 0) {
      errors = 0;
      ldout(msgr->cct, 10) << __func__ << " accepted incoming on sd " << cli_socket.fd() << dendl;

      msgr->add_accept(worker, std::move(cli_socket), addr);
      continue;
    } else {
      if (r == -EINTR) {
        continue;
      } else if (r == -EAGAIN) {
        break;
      } else {
        errors++;
        ldout(msgr->cct, 20) << __func__ << " no incoming connection? "
                             << " errno " << r << " " << cpp_strerror(r) << dendl;
      }
    }
  }
}

void Processor::stop()
{
  ldout(msgr->cct,10) << __func__ << dendl;

  if (listen_socket) {
    worker->center.submit_event([this]() {
      worker->center.delete_file_event(listen_socket.fd(), EVENT_READABLE);
      listen_socket.abort_accept();
    });
  }
}


/*******************
 * AsyncMessenger
 */

struct StackSingleton {
  std::shared_ptr<NetworkStack> stack;
  StackSingleton(CephContext *c) {
    stack = NetworkStack::create(c, c->_conf->ms_async_transport_type);
    stack->start();
  }
  ~StackSingleton() {
    stack->stop();
  }
};

AsyncMessenger::AsyncMessenger(CephContext *cct, entity_name_t name,
                               string mname, uint64_t _nonce, uint64_t features)
  : SimplePolicyMessenger(cct, name,mname, _nonce),
    lock("AsyncMessenger::lock"),
    nonce(_nonce), need_addr(true), did_bind(false),
    global_seq(0), deleted_lock("AsyncMessenger::deleted_lock"),
    cluster_protocol(0), stopped(true)
{
  ceph_spin_init(&global_seq_lock);
  static const string uniq_name = "AsyncMessenger::NetworkStack";
  StackSingleton *single;
  cct->lookup_or_create_singleton_object<StackSingleton>(single, uniq_name);
  stack = single->stack;
  Worker *w = stack->get_worker();
  local_connection = new AsyncConnection(cct, this, w);
  local_worker = w;
  local_features = features;
  init_local_connection();
  reap_handler = new C_handle_reap(this);
  unsigned processor_num = 1;
  if (stack->support_local_listen_table()) {
    processor_num = stack->get_num_worker();
  }
  for (unsigned i = 0; i < processor_num; ++i) {
    processors.push_back(
            new Processor(this, stack->get_worker(i), cct, _nonce));
  }
}

/**
 * Destroy the AsyncMessenger. Pretty simple since all the work is done
 * elsewhere.
 */
AsyncMessenger::~AsyncMessenger()
{
  delete reap_handler;
  assert(!did_bind); // either we didn't bind or we shut down the Processor
  local_connection->mark_down();
  for (auto &&p : processors)
    delete p;
}

void AsyncMessenger::ready()
{
  ldout(cct,10) << __func__ << " " << get_myaddr() << dendl;

  for (auto &&p : processors)
    p->start();
}

int AsyncMessenger::shutdown()
{
  ldout(cct,10) << __func__ << " " << get_myaddr() << dendl;

  // break ref cycles on the loopback connection
  for (auto &&p : processors)
    p->stop();
  mark_down_all();
  local_connection->set_priv(NULL);
  stack->barrier();
  lock.Lock();
  stop_cond.Signal();
  lock.Unlock();
  stopped = true;
  return 0;
}


int AsyncMessenger::bind(const entity_addr_t &bind_addr)
{
  lock.Lock();
  if (started) {
    ldout(cct,10) << __func__ << " already started" << dendl;
    lock.Unlock();
    return -1;
  }
  ldout(cct,10) << __func__ << " bind " << bind_addr << dendl;
  lock.Unlock();

  // bind to a socket
  set<int> avoid_ports;
  int r = 0;
  unsigned i = 0;
  for (auto &&p : processors) {
    r = p->bind(bind_addr, avoid_ports);
    if (r < 0) {
      assert(i == 0);
      break;
    }
    ++i;
  }
  if (r == 0)
    did_bind = true;
  return r;
}

int AsyncMessenger::rebind(const set<int>& avoid_ports)
{
  ldout(cct,1) << __func__ << " rebind avoid " << avoid_ports << dendl;
  assert(did_bind);

  int r = 0;
  for (auto &&p : processors)
    p->stop();
  mark_down_all();
  unsigned i = 0;
  for (auto &&p : processors) {
    r = p->rebind(avoid_ports);
    if (r == 0) {
      p->start();
    } else {
      assert(i == 0);
      break;
    }
  }
  return r;
}

int AsyncMessenger::start()
{
  lock.Lock();
  ldout(cct,1) << __func__ << " start" << dendl;

  // register at least one entity, first!
  assert(my_inst.name.type() >= 0);

  assert(!started);
  started = true;
  stopped = false;

  if (!did_bind) {
    my_inst.addr.nonce = nonce;
    _init_local_connection();
  }

  lock.Unlock();
  return 0;
}

void AsyncMessenger::wait()
{
  lock.Lock();
  if (!started) {
    lock.Unlock();
    return;
  }
  if (!stopped)
    stop_cond.Wait(lock);

  lock.Unlock();

  // done!  clean up.
  ldout(cct,20) << __func__ << ": stopping processor thread" << dendl;
  for (auto &&p : processors)
    p->stop();
  did_bind = false;
  ldout(cct,20) << __func__ << ": stopped processor thread" << dendl;

  // close all connections
  mark_down_all();

  ldout(cct, 10) << __func__ << ": done." << dendl;
  ldout(cct, 1) << __func__ << " complete." << dendl;
  started = false;
}

AsyncConnectionRef AsyncMessenger::add_accept(Worker *w, ConnectedSocket cli_socket, entity_addr_t &addr)
{
  lock.Lock();
  if (!stack->accept_require_same_thread())
    w = stack->get_worker();
  AsyncConnectionRef conn = new AsyncConnection(cct, this, w);
  conn->accept(std::move(cli_socket), addr);
  accepting_conns.insert(conn);
  lock.Unlock();
  return conn;
}

AsyncConnectionRef AsyncMessenger::create_connect(const entity_addr_t& addr, int type)
{
  assert(lock.is_locked());
  assert(addr != my_inst.addr);

  ldout(cct, 10) << __func__ << " " << addr
      << ", creating connection and registering" << dendl;

  // create connection
  Worker *w = stack->get_worker();
  AsyncConnectionRef conn = new AsyncConnection(cct, this, w);
  conn->connect(addr, type);
  assert(!conns.count(addr));
  conns[addr] = conn;
  w->get_perf_counter()->inc(l_msgr_active_connections);

  return conn;
}

ConnectionRef AsyncMessenger::get_connection(const entity_inst_t& dest)
{
  Mutex::Locker l(lock);
  if (my_inst.addr == dest.addr) {
    // local
    return local_connection;
  }

  AsyncConnectionRef conn = _lookup_conn(dest.addr);
  if (conn) {
    ldout(cct, 10) << __func__ << " " << dest << " existing " << conn << dendl;
  } else {
    conn = create_connect(dest.addr, dest.name.type());
    ldout(cct, 10) << __func__ << " " << dest << " new " << conn << dendl;
  }

  return conn;
}

ConnectionRef AsyncMessenger::get_loopback_connection()
{
  return local_connection;
}

int AsyncMessenger::_send_message(Message *m, const entity_inst_t& dest)
{
  ldout(cct, 1) << __func__ << "--> " << dest.name << " "
      << dest.addr << " -- " << *m << " -- ?+"
      << m->get_data().length() << " " << m << dendl;

  if (dest.addr == entity_addr_t()) {
    ldout(cct,0) << __func__ <<  " message " << *m
        << " with empty dest " << dest.addr << dendl;
    m->put();
    return -EINVAL;
  }

  AsyncConnectionRef conn = _lookup_conn(dest.addr);
  submit_message(m, conn, dest.addr, dest.name.type());
  return 0;
}

void AsyncMessenger::submit_message(Message *m, AsyncConnectionRef con,
                                    const entity_addr_t& dest_addr, int dest_type)
{
  if (cct->_conf->ms_dump_on_send) {
    m->encode(-1, MSG_CRC_ALL);
    ldout(cct, 0) << __func__ << "submit_message " << *m << "\n";
    m->get_payload().hexdump(*_dout);
    if (m->get_data().length() > 0) {
      *_dout << " data:\n";
      m->get_data().hexdump(*_dout);
    }
    *_dout << dendl;
    m->clear_payload();
  }

  // existing connection?
  if (con) {
    con->send_message(m);
    return ;
  }

  // local?
  if (my_inst.addr == dest_addr) {
    // local
    static_cast<AsyncConnection*>(local_connection.get())->send_message(m);
    return ;
  }

  // remote, no existing connection.
  const Policy& policy = get_policy(dest_type);
  if (policy.server) {
    ldout(cct, 20) << __func__ << " " << *m << " remote, " << dest_addr
        << ", lossy server for target type "
        << ceph_entity_type_name(dest_type) << ", no session, dropping." << dendl;
    m->put();
  } else {
    ldout(cct,20) << __func__ << " " << *m << " remote, " << dest_addr << ", new connection." << dendl;
    con = create_connect(dest_addr, dest_type);
    con->send_message(m);
  }
}

/**
 * If my_inst.addr doesn't have an IP set, this function
 * will fill it in from the passed addr. Otherwise it does nothing and returns.
 */
void AsyncMessenger::set_addr_unknowns(entity_addr_t &addr)
{
  Mutex::Locker l(lock);
  if (my_inst.addr.is_blank_ip()) {
    int port = my_inst.addr.get_port();
    my_inst.addr.addr = addr.addr;
    my_inst.addr.set_port(port);
    _init_local_connection();
  }
}

int AsyncMessenger::send_keepalive(Connection *con)
{
  con->send_keepalive();
  return 0;
}

void AsyncMessenger::mark_down_all()
{
  ldout(cct,1) << __func__ << " " << dendl;
  lock.Lock();
  for (set<AsyncConnectionRef>::iterator q = accepting_conns.begin();
       q != accepting_conns.end(); ++q) {
    AsyncConnectionRef p = *q;
    ldout(cct, 5) << __func__ << " accepting_conn " << p.get() << dendl;
    p->stop();
  }
  accepting_conns.clear();

  while (!conns.empty()) {
    ceph::unordered_map<entity_addr_t, AsyncConnectionRef>::iterator it = conns.begin();
    AsyncConnectionRef p = it->second;
    ldout(cct, 5) << __func__ << " mark down " << it->first << " " << p << dendl;
    conns.erase(it);
    p->get_perf_counter()->dec(l_msgr_active_connections);
    p->stop();
  }

  {
    Mutex::Locker l(deleted_lock);
    while (!deleted_conns.empty()) {
      set<AsyncConnectionRef>::iterator it = deleted_conns.begin();
      AsyncConnectionRef p = *it;
      ldout(cct, 5) << __func__ << " delete " << p << dendl;
      deleted_conns.erase(it);
    }
  }
  lock.Unlock();
}

void AsyncMessenger::mark_down(const entity_addr_t& addr)
{
  lock.Lock();
  AsyncConnectionRef p = _lookup_conn(addr);
  if (p) {
    ldout(cct, 1) << __func__ << " " << addr << " -- " << p << dendl;
    p->stop();
  } else {
    ldout(cct, 1) << __func__ << " " << addr << " -- connection dne" << dendl;
  }
  lock.Unlock();
}

int AsyncMessenger::get_proto_version(int peer_type, bool connect)
{
  int my_type = my_inst.name.type();

  // set reply protocol version
  if (peer_type == my_type) {
    // internal
    return cluster_protocol;
  } else {
    // public
    if (connect) {
      switch (peer_type) {
        case CEPH_ENTITY_TYPE_OSD: return CEPH_OSDC_PROTOCOL;
        case CEPH_ENTITY_TYPE_MDS: return CEPH_MDSC_PROTOCOL;
        case CEPH_ENTITY_TYPE_MON: return CEPH_MONC_PROTOCOL;
      }
    } else {
      switch (my_type) {
        case CEPH_ENTITY_TYPE_OSD: return CEPH_OSDC_PROTOCOL;
        case CEPH_ENTITY_TYPE_MDS: return CEPH_MDSC_PROTOCOL;
        case CEPH_ENTITY_TYPE_MON: return CEPH_MONC_PROTOCOL;
      }
    }
  }
  return 0;
}

void AsyncMessenger::learned_addr(const entity_addr_t &peer_addr_for_me)
{
  // be careful here: multiple threads may block here, and readers of
  // my_inst.addr do NOT hold any lock.

  // this always goes from true -> false under the protection of the
  // mutex.  if it is already false, we need not retake the mutex at
  // all.
  if (!need_addr)
    return ;
  lock.Lock();
  if (need_addr) {
    need_addr = false;
    entity_addr_t t = peer_addr_for_me;
    t.set_port(my_inst.addr.get_port());
    my_inst.addr.addr = t.addr;
    ldout(cct, 1) << __func__ << " learned my addr " << my_inst.addr << dendl;
    _init_local_connection();
  }
  lock.Unlock();
}

int AsyncMessenger::reap_dead()
{
  ldout(cct, 1) << __func__ << " start" << dendl;
  int num = 0;

  Mutex::Locker l1(lock);
  Mutex::Locker l2(deleted_lock);

  while (!deleted_conns.empty()) {
    auto it = deleted_conns.begin();
    AsyncConnectionRef p = *it;
    ldout(cct, 5) << __func__ << " delete " << p << dendl;
    auto conns_it = conns.find(p->peer_addr);
    if (conns_it != conns.end() && conns_it->second == p)
      conns.erase(conns_it);
    accepting_conns.erase(p);
    deleted_conns.erase(it);
    ++num;
  }

  return num;
}
