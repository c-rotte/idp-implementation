#pragma once

#include "ConnectClient.h"
#include "socket/LayeredConnectUDPSocket.h"
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <proxygen/httpserver/samples/masque/tuntap/TunDevice.h>

namespace MasqueService {

class ConnectUDPClient {

 public:
  struct UDPClientTUNOptions {
    folly::CIDRNetworkV4 tunDeviceNetwork;
    std::size_t sourcePort;
    std::size_t destinationPort;
  };

 private:
  class ConnectUDPTunCallback : public TunReadCallback {
   private:
    UDPClientTUNOptions tunOptions;
    folly::IPAddressV4 tunDeviceAddress;

   public:
    ConnectUDPTunCallback(proxygen::TunDevice *tunDevice,
                          LayeredMasqueSocket *socket,
                          proxygen::HTTPCodec::StreamID streamID,
                          folly::SocketAddress address,
                          UDPClientTUNOptions tunOptions,
                          folly::IPAddressV4 tunDeviceAddress)
        : TunReadCallback(tunDevice, socket, streamID, std::move(address)),
          tunOptions(tunOptions),
          tunDeviceAddress(tunDeviceAddress) {
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
  proxygen::SubNetGenerator subNetGenerator;
  const UDPClientTUNOptions tunOptions;
  std::size_t tunMTU;
  using TunPair = std::pair<std::unique_ptr<proxygen::TunDevice>,
                            std::unique_ptr<TunReadCallback>>;
  std::unordered_map<proxygen::HTTPCodec::StreamID, TunPair> tunDevices;
  std::unique_ptr<LayeredConnectUDPSocket> socket;

 public:
  ConnectUDPClient(folly::EventBase *,
                   std::vector<OptionPair>,
                   UDPClientTUNOptions,
                   std::optional<std::string> = std::nullopt);

 private:
  void createTunDevice(proxygen::HTTPCodec::StreamID);

 public:
  void start();
};

} // namespace MasqueService