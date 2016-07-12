// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/Cond.h"
#include "common/errno.h"
#include "PosixStack.h"

#include "common/dout.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "stack "

void NetworkStack::add_thread(unsigned i, std::function<void ()> &thread)
{
  Worker *w = workers[i];
  thread = std::move(
    [this, w]() {
      const uint64_t InitEventNumber = 5000;
      const uint64_t EventMaxWaitUs = 30000000;
      w->center.init(InitEventNumber, w->id);
      ldout(cct, 10) << __func__ << " starting" << dendl;
      w->initialize();
      w->init_done();
      while (!w->done) {
        ldout(cct, 30) << __func__ << " calling event process" << dendl;

        int r = w->center.process_events(EventMaxWaitUs);
        if (r < 0) {
          ldout(cct, 20) << __func__ << " process events failed: "
                         << cpp_strerror(errno) << dendl;
          // TODO do something?
        }
      }
      w->reset();
    }
  );
}

std::shared_ptr<NetworkStack> NetworkStack::create(CephContext *c, const string &t)
{
  if (t == "posix")
    return std::shared_ptr<NetworkStack>(new PosixNetworkStack(c, t));

  return nullptr;
}

Worker* NetworkStack::create_worker(CephContext *c, const string &type, unsigned i)
{
  if (type == "posix")
    return new PosixWorker(c, i);
  return nullptr;
}

NetworkStack::NetworkStack(CephContext *c, const string &t): type(t), started(false), cct(c)
{
  num_workers = cct->_conf->ms_async_op_threads;
  for (unsigned i = 0; i < num_workers; ++i) {
    Worker *w = create_worker(cct, type, i);
    workers.push_back(w);
  }
}

void NetworkStack::start()
{
  simple_spin_lock(&pool_spin);
  if (started) {
    simple_spin_unlock(&pool_spin);
    return ;
  }
  for (unsigned i = 0; i < num_workers; ++i) {
    if (workers[i]->is_init())
      continue;
    std::function<void ()> thread;
    add_thread(i, thread);
    spawn_worker(i, std::move(thread));
  }
  started = true;
  simple_spin_unlock(&pool_spin);

  for (unsigned i = 0; i < num_workers; ++i)
    workers[i]->wait_for_init();
}

Worker* NetworkStack::get_worker()
{
  ldout(cct, 10) << __func__ << dendl;

   // start with some reasonably large number
  unsigned min_load = std::numeric_limits<int>::max();
  Worker* current_best = nullptr;

  simple_spin_lock(&pool_spin);
  // find worker with least references
  // tempting case is returning on references == 0, but in reality
  // this will happen so rarely that there's no need for special case.
  for (unsigned i = 0; i < num_workers; ++i) {
    unsigned worker_load = workers[i]->references.load();
    ldout(cct, 20) << __func__ << " Worker " << workers[i] << " load: " << worker_load << dendl;
    if (worker_load < min_load) {
      current_best = workers[i];
      min_load = worker_load;
    }
  }

  // if minimum load exceeds amount of workers, make a new worker
  // logic behind this is that we're not going to create new worker
  // just because others have *some* load, we'll defer worker creation
  // until others have *plenty* of load. This will cause new worker
  // to get assigned to all new connections *unless* one or more
  // of workers get their load reduced - in that case, this worker
  // will be assigned to new connection.
  // TODO: add more logic and heuristics, so connections known to be
  // of light workload (heartbeat service, etc.) won't overshadow
  // heavy workload (clients, etc).
  // if (!current_best || ((num_workers < (unsigned)cct->_conf->ms_async_max_op_threads)
  //     && (min_load > num_workers))) {
  //    ldout(cct, 20) << __func__ << " creating worker" << dendl;
  //    add_thread(num_workers);
  //    spawn_workers(threads);
  //    current_best = workers[num_workers++];
  //    simple_spin_unlock(&pool_spin);
  //    current_best->wait_for_init();
  // } else {
  //   simple_spin_unlock(&pool_spin);
  //   ldout(cct, 20) << __func__ << " picked " << current_best 
  //                  << " as best worker with load " << min_load << dendl;
  // }
  simple_spin_unlock(&pool_spin);

  assert(current_best);
  ++current_best->references;
  return current_best;
}

void NetworkStack::stop()
{
  simple_spin_lock(&pool_spin);
  for (unsigned i = 0; i < num_workers; ++i) {
    workers[i]->done = true;
    workers[i]->center.wakeup();
    join_worker(i);
  }
  started = false;
  simple_spin_unlock(&pool_spin);
}

class C_barrier : public EventCallback {
  Mutex barrier_lock;
  Cond barrier_cond;
  atomic_t barrier_count;

 public:
  explicit C_barrier(size_t c)
      : barrier_lock("C_barrier::barrier_lock"),
        barrier_count(c) {}
  void do_request(int id) {
    Mutex::Locker l(barrier_lock);
    barrier_count.dec();
    barrier_cond.Signal();
  }
  void wait() {
    Mutex::Locker l(barrier_lock);
    while (barrier_count.read())
      barrier_cond.Wait(barrier_lock);
  }
};

void NetworkStack::barrier()
{
  ldout(cct, 10) << __func__ << " started." << dendl;
  pthread_t cur = pthread_self();
  simple_spin_lock(&pool_spin);
  C_barrier barrier(num_workers);
  for (unsigned i = 0; i < num_workers; ++i) {
    assert(cur != workers[i]->center.get_owner());
    workers[i]->center.dispatch_event_external(EventCallbackRef(&barrier));
  }
  simple_spin_unlock(&pool_spin);
  barrier.wait();
  ldout(cct, 10) << __func__ << " end." << dendl;
}
