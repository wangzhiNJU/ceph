#include "RDMAStack.h"
#include "include/str_list.h"
#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "RDMAStack "

int RDMAWorker::listen(entity_addr_t &sa, const SocketOptions &opt,ServerSocket *sock)
{
  assert(sock);
  /*client_setup_socket = ::socket(sa.get_family(), SOCK_DGRAM, 0);
    if(client_setup_socket == -1) {
    lderr(cct) << __func__ << " failed to create server socket: "
    << cpp_strerror(errno) << dendl;
    return -1;
    }

    int on = 1;
    int rc = ::setsockopt(client_setup_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (rc < 0) {
    lderr(cct) << __func__ << " unable to setsockopt: "
    << cpp_strerror(errno) << dendl;
    return -1;
    }

    int flags;
    if ((flags = fcntl(client_setup_socket, F_GETFL)) < 0 ) {
    lderr(cct) << __func__ << " fcntl(F_GETFL) failed: %s" << cpp_strerror(errno) << dendl;
    return -errno;
    }
    if (fcntl(client_setup_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
    lderr(cct) << __func__ << " fcntl(F_SETFL,O_NONBLOCK): %s" << cpp_strerror(errno) << dendl;
    return -errno;
    }*/

  auto p = new RDMAServerSocketImpl(cct, infiniband, sa);
  int r = p->listen(sa, opt);
  if (r < 0) {
    delete p;
    return r;
  }

  *sock = ServerSocket(std::unique_ptr<ServerSocketImpl>(p));
  return 0;
}

void RDMAWorker::connect(const entity_addr_t &peer_addr) {
  client_setup_socket = ::socket(PF_INET, SOCK_DGRAM, 0);
  if (client_setup_socket == -1) {
    lderr(cct) << __func__ << " failed to create client socket: "
      << strerror(errno) << dendl;
    goto fail;
  }

  int  r;
  r = ::connect(client_setup_socket, (sockaddr*)(&peer_addr.addr), peer_addr.addr_size());
  if (r < 0) {
    lderr(cct) << __func__ << " failed to connect " << peer_addr << ": "
      << strerror(errno) << dendl;
    goto fail;
  }

  int flags;
  /* Set the socket nonblocking.
   *          * Note that fcntl(2) for F_GETFL and F_SETFL can't be
   *                   * interrupted by a signal. */
  /*  if ((flags = fcntl(client_setup_socket, F_GETFL)) < 0 ) {
      lderr(cct) << __func__ << " fcntl(F_GETFL) failed: " << cpp_strerror(errno) << dendl;
      goto fail;
      }
      if (fcntl(client_setup_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
      lderr(cct) << __func__ << " fcntl(F_SETFL,O_NONBLOCK): " << cpp_strerror(errno) << dendl;
      goto fail;
      }
      */
fail:
  return ;
}

int RDMAWorker::connect(const entity_addr_t &addr, const SocketOptions &opts, ConnectedSocket *socket)
{
  RDMAConnectedSocketImpl* p = new RDMAConnectedSocketImpl(cct, infiniband, this);
  entity_addr_t sa;
  memcpy(&sa, &addr, sizeof(addr));

  IBSYNMsg msg = p->get_my_msg();
  ldout(cct, 20) << __func__ << " connecting to " << sa.ss_addr() << " : " << sa.get_port() << dendl;
  ldout(cct, 20) << __func__ << " my syn msg :  < " << msg.qpn << ", " << msg.psn <<  ", " << msg.lid << ">"<< dendl;

  connect(addr);

  ldout(cct, 20) << __func__ << infiniband << dendl;
  int r = infiniband->send_udp_msg(client_setup_socket, msg, sa); 
  if (r < 0) {
    ldout(cct, 0) << __func__ << " send msg failed." << dendl;
    return r;
  }

  r = infiniband->recv_udp_msg(client_setup_socket, msg, &sa);
  if (r < 0) {
    ldout(cct, 0) << __func__ << " recv msg failed." << dendl;
    return r;
  }
  p->set_peer_msg(msg);
  ldout(cct, 20) << __func__ << " peer msg :  < " << msg.qpn << ", " << msg.psn <<  ", " << msg.lid << "> " << dendl;
  r = p->activate();
  assert(!r);
  std::unique_ptr<RDMAConnectedSocketImpl> csi(p);
  *socket = ConnectedSocket(std::move(csi));
  ldout(cct, 0) << __func__ << " fd: " << socket->fd() << dendl;

  return 0;
}

RDMAStack::RDMAStack(CephContext *cct, const string &t): NetworkStack(cct, t), infiniband(NULL) {
  Infiniband::create_infiniband(this, cct,"mlx4_0");
  infiniband = Infiniband::get_infiniband();

  vector<string> corestrs;
  get_str_vec(cct->_conf->ms_async_affinity_cores, corestrs);
  for (vector<string>::iterator it = corestrs.begin();
      it != corestrs.end(); ++it) {
    string err;
    int coreid = strict_strtol(it->c_str(), 10, &err);
    if (err == "")
      coreids.push_back(coreid);
    else
      lderr(cct) << __func__ << " failed to parse " << *it << " in " << cct->_conf->ms_async_affinity_cores << dendl;
  }

  for (vector<Worker*>::iterator it = workers.begin(); it != workers.end(); ++it) {
    (static_cast<RDMAWorker*>(*it))->set_ib(infiniband);
  }
}

void RDMAWorker::initialize() { 
  tx_cc = infiniband->create_comp_channel();
  tx_cq = infiniband->create_comp_queue(tx_cc);
  center.create_file_event(tx_cc->get_fd(), EVENT_READABLE, tx_handler);
}

void RDMAWorker::handle_tx_event() {
  ldout(cct, 20) << __func__ << dendl;
  bool got_event = tx_cc->get_cq_event();
  tx_cq->rearm_notify();

  static const int MAX_COMPLETIONS = 16;
  static ibv_wc wc[MAX_COMPLETIONS];

  bool rearmed = false;
again:
  int n = tx_cq->poll_cq(MAX_COMPLETIONS, wc);
  ldout(cct, 20) << __func__ << " pool completion queue got " << n
    << " responses."<< dendl;
  for(int i=0;i<n;++i){
    ibv_wc* response = &wc[i];
    Chunk* chunk = reinterpret_cast<Chunk *>(response->wr_id);
    ldout(cct, 20) << __func__ << " opcode: " << response->opcode << " len: " << response->byte_len << dendl;
    if (response->status != IBV_WC_SUCCESS){
      lderr(cct) << __func__ << " poll cqe failed! " << " number: " << n << ", status: "<< response->status << cpp_strerror(errno) << dendl;
      assert(0);
    }

    if(memory_manager->is_tx_chunk(chunk))
      infiniband->get_memory_manager()->return_tx(chunk); 
    else {
      ldout(cct, 20) << __func__ << " chunk belongs to none " << dendl;
    }
  }

  if (n == MAX_COMPLETIONS)
    goto again;

  /*if (!rearmed) {
    tx_cq->rearm_notify();
    rearmed = true;
  // Clean up cq events after rearm notify ensure no new incoming event
  //     // arrived between polling and rearm
  goto again;
  }*/
  ldout(cct, 20) << __func__ << " leaving handle_tx_event. " << dendl;
}
