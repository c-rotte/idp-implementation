#pragma once

#include "ConnectClient.h"
#include "socket/LayeredConnectIPSocket.h"
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <proxygen/httpserver/samples/masque/tuntap/TunDevice.h>
#include <proxygen/httpserver/samples/masque/tuntap/TunManager.h>

namespace MasqueService {

class ConnectIPClient {

  class ConnectIPTunCallback : public TunReadCallback {
   public:
    ConnectIPTunCallback(proxygen::TunDevice *tunDevice,
                         LayeredMasqueSocket *socket,
                         proxygen::HTTPCodec::StreamID streamID,
                         folly::SocketAddress address)
        : TunReadCallback(tunDevice, socket, streamID, std::move(address)) {
    }

   public:
    // TunDevice::ReadCallback
    void onPacket(std::uint8_t *, std::size_t) noexcept override;
    // AsyncUDPSocket::ReadCallback
    void onDataAvailable(const folly::SocketAddress &,
                         size_t,
                         bool,
                         OnDataAvailableParams) noexcept override;
  };

 private:
  folly::EventBase *eventBase;
  const std::vector<OptionPair> hops;
  std::optional<proxygen::SubNetGenerator> manualGenerator;
  size_t tunMTU;
  using TunPair = std::pair<std::unique_ptr<proxygen::TunDevice>,
                            std::unique_ptr<ConnectIPTunCallback>>;
  std::unordered_map<proxygen::HTTPCodec::StreamID, TunPair> tunDevices;
  std::unique_ptr<LayeredConnectIPSocket> socket;

 public:
  ConnectIPClient(
      folly::EventBase *,
      std::vector<OptionPair>,
      std::optional<folly::CIDRNetworkV4> manualTunNetwork = std::nullopt,
      std::optional<std::string> = std::nullopt);

 private:
  void createTunDevice(proxygen::HTTPCodec::StreamID, const Address::Address &);

 public:
  void start();
};

} // namespace MasqueService