#include "RDMAStack.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << " RDMAConnectedSocketImpl "

int RDMAConnectedSocketImpl::globe_seq = 0;

int RDMAConnectedSocketImpl::activate() {
  ibv_qp_attr qpa;
  int r;

  // now connect up the qps and switch to RTR
  memset(&qpa, 0, sizeof(qpa));
  qpa.qp_state = IBV_QPS_RTR;
  qpa.path_mtu = IBV_MTU_1024;
  qpa.dest_qp_num = peer_msg.qpn;
  qpa.rq_psn = peer_msg.psn;
  qpa.max_dest_rd_atomic = 1;
  qpa.min_rnr_timer = 12;
  //qpa.ah_attr.is_global = 0;
  qpa.ah_attr.is_global = 1;
  qpa.ah_attr.grh.hop_limit = 6;
  qpa.ah_attr.grh.dgid = peer_msg.gid;
  qpa.ah_attr.grh.sgid_index = 0;

  qpa.ah_attr.dlid = peer_msg.lid;
  qpa.ah_attr.sl = 0;
  qpa.ah_attr.src_path_bits = 0;
  qpa.ah_attr.port_num = (uint8_t)(infiniband->get_ib_physical_port());

  r = ibv_modify_qp(qp->get_qp(), &qpa, IBV_QP_STATE |
      IBV_QP_AV |
      IBV_QP_PATH_MTU |
      IBV_QP_DEST_QPN |
      IBV_QP_RQ_PSN |
      IBV_QP_MIN_RNR_TIMER |
      IBV_QP_MAX_DEST_RD_ATOMIC);
  if (r) {
    lderr(cct) << __func__ << " failed to transition to RTR state: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }

  ldout(cct, 20) << __func__ << " transition to RTR state successfully." << dendl;

  // now move to RTS
  qpa.qp_state = IBV_QPS_RTS;

  // How long to wait before retrying if packet lost or server dead.
  // Supposedly the timeout is 4.096us*2^timeout.  However, the actual
  // timeout appears to be 4.096us*2^(timeout+1), so the setting
  // below creates a 135ms timeout.
  qpa.timeout = 14;

  // How many times to retry after timeouts before giving up.
  qpa.retry_cnt = 7;

  // How many times to retry after RNR (receiver not ready) condition
  // before giving up. Occurs when the remote side has not yet posted
  // a receive request.
  qpa.rnr_retry = 7; // 7 is infinite retry.
  qpa.sq_psn = my_msg.psn;
  qpa.max_rd_atomic = 1;

  r = ibv_modify_qp(qp->get_qp(), &qpa, IBV_QP_STATE |
      IBV_QP_TIMEOUT |
      IBV_QP_RETRY_CNT |
      IBV_QP_RNR_RETRY |
      IBV_QP_SQ_PSN |
      IBV_QP_MAX_QP_RD_ATOMIC);
  if (r) {
    lderr(cct) << __func__ << " failed to transition to RTS state: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }

  // the queue pair should be ready to use once the client has finished
  // setting up their end.
  ldout(cct, 20) << __func__ << " transition to RTS state successfully." << dendl;

  connected = 1;//indicate successfully
  return 0;
}

ssize_t RDMAConnectedSocketImpl::read(char* buf, size_t len) {
  ldout(cct, 20) << __func__  << " buffers size: " << buffers.size() << " read_counter: " << ++read_counter << " csi" << my_seq  << dendl;
  ssize_t read = 0;

  struct timeval start,end;
  gettimeofday(&start,NULL);
  ldout(cct, 20) << __func__ << " need to read bytes: " << len  << dendl;
  bool got_event = rx_cc->get_cq_event();
  rx_cq->rearm_notify();

  if(!buffers.empty()) 
    read = read_buffers(buf,len);


  const int MAX_COMPLETIONS = 16;
  ibv_wc wc[MAX_COMPLETIONS];

  int tried = 0;
again:
  ++tried;
  int n = rx_cq->poll_cq(MAX_COMPLETIONS, wc);
  if(n)
    ldout(cct, 20) << __func__ << " got " << n << " cqe." << " tried: " << tried << " id: " << read_counter << dendl;

  for(int i=0;i<n;++i){
    ibv_wc* response = &wc[i];
    ldout(cct, 20) << __func__ << " cqe  " << response->byte_len << " bytes." << dendl;
    Chunk* chunk = reinterpret_cast<Chunk *>(response->wr_id);
    chunk->prepare_read(response->byte_len);
    if(!response->byte_len) {
      wait_close = true;
      return 0;
    }
    if (response->status != IBV_WC_SUCCESS){
      lderr(cct) << __func__ << " poll cqe failed! " << " number: " << n << ", status: "<< response->status << cpp_strerror(errno) << dendl;
      assert(0);
    }
    else{
      if(read == len) {
        buffers.push_back(chunk);
        ldout(cct, 20) << __func__ << " buffers add a chunk: " << response->byte_len << dendl;
      }
      else if(read+response->byte_len > len) {
        read += chunk->read(buf+read, len-read);
        buffers.push_back(chunk);
        ldout(cct, 20) << __func__ << " buffers add a chunk: " << chunk->get_offset() << ":" << chunk->get_bound() << dendl;
      }
      else{
        read += chunk->read(buf+read, response->byte_len);
        assert(infiniband->post_chunk(chunk) == 0);
      }
    }
  }

  if (!n && !read)
    goto again;

  gettimeofday(&end,NULL);
  double usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
  ldout(cct, 20) << __func__ << " benchmark time: " << usec << " id: " << read_counter << " , got event: " << got_event << ", tried: " << tried  << dendl;
  return read;
}

ssize_t RDMAConnectedSocketImpl::read_buffers(char* buf, size_t len) {
  size_t read = 0, tmp = 0;
  vector< Chunk* >::iterator c = buffers.begin();
  for(; c != buffers.end() ; ++c) {
    tmp = (*c)->read(buf+read, len-read);
    read += tmp;
    ldout(cct, 20) << __func__ << " this iter read: " << tmp << " bytes." << " offset: " << (*c)->get_offset() << " ,bound: " << (*c)->get_bound()  << ". Chunk:" << *c  << dendl;
    if((*c)->over()) {
      assert(infiniband->post_chunk(*c) == 0);
      ldout(cct, 20) << __func__ << " one chunk over." << dendl;
    }
    if(read == len) {
      break;
    }
  }

  if(c != buffers.end() && (*c)->over())
    c++;
  //if(c != buffers.begin())
    buffers.erase(buffers.begin(), c);
  ldout(cct, 20) << __func__ << " got " << read << " bytes here. buffers size : " << buffers.size() << dendl;
  return read;
}

ssize_t RDMAConnectedSocketImpl::zero_copy_read(bufferptr &data) {
  return 0;
}

ssize_t RDMAConnectedSocketImpl::send(bufferlist &bl, bool more) {
 ldout(cct, 20) << __func__  << " send_msg_counter: " << ++send_counter << " csi" << my_seq << dendl;
  struct timeval start,end;
  gettimeofday(&start,NULL);
  ssize_t bytes = 0;//to send
  list<bufferptr>::const_iterator it = bl.buffers().begin();
  while (it != bl.buffers().end()) {
    bytes += it->length();
    ++it;
  }
  //  ldout(cct, 20) << __func__ << " gonna send " << bytes << " bytes." << dendl;

  vector<Chunk*> tx_buffers = infiniband->get_tx_buffers(bytes);
  ldout(cct, 20) << __func__ << " tx buffer count: " << tx_buffers.size() << dendl;
  vector<Chunk*>::iterator current_buffer = tx_buffers.begin();
  it = bl.buffers().begin();
  while(it != bl.buffers().end()) {
    const uintptr_t addr = reinterpret_cast<const uintptr_t>(it->c_str());
    uint32_t copied = 0;
    //  ldout(cct, 20) << __func__ << " app_buffer: " << addr << " length:  " << it->length()  << dendl;
    while(copied < it->length()) {
      //   ldout(cct, 20) << __func__ << " current_buffer: " << *current_buffer << " copied:  " << copied  << dendl;
      size_t ret = (*current_buffer)->write((char*)addr+copied, it->length() - copied);
      copied += ret;
      //  ldout(cct, 20) << __func__ << " ret: " << ret << " copied:  " << copied  << dendl;
      if((*current_buffer)->full()){
        ++current_buffer;
      }
    }
    ++it;
  }

  post_work_request(tx_buffers);

  ldout(cct, 20) << __func__ << " finished sending " << bytes << " bytes." << dendl;
  bl.clear();
  gettimeofday(&end,NULL);
  float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
  ldout(cct, 20) << __func__ << " benchmark time: " << usec << dendl;
  return bytes;
}

void RDMAConnectedSocketImpl::post_work_request(vector<Chunk*> tx_buffers) {
  vector<Chunk*>::iterator current_buffer = tx_buffers.begin();
  ibv_sge isge[tx_buffers.size()];
  uint32_t current_sge = 0;
  ibv_send_wr iswr[tx_buffers.size()];
  uint32_t current_swr = 0;
  ibv_send_wr* pre_wr = NULL;

  current_buffer = tx_buffers.begin();
  while(current_buffer != tx_buffers.end()) {
    isge[current_sge].addr = reinterpret_cast<uint64_t>((*current_buffer)->buffer);
    isge[current_sge].length = (*current_buffer)->get_offset();
    isge[current_sge].lkey = (*current_buffer)->mr->lkey;
    ldout(cct, 20) << __func__ << " current_buffer: " << *current_buffer << " length:  " << isge[current_sge].length  << dendl;

    iswr[current_swr].wr_id = reinterpret_cast<uint64_t>(*current_buffer);
    iswr[current_swr].next = NULL;
    iswr[current_swr].sg_list = &isge[current_sge];
    iswr[current_swr].num_sge = 1;
    iswr[current_swr].opcode = IBV_WR_SEND;
    iswr[current_swr].send_flags = IBV_SEND_SIGNALED;
    /*if(isge[current_sge].length < infiniband->max_inline_data) {
      iswr[current_swr].send_flags = IBV_SEND_INLINE;
      ldout(cct, 20) << __func__ << " send_inline." << dendl;
      }*/

    if(pre_wr != NULL) {
      pre_wr->next = &iswr[current_swr];
      pre_wr = &iswr[current_swr];
    }
    else {
      pre_wr = &iswr[current_swr];
    }
    ++current_sge;
    ++current_swr;
    ++current_buffer;
  }

  ibv_send_wr *bad_tx_work_request;
  if (ibv_post_send(qp->get_qp(), iswr, &bad_tx_work_request)) {
    lderr(cct) << __func__ << " failed to send message="
      << " ibv_post_send failed(most probably should be peer not ready): "
      << cpp_strerror(errno) << dendl;
    return ;
  }

}

void RDMAConnectedSocketImpl::fin() {
  ibv_sge list = {};
  ibv_send_wr wr;
  wr.wr_id = reinterpret_cast<uint64_t>(this);
  wr.num_sge = 0;
  wr.sg_list = &list;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  ibv_send_wr* bad_tx_work_request;
  if (ibv_post_send(qp->get_qp(), &wr, &bad_tx_work_request)) {
    lderr(cct) << __func__ << " failed to send message="
      << " ibv_post_send failed(most probably should be peer not ready): "
      << cpp_strerror(errno) << dendl;
    return ;
  }
}
