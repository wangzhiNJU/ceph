#include "RDMAStack.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << " RDMAServerSocketImpl "

int RDMAServerSocketImpl::listen(entity_addr_t &sa, const SocketOptions &opt) {
  server_setup_socket = ::socket(sa.get_family(), SOCK_DGRAM, 0);
  if(server_setup_socket == -1) {
    lderr(cct) << __func__ << " failed to create server socket: "
      << cpp_strerror(errno) << dendl;
    return -1;
  }

  int on = 1;
  int rc = ::setsockopt(server_setup_socket, SOL_SOCKET, SO_REUSEADDR,
      &on, sizeof(on));
  if (rc < 0) {
    lderr(cct) << __func__ << " unable to setsockopt: "
      << cpp_strerror(errno) << dendl;
    goto err;
  }

  rc = ::bind(server_setup_socket, (struct sockaddr *)&sa.ss_addr(),
      sa.addr_size());
  if (rc < 0) {
    lderr(cct) << __func__ << " unable to bind to " << sa.ss_addr()
      << " on port " << sa.get_port()
      << ": " << cpp_strerror(errno) << dendl;
    goto err;
  }

  /*int flags;
  if ((flags = fcntl(server_setup_socket, F_GETFL)) < 0 ) {
    lderr(cct) << __func__ << " fcntl(F_GETFL) failed: %s" << cpp_strerror(errno) << dendl;
    return -errno;
  }
  if (fcntl(server_setup_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
    lderr(cct) << __func__ << " fcntl(F_SETFL,O_NONBLOCK): %s" << cpp_strerror(errno) << dendl;
    return -errno;
  }*/

  ldout(cct, 20) << __func__ << " bind to " << sa.ss_addr() << " on port " << sa.get_port()  << dendl;
  return 0;

err:
  ::close(server_setup_socket);
  server_setup_socket = -1;
  return -1;
}

int RDMAServerSocketImpl::accept(ConnectedSocket *sock, const SocketOptions &opts, entity_addr_t *out)
{
  ldout(cct, 15) << __func__ << dendl;
  int r;
  RDMAConnectedSocketImpl* server;
  while (1) {
    IBSYNMsg msg;//TODO
    entity_addr_t addr;
    r = infiniband->recv_udp_msg(server_setup_socket, msg, &addr);
    if (r < 0) {
      ldout(cct, 0) << __func__ << " recv msg failed." << dendl;
      break;
    } else if (r > 0) {
      continue;
    } else {
      //RDMAWorker* w = static_cast<RDMAWorker*>(infiniband->stack->get_worker());
      server = new RDMAConnectedSocketImpl(cct, infiniband, NULL, msg);
      msg = server->get_my_msg();
      r = infiniband->send_udp_msg(server_setup_socket, msg, addr);
      server->activate();
      std::unique_ptr<RDMAConnectedSocketImpl> csi(server);
      *sock = ConnectedSocket(std::move(csi));
      if(out)
        *out = sa;
      return r;
    }
  }

  return -1;
}

