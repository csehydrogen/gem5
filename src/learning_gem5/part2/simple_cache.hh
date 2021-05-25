#ifndef __LEARNING_GEM5_SIMPLE_CACHE_HH__
#define __LEARNING_GEM5_SIMPLE_CACHE_HH__

#include "mem/port.hh"
#include "mem/mem_object.hh"
#include "params/SimpleCache.hh"
#include "sim/sim_object.hh"

class SimpleCache : public MemObject {
private:
  class CPUSidePort : public SlavePort {
  private:
    int id;
    SimpleCache *owner;
    bool needRetry;
    PacketPtr blockedPacket;
  public:
    CPUSidePort(const std::string& name, int id, SimpleCache *owner) :
        SlavePort(name, owner), id(id), owner(owner), needRetry(false), blockedPacket(nullptr) {}
    AddrRangeList getAddrRanges() const override;
    void sendPacket(PacketPtr pkt);
    void trySendRetry();
  protected:
    Tick recvAtomic(PacketPtr pkt) override { panic("recvAtomic unimpl."); }
    void recvFunctional(PacketPtr pkt) override;
    bool recvTimingReq(PacketPtr pkt) override;
    void recvRespRetry() override;
  };

  class MemSidePort : public MasterPort {
  private:
    SimpleCache *owner;
    PacketPtr blockedPacket;
  public:
    MemSidePort(const std::string& name, SimpleCache *owner) :
        MasterPort(name, owner), owner(owner), blockedPacket(nullptr) {}
    void sendPacket(PacketPtr pkt);
  protected:
    bool recvTimingResp(PacketPtr pkt) override;
    void recvReqRetry() override;
    void recvRangeChange() override;
  };

  AddrRangeList getAddrRanges() const;
  void handleFunctional(PacketPtr pkt);
  void sendRangeChange();
  bool handleRequest(PacketPtr pkt, int port_id);
  bool handleResponse(PacketPtr pkt);
  void accessTiming(PacketPtr pkt);
  void sendResponse(PacketPtr pkt);
  bool accessFunctional(PacketPtr pkt);
  void insert(PacketPtr pkt);

  const Cycles latency;
  const unsigned blockSize;
  const unsigned capacity;
  std::vector<CPUSidePort> cpuPorts;
  MemSidePort memPort;
  bool blocked;
  PacketPtr outstandingPacket;
  int waitingPortId;
  std::unordered_map<Addr, uint8_t*> cacheStore;
  Tick missTime;
  Stats::Scalar hits;
  Stats::Scalar misses;
  Stats::Histogram missLatency;
  Stats::Formula hitRatio;

public:
  SimpleCache(const SimpleCacheParams &params);
  Port& getPort(const std::string &if_name,
                PortID idx=InvalidPortID) override;
  void regStats() override;
};

#endif // __LEARNING_GEM5_SIMPLE_CACHE_HH__