/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <fizz/client/FizzClientContext.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <proxygen/lib/http/session/HQUpstreamSession.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <quic/api/QuicSocket.h>
#include <quic/client/QuicClientTransport.h>

#include <utility>

namespace proxygen {

class H3DatagramAsyncSocketTest;

class H3DatagramAsyncSocket
    : public folly::AsyncUDPSocket
    , HQSession::ConnectCallback
    , HTTPSessionBase::InfoCallback
    , public folly::DelayedDestruction {

  friend class H3DatagramAsyncSocketTest;

 public:
  enum class Mode {
    CLIENT = 0,
    SERVER = 1,
  };

  struct Options {
    quic::TransportSettings transportSettings;
    Mode mode_;
    std::size_t transactions_{1};
    std::chrono::milliseconds txnTimeout_{10000};
    std::chrono::milliseconds connectTimeout_{3000};
    std::shared_ptr<proxygen::HTTPMessage> httpRequest_;
    folly::SocketAddress targetAddress_;
    folly::Optional<std::pair<std::string, std::string>> certAndKey_;
    std::shared_ptr<const fizz::CertificateVerifier> certVerifier_;
    uint16_t maxRecvPacketSize_{1500};
    uint16_t maxSendSize_{1500};
    quic::CongestionControlType defaultCCType =
        quic::CongestionControlType::None;
    bool framePerPacket = true;
  };

  struct UDPSocketGenerator {
    virtual std::unique_ptr<folly::AsyncUDPSocket> operator()(
        folly::EventBase*) = 0;
    virtual ~UDPSocketGenerator() = default;
  };

  struct TransactionReadCallback : public AsyncUDPSocket::ReadCallback {
    HTTPCodec::StreamID streamID;
  };

  struct TransactionBodyReadCallback : public TransactionReadCallback {
    virtual void onBody(std::unique_ptr<folly::IOBuf> body) noexcept = 0;
  };

  class TransactionHandler : public proxygen::HTTPTransactionHandler {

   private:
    const Options options;
    proxygen::HQUpstreamSession* upstreamSession = nullptr;
    proxygen::HTTPTransaction* httpTransaction = nullptr;
    // Buffers Outgoing Datagrams before the transport is ready
    std::deque<std::unique_ptr<folly::IOBuf>> writeBuf;
    std::mutex writeBufMutex;
    // Buffers Incoming Datagrams before the transport is ready
    std::deque<std::unique_ptr<folly::IOBuf>> readBuf;
    std::mutex readBufMutex;
    // Buffers the body of the current transaction
    std::deque<std::unique_ptr<folly::IOBuf>> bodyBuf;
    std::mutex bodyBufMutex;
    ReadCallback* readCallback = nullptr;

   public:
    explicit TransactionHandler(Options options,
                                proxygen::HQUpstreamSession* upstreamSession)
        : options(std::move(options)), upstreamSession(upstreamSession) {
    }
    TransactionHandler() = delete;

    proxygen::HTTPTransaction* getTransaction() {
      return httpTransaction;
    }

    proxygen::HTTPCodec::StreamID getTransactionID() const {
      return httpTransaction->getID();
    }

    void setReadCallback(ReadCallback* readCallback) {
      this->readCallback = readCallback;
      // deliver body
      {
        std::unique_lock lock(bodyBufMutex);
        if (auto* bodyReadCallback =
                dynamic_cast<TransactionBodyReadCallback*>(readCallback)) {
          for (auto& bodyChunk : bodyBuf) {
            bodyReadCallback->onBody(std::move(bodyChunk));
          }
        }
        bodyBuf.clear();
      }
      // deliver datagrams
      {
        std::unique_lock lock(readBufMutex);
        for (auto& datagram : readBuf) {
          deliverDatagram(std::move(datagram));
        }
        readBuf.clear();
      }
    }

    std::deque<std::unique_ptr<folly::IOBuf>>& writeBuffer() {
      return writeBuf;
    }

    std::deque<std::unique_ptr<folly::IOBuf>>& readBuffer() {
      return readBuf;
    }

    bool isReading() const {
      return readCallback != nullptr;
    }

    void pauseRead() {
      readCallback = nullptr;
    }

    void closeRead() {
      if (readCallback) {
        auto* cb = readCallback;
        readCallback = nullptr;
        cb->onReadClosed();
      }
    }

    void closeWithError(const folly::AsyncSocketException& ex);
    void deliverDatagram(std::unique_ptr<folly::IOBuf> datagram) noexcept;

   public:
    // HTTPTransactionHandler methods
    void setTransaction(proxygen::HTTPTransaction* txn) noexcept override;
    void detachTransaction() noexcept override;
    void onHeadersComplete(
        std::unique_ptr<proxygen::HTTPMessage> msg) noexcept override;
    void onDatagram(std::unique_ptr<folly::IOBuf> datagram) noexcept override;
    void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept override;
    void onTrailers(
        std::unique_ptr<proxygen::HTTPHeaders> trailers) noexcept override;
    void onEOM() noexcept override;
    void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;
    void onError(const proxygen::HTTPException& error) noexcept override;
    void onEgressPaused() noexcept override;
    void onEgressResumed() noexcept override;
  };

  using NewTransactionCallback = std::function<void(HTTPCodec::StreamID)>;

 public:
  H3DatagramAsyncSocket(folly::EventBase* evb,
                        H3DatagramAsyncSocket::Options options,
                        std::unique_ptr<UDPSocketGenerator> socketGenerator);

  ~H3DatagramAsyncSocket() override {
    for (auto& [_, handler] : transactions_) {
      auto* txn = handler->getTransaction();
      txn->setHandler(nullptr);
    }
    if (upstreamSession_) {
      upstreamSession_->setConnectCallback(nullptr);
      upstreamSession_->setInfoCallback(nullptr);
    }
  }

  /*
   * AsyncUDPSocket
   */
  const folly::SocketAddress& address() const override;

  void bind(const folly::SocketAddress& address,
            BindOptions /*options*/ = BindOptions()) override {
    bindAddress_ = address;
  }

  void connect(const folly::SocketAddress& address) override {
    CHECK(options_.mode_ == Mode::CLIENT);
    LOG(INFO) << "Connecting to " << address.describe();
    connectAddress_ = address;
    startClient();
  }

  void setFD(folly::NetworkSocket /*fd*/, FDOwnership /*ownership*/) override {
    LOG(WARNING) << __func__ << " not supported";
  }

  void setCmsgs(const folly::SocketOptionMap& /*cmsgs*/) override {
    LOG(WARNING) << __func__ << " not supported";
  }

  void appendCmsgs(const folly::SocketOptionMap& /*cmsgs*/) override {
    LOG(WARNING) << __func__ << " not supported";
  }

  ssize_t write(const folly::SocketAddress& address,
                const std::unique_ptr<folly::IOBuf>& buf) override;

  int writem(folly::Range<folly::SocketAddress const*> /*addrs*/,
             const std::unique_ptr<folly::IOBuf>* /*bufs*/,
             size_t /*count*/) override {
    return -1;
  }

  ssize_t writeGSO(const folly::SocketAddress& /*address*/,
                   const std::unique_ptr<folly::IOBuf>& /*buf*/,
                   int /*gso*/) override {
    return -1;
  }

  ssize_t writeChain(const folly::SocketAddress& /*address*/,
                     std::unique_ptr<folly::IOBuf>&& /*buf*/,
                     WriteOptions /*options*/) override {
    return -1;
  }

  int writemGSO(folly::Range<folly::SocketAddress const*> /*addrs*/,
                const std::unique_ptr<folly::IOBuf>* /*bufs*/,
                size_t /*count*/,
                const int* /*gso*/) override {
    return -1;
  }

  ssize_t writev(const folly::SocketAddress& /*address*/,
                 const struct iovec* /*vec*/,
                 size_t /*iovec_len*/,
                 int /*gso*/) override {
    return -1;
  }

  ssize_t writev(const folly::SocketAddress& /*address*/,
                 const struct iovec* /*vec*/,
                 size_t /*iovec_len*/) override {
    return -1;
  }

  ssize_t recvmsg(struct msghdr* /*msg*/, int /*flags*/) override {
    return -1;
  }

  int recvmmsg(struct mmsghdr* /*msgvec*/,
               unsigned int /*vlen*/,
               unsigned int /*flags*/,
               struct timespec* /*timeout*/) override {
    return -1;
  }

  void resumeRead(ReadCallback* cob) override;

  void pauseRead() override {
    readCallback_ = nullptr;
  }

  void close() override {
    for (auto& [_, handler] : transactions_) {
      auto* txn = handler->getTransaction();
      if (!txn->isEgressEOMSeen()) {
        txn->sendEOM();
      }
    }
    if (upstreamSession_) {
      upstreamSession_->closeWhenIdle();
    }
  }

  folly::NetworkSocket getNetworkSocket() const override {
    LOG(FATAL) << __func__ << " not supported";
    folly::assume_unreachable();
  }

  HTTPTransaction* findTransaction(
      proxygen::HTTPCodec::StreamID streamID) const {
    if (transactions_.count(streamID)) {
      return transactions_.at(streamID)->getTransaction();
    }
    return nullptr;
  }

  void setReusePort(bool /*reusePort*/) override {
    LOG(WARNING) << __func__ << " not supported";
  }

  void setReuseAddr(bool /*reuseAddr*/) override {
    LOG(WARNING) << __func__ << " not supported";
  }

  void setRcvBuf(int rcvBuf) override {
    if (rcvBuf > 0) {
      // This is going to be the maximum number of packets buffered here or at
      // the the quic transport
      rcvBufPkts_ = rcvBuf / options_.maxRecvPacketSize_;
    }
  }

  void setSndBuf(int sndBuf) override {
    if (sndBuf > 0) {
      // This is going to be the maximum number of packets buffered here or at
      // the the quic transport
      sndBufPkts_ = sndBuf / options_.maxRecvPacketSize_;
    }
  }

  void setBusyPoll(int /*busyPollUs*/) override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  void dontFragment(bool /*df*/) override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  void setDFAndTurnOffPMTU() override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  void setErrMessageCallback(
      ErrMessageCallback* /*errMessageCallback*/) override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  bool isBound() const override {
    return false;
  }

  bool isReading() const override {
    return defaultStreamId_ && transactions_.at(*defaultStreamId_)->isReading();
  }

  void detachEventBase() override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  void attachEventBase(folly::EventBase* /*evb*/) override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  int getGSO() override {
    return -1;
  }

  void setOverrideNetOpsDispatcher(
      std::shared_ptr<folly::netops::Dispatcher> /*dispatcher*/) override {
    LOG(WARNING) << __func__ << " unsupported";
  }

  std::shared_ptr<folly::netops::Dispatcher> getOverrideNetOpsDispatcher()
      const override {
    LOG(WARNING) << __func__ << " unsupported";
  }

 private:
  /*
   * proxygen::HQSession::ConnectCallback
   */
  void connectSuccess() override;
  void onReplaySafe() override;
  void connectError(quic::QuicError error) override;

  // Set the HTTP/3 Session. for testing only
  void setUpstreamSession(proxygen::HQUpstreamSession* session) {
    CHECK(!upstreamSession_);
    upstreamSession_ = session;
    upstreamSession_->setConnectCallback(this);
    upstreamSession_->setInfoCallback(this);
  }

  /*
   * HTTPSessionBase::InfoCallback
   */
  void onDestroy(const HTTPSessionBase&) override {
    upstreamSession_ = nullptr;
  }

 protected:
  virtual ssize_t write(HTTPCodec::StreamID streamID,
                        const folly::SocketAddress& address,
                        const std::unique_ptr<folly::IOBuf>& datagram);

 public:
  virtual void resumeRead(HTTPCodec::StreamID streamID,
                          TransactionReadCallback* cob);
  virtual void resumeRead(HTTPCodec::StreamID streamID, ReadCallback* cob);

 private:
  void startClient();
  std::shared_ptr<fizz::client::FizzClientContext> createFizzClientContext();

 public:
  std::vector<proxygen::HTTPCodec::StreamID> getOpenTransactions() const {
    std::vector<proxygen::HTTPCodec::StreamID> streams;
    for (auto& [streamID, _] : transactions_) {
      streams.push_back(streamID);
    }
    return streams;
  }

  void setNewTransactionCallback(NewTransactionCallback callback) {
    newTransactionCallback_ = std::move(callback);
  }

  void setQLogPath(std::string qlogPath) {
    qlogPath_ = std::move(qlogPath);
  }

 private:
  folly::EventBase* evb_;
  Options options_;
  std::unique_ptr<UDPSocketGenerator> socketGenerator_;
  folly::SocketAddress bindAddress_;
  folly::SocketAddress connectAddress_;
  proxygen::HQUpstreamSession* upstreamSession_{nullptr};
  std::optional<NewTransactionCallback> newTransactionCallback_;
  // Buffers Outgoing Datagrams before the transport is ready
  std::deque<std::unique_ptr<folly::IOBuf>> writeBuf;

  std::unordered_map<proxygen::HTTPCodec::StreamID,
                     std::unique_ptr<TransactionHandler>>
      transactions_;

  std::optional<std::string> qlogPath_;

  bool transportConnected_ : 1;

  std::optional<HTTPCodec::StreamID> defaultStreamId_;
  AsyncUDPSocket::ReadCallback* defaultReadCallback_{nullptr};

  static unsigned int rcvBufPkts_;
  static unsigned int sndBufPkts_;
};

} // namespace proxygen
