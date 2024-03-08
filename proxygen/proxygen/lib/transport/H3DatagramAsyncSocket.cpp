/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/transport/H3DatagramAsyncSocket.h>

#include "proxygen/httpserver/samples/masque/help/MasqueUtils.h"
#include <folly/FileUtil.h>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <utility>
#include <wangle/acceptor/TransportInfo.h>

#include "proxygen/httpserver/samples/masque/help/QLogger.h"

using namespace std;

namespace proxygen {

using folly::AsyncSocketException;

// H3DatagramAsyncSocket::TransactionHandler

void H3DatagramAsyncSocket::TransactionHandler::closeWithError(
    const folly::AsyncSocketException& ex) {
  LOG(ERROR) << "Closing transaction due to error: " << ex.what();
  readCallback->onReadError(ex);
  closeRead();
}

void H3DatagramAsyncSocket::TransactionHandler::deliverDatagram(
    unique_ptr<folly::IOBuf> datagram) noexcept {
  void* buf{nullptr};
  size_t len{0};
  ReadCallback::OnDataAvailableParams params;
  CHECK(readCallback);
  CHECK(datagram);
  readCallback->getReadBuffer(&buf, &len);
  if (buf == nullptr || len == 0 || len < datagram->computeChainDataLength()) {
    LOG(ERROR) << "Buffer too small to deliver "
               << datagram->computeChainDataLength() << " bytes datagram";
    return;
  }
  datagram->coalesce();
  memcpy(buf, datagram->data(), datagram->length());
  readCallback->onDataAvailable(httpTransaction->getPeerAddress(),
                                size_t(datagram->length()),
                                /*truncated*/ false,
                                params);
}

void H3DatagramAsyncSocket::TransactionHandler::setTransaction(
    HTTPTransaction* httpTransaction) noexcept {
  this->httpTransaction = httpTransaction;
}

void H3DatagramAsyncSocket::TransactionHandler::detachTransaction() noexcept {
  httpTransaction = nullptr;
}

void H3DatagramAsyncSocket::TransactionHandler::onHeadersComplete(
    unique_ptr<proxygen::HTTPMessage> msg) noexcept {
  if (msg->getStatusCode() != 200) {
    LOG(ERROR) << "HTTP Error: status code " << msg->getStatusCode();
    return;
  }
  LOG(INFO) << "received headers with datagram size "
            << httpTransaction->getDatagramSizeLimit();
  // Upstream ready to receive buffers.
  {
    unique_lock lock(writeBufMutex);
    for (auto& datagram : writeBuf) {
      httpTransaction->sendDatagram(std::move(datagram));
    }
    writeBuf.clear();
  }
}

void H3DatagramAsyncSocket::TransactionHandler::onDatagram(
    unique_ptr<folly::IOBuf> datagram) noexcept {
  if (!readCallback) {
    {
      unique_lock lock(readBufMutex);
      if (!readCallback) {
        if (readBuf.size() < rcvBufPkts_) {
          // buffer until reads are resumed
          readBuf.emplace_back(std::move(datagram));
        } else {
          VLOG_EVERY_N(2, 1000) << "Dropped incoming datagram.";
        }
        return;
      }
    }
  }
  deliverDatagram(std::move(datagram));
}

void H3DatagramAsyncSocket::TransactionHandler::onBody(
    unique_ptr<folly::IOBuf> body) noexcept {
  if (!readCallback) {
    // buffer body
    {
      unique_lock lock(bodyBufMutex);
      if (!readCallback) {
        bodyBuf.push_back(std::move(body));
        return;
      }
    }
  }
  auto* bodyListener = dynamic_cast<TransactionBodyReadCallback*>(readCallback);
  if (bodyListener) {
    bodyListener->onBody(std::move(body));
  } else {
    LOG(WARNING) << "onBody called with no body listener";
  }
}

void H3DatagramAsyncSocket::TransactionHandler::onTrailers(
    unique_ptr<proxygen::HTTPHeaders>) noexcept {
}

void H3DatagramAsyncSocket::TransactionHandler::onEOM() noexcept {
  LOG(WARNING) << "onEOM";
  closeRead();
}

void H3DatagramAsyncSocket::TransactionHandler::onUpgrade(
    UpgradeProtocol) noexcept {
  closeWithError(
      {AsyncSocketException::NOT_SUPPORTED, "onUpgrade not supported"});
}

void H3DatagramAsyncSocket::TransactionHandler::onError(
    const HTTPException& error) noexcept {
  closeWithError({AsyncSocketException::INTERNAL_ERROR, error.what()});
}

void H3DatagramAsyncSocket::TransactionHandler::onEgressPaused() noexcept {
}

void H3DatagramAsyncSocket::TransactionHandler::onEgressResumed() noexcept {
}

// H3DatagramAsyncSocket

unsigned int H3DatagramAsyncSocket::rcvBufPkts_;
unsigned int H3DatagramAsyncSocket::sndBufPkts_;

H3DatagramAsyncSocket::H3DatagramAsyncSocket(
    folly::EventBase* evb,
    Options options,
    unique_ptr<UDPSocketGenerator> socketGenerator)
    : folly::AsyncUDPSocket(evb),
      evb_(evb),
      options_(std::move(options)),
      socketGenerator_(std::move(socketGenerator)),
      transportConnected_(false) {
  CHECK(socketGenerator_ != nullptr);
  rcvBufPkts_ = 100;
  sndBufPkts_ = 100;
}

const folly::SocketAddress& H3DatagramAsyncSocket::address() const {
  static folly::SocketAddress localFallbackAddress;
  if (!upstreamSession_) {
    return localFallbackAddress;
  }
  return upstreamSession_->getLocalAddress();
}

void H3DatagramAsyncSocket::connectSuccess() {
  if (!options_.httpRequest_) {
    LOG(ERROR) << "No HTTP Request";
    return;
  }
  if (!upstreamSession_) {
    LOG(ERROR) << "ConnectSuccess with invalid session";
    return;
  }
  {
    // weird fix but works
    auto* client = static_cast<quic::QuicClientTransport*>(
        upstreamSession_->getQuicSocket());
    client->getStateNonConst()->udpSendPacketLen = options_.maxSendSize_;
  }
  for (size_t i = 0; i < options_.transactions_; i++) {
    auto handler = make_unique<TransactionHandler>(options_, upstreamSession_);
    auto* txn = upstreamSession_->newTransaction(handler.get());
    handler->setTransaction(txn);
    if (!txn || !txn->canSendHeaders()) {
      LOG(ERROR) << "Transaction Error on i=" << i;
      return;
    }
    // send the HTTPMessage
    txn->sendHeaders(*options_.httpRequest_);
    auto streamID = txn->getID();
    if (i == 0 && defaultReadCallback_) {
      handler->setReadCallback(defaultReadCallback_);
      LOG(INFO) << "Setting default read callback for streamID="
                << txn->getID();
    }
    // move to map
    if (!defaultStreamId_) {
      defaultStreamId_ = streamID;
      // move write buffer
      handler->writeBuffer() = std::move(writeBuf);
    }
    transactions_[streamID] = std::move(handler);
    if (newTransactionCallback_) {
      (*newTransactionCallback_)(streamID);
    }
    LOG(INFO) << "Created transaction for streamID=" << streamID;
  }

  upstreamSession_->closeWhenIdle();
  transportConnected_ = true;
}

void H3DatagramAsyncSocket::onReplaySafe() {
}

void H3DatagramAsyncSocket::connectError(quic::QuicError error) {
  LOG(ERROR) << "ConnectError " << error
             << " on address=" << options_.targetAddress_;
}

ssize_t H3DatagramAsyncSocket::write(HTTPCodec::StreamID streamID,
                                     const folly::SocketAddress& address,
                                     const unique_ptr<folly::IOBuf>& buf) {
  if (!buf) {
    LOG(ERROR) << "Invalid write data";
    errno = EINVAL;
    return -1;
  }
  if (!connectAddress_.isInitialized()) {
    LOG(ERROR) << "Socket not connected. Must call connect()";
    errno = ENOTCONN;
    return -1;
  }
  auto size = buf->computeChainDataLength();
  if (!transactions_.count(streamID)) {
    LOG(ERROR) << "Stream ID " << streamID << " not found";
    return -1;
  }
  auto* handler = transactions_[streamID].get();
  auto* txn = handler->getTransaction();
  if (!transportConnected_) {
    if (handler->writeBuffer().size() < sndBufPkts_) {
      VLOG(10) << "Socket not connected yet. Buffering datagram";
      handler->writeBuffer().emplace_back(buf->clone());
      return size;
    }
    LOG(ERROR) << "Socket write buffer is full. Discarding datagram";
    errno = ENOBUFS;
    return -1;
  }
  if (!transactions_.count(streamID)) {
    LOG(ERROR) << "Unable to create HTTP/3 transaction. Discarding datagram";
    errno = ECANCELED;
    return -1;
  }
  if (size > txn->getDatagramSizeLimit()) {
    auto datagramSizeLimit = txn->getDatagramSizeLimit();
    LOG(ERROR) << "streamID=" << streamID << ": Datagram too large len=" << size
               << " transport max datagram size len=" << datagramSizeLimit
               << ". Discarding datagram";
    errno = EMSGSIZE;
    return -1;
  }
  if (!txn->sendDatagram(buf->clone())) {
    LOG(ERROR) << "Transport write buffer is full. Discarding datagram";
    // sendDatagram can only fail for exceeding the maximum size (checked
    // above) and if the write buffer is full
    errno = ENOBUFS;
    return -1;
  }
  return size;
}

void H3DatagramAsyncSocket::resumeRead(HTTPCodec::StreamID streamID,
                                       TransactionReadCallback* cob) {
  cob->streamID = streamID;
  H3DatagramAsyncSocket::resumeRead(
      streamID, static_cast<folly::AsyncUDPSocket::ReadCallback*>(cob));
}

void H3DatagramAsyncSocket::resumeRead(
    HTTPCodec::StreamID streamID, folly::AsyncUDPSocket::ReadCallback* cob) {
  if (!transactions_.count(streamID)) {
    LOG(ERROR) << "Unable to resume read. Invalid streamID=" << streamID;
    return;
  }
  auto* handler = transactions_[streamID].get();
  handler->setReadCallback(cob);
}

void H3DatagramAsyncSocket::startClient() {
  MasqueService::ScopeExecutor _([]() {
    // restore default
    MasqueService::setQUICPacketLenV4(1472);
  });
  MasqueService::setQUICPacketLenV4(options_.maxSendSize_);
  auto transportSettings = options_.transportSettings;
  transportSettings.datagramConfig.enabled = true;
  transportSettings.maxRecvPacketSize = options_.maxRecvPacketSize_;
  transportSettings.canIgnorePathMTU = true;
  transportSettings.defaultCongestionController = options_.defaultCCType;
  transportSettings.pacingEnabled =
      options_.defaultCCType != quic::CongestionControlType::None;
  if (!upstreamSession_) {
    auto sock = (*socketGenerator_)(evb_);
    auto fizzClientContext =
        quic::FizzClientQuicHandshakeContext::Builder()
            .setFizzClientContext(createFizzClientContext())
            .setCertificateVerifier(options_.certVerifier_)
            .build();
    auto client = make_shared<quic::QuicClientTransport>(
        evb_, std::move(sock), fizzClientContext);
    CHECK(connectAddress_.isInitialized());
    client->addNewPeerAddress(connectAddress_);
    if (bindAddress_.isInitialized()) {
      client->setLocalAddress(bindAddress_);
    }
    client->setCongestionControllerFactory(
        make_shared<quic::DefaultCongestionControllerFactory>());
    if (transportSettings.pacingEnabled) {
      client->setPacingTimer(quic::TimerHighRes::newTimer(
          evb_, transportSettings.pacingTickInterval));
    }
    client->setTransportSettings(transportSettings);
    client->setSupportedVersions({quic::QuicVersion::QUIC_V1,
                                  quic::QuicVersion::QUIC_V2,
                                  quic::QuicVersion::MVFST});
    if (qlogPath_) {
      client->setQLogger(std::make_shared<MasqueService::QLogger>(
          getEventBase(), *qlogPath_, quic::VantagePoint::Client));
    }
    wangle::TransportInfo tinfo;
    upstreamSession_ =
        new proxygen::HQUpstreamSession(options_.txnTimeout_,
                                        options_.connectTimeout_,
                                        nullptr, // controller
                                        tinfo,
                                        nullptr); // codecfiltercallback
    upstreamSession_->setSocket(client);
    upstreamSession_->setConnectCallback(this);
    upstreamSession_->setInfoCallback(this);
    upstreamSession_->setEgressSettings(
        {{proxygen::SettingsId::_HQ_DATAGRAM, 1}});
    upstreamSession_->setMaxConcurrentOutgoingStreams(
        numeric_limits<uint32_t>::max());

    upstreamSession_->startNow();
    client->start(upstreamSession_, upstreamSession_);
  }
}

shared_ptr<fizz::client::FizzClientContext>
H3DatagramAsyncSocket::createFizzClientContext() {
  auto ctx = make_shared<fizz::client::FizzClientContext>();

  if (options_.certAndKey_.has_value()) {
    string certData;
    folly::readFile(options_.certAndKey_->first.c_str(), certData);
    string keyData;
    folly::readFile(options_.certAndKey_->second.c_str(), keyData);
    auto cert = fizz::CertUtils::makeSelfCert(certData, keyData);
    ctx->setClientCertificate(std::move(cert));
  }

  vector<string> supportedAlpns = {proxygen::kH3};
  ctx->setSupportedAlpns(supportedAlpns);
  ctx->setDefaultShares(
      {fizz::NamedGroup::x25519, fizz::NamedGroup::secp256r1});
  ctx->setSendEarlyData(false);
  return ctx;
}

ssize_t H3DatagramAsyncSocket::write(const folly::SocketAddress& address,
                                     const unique_ptr<folly::IOBuf>& buf) {
  if (!defaultStreamId_) {
    LOG(WARNING) << "No default stream id yet, buffering";
    if (writeBuf.size() < sndBufPkts_) {
      writeBuf.emplace_back(buf->clone());
      return buf->computeChainDataLength();
    }
    LOG(ERROR) << "Socket write buffer is full. Discarding datagram";
    return -1;
  }
  return write(*defaultStreamId_, address, buf);
}

void H3DatagramAsyncSocket::resumeRead(ReadCallback* cob) {
  folly::DelayedDestruction::DestructorGuard dg(this);
  if (!defaultStreamId_) {
    LOG(WARNING) << "No default stream id.";
    if (defaultReadCallback_) {
      LOG(ERROR) << "Default read callback already set.";
      return;
    }
    defaultReadCallback_ = cob;
    return;
  }
  resumeRead(*defaultStreamId_, cob);
}
} // namespace proxygen
