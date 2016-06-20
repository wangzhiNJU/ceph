#include "Infiniband.h"
#include "common/errno.h"
#include "common/debug.h"
#include "RDMAStack.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "Infiniband "


DeviceList* ResourceManager::devices;
Infiniband* Infiniband::ib;
long Infiniband::send_msg_counter = 0; 
long Infiniband::read_msg_counter = 0;

int Infiniband::set_nonblocking(int fd)
{
  ldout(cct, 20) << __func__ << " fd=" << fd << dendl;
  int flags = ::fcntl(fd, F_GETFL);
  if (flags == -1) {
    lderr(cct) << __func__ << " fcntl F_GETFL failed: "
      << strerror(errno) << dendl;
    return -1;
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
    lderr(cct) << __func__ << " fcntl F_SETFL failed: "
      << strerror(errno) << dendl;
    return -1;
  }
  return 0;
}

/**
 * Create a shared receive queue. This basically wraps the verbs call. 
 *
 * \param[in] max_wr
 *      The max number of outstanding work requests in the SRQ.
 * \param[in] max_sge
 *      The max number of scatter elements per WR.
 * \return
 *      A valid ibv_srq pointer, or NULL on error.
 */
ibv_srq* Infiniband::create_shared_receive_queue(uint32_t max_wr, uint32_t max_sge)
{
  ldout(cct, 20) << __func__ << " max_wr=" << max_wr << " max_sge=" << max_sge << dendl;
  ibv_srq_init_attr sia;
  memset(&sia, 0, sizeof(sia));
  sia.srq_context = device->ctxt;
  sia.attr.max_wr = max_wr;
  sia.attr.max_sge = max_sge;
  return ibv_create_srq(pd->pd, &sia);
}

/**
 * Create a new QueuePair. This factory should be used in preference to
 * the QueuePair constructor directly, since this lets derivatives of
 * Infiniband, e.g. MockInfiniband (if it existed),
 * return mocked out QueuePair derivatives.
 *
 * \return
 *      QueuePair on success or NULL if init fails
 * See QueuePair::QueuePair for parameter documentation.
 */
  Infiniband::QueuePair*
Infiniband::create_queue_pair(ibv_qp_type type)
{
  Infiniband::CompletionChannel* cc = create_comp_channel();
  if (!cc)
    return NULL;

  Infiniband::CompletionQueue* cq = create_comp_queue(cc);
  if (!cq) {
    int r = ibv_destroy_comp_channel(cc->get_channel());
    ldout(cct,20) << __func__ << " failed to create cq, " << " destroy cc result: " << r << dendl;
  }

      RDMAWorker* w = static_cast<RDMAWorker*>(stack->get_worker());
  Infiniband::QueuePair *qp = new QueuePair(*this, type, ib_physical_port, srq, w->get_tx_cq(), cq, max_send_wr, max_recv_wr);
  if (qp->init()) {
    delete qp;
    return NULL;
  }
  return qp;
}

int Infiniband::QueuePair::init()
{
  ldout(infiniband.cct, 20) << __func__ << " started." << dendl;
  ibv_qp_init_attr qpia;
  memset(&qpia, 0, sizeof(qpia));
  qpia.send_cq = txcq->get_cq();
  qpia.recv_cq = rxcq->get_cq();
  qpia.srq = srq;                      // use the same shared receive queue
  qpia.cap.max_send_wr  = max_send_wr; // max outstanding send requests
  qpia.cap.max_send_sge = 1;           // max send scatter-gather elements
  qpia.cap.max_inline_data = infiniband.max_inline_data;          // max bytes of immediate data on send q
  qpia.qp_type = type;                 // RC, UC, UD, or XRC
  qpia.sq_sig_all = 0;                 // only generate CQEs on requested WQEs

  qp = ibv_create_qp(pd, &qpia);
  if (qp == NULL) {
    lderr(infiniband.cct) << __func__ << " failed to create queue pair" << cpp_strerror(errno) << dendl;
    return -1;
  }

  ldout(infiniband.cct, 20) << __func__ << " successfully create queue pair: "
    << "qp=" << qp << dendl;

  // move from RESET to INIT state
  ibv_qp_attr qpa;
  memset(&qpa, 0, sizeof(qpa));
  qpa.qp_state   = IBV_QPS_INIT;
  qpa.pkey_index = 0;
  qpa.port_num   = (uint8_t)(ib_physical_port);
  qpa.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
  qpa.qkey       = q_key;

  int mask = IBV_QP_STATE | IBV_QP_PORT;
  switch (type) {
    case IBV_QPT_RC:
      mask |= IBV_QP_ACCESS_FLAGS;
      mask |= IBV_QP_PKEY_INDEX;
      break;
    case IBV_QPT_UD:
      mask |= IBV_QP_QKEY;
      mask |= IBV_QP_PKEY_INDEX;
      break;
    case IBV_QPT_RAW_PACKET:
      break;
    default:
      assert(0);
  }

  int ret = ibv_modify_qp(qp, &qpa, mask);
  if (ret) {
    ibv_destroy_qp(qp);
    lderr(infiniband.cct) << __func__ << " failed to transition to INIT state: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }
  ldout(infiniband.cct, 20) << __func__ << " successfully change queue pair to INIT:"
    << " qp=" << qp << dendl;
  return 0;
}

int Infiniband::post_chunk(Chunk* chunk){
  ibv_sge isge;
  isge.addr = reinterpret_cast<uint64_t>(chunk->buffer);
  isge.length = chunk->bytes;
  isge.lkey = chunk->mr->lkey;
  ibv_recv_wr rx_work_request;

  memset(&rx_work_request, 0, sizeof(rx_work_request));
  rx_work_request.wr_id = reinterpret_cast<uint64_t>(chunk);// stash descriptor ptr
  rx_work_request.next = NULL;
  rx_work_request.sg_list = &isge;
  rx_work_request.num_sge = 1;

  ibv_recv_wr *badWorkRequest;
  int ret = ibv_post_srq_recv(srq, &rx_work_request, &badWorkRequest);
  if (ret) {
    lderr(cct) << __func__ << " ib_post_srq_recv failed on post "
      << cpp_strerror(errno) << dendl;
    return -1;
  }
  return 0;
}

int Infiniband::post_channel_cluster() {
  int r;
  vector<Chunk*> free_chunks = memory_manager->get_channel_buffers(0);
  for(vector<Chunk*>::iterator iter = free_chunks.begin();iter!=free_chunks.end();++iter){
    r = post_chunk(*iter);
    if(r != 0)
      return r;
  }
  ldout(cct, 20) << __func__ << " posted buffers to srq. "<< dendl;
  return 0;
}

Infiniband::CompletionChannel* Infiniband::create_comp_channel()
{
  ldout(cct, 20) << __func__ << " started." << dendl;
  Infiniband::CompletionChannel *cc = new Infiniband::CompletionChannel(*this);
  if (cc->init()) {
    delete cc;
    return NULL;
  }
  return cc;
}

Infiniband::CompletionQueue* Infiniband::create_comp_queue(CompletionChannel *cc)
{
  ldout(cct, 20) << __func__ << " max_cqe=" << max_cqe << " completion channel=" << cc << dendl;
  Infiniband::CompletionQueue *cq = new Infiniband::CompletionQueue(*this, 1000, cc);
  if (cq->init()) {
    delete cq;
    return NULL;
  }
  return cq;
}


  Infiniband::QueuePair::QueuePair(Infiniband& infiniband, ibv_qp_type type, int port, ibv_srq *srq, Infiniband::CompletionQueue* txcq, Infiniband::CompletionQueue* rxcq, uint32_t max_send_wr, uint32_t max_recv_wr, uint32_t q_key)
: infiniband(infiniband),
  type(type),
  ctxt(infiniband.device->ctxt),
  ib_physical_port(port),
  pd(infiniband.pd->pd),
  srq(srq),
  qp(NULL),
  txcq(txcq),
  rxcq(rxcq),
  initial_psn(0),
  max_send_wr(max_send_wr),
  max_recv_wr(max_recv_wr),
  q_key(q_key)
{
  initial_psn = lrand48() & 0xffffff;
  if (type != IBV_QPT_RC && type != IBV_QPT_UD && type != IBV_QPT_RAW_PACKET) {
    lderr(infiniband.cct) << __func__ << "invalid queue pair type" << cpp_strerror(errno) << dendl;
    assert(0);
  }
  pd = infiniband.pd->pd;
}
// 1 means no valid buffer read, 0 means got enough buffer
// else return < 0 means error
int Infiniband::recv_udp_msg(int sd, IBSYNMsg& im, entity_addr_t *addr)
{
  assert(sd >= 0);
  ssize_t r;
  entity_addr_t socket_addr;
  socklen_t slen = sizeof(socket_addr.ss_addr());
  char msg[sizeof "0000:00000000:00000000:00000000000000000000000000000000"];
  char gid[33];
  r = ::recvfrom(sd, &msg, sizeof(msg), 0,
      reinterpret_cast<sockaddr *>(&socket_addr.ss_addr()), &slen);
  sscanf(msg, "%x:%x:%x:%s", &(im.lid), &(im.qpn), &(im.psn), gid);
  wire_gid_to_gid(gid, &(im.gid));
  ldout(cct, 0) << __func__ << " recevd: " << im.lid << ", " << im.qpn << ", " << im.psn << ", " << gid  << dendl;
  // Drop incoming qpt
  if (cct->_conf->ms_inject_socket_failures && sd >= 0) {
    if (rand() % cct->_conf->ms_inject_socket_failures == 0) {
      ldout(cct, 0) << __func__ << " injecting socket failure" << dendl;
      r = -1;
    }
  }
  if (r == -1) {
    if (errno == EINTR || errno == EAGAIN) {
      return 1;
    } else {
      lderr(cct) << __func__ << " recv got error " << errno << ": "
        << cpp_strerror(errno) << dendl;
      return -1;
    }
  } else if ((size_t)r != sizeof(msg)) { // valid message length
    lderr(cct) << __func__ << " recv got bad length (" << r << ")." << cpp_strerror(errno) << dendl;
    return 1;
  } else { // valid message
    if (addr) {
      *addr = socket_addr;
    }
    return 0;
  }
}

int Infiniband::send_udp_msg(int sd, IBSYNMsg& im, entity_addr_t &peeraddr)
{
  assert(sd >= 0);
  int retry = 0;
  ssize_t r;

  char msg[sizeof "0000:00000000:00000000:00000000000000000000000000000000"];
  char gid[33];
retry:
  gid_to_wire_gid(&(im.gid), gid);
  sprintf(msg, "%04x:%08x:%08x:%s", im.lid, im.qpn, im.psn, gid);
  ldout(cct, 0) << __func__ << " sending: " << im.lid << ", " << im.qpn << ", " << im.psn << ", " << gid  << dendl;
  r = ::sendto(sd, msg, sizeof(msg), 0, reinterpret_cast<sockaddr *>(&peeraddr.ss_addr()),
      sizeof(peeraddr.ss_addr()));
  // Drop incoming qpt
  if (cct->_conf->ms_inject_socket_failures && sd >= 0) {
    if (rand() % cct->_conf->ms_inject_socket_failures == 0) {
      ldout(cct, 0) << __func__ << " injecting socket failure" << dendl;
      r = -1;
    }
  }

  if ((size_t)r != sizeof(msg)) {
    if (r < 0 && (errno == EINTR || errno == EAGAIN) && retry < 3) {
      retry++;
      goto retry;
    }
    if (r < 0)
      lderr(cct) << __func__ << " send returned error " << errno << ": "
        << cpp_strerror(errno) << dendl;
    else
      lderr(cct) << __func__ << " send got bad length (" << r << ") " << cpp_strerror(errno) << dendl;
    return -1;
  }
  return 0;
}

void Infiniband::wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
  char tmp[9];
  uint32_t v32;
  int i;

  for (tmp[8] = 0, i = 0; i < 4; ++i) {
    memcpy(tmp, wgid + i * 8, 8);
    sscanf(tmp, "%x", &v32);
    *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
  }
}

void Infiniband::gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
  int i;
  for (i = 0; i < 4; ++i)
    sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

Infiniband::QueuePair::~QueuePair()
{
  if (qp)
    assert(!ibv_destroy_qp(qp));
  ldout(infiniband.cct, 20) << __func__ << " successfully destroyed QueuePair." << dendl;
}

Infiniband::CompletionChannel::~CompletionChannel()
{
  if (channel) {
    int r = ibv_destroy_comp_channel(channel);
    ldout(infiniband.cct, 20) << __func__ << " r: " << r << dendl;
    assert(r == 0);
  }
  ldout(infiniband.cct, 20) << __func__ << " successfully destroyed CompletionChannel." << dendl;
}

Infiniband::CompletionQueue::~CompletionQueue()
{
  if (cq) {
    int r = ibv_destroy_cq(cq);
    ldout(infiniband.cct, 20) << __func__ << " r: " << cpp_strerror(errno) << dendl;
    assert(r == 0);
  }
  ldout(infiniband.cct, 20) << __func__ << " successfully destroyed CompletionQueue." << dendl;
}

int Infiniband::CompletionQueue::rearm_notify(bool solicite_only)
{
  ldout(infiniband.cct, 20) << __func__ << " started." << dendl;
  int r = ibv_req_notify_cq(cq, 0);
  if (r) {
    lderr(infiniband.cct) << __func__ << " failed to notify cq: " << cpp_strerror(errno) << dendl;
  }
  return r;
}

int Infiniband::CompletionQueue::poll_cq(int num_entries, ibv_wc *ret_wc_array) {
  int r = ibv_poll_cq(cq, num_entries, ret_wc_array);
  if (r < 0) {
    lderr(infiniband.cct) << __func__ << " poll_completion_queue occur met error: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }
  return r;
}

bool Infiniband::CompletionChannel::get_cq_event()
{
//  ldout(infiniband.cct, 21) << __func__ << " started." << dendl;
  ibv_cq *cq = NULL;
  void *ev_ctx;
  if (ibv_get_cq_event(channel, &cq, &ev_ctx)) {
    if (errno != EAGAIN && errno != EINTR)
      lderr(infiniband.cct) << __func__ << "failed to retrieve CQ event: "
        << cpp_strerror(errno) << dendl;
    return false;
  }

  /* accumulate number of cq events that need to
   *    * be acked, and periodically ack them
   *       */
  if (++cq_events_that_need_ack == MAX_ACK_EVENT) {
    ldout(infiniband.cct, 20) << __func__ << " ack aq events." << dendl;
    ibv_ack_cq_events(cq, MAX_ACK_EVENT);
    cq_events_that_need_ack = 0;
  }

  return true;
}

int Infiniband::CompletionQueue::init()
{
  cq = ibv_create_cq(infiniband.device->ctxt, queue_depth, this, channel->get_channel(), 0);
  if (!cq) {
    lderr(infiniband.cct) << __func__ << " failed to create receive completion queue: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }

  if (ibv_req_notify_cq(cq, 0)) {
    lderr(infiniband.cct) << __func__ << " ibv_req_notify_cq failed: " << cpp_strerror(errno) << dendl;
    ibv_destroy_cq(cq);
    return -1;
  }

  channel->bind_cq(cq);
  ldout(infiniband.cct, 20) << __func__ << " successfully create cq=" << cq << dendl;
  return 0;
}

int Infiniband::CompletionChannel::init()
{
  ldout(infiniband.cct, 20) << __func__ << " started." << dendl;
  channel = ibv_create_comp_channel(infiniband.device->ctxt);
  if (!channel) {
    lderr(infiniband.cct) << __func__ << " failed to create receive completion channel: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }
  int rc = infiniband.set_nonblocking(channel->fd);
  if (rc < 0) {
    ibv_destroy_comp_channel(channel);
    return -1;
  }
  return 0;
}

int Infiniband::close() {
  delete memory_manager;
  delete pd;
  assert(ibv_destroy_srq(srq) == 0);
  ResourceManager::close(); 
  ldout(cct, 20) << __func__ << " closed infiniband." << dendl;
}
