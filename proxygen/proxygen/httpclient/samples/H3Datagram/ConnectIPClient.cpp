#include "ConnectIPClient.h"

#include "proxygen/httpserver/samples/masque/Capsule.h"
#include <folly/executors/GlobalExecutor.h>
#include <iostream>
#include <proxygen/lib/utils/Logging.h>
#include <thread>

#include "proxygen/httpserver/samples/masque/help/MasqueConstants.h"
#include "socket/LayeredSocketGenerator.h"

#include "proxygen/httpserver/samples/masque/help/SignalHandler.h"

// #include <easy/profiler.h>

using namespace std;
using namespace folly;
using namespace proxygen;
using namespace MasqueService::Constants;

namespace MasqueService {

std::size_t FIRST_TUN_NUMBER = 0;

void ConnectIPClient::ConnectIPTunCallback::onPacket(uint8_t* buffer,
                                                     size_t len) noexcept {
  auto packet = IOBuf::copyBuffer(buffer, len);
  socket->getEventBase()->runInEventBaseThread(
      [this, packet = std::move(packet)]() mutable {
        socket->LayeredMasqueSocket::write(address, packet);
      });
}

void ConnectIPClient::ConnectIPTunCallback::onDataAvailable(
    const SocketAddress&,
    size_t len,
    bool,
    AsyncUDPSocket::ReadCallback::OnDataAvailableParams) noexcept {
  if (!tunDevice) {
    LOG(WARNING) << __func__ << ": Tun device not ready";
    return;
  }
  tunDevice->write(readBuffer.data(), len);
}

ConnectIPClient::ConnectIPClient(EventBase* eventBase,
                                 vector<OptionPair> hops,
                                 optional<CIDRNetworkV4> manualTunNetwork,
                                 optional<string> qlogPath)
    : eventBase(eventBase), hops(hops), tunMTU(-1) {
  if (manualTunNetwork) {
    manualGenerator.emplace(*manualTunNetwork);
  }
  CHECK(!hops.empty());
  // build the chain bottom up
  auto baseSocket = make_unique<AsyncUDPSocket>(eventBase);
  for (size_t i = 0; i < hops.size(); i++) {
    auto& hop = hops[i];
    hop.options.maxSendSize_ = hop.UDPSendPacketLen;
    hop.options.maxRecvPacketSize_ = hop.maxRecvPacketSize;
    bool outerMostSocket = (i == hops.size() - 1);
    baseSocket = MasqueService::generateSocket(eventBase,
                                               hop.connectIP,
                                               hop.options,
                                               move(baseSocket),
                                               outerMostSocket);
  }
  tunMTU = hops.back().UDPSendPacketLen - H3_OVERHEAD - CONNECT_IP_OVERHEAD + 28;
  auto* releasedBaseSocket =
      dynamic_cast<LayeredConnectIPSocket*>(baseSocket.release());
  if (!releasedBaseSocket) {
    throw std::runtime_error("Last socket is not a LayeredConnectIPSocket");
  }
  socket.reset(releasedBaseSocket);
  // socket is not connected yet, set qlogpath
  if (qlogPath) {
    socket->setQLogPath(*qlogPath);
  }
}

void ConnectIPClient::createTunDevice(HTTPCodec::StreamID streamID,
                                      const Address::Address& address) {
  // EASY_FUNCTION();
  CHECK(tunDevices.count(streamID) && !tunDevices.at(streamID).first &&
        tunDevices.at(streamID).second);
  // create the tun device
  unique_ptr<TunDevice> tunDevice;
  size_t tunNr = FIRST_TUN_NUMBER + streamID;
  if (manualGenerator) {
    auto ip = manualGenerator->generateSubNet();
    tunDevice = make_unique<TunDevice>("tun_client_" + to_string(tunNr),
                                       make_pair(ip.first, 31),
                                       tunMTU,
                                       address.ipAddress.first.asV4());
  } else {
    tunDevice =
        make_unique<TunDevice>("tun_client_" + to_string(tunNr),
                               make_pair(address.ipAddress.first.asV4(), 31),
                               tunMTU);
  }
  auto* tunPtr = tunDevice.get();
  tunDevice->setReadCallback(tunDevices.at(streamID).second.get());
  tunDevices.at(streamID).first = std::move(tunDevice);
  // run the tun device in a separate thread
  tunPtr->run();
  tunDevices.at(streamID).second->setTunDevice(tunPtr);
}

void ConnectIPClient::start() {
  socket->setReceivedAddressCallback(
      [this](HTTPCodec::StreamID streamID, const Address::Address& address) {
        LOG(INFO) << "Received address: " << address.ipAddress.first.str();
        createTunDevice(streamID, address);
        CHECK(tunDevices.count(streamID) && tunDevices.at(streamID).first);
      });
  LOG(INFO) << "connecting...";
  socket->setNewTransactionCallback([this](HTTPCodec::StreamID streamID) {
    LOG(INFO) << "New transaction: " << streamID;
    auto tunReadCallback = make_unique<ConnectIPTunCallback>(
        nullptr, socket.get(), streamID, hops.back().options.targetAddress_);
    // note: pair.first will be null
    CHECK(!tunDevices.count(streamID));
    tunDevices[streamID].second = std::move(tunReadCallback);
    socket->resumeRead(streamID, tunDevices.at(streamID).second.get());
  });
  socket->connect(hops.back().options.targetAddress_);
}

} // namespace MasqueService
