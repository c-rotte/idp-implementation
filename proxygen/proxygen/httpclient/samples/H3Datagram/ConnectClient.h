#pragma once

#include "socket/LayeredMasqueSocket.h"
#include <proxygen/httpserver/samples/masque/tuntap/TunDevice.h>
#include <proxygen/httpserver/samples/masque/tuntap/TunManager.h>
#include <proxygen/lib/transport/H3DatagramAsyncSocket.h>

namespace MasqueService {

struct OptionPair {
  bool connectIP = false;
  proxygen::H3DatagramAsyncSocket::Options options;
  std::size_t UDPSendPacketLen = 0;
  std::size_t maxRecvPacketSize = 0;
};

extern std::size_t FIRST_TUN_NUMBER;

class TunReadCallback
    : public proxygen::TunDevice::ReadCallback
    , public folly::AsyncUDPSocket::ReadCallback {
 protected:
  proxygen::TunDevice *tunDevice;
  LayeredMasqueSocket *socket;
  const proxygen::HTTPCodec::StreamID streamID;
  const folly::SocketAddress address;
  std::array<std::uint8_t, 2048> readBuffer;

 public:
  TunReadCallback(proxygen::TunDevice *tunDevice,
                  LayeredMasqueSocket *socket,
                  proxygen::HTTPCodec::StreamID streamID,
                  folly::SocketAddress address)
      : tunDevice(tunDevice),
        socket(socket),
        streamID(streamID),
        address(std::move(address)) {
  }

 public:
  void setTunDevice(proxygen::TunDevice *tunDevice) {
    this->tunDevice = tunDevice;
  }
  // AsyncUDPSocket::ReadCallback
  void getReadBuffer(void **, size_t *) noexcept override;
  void onReadError(const folly::AsyncSocketException &) noexcept override;
  void onReadClosed() noexcept override;
};

} // namespace MasqueService