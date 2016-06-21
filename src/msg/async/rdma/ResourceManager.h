#ifndef CEPH_MSG_RESOURCEMANAGER_H
#define CEPH_MSG_RESOURCEMANAGER_H
#include <infiniband/verbs.h>
#include <string>
#include <cstring>
#include "common/debug.h"
#include "common/errno.h"
using std::string;

class CephContext;
class DeviceList;
class Device;
class Port;

class Port {
  CephContext *cct;
  struct ibv_context* ctxt;
  uint8_t port_num;
  struct ibv_port_attr* port_attr;
  int gid_tbl_len;
  uint16_t lid;
  union ibv_gid gid;

  public:
  explicit Port(CephContext *c, struct ibv_context* ictxt, uint8_t ipn): cct(c), ctxt(ictxt), port_num(ipn), port_attr(new ibv_port_attr) {
    if(ctxt == NULL) {
      lderr(cct) << __func__ << " ibv_context is null." << cpp_strerror(errno) << dendl;
    }
    query_port();
    lid = port_attr->lid;
    query_gid();
  }

  int query_port() {
    int r = ibv_query_port(ctxt, port_num, port_attr);
    if(r == -1) {
      lderr(cct) << __func__  << " query port failed  " << cpp_strerror(errno) << dendl;
    }
    return r;
  }

  void query_gid() {
    int r = ibv_query_gid(ctxt, port_num, 0, &gid);
    if(r) {
      lderr(cct) << __func__  << " query gid failed  " << cpp_strerror(errno) << dendl;
    }
  }

  uint16_t get_lid() { return lid; }
  ibv_gid  get_gid() { return gid; }

  uint8_t get_port_num() { return port_num; }

  ibv_port_attr* get_port_attr() { return port_attr; }
};


class Device {
  CephContext *cct;
  ibv_device *device;
  const char* name;
  uint8_t  port_cnt;
  Port** ports;
  public:
  explicit Device(CephContext *c, ibv_device* d): cct(c), device(d), device_attr(new ibv_device_attr) {
    if(device == NULL) {
      lderr(cct) << __func__ << "device == NULL" << cpp_strerror(errno) << dendl;
    }
    name = ibv_get_device_name(device);
    ctxt = ibv_open_device(device);
    if(ctxt == NULL) {
      lderr(cct) << __func__ << "open rdma device failed. " << cpp_strerror(errno) << dendl;
    }
    int r = ibv_query_device(ctxt, device_attr);
    if(r == -1) {
      lderr(cct) << __func__ << " failed to query rdma device. " << cpp_strerror(errno) << dendl;
    }
    port_cnt = device_attr->phys_port_cnt;
    ports = new Port*[port_cnt];
    for(uint8_t i = 0;i < port_cnt; ++i) {
      ports[i] = new Port(cct, ctxt, i+1);
      if(ports[i]->get_port_attr()->state == IBV_PORT_ACTIVE) {
        active_port = ports[i];
      }
    }
  }

  ~Device() {
    assert(ibv_close_device(ctxt) == 0);
  }
  const char* get_name() { return name;}
  uint16_t get_lid() { return active_port->get_lid(); }
  ibv_gid get_gid() { return active_port->get_gid(); }
  struct ibv_context *ctxt;
  ibv_device_attr *device_attr;
  Port* active_port;
};

class DeviceList {
  CephContext *cct;
  struct ibv_device ** device_list;
  int num;
  Device** devices;
  public:
  DeviceList(CephContext *c): cct(c), device_list(ibv_get_device_list(&num)) {
    if(!c)
      return ;
    if(device_list == NULL || num == 0) {
      lderr(cct) << __func__ << " failed to get rdma device list.  " << cpp_strerror(errno) << dendl;
    }
    devices = new Device*[num];

    for(int i = 0;i < num; ++i) {
      devices[i] = new Device(cct, device_list[i]);
    }
  }
  ~DeviceList() {
    for(int i=0; devices[i] != NULL; ++i) {
      delete devices[i];
    }
    ibv_free_device_list(device_list);
  }

  Device* get_device(const char* device_name) {
    assert(devices);
    for(Device* r = devices[0]; r; ++r) {
      if(!strcmp(device_name, r->get_name())) {
        return r;
      }
    }

    return NULL;
  }
};

class ResourceManager {
  private:
    static DeviceList* devices;

  public:
    static Device* get_device(const char* device_name) {
      return devices->get_device(device_name);
    }
    static void refresh(CephContext* cct) {
      devices = new DeviceList(cct);
    }

    static void close() {
      delete devices;
    }
};

#endif
