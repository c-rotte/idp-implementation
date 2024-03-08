#pragma once

#include "Capsule.h"
#include "MasqueDownstream.h"
#include "MasqueUpstream.h"
#include "tuntap/TunManager.h"
#include <boost/program_options.hpp>
#include <memory>
#include <quic/server/QuicServer.h>

namespace MasqueService {

class DatagramTransactionHandler : public TransactionHandler {

 private:
  folly::EventBase *eventBase;
  std::size_t numberOfStreams;
  SharedTun *tunDevice;

 public:
  DatagramTransactionHandler(folly::EventBase *, SharedTun *);
  ~DatagramTransactionHandler() override = default;

 private:
  std::shared_ptr<QuicStream> getStream(quic::StreamId) const;

 public:
  // TransactionHandler
  void onHeadersComplete(
      std::unique_ptr<proxygen::HTTPMessage>) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override;
  void onEOM() noexcept override;
  void onError(const proxygen::HTTPException &) noexcept override;
  // proxygen::HTTPTransactionHandler
  void onDatagram(std::unique_ptr<folly::IOBuf>) noexcept override;
};

struct MasqueCallback {

 protected:
  folly::EventBase *eventBase;
  QuicStream *upstream;
  proxygen::HTTPTransaction *downstreamTransaction;

 public:
  explicit MasqueCallback(folly::EventBase *,
                          QuicStream *,
                          proxygen::HTTPTransaction *);
  virtual ~MasqueCallback() = default;

 protected:
  folly::IPAddressV4 getClientIP() const;
};

class ConnectUDPCallback
    : public MasqueCallback
    , public folly::AsyncUDPSocket::ReadCallback
    , public folly::AsyncUDPSocket::ErrMessageCallback {

 private:
  std::array<char, 2048> readBuffer;

 public:
  explicit ConnectUDPCallback(folly::EventBase *,
                              QuicStream *,
                              proxygen::HTTPTransaction *);

 private:
  QuicStream::UDPProperties &getProperties();

 public:
  // ReadCallback
  void getReadBuffer(void **, size_t *) noexcept override;
  void onDataAvailable(const folly::SocketAddress &,
                       size_t,
                       bool,
                       OnDataAvailableParams) noexcept override;
  void onReadError(const folly::AsyncSocketException &) noexcept override;
  void onReadClosed() noexcept override;
  // ErrMessageCallback
  void errMessage(const cmsghdr &) noexcept override;
  void errMessageError(const folly::AsyncSocketException &) noexcept override;
};

class ConnectIPCallback
    : public MasqueCallback
    , public proxygen::TunDevice::ReadCallback {
 public:
  explicit ConnectIPCallback(folly::EventBase *,
                             QuicStream *,
                             proxygen::HTTPTransaction *);

 private:
  QuicStream::IPProperties &getProperties();

 public:
  // ReadCallback
  void onPacket(std::uint8_t *, std::size_t) noexcept override;
};

class DatagramServer {

 public:
  struct Options {
    uint16_t port;
    std::size_t timeout;
    folly::CIDRNetworkV4 tunNetwork;
    quic::CongestionControlType ccAlgorithm;
    bool framePerPacket;
    std::size_t UDPSendPacketLen;
    std::size_t maxRecvPacketSize;
    std::optional<std::string> qlogPath;
    bool enableMigration;
    std::size_t tunMTU;
  };

 private:
  std::shared_ptr<quic::QuicServer> quicServer;
  std::unique_ptr<SharedTun> sharedTunDevice;
  const Options serverOptions;

 public:
  explicit DatagramServer(Options);
  ~DatagramServer();

 public:
  void start();
  void shutdown();
};

} // namespace MasqueService