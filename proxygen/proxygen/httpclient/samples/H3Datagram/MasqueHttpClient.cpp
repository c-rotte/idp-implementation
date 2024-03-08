#include "MasqueHttpClient.h"

#include "AlwaysAcceptCertificateVerifier.h"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <fizz/client/FizzClientContext.h>
#include <folly/init/Init.h>
#include <folly/ssl/Init.h>
#include <fstream>
#include <iostream>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <utility>
#include <wangle/acceptor/TransportInfo.h>

#include "ConnectClient.h"
#include "proxygen/httpserver/samples/masque/help/MasqueUtils.h"
#include "socket/LayeredConnectIPSocket.h"
#include "socket/LayeredSocketGenerator.h"

#include "proxygen/httpserver/samples/masque/help/QLogger.h"

using namespace proxygen;
using namespace folly;
using namespace std;
using namespace std::chrono;

namespace MasqueService {

void MasqueHttpClient::TransactionHandler::postBody() {
  const uint16_t kReadSize =
      2048; // TODO: calculate correct overhead from UDPSendPacketLen
  CHECK(options.inputFile);
  // Reading from the file by chunks
  while (options.inputFile->good()) {
    unique_ptr<IOBuf> buf = IOBuf::createCombined(kReadSize);
    options.inputFile->read(reinterpret_cast<char*>(buf->writableData()),
                            kReadSize);
    buf->append(options.inputFile->gcount());
    httpTransaction->sendBody(std::move(buf));
  }
  LOG(INFO) << httpTransaction->getID() << ": Sent post body";
}

void MasqueHttpClient::TransactionHandler::onHeadersComplete(
    unique_ptr<HTTPMessage> msg) noexcept {
  // LOG(INFO) << httpTransaction->getID() << ": Headers complete";
  if (options.httpRequest.getMethodString() == "POST") {
    postBody();
    httpTransaction->sendEOM();
    std::cout << httpTransaction->getID() << ": Sent post body in "
              << duration_cast<milliseconds>(steady_clock::now() -
                                             MasqueHttpClient::startTime)
                     .count()
              << " ms" << std::endl;
    if (!options.qlogPath) {
      exit(0);
    }
    // SignalHandler::raise(SIGTERM);
  }
  headersCompleteTime = std::chrono::steady_clock::now();
}

void MasqueHttpClient::TransactionHandler::onBody(
    unique_ptr<folly::IOBuf> data) noexcept {
  if (bodyReceivedBytes == 0) {
    std::cout << "TTFB="
              << duration_cast<milliseconds>(steady_clock::now() -
                                             MasqueHttpClient::startTime)
                     .count()
              << "ms" << std::endl;
  }
  bodyReceivedBytes += data->computeChainDataLength();
}

void MasqueHttpClient::TransactionHandler::onEOM() noexcept {
  // LOG(INFO) << __func__;
  if (options.httpRequest.getMethodString() == "GET") {
    std::cout << bodyReceivedBytes << "bytes received in "
              << duration_cast<milliseconds>(steady_clock::now() -
                                             MasqueHttpClient::startTime)
                     .count()
              << "ms" << std::endl;
    if (++(*completedTransactions) == options.numTransactions) {
      if (!options.qlogPath) {
        exit(0);
      }
      // SignalHandler::raise(SIGTERM);
    }
  }
}

std::chrono::steady_clock::time_point MasqueHttpClient::startTime;

MasqueHttpClient::MasqueHttpClient(EventBase* eventBase,
                                   unique_ptr<AsyncUDPSocket> socket,
                                   Options options)
    : eventBase(eventBase),
      socket(std::move(socket)),
      options(std::move(options)),
      completedTransactions(0),
      upstreamSession(nullptr) {
}

void MasqueHttpClient::connectSuccess() {
  {
    // weird fix but works
    auto* client = static_cast<quic::QuicClientTransport*>(
        upstreamSession->getQuicSocket());
    client->getStateNonConst()->udpSendPacketLen = options.UDPSendPacketLen;
  }
  LOG(INFO) << "Connected to " << options.targetAddress.describe();
  for (size_t i = 0; i < options.numTransactions; i++) {
    auto handler = make_unique<TransactionHandler>(
        upstreamSession, options, &completedTransactions);
    auto* httpTransaction = upstreamSession->newTransaction(handler.get());
    handler->setTransaction(httpTransaction);
    if (!httpTransaction || !httpTransaction->canSendHeaders()) {
      LOG(ERROR) << httpTransaction->getID() << ": Cannot send headers";
      continue;
    }
    httpTransaction->sendHeaders(options.httpRequest);
    transactionHandlers.push_back(std::move(handler));
  }
  // start the time measurement
  MasqueHttpClient::startTime = std::chrono::steady_clock::now();
  for (auto& handler : transactionHandlers) {
    handler->sendEOM();
  }
  upstreamSession->closeWhenIdle();
}

void MasqueHttpClient::call() {
  MasqueService::ScopeExecutor _([]() {
    // restore default
    MasqueService::setQUICPacketLenV4(1472);
  });
  MasqueService::setQUICPacketLenV4(options.UDPSendPacketLen);
  // 1) fizz client context
  auto ctx = make_shared<fizz::client::FizzClientContext>();
  {
    vector<string> supportedAlpns = {proxygen::kH3};
    ctx->setSupportedAlpns(supportedAlpns);
    ctx->setDefaultShares(
        {fizz::NamedGroup::x25519, fizz::NamedGroup::secp256r1});
    ctx->setSendEarlyData(false);
  }
  auto fizzClientContext =
      quic::FizzClientQuicHandshakeContext::Builder()
          .setFizzClientContext(ctx)
          .setCertificateVerifier(
              make_shared<AlwaysAcceptCertificateVerifier>())
          .build();
  // 2) quic client transport
  CHECK(socket);
  auto client = make_shared<quic::QuicClientTransport>(
      eventBase, std::move(socket), fizzClientContext);
  client->addNewPeerAddress(options.targetAddress);
  quic::TransportSettings transportSettings;
  transportSettings.maxRecvPacketSize = options.maxRecvPacketSize;
  transportSettings.canIgnorePathMTU = true;
  transportSettings.idleTimeout = milliseconds(99999999);
  transportSettings.pacingEnabled =
      (options.cc != quic::CongestionControlType::None);
  transportSettings.defaultCongestionController = options.cc;
  client->setCongestionControllerFactory(
      std::make_shared<quic::DefaultCongestionControllerFactory>());
  if (transportSettings.pacingEnabled) {
    client->setPacingTimer(quic::TimerHighRes::newTimer(
        eventBase, transportSettings.pacingTickInterval));
  }
  client->setTransportSettings(transportSettings);
  client->setSupportedVersions({quic::QuicVersion::QUIC_V1,
                                quic::QuicVersion::QUIC_V2,
                                quic::QuicVersion::MVFST});
  if (options.qlogPath) {
    client->setQLogger(std::make_shared<MasqueService::QLogger>(
        eventBase, *options.qlogPath, quic::VantagePoint::Client));
  }
  // client->getStateNonConst()->udpSendPacketLen = options.UDPSendPacketLen;
  // 3) proxygen upstream session
  wangle::TransportInfo tinfo;
  upstreamSession =
      new HQUpstreamSession(999999s, 999999s, nullptr, tinfo, nullptr);
  upstreamSession->setMaxConcurrentIncomingStreams(
      numeric_limits<uint32_t>::max());
  upstreamSession->setMaxConcurrentOutgoingStreams(
      numeric_limits<uint32_t>::max());
  upstreamSession->setSocket(client);
  upstreamSession->setConnectCallback(this);
  upstreamSession->setInfoCallback(this);
  upstreamSession->setInfoCallback(this);
  // 4) start
  upstreamSession->startNow();
  client->start(upstreamSession, upstreamSession);
}

} // namespace MasqueService

vector<MasqueService::OptionPair> generateOptions(
    vector<bool> connectIPModes,
    size_t timeout,
    vector<string> hosts,
    vector<uint16_t> ports,
    vector<string> paths,
    vector<size_t> UDPSendPacketLens,
    vector<size_t> maxRecvPacketSizes,
    vector<quic::CongestionControlType> ccAlgorithms,
    vector<bool> framePerPackets) {
  // check if all vectors are of the same length
  if (hosts.size() != connectIPModes.size() || hosts.size() != ports.size() ||
      hosts.size() != paths.size() || hosts.size() != ccAlgorithms.size() ||
      hosts.size() != framePerPackets.size() ||
      hosts.size() != UDPSendPacketLens.size() ||
      hosts.size() != maxRecvPacketSizes.size()) {
    throw std::runtime_error("All vectors must be of the same length");
  }
  vector<MasqueService::OptionPair> result;
  for (size_t i = 0; i < hosts.size(); i++) {
    H3DatagramAsyncSocket::Options options;
    options.mode_ = H3DatagramAsyncSocket::Mode::CLIENT;
    options.txnTimeout_ = chrono::milliseconds(timeout);
    options.connectTimeout_ = chrono::milliseconds(timeout);
    options.httpRequest_ = make_unique<HTTPMessage>();
    options.httpRequest_->setMasque();
    {
      options.httpRequest_->setMethod(
          connectIPModes[i] ? HTTPMethod::CONNECT_IP : HTTPMethod::CONNECT_UDP);
      // note: this effectively just sets the path
      options.httpRequest_->setURL(paths[i]);
      options.httpRequest_->getHeaders().add(HTTP_HEADER_HOST, "example.org");
      options.httpRequest_->getHeaders().add(
          ":protocol", connectIPModes[i] ? "connect-ip" : "connect-udp");
      options.httpRequest_->getHeaders().add("capsule-protocol", "?1");
    }
    options.targetAddress_ = SocketAddress(hosts[i], ports[i]);
    options.certVerifier_ =
        make_unique<proxygen::AlwaysAcceptCertificateVerifier>();
    options.transactions_ = 1;
    options.defaultCCType = ccAlgorithms[i];
    options.framePerPacket = framePerPackets[i];
    MasqueService::applyPerformanceSettingsClient(options.transportSettings);
    MasqueService::OptionPair pair{.connectIP = connectIPModes[i],
                                   .options = std::move(options),
                                   .UDPSendPacketLen = UDPSendPacketLens[i],
                                   .maxRecvPacketSize = maxRecvPacketSizes[i]};
    result.push_back(std::move(pair));
  }
  return result;
}

int main(int argc, char** argv) {
  namespace po = boost::program_options;
  // glog
#if FOLLY_HAVE_LIBGFLAGS
  // Enable glog logging to stderr by default.
  gflags::SetCommandLineOptionWithMode(
      "logtostderr", "1", gflags::SET_FLAGS_DEFAULT);
  // gflags::SetCommandLineOptionWithMode(
  //   "minloglevel", "3", gflags::SET_FLAGS_DEFAULT);
#endif
  // folly
  int dummyArgc = 0;
  folly::init(&dummyArgc, &argv, false);
  // ---------------------------------------------------------------------------
  po::options_description desc("Allowed options");
  desc.add_options()(
      "ip", po::value<string>()->default_value("0.0.0.0"), "IP address")(
      "port", po::value<uint16_t>()->default_value(1337), "Port number")(
      "method", po::value<string>()->default_value("GET"), "HTTP method")(
      "file", po::value<string>(), "Filename for POST body")(
      "UDPSendPacketLen",
      po::value<size_t>(),
      "UDPSendPacketLen for the HTTP Client")(
      "maxRecvPacketSize", po::value<size_t>(), "set maxRecvPacketSize")(
      "qlog", po::value<string>(), "set qlog path")(
      "cc", po::value<string>(), "cc algo for http client")(
      "modes",
      po::value<vector<string>>()->multitoken(),
      "{connect-ip' | 'connect-udp}'")(
      "timeout",
      po::value<size_t>()->default_value(10000),
      "set timeout in ms")("hosts",
                           po::value<vector<string>>()->multitoken(),
                           "set proxy server ips")(
      "ports",
      po::value<vector<uint16_t>>()->multitoken(),
      "set proxy server ports")("paths",
                                po::value<vector<string>>()->multitoken(),
                                "set proxy server paths")(
      "numTransactions",
      po::value<size_t>(),
      "number of concurrent http transactions")(
      "UDPSendPacketLens",
      po::value<vector<size_t>>()->multitoken(),
      "set UDPSendPacketLens for each hop")(
      "maxRecvPacketSizes",
      po::value<vector<size_t>>()->multitoken(),
      "set maxRecvPacketSizes for each hop")(
      "ccs",
      po::value<vector<string>>()->multitoken(),
      "set congestion control algorithms")(
      "framePerPackets",
      po::value<vector<bool>>()->multitoken(),
      "force QUIC to use one frame for each packet")(
      "datagramReadBuf", po::value<size_t>()->default_value(16384), "set datagram read buffer size")(
      "datagramWriteBuf", po::value<size_t>()->default_value(16384), "set datagram write buffer size");
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  // ---------------------------------------------------------------------------
  if (vm.count("file") && vm["method"].as<string>() != "POST") {
    std::cerr << "File can only be used with POST method." << std::endl;
    return 1;
  }
  // ---------------------------------------------------------------------------
  vector<bool> connectIPModes;
  if (vm.count("modes")) {
    auto modes = vm["modes"].as<vector<string>>();
    std::transform(modes.begin(),
                   modes.end(),
                   std::back_inserter(connectIPModes),
                   [](const string& mode) {
                     if (mode == "connect-ip") {
                       return true;
                     } else if (mode == "connect-udp") {
                       return false;
                     } else {
                       throw std::runtime_error("Unknown mode: " + mode);
                     }
                   });
  }
  if (!vm.count("cc")) {
    throw std::runtime_error("No congestion control algorithm specified");
  }
  auto cc = quic::congestionControlStrToType(
      boost::algorithm::to_lower_copy(vm["cc"].as<string>()));
  if (!cc) {
    throw std::runtime_error("Unknown congestion control algorithm: " +
                             vm["cc"].as<string>());
  }
  vector<quic::CongestionControlType> ccAlgorithms;
  if (vm.count("ccs")) {
    auto ccs = vm["ccs"].as<vector<string>>();
    std::transform(ccs.begin(),
                   ccs.end(),
                   std::back_inserter(ccAlgorithms),
                   [](const string& cc) {
                     auto type = quic::congestionControlStrToType(
                         boost::algorithm::to_lower_copy(cc));
                     if (!type) {
                       throw std::runtime_error(
                           "Unknown congestion control algorithm: " + cc);
                     }
                     return *type;
                   });
  }
  // ---------------------------------------------------------------------------
  vector<MasqueService::OptionPair> hops;
  if (!vm.count("hosts") || vm["hosts"].empty()) {
    hops = generateOptions(connectIPModes,
                           9999999, // timeout
                           {},      // hosts
                           {},      // ports
                           {},      // paths
                           {},      // UDPSendPacketLens
                           {},      // maxRecvPacketSizes
                           {},      // ccAlgorithms
                           {});     // framePerPackets
  } else {
    hops = generateOptions(connectIPModes,
                           vm["timeout"].as<size_t>(),
                           vm["hosts"].as<vector<string>>(),
                           vm["ports"].as<vector<uint16_t>>(),
                           vm["paths"].as<vector<string>>(),
                           vm["UDPSendPacketLens"].as<vector<size_t>>(),
                           vm["maxRecvPacketSizes"].as<vector<size_t>>(),
                           ccAlgorithms,
                           vm["framePerPackets"].as<vector<bool>>());
  }
  // ---------------------------------------------------------------------------
  EventBase eventBase;
  auto baseSocket = make_unique<AsyncUDPSocket>(&eventBase);
  for (auto& hop : hops) {
    hop.options.maxSendSize_ = hop.UDPSendPacketLen;
    hop.options.maxRecvPacketSize_ = hop.maxRecvPacketSize;
    auto masqueSocket = MasqueService::generateSocket(
        &eventBase, hop.connectIP, hop.options, std::move(baseSocket), false);
    baseSocket = std::move(masqueSocket);
  }
  // ---------------------------------------------------------------------------
  MasqueService::MasqueHttpClient::Options options;
  {
    options.targetAddress =
        SocketAddress(vm["ip"].as<string>(), vm["port"].as<uint16_t>());
    options.httpRequest.getHeaders().add(HTTP_HEADER_HOST, "example.org");
    options.httpRequest.setMethod(vm["method"].as<string>());
    options.httpRequest.setHTTPVersion(3, 0);
    options.httpRequest.getHeaders().add("user-agent", "masque");
    options.httpRequest.getHeaders().add("accept", "*/*");
    if (vm["method"].as<string>() == "POST") {
      options.inputFile = std::make_shared<std::ifstream>(
          vm["file"].as<string>(), std::ios::binary);
      if (!options.inputFile->is_open()) {
        std::cerr << "Could not open file: " << vm["file"].as<string>() << "\n";
        return 1;
      }
    }
    options.cc = *cc;
    options.UDPSendPacketLen = vm["UDPSendPacketLen"].as<size_t>();
    options.maxRecvPacketSize = vm["maxRecvPacketSize"].as<size_t>();
    options.numTransactions = vm["numTransactions"].as<size_t>();
    {
      size_t allowedLength = 1ULL * 1000 * 1000 * 1000; // 1GB
      allowedLength /= options.numTransactions;
      allowedLength =
          std::max<size_t>(allowedLength, 1ULL * 1000 * 1000 * 10); // 100MB
      options.httpRequest.setURL("/" + to_string(allowedLength));
      LOG(INFO) << "URL: " << options.httpRequest.getURL();
    }
    if (vm.count("qlog") && !vm["qlog"].as<string>().empty()) {
      options.qlogPath = vm["qlog"].as<string>();
    }
  }
  MasqueService::datagramReadBufSize = vm["datagramReadBuf"].as<size_t>();
  MasqueService::datagramWriteBufSize = vm["datagramWriteBuf"].as<size_t>();
  // ---------------------------------------------------------------------------
  // this has nothing to do with tun devices, it just already contains the
  // functionality to pin the thread to a random CPU
  TunDevice::pinThread({TunDevice::randomCPU()});
  TunDevice::increaseThreadPriority();
  // ---------------------------------------------------------------------------
  LOG(INFO) << options.httpRequest;
  MasqueService::MasqueHttpClient client(
      &eventBase, std::move(baseSocket), options);
  client.call();
  eventBase.loopForever();
  return 0;
}