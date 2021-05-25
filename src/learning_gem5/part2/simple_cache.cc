#include "learning_gem5/part2/simple_cache.hh"

#include "base/random.hh"
#include "base/trace.hh"
#include "debug/SimpleCache.hh"
#include "sim/system.hh"

SimpleCache::SimpleCache(const SimpleCacheParams &params) :
    MemObject(params),
    latency(params.latency),
    blockSize(params.system->cacheLineSize()),
    capacity(params.size / blockSize),
    memPort(params.name + ".mem_side", this),
    blocked(false), outstandingPacket(nullptr), waitingPortId(-1) {
  for (int i = 0; i < params.port_cpu_side_connection_count; ++i) {
    cpuPorts.emplace_back(name() + csprintf(".cpu_side[%d]", i), i, this);
  }
}

Port& SimpleCache::getPort(const std::string& if_name, PortID idx) {
  if (if_name == "mem_side") {
    panic_if(idx != InvalidPortID, "Mem side of simple cache not a vector port");
    return memPort;
  } else if (if_name == "cpu_side" && idx < cpuPorts.size()) {
    return cpuPorts[idx];
  } else {
    return MemObject::getPort(if_name, idx);
  }
}

AddrRangeList SimpleCache::CPUSidePort::getAddrRanges() const {
  return owner->getAddrRanges();
}

AddrRangeList SimpleCache::getAddrRanges() const {
  DPRINTF(SimpleCache, "Sending new ranges\n");
  return memPort.getAddrRanges();
}

void SimpleCache::CPUSidePort::recvFunctional(PacketPtr pkt) {
  return owner->handleFunctional(pkt);
}

void SimpleCache::handleFunctional(PacketPtr pkt) {
  memPort.sendFunctional(pkt);
}

void SimpleCache::MemSidePort::recvRangeChange() {
  owner->sendRangeChange();
}

void SimpleCache::sendRangeChange() {
  for (auto& port : cpuPorts) {
    port.sendRangeChange();
  }
}

bool SimpleCache::CPUSidePort::recvTimingReq(PacketPtr pkt) {
  if (!owner->handleRequest(pkt, id)) {
    needRetry = true;
    return false;
  }
  return true;
}

bool SimpleCache::handleRequest(PacketPtr pkt, int port_id) {
  if (blocked) {
    return false;
  }
  DPRINTF(SimpleCache, "Got request for addr %#x\n", pkt->getAddr());
  blocked = true;
  waitingPortId = port_id;
  schedule(
    new EventFunctionWrapper([this, pkt]{ accessTiming(pkt); }, name() + ".accessEvent", true),
    clockEdge(latency));
  return true;
}

void SimpleCache::accessTiming(PacketPtr pkt) {
  bool hit = accessFunctional(pkt);
  if (hit) {
    hits++;
    pkt->makeResponse();
    sendResponse(pkt);
  } else {
    misses++;
    missTime = curTick();
    Addr addr = pkt->getAddr();
    Addr block_addr = pkt->getBlockAddr(blockSize);
    unsigned size = pkt->getSize();
    if (addr == block_addr && size == blockSize) {
      DPRINTF(SimpleCache, "forwarding packet\n");
      memPort.sendPacket(pkt);
    } else {
      DPRINTF(SimpleCache, "Upgrading packet to block size\n");
      panic_if(addr - block_addr + size > blockSize,
          "Cannot handle accesses that span multiple cache lines");
      assert(pkt->needsResponse());
      MemCmd cmd;
      if (pkt->isWrite() || pkt->isRead()) {
        cmd = MemCmd::ReadReq;
      } else {
        panic("Unknown packet type in upgrade size");
      }
      PacketPtr new_pkt = new Packet(pkt->req, cmd, blockSize);
      new_pkt->allocate();
      outstandingPacket = pkt;
      memPort.sendPacket(new_pkt);
    }
  }
}

void SimpleCache::sendResponse(PacketPtr pkt) {
  int port = waitingPortId;
  blocked = false;
  waitingPortId = -1;
  cpuPorts[port].sendPacket(pkt);
  for (auto& port : cpuPorts) {
    port.trySendRetry();
  }
}

void SimpleCache::MemSidePort::sendPacket(PacketPtr pkt) {
  panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");
  if (!sendTimingReq(pkt)) {
    blockedPacket = pkt;
  }
}

void SimpleCache::MemSidePort::recvReqRetry() {
  assert(blockedPacket != nullptr);
  PacketPtr pkt = blockedPacket;
  blockedPacket = nullptr;
  sendPacket(pkt);
}

bool SimpleCache::MemSidePort::recvTimingResp(PacketPtr pkt) {
  return owner->handleResponse(pkt);
}

bool SimpleCache::handleResponse(PacketPtr pkt) {
  assert(blocked);
  DPRINTF(SimpleCache, "Got response for addr %#x\n", pkt->getAddr());
  insert(pkt);
  missLatency.sample(curTick() - missTime);

  if (outstandingPacket != nullptr) {
    accessFunctional(outstandingPacket);
    outstandingPacket->makeResponse();
    delete pkt;
    pkt = outstandingPacket;
    outstandingPacket = nullptr;
  }

  sendResponse(pkt);

  return true;
}

void SimpleCache::CPUSidePort::sendPacket(PacketPtr pkt) {
  panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");
  if (!sendTimingResp(pkt)) {
    blockedPacket = pkt;
  }
}

void SimpleCache::CPUSidePort::recvRespRetry() {
  assert(blockedPacket != nullptr);
  PacketPtr pkt = blockedPacket;
  blockedPacket = nullptr;
  sendPacket(pkt);
}

void SimpleCache::CPUSidePort::trySendRetry() {
  if (needRetry && blockedPacket == nullptr) {
    needRetry = false;
    DPRINTF(SimpleCache, "Sending retry req for %d\n", id);
    sendRetryReq();
  }
}

bool SimpleCache::accessFunctional(PacketPtr pkt) {
  Addr block_addr = pkt->getBlockAddr(blockSize);
  auto it = cacheStore.find(block_addr);
  if (it != cacheStore.end()) {
    if (pkt->isWrite()) {
      pkt->writeDataToBlock(it->second, blockSize);
    } else if (pkt->isRead()) {
      pkt->setDataFromBlock(it->second, blockSize);
    } else {
      panic("Unknown packet type!");
    }
    return true;
  }
  return false;
}

void SimpleCache::insert(PacketPtr pkt) {
  if (cacheStore.size() >= capacity) {
    int bucket, bucket_size;
    do {
      bucket = random_mt.random(0, (int)cacheStore.bucket_count() - 1);
    } while ((bucket_size = cacheStore.bucket_size(bucket)) == 0);
    auto block = std::next(cacheStore.begin(bucket), random_mt.random(0, bucket_size - 1));

    DPRINTF(SimpleCache, "Removing addr %#x\n", block->first);

    RequestPtr req = std::make_shared<Request>(block->first, blockSize, 0, 0);
    PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
    new_pkt->dataDynamic(block->second);

    DPRINTF(SimpleCache, "Writing packet back %s\n", pkt->print());
    memPort.sendPacket(new_pkt);
    cacheStore.erase(block->first);
  }

  uint8_t* data = new uint8_t[blockSize];
  cacheStore[pkt->getAddr()] = data;
  pkt->writeDataToBlock(data, blockSize);
}

void SimpleCache::regStats() {
  MemObject::regStats();
  hits.name(name() + ".hits").desc("Number of hits");
  misses.name(name() + ".misses").desc("Number of misses");
  missLatency.name(name() + ".missLatency").desc("Tickes for misses to the cache").init(16);
  hitRatio.name(name() + ".hitRatio").desc("The ratio of hits to the total accesses to the cache");
  hitRatio = hits / (hits + misses);
}