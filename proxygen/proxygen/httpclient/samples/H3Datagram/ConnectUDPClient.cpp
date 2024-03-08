#include "ConnectUDPClient.h"

#include <folly/executors/GlobalExecutor.h>
#include <iostream>
#include <proxygen/lib/utils/Logging.h>
#include <thread>

#include <tins/tins.h>

#include "proxygen/httpserver/samples/masque/help/MasqueConstants.h"
#include "socket/LayeredSocketGenerator.h"
#include "proxygen/httpserver/samples/masque/help/SignalHandler.h"

// #include <easy/profiler.h>

using namespace std;
using namespace folly;
using namespace proxygen;
using namespace MasqueService::Constants;

namespace MasqueService {

void ConnectUDPClient::ConnectUDPTunCallback::onPacket(uint8_t* buffer,
                                                       size_t len) noexcept {
  auto payloadInfo = PacketTranslator::udpPayloadInfo(buffer, len);
  if (payloadInfo.len == 0) {
    return;
  }
  if (payloadInfo.srcPort != tunOptions.sourcePort ||
      payloadInfo.dstPort != tunOptions.destinationPort) {
    return;
  }
  uint8_t* payloadBuf = buffer + payloadInfo.startIndex;
  size_t payloadLen = payloadInfo.len;
  auto rawPayload = IOBuf::copyBuffer(payloadBuf, payloadLen);
  socket->getEventBase()->runInEventBaseThread(
      [this, rawPayload = std::move(rawPayload)]() mutable {
        socket->LayeredMasqueSocket::write(address, rawPayload);
      });
}

void ConnectUDPClient::ConnectUDPTunCallback::onDataAvailable(
    const SocketAddress&,
    size_t len,
    bool,
    AsyncUDPSocket::ReadCallback::OnDataAvailableParams) noexcept {
  if (!tunDevice) {
    LOG(WARNING) << __func__ << ": Tun device not ready";
    return;
  }
  // create a new IP packet
  auto packet = Tins::IP(tunDeviceAddress.str(), address.getIPAddress().str()) /
                Tins::UDP(tunOptions.sourcePort, tunOptions.destinationPort) /
                Tins::RawPDU(readBuffer.data(), len);
  // EASY_FUNCTION();
  tunDevice->write(readBuffer.data(), len);
}

ConnectUDPClient::ConnectUDPClient(EventBase* eventBase,
                                   vector<OptionPair> hops,
                                   UDPClientTUNOptions tunOptions,
                                   optional<string> qlogPath)
    : eventBase(eventBase),
      hops(hops),
      subNetGenerator(tunOptions.tunDeviceNetwork),
      tunOptions(tunOptions),
      tunMTU(-1) {
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
  tunMTU = hops.back().UDPSendPacketLen - H3_OVERHEAD +
           28; // 28 is the IP header size
  auto* releasedBaseSocket =
      dynamic_cast<LayeredConnectUDPSocket*>(baseSocket.release());
  if (!releasedBaseSocket) {
    throw std::runtime_error("Last socket is not a LayeredConnectUDPSocket");
  }
  socket.reset(releasedBaseSocket);
  // socket is not connected yet, set qlogpath
  if (qlogPath) {
    socket->setQLogPath(*qlogPath);
  }
}

void ConnectUDPClient::createTunDevice(HTTPCodec::StreamID streamID) {
  // EASY_FUNCTION();
  auto address = subNetGenerator.generateSubNet().first;
  auto tunDevice = make_unique<TunDevice>(
      "tun_client_" + to_string(streamID), make_pair(address, 31), tunMTU);
  auto* tunPtr = tunDevice.get();
  auto tunReadCallback =
      make_unique<ConnectUDPTunCallback>(tunPtr,
                                         socket.get(),
                                         streamID,
                                         hops.back().options.targetAddress_,
                                         tunOptions,
                                         address);
  tunDevice->setReadCallback(tunReadCallback.get());
  tunDevices.emplace(
      streamID, make_pair(std::move(tunDevice), std::move(tunReadCallback)));
  // run the tun device in a separate thread
  tunPtr->run();
  tunDevices.at(streamID).second->setTunDevice(tunPtr);
}

void ConnectUDPClient::start() {
  // EASY_FUNCTION();
  socket->setNewTransactionCallback([this](HTTPCodec::StreamID streamID) {
    LOG(INFO) << "New transaction: " << streamID;
    createTunDevice(streamID);
    socket->resumeRead(streamID, tunDevices.at(streamID).second.get());
  });
  socket->connect(hops.back().options.targetAddress_);
}

} // namespace MasqueService
