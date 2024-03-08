#pragma once

#include "help/MasqueUtils.h"
#include "tuntap/PacketUtils.h"
#include "tuntap/TunManager.h"
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <functional>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <quic/server/QuicServer.h>

namespace MasqueService {

class SharedTun : public proxygen::TunDevice::ReadCallback {

 private:
  std::unique_ptr<proxygen::TunDevice> tunDevice;
  std::atomic_size_t subnetCounter;
  // ip -> callback
  folly::ConcurrentHashMap<std::uint32_t, proxygen::TunDevice::ReadCallback*>
      streamMap;

 public:
  explicit SharedTun(std::unique_ptr<proxygen::TunDevice> tunDevice)
      : tunDevice(std::move(tunDevice)), subnetCounter(0) {
    CHECK(this->tunDevice->getTunSubnet().second <= 24);
    this->tunDevice->setReadCallback(this);
  };

 public:
  void write(std::uint8_t* buf, std::size_t len) {
    // assume that the src ip has been set correctly
    tunDevice->write(buf, len);
  }

  void onPacket(std::uint8_t* buf, std::size_t len) noexcept override {
    auto dstIP = MasqueService::PacketTranslator::dstIP(buf, len);
    if (!dstIP) {
      return;
    }
    if (streamMap.find(*dstIP) == streamMap.end()) {
      auto ip = folly::IPAddressV4::fromLongHBO(*dstIP);
      LOG(WARNING) << __func__ << ": dstIP " << ip << " not found";
      return;
    }
    auto callback = streamMap.at(*dstIP);
    CHECK(callback);
    callback->onPacket(buf, len);
  }

  folly::IPAddressV4 registerTransaction(
      proxygen::TunDevice::ReadCallback* callback) {
    folly::IPAddressV4 assignedIP;
    {
      auto currentIP = tunDevice->getTunSubnet().first;
      subnetCounter++;
      std::size_t currentCounter = subnetCounter++;
      currentIP = currentIP + currentCounter;
      assignedIP = currentIP;
    }
    CHECK(streamMap.find(assignedIP.toLongHBO()) == streamMap.end());
    streamMap.insert(assignedIP.toLongHBO(), callback);
    return assignedIP;
  }
};

struct QuicStream {

  enum TYPE { UDP, IP };

  struct UDPProperties {
    folly::SocketAddress target;
    std::unique_ptr<folly::AsyncUDPSocket> socket;
    std::function<void()> destructCallbacks;
  };

  struct IPProperties {
    SharedTun* tunDevice;
    folly::IPAddressV4 assignedIP;
    std::function<void()> destructCallbacks;
  };

  TYPE type;
  std::variant<UDPProperties, IPProperties> properties;

 public:
  explicit QuicStream(UDPProperties);
  explicit QuicStream(IPProperties);
};

using StreamSocketMap =
    folly::ConcurrentHashMap<quic::StreamId, std::shared_ptr<QuicStream>>;

} // namespace MasqueService
