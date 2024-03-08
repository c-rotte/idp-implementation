#pragma once

#include "MasqueUpstream.h"
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBaseLocal.h>
#include <memory>
#include <proxygen/lib/http/session/HQSession.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <quic/server/QuicServer.h>

namespace MasqueService {

class TransactionHandler : public proxygen::HTTPTransactionHandler {

 protected:
  proxygen::HTTPTransaction* httpTransaction = nullptr;
  std::weak_ptr<StreamSocketMap> streamSocketMap;

 public:
  TransactionHandler() = default;
  ~TransactionHandler() override = default;

 public:
  void setStreamUDPSocketMap(std::weak_ptr<StreamSocketMap>);
  // proxygen::HTTPTransactionHandler
  void setTransaction(proxygen::HTTPTransaction*) noexcept override;
  void detachTransaction() noexcept override{};
  void onHeadersComplete(
      std::unique_ptr<proxygen::HTTPMessage>) noexcept override{};
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override{};
  void onTrailers(std::unique_ptr<proxygen::HTTPHeaders>) noexcept override{};
  void onEOM() noexcept override{};
  void onUpgrade(proxygen::UpgradeProtocol) noexcept override{};
  void onError(const proxygen::HTTPException& error) noexcept override{};
  void onEgressPaused() noexcept override{};
  void onEgressResumed() noexcept override{};
};

class DatagramSessionController
    : public proxygen::HTTPSessionController
    , public proxygen::HTTPSessionBase::InfoCallback {

 private:
  folly::EventBase* eventBase;
  proxygen::HQSession* datagramSession;
  const std::shared_ptr<std::function<TransactionHandler*(folly::EventBase*)>>
      transactionHandlerGenerator;
  const std::size_t timeout;
  std::shared_ptr<StreamSocketMap> streamSocketMap;

 public:
  explicit DatagramSessionController(
      folly::EventBase*,
      std::shared_ptr<std::function<TransactionHandler*(folly::EventBase*)>>,
      std::size_t);
  ~DatagramSessionController() override = default;

 public:
  proxygen::HQSession* createSession();
  void startSession(std::shared_ptr<quic::QuicSocket>);
  //
  void onDestroy(const proxygen::HTTPSessionBase&) override{};
  proxygen::HTTPTransactionHandler* getRequestHandler(
      proxygen::HTTPTransaction&, proxygen::HTTPMessage*) override;
  proxygen::HTTPTransactionHandler* FOLLY_NULLABLE
  getParseErrorHandler(proxygen::HTTPTransaction*,
                       const proxygen::HTTPException&,
                       const folly::SocketAddress&) override;
  proxygen::HTTPTransactionHandler* FOLLY_NULLABLE getTransactionTimeoutHandler(
      proxygen::HTTPTransaction*, const folly::SocketAddress&) override;
  void attachSession(proxygen::HTTPSessionBase*) override;
  void detachSession(const proxygen::HTTPSessionBase*) override;
  void onTransportReady(proxygen::HTTPSessionBase*) override;
  void onTransportReady(const proxygen::HTTPSessionBase&) override{};
};

class QuicSetupCallback : public quic::QuicSocket::ConnectionSetupCallback {

 private:
  DatagramSessionController* sessionController = nullptr;
  wangle::ConnectionManager* connectionManager = nullptr;

 public:
  std::shared_ptr<quic::QuicSocket> quicSocket;

 public:
  QuicSetupCallback(DatagramSessionController*, wangle::ConnectionManager*);
  ~QuicSetupCallback() override = default;

 public:
  // quic::QuicSocket::ConnectionSetupCallback
  void onTransportReady() noexcept override;
  void onConnectionSetupError(quic::QuicError) noexcept override;
};

class DatagramTransportFactory : public quic::QuicServerTransportFactory {

 private:
  folly::EventBaseLocal<wangle::ConnectionManager::UniquePtr> connectionManager;
  const std::shared_ptr<std::function<TransactionHandler*(folly::EventBase*)>>
      transactionHandlerGenerator;
  const std::size_t timeout;
  const std::optional<std::string> qlogPath;

 public:
  explicit DatagramTransportFactory(
      std::function<TransactionHandler*(folly::EventBase*)>,
      std::size_t,
      std::optional<std::string>);
  ~DatagramTransportFactory() override = default;

 public:
  quic::QuicServerTransport::Ptr make(
      folly::EventBase*,
      std::unique_ptr<folly::AsyncUDPSocket>,
      const folly::SocketAddress&,
      quic::QuicVersion,
      std::shared_ptr<const fizz::server::FizzServerContext>) noexcept override;
};

} // namespace MasqueService
