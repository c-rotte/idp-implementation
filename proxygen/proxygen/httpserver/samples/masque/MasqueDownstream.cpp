#include "MasqueDownstream.h"

#include "help/QLogger.h"
#include <proxygen/lib/http/session/HQDownstreamSession.h>

using namespace quic;
using namespace folly;
using namespace netops;
using namespace proxygen;
using namespace std;
using namespace std::chrono;

namespace MasqueService {

void TransactionHandler::setStreamUDPSocketMap(
    weak_ptr<StreamSocketMap> streamSocketMap) {
  this->streamSocketMap = move(streamSocketMap);
}

void TransactionHandler::setTransaction(
    HTTPTransaction* httpTransaction) noexcept {
  this->httpTransaction = httpTransaction;
}

DatagramSessionController::DatagramSessionController(
    EventBase* eventBase,
    shared_ptr<function<TransactionHandler*(EventBase*)>>
        transactionHandlerGenerator,
    size_t timeout)
    : eventBase(eventBase),
      datagramSession(nullptr),
      transactionHandlerGenerator(move(transactionHandlerGenerator)),
      timeout(timeout),
      streamSocketMap(make_shared<StreamSocketMap>()) {
}

HQSession* DatagramSessionController::createSession() {
  wangle::TransportInfo transportInfo;
  datagramSession =
      new HQDownstreamSession(milliseconds(timeout), this, transportInfo, this);
  datagramSession->setMaxConcurrentIncomingStreams(
      numeric_limits<uint32_t>::max());
  return datagramSession;
}

void DatagramSessionController::startSession(
    shared_ptr<quic::QuicSocket> quicSocket) {
  CHECK(datagramSession);
  datagramSession->setSocket(std::move(quicSocket));
  datagramSession->startNow();
}

HTTPTransactionHandler* DatagramSessionController::getRequestHandler(
    HTTPTransaction&, HTTPMessage*) {
  auto* transactionHandler = (*transactionHandlerGenerator)(eventBase);
  transactionHandler->setStreamUDPSocketMap(streamSocketMap);
  return transactionHandler;
}

HTTPTransactionHandler* DatagramSessionController::getParseErrorHandler(
    HTTPTransaction*, const HTTPException&, const SocketAddress&) {
  return (*transactionHandlerGenerator)(eventBase);
}

HTTPTransactionHandler* DatagramSessionController::getTransactionTimeoutHandler(
    HTTPTransaction*, const SocketAddress&) {
  return (*transactionHandlerGenerator)(eventBase);
}

void DatagramSessionController::attachSession(HTTPSessionBase*) {
  // TODO
}

void DatagramSessionController::detachSession(const HTTPSessionBase*) {
  // TODO
}

void DatagramSessionController::onTransportReady(HTTPSessionBase*) {
  // TODO
}

QuicSetupCallback::QuicSetupCallback(
    DatagramSessionController* sessionController,
    wangle::ConnectionManager* connectionManager)
    : sessionController(sessionController),
      connectionManager(connectionManager) {
}

void QuicSetupCallback::onTransportReady() noexcept {
  // reset the callback
  quicSocket->setConnectionSetupCallback(nullptr);
  wangle::TransportInfo tinfo;
  // create a new dowstream session
  auto* downstreamSession =
      new HQDownstreamSession(connectionManager->getDefaultTimeout(),
                              sessionController,
                              tinfo,
                              sessionController);
  // enable datagram + CONNECT support
  downstreamSession->setEgressSettings(
      {HTTPSetting(SettingsId::_HQ_DATAGRAM, true),
       HTTPSetting(SettingsId::ENABLE_CONNECT_PROTOCOL, true)});
  quicSocket->setConnectionSetupCallback(downstreamSession);
  quicSocket->setConnectionCallback(downstreamSession);
  quicSocket->setDatagramCallback(downstreamSession);
  downstreamSession->setSocket(std::move(quicSocket));
  // start the downstream session
  downstreamSession->startNow();
  downstreamSession->onTransportReady();
  delete this;
}

void QuicSetupCallback::onConnectionSetupError(QuicError quicError) noexcept {
  LOG(ERROR) << "Failed to accept QUIC connection: " << quicError.message;
  if (quicSocket) {
    quicSocket->setConnectionSetupCallback(nullptr);
  }
  delete this;
}

DatagramTransportFactory::DatagramTransportFactory(
    function<TransactionHandler*(EventBase*)> transactionHandlerGenerator,
    size_t timeout,
    optional<string> qlogPath)
    : transactionHandlerGenerator(
          make_shared<function<TransactionHandler*(EventBase*)>>(
              move(transactionHandlerGenerator))),
      timeout(timeout),
      qlogPath(move(qlogPath)) {
}

quic::QuicServerTransport::Ptr DatagramTransportFactory::make(
    EventBase* eventBase,
    std::unique_ptr<AsyncUDPSocket> socket,
    const SocketAddress& socketAddress,
    QuicVersion quicVersion,
    shared_ptr<const fizz::server::FizzServerContext> serverContext) noexcept {

  auto* sessionController = new DatagramSessionController(
      eventBase, transactionHandlerGenerator, timeout);

  auto connectionManagerPtrPtr = connectionManager.get(*eventBase);
  wangle::ConnectionManager* localConnectionManager;
  if (connectionManagerPtrPtr) {
    // existing connection
    localConnectionManager = connectionManagerPtrPtr->get();
  } else {
    // new connection
    auto& connectionManagerPtrRef =
        connectionManager.emplace(*eventBase,
                                  wangle::ConnectionManager::makeUnique(
                                      eventBase, milliseconds(timeout)));
    localConnectionManager = connectionManagerPtrRef.get();
  }
  auto* setupCallback =
      new QuicSetupCallback(sessionController, localConnectionManager);
  auto serverTransport = QuicServerTransport::make(
      eventBase, std::move(socket), setupCallback, nullptr, serverContext);
  setupCallback->quicSocket = serverTransport;

  if (qlogPath) {
    serverTransport->setQLogger(std::make_shared<MasqueService::QLogger>(
        eventBase, *qlogPath, VantagePoint::Client));
  }

  return serverTransport;
}

} // namespace MasqueService