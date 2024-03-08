#pragma once

#include <chrono>
#include <proxygen/lib/http/session/HQUpstreamSession.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <quic/api/QuicSocket.h>
#include <quic/client/QuicClientTransport.h>

namespace MasqueService {

class MasqueHttpClient
    : public proxygen::HQSession::ConnectCallback
    , public proxygen::HTTPSessionBase::InfoCallback {

 public:
  struct Options {
    folly::SocketAddress targetAddress;
    proxygen::HTTPMessage httpRequest;
    std::size_t numTransactions;
    std::size_t UDPSendPacketLen;
    std::size_t maxRecvPacketSize;
    std::shared_ptr<std::ifstream> inputFile;
    std::optional<std::string> qlogPath;
    quic::CongestionControlType cc;
  };

  class TransactionHandler : public proxygen::HTTPTransactionHandler {

   private:
    proxygen::HQUpstreamSession* upstreamSession;
    proxygen::HTTPTransaction* httpTransaction;
    Options options;
    std::atomic_size_t* completedTransactions;
    std::size_t bodyReceivedBytes;
    std::chrono::steady_clock::time_point headersCompleteTime;

   public:
    explicit TransactionHandler(proxygen::HQUpstreamSession* upstreamSession,
                                Options options,
                                std::atomic_size_t* completedTransactions)
        : upstreamSession(upstreamSession),
          options(std::move(options)),
          completedTransactions(completedTransactions),
          bodyReceivedBytes(0) {
    }
    ~TransactionHandler() override = default;

   private:
    void postBody();

   public:
    void sendEOM() {
      CHECK(httpTransaction);
      httpTransaction->sendEOM();
      // startTime = std::chrono::steady_clock::now(); // start measuring time
    }
    void setTransaction(
        proxygen::HTTPTransaction* httpTransaction) noexcept override {
      this->httpTransaction = httpTransaction;
    }
    void detachTransaction() noexcept override {
      httpTransaction = nullptr;
    }
    void onHeadersComplete(
        std::unique_ptr<proxygen::HTTPMessage>) noexcept override;
    void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept override;
    void onTrailers(
        std::unique_ptr<proxygen::HTTPHeaders> trailers) noexcept override {
      LOG(INFO) << __func__;
    }
    void onEOM() noexcept override;
    void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override {
      LOG(INFO) << __func__ << ": " << static_cast<int>(protocol);
    }
    void onError(const proxygen::HTTPException& ex) noexcept override {
      LOG(ERROR) << ex.what();
    }
    void onEgressPaused() noexcept override {
      LOG(INFO) << __func__;
    }
    void onEgressResumed() noexcept override {
      LOG(INFO) << __func__;
    }
  };

 private:
  folly::EventBase* eventBase;
  std::unique_ptr<folly::AsyncUDPSocket> socket;
  Options options;
  std::atomic_size_t completedTransactions;
  proxygen::HQUpstreamSession* upstreamSession;
  std::vector<std::unique_ptr<TransactionHandler>> transactionHandlers;

 public:
  static std::chrono::steady_clock::time_point startTime;

  MasqueHttpClient(folly::EventBase*,
                   std::unique_ptr<folly::AsyncUDPSocket>,
                   Options);
  ~MasqueHttpClient() override = default;

 private:
  void connectSuccess() override;

 public:
  void onReplaySafe() override {
    LOG(INFO) << __func__;
  }
  void connectError(quic::QuicError code) override {
    LOG(ERROR) << __func__ << ": " << code.message;
  }

 public:
  void call();
};

} // namespace MasqueService
