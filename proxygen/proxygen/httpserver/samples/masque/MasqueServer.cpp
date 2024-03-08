#include "MasqueServer.h"

#include "help/MasqueUtils.h"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/ssl/Init.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>
#include <proxygen/lib/utils/Logging.h>
#include <quic/server/QuicSharedUDPSocketFactory.h>

#include "help/MasqueConstants.h"
#include "help/SignalHandler.h"

// #include <easy/profiler.h>

namespace po = boost::program_options;

using namespace quic;
using namespace folly;
using namespace netops;
using namespace proxygen;
using namespace std;
using namespace std::chrono;

namespace MasqueService {

const string CONNECT_UDP_PATH = "/.well-known/masque/udp/";
const string CONNECT_IP_PATH = "/.well-known/masque/ip";

////////////////
// downstream //
////////////////

DatagramTransactionHandler::DatagramTransactionHandler(EventBase* eventBase,
                                                       SharedTun* tunDevice)
    : eventBase(eventBase), numberOfStreams(0), tunDevice(tunDevice) {
}

shared_ptr<QuicStream> DatagramTransactionHandler::getStream(
    quic::StreamId streamId) const {
  CHECK(!streamSocketMap.expired());
  return streamSocketMap.lock()->at(streamId);
}

void DatagramTransactionHandler::onHeadersComplete(
    unique_ptr<proxygen::HTTPMessage> httpMessage) noexcept {
  // TODO: https://www.rfc-editor.org/rfc/rfc9297.html#section-3.2-5
  // https://datatracker.ietf.org/doc/html/rfc9298
  CHECK(httpMessage);
  const auto replyWithError = [this](const string& errorMessage) {
    HTTPMessage response;
    response.setStatusCode(400);
    response.setStatusMessage(errorMessage);
    httpTransaction->sendHeaders(response);
    LOG(INFO) << "got invalid connection: " << errorMessage;
  };
  const auto replyWithSuccess = [this]() {
    HTTPMessage response;
    response.setStatusCode(200);
    response.setStatusMessage("Ok");
    response.getHeaders().add("capsule-protocol", "?1");
    httpTransaction->sendHeaders(response);
    LOG(INFO) << "MASQUE: streamID=" << httpTransaction->getID()
              << " address=" << httpTransaction->getPeerAddress();
  };
  // https://datatracker.ietf.org/doc/html/rfc9298#name-http-2-and-http-3-requests
  if (!httpMessage->getMethod()) {
    replyWithError("needs a method");
    return;
  }
  if (*httpMessage->getMethod() != HTTPMethod::CONNECT_UDP &&
      *httpMessage->getMethod() != proxygen::HTTPMethod::CONNECT_IP) {
    replyWithError("needs connect-udp or connect-ip");
    return;
  }
  QuicStream::TYPE streamType;
  if (!httpMessage->getUpgradeProtocol()) {
    // proxygen treats :protocol as upgrade protocol
    replyWithError("protocol not included");
    return;
  }
  if (*httpMessage->getMethod() == HTTPMethod::CONNECT_UDP) {
    streamType = QuicStream::UDP;
  } else if (*httpMessage->getMethod() == HTTPMethod::CONNECT_IP) {
    streamType = QuicStream::IP;
  } else {
    replyWithError("invalid protocol: " + *httpMessage->getUpgradeProtocol());
    return;
  }
  if (!httpMessage->getHeaders().exists(HTTP_HEADER_HOST)) {
    replyWithError("must include :authority");
    return;
  }
  if (!httpMessage->getPathAsStringPiece().startsWith(
          streamType == QuicStream::UDP ? CONNECT_UDP_PATH : CONNECT_IP_PATH)) {
    replyWithError("invalid path");
    return;
  }
  if (httpMessage->getHeaders().getSingleOrEmpty("capsule-protocol") != "?1") {
    replyWithError("must start the capsule protocol");
    return;
  }
  shared_ptr<QuicStream> quicStream;
  if (streamType == QuicStream::UDP) { // connect-udp
    // 1) create the stream
    string targetIP;
    size_t targetPort;
    {
      vector<string> splitRes;
      folly::split("/", httpMessage->getPath(), splitRes);
      if (splitRes.size() != 6) {
        replyWithError("invalid path arguments");
        return;
      }
      targetIP = splitRes[splitRes.size() - 2];
      targetPort = stoi(splitRes[splitRes.size() - 1]);
    }
    SocketAddress targetAddress(IPAddress(targetIP), targetPort);
    auto socket =
        make_unique<AsyncUDPSocket>(getGlobalIOExecutor()->getEventBase());
    socket->connect(targetAddress);
    quicStream = make_shared<QuicStream>(QuicStream::UDPProperties{
        .target = move(targetAddress), .socket = move(socket)});
    // 2) create the callback
    auto* callback =
        new ConnectUDPCallback(eventBase, quicStream.get(), httpTransaction);
    auto& properties =
        std::get<QuicStream::UDPProperties>(quicStream->properties);
    properties.socket->resumeRead(callback);
    properties.socket->setErrMessageCallback(callback);
    properties.destructCallbacks = [callback]() { delete callback; };
  } else { // connect-ip
    // 1) create the stream
    string tunName =
        TunDevice::uniqueName("tun_s" + to_string(httpTransaction->getID()));
    quicStream = make_shared<QuicStream>(
        QuicStream::IPProperties{.tunDevice = tunDevice});
    // 2) create the callback
    auto* callback =
        new ConnectIPCallback(eventBase, quicStream.get(), httpTransaction);
    auto assignedIP = tunDevice->registerTransaction(callback);
    auto& properties =
        std::get<QuicStream::IPProperties>(quicStream->properties);
    properties.assignedIP = assignedIP;
    properties.destructCallbacks = [callback]() { delete callback; };
    LOG(INFO) << "assigned IP " << assignedIP.str() << " to stream "
              << httpTransaction->getID();
  }
  CHECK(!streamSocketMap.expired());
  auto* quicStreamPtr = quicStream.get();
  streamSocketMap.lock()->insert(httpTransaction->getID(), move(quicStream));
  replyWithSuccess();
  numberOfStreams++;
  if (streamType == QuicStream::IP) {
    // We immediately send a capsule to the client to let it know the IP address
    auto& properties =
        std::get<QuicStream::IPProperties>(quicStreamPtr->properties);
    // 1) address assignment
    AddressAssignCapsule assignCapsule;
    assignCapsule.addresses.push_back(
        {.requestID = 0,
         .ipAddress = IPAddress::createNetwork(properties.assignedIP.str())});
    httpTransaction->sendBody(assignCapsule.toBuffer());
  }
  LOG(INFO) << "Stream " << httpTransaction->getID()
            << " created with datagram size "
            << httpTransaction->getDatagramSizeLimit();
}

void DatagramTransactionHandler::onBody(unique_ptr<IOBuf> body) noexcept {
  if (getStream(httpTransaction->getID())->type == QuicStream::UDP) {
    // CONNECT-UDP doesn't use capsules
    return;
  }
  // parse capsule
  // https://www.rfc-editor.org/rfc/rfc9297.html#name-capsules
  auto capsule = Capsule::parseCapsule(move(body));
  if (!capsule || capsule->type == Capsule::UNKNOWN) {
    // TODO: handle invalid capsule here
    return;
  }
  auto stream = getStream(httpTransaction->getID());
  switch (capsule->type) {
    case Capsule::DATA: {
      auto& datagramCapsule = *static_cast<DatagramCapsule*>(capsule.get());
      // forward the data to upstream
      tunDevice->write(datagramCapsule.data->writableData(),
                       datagramCapsule.data->length());
      break;
    }
    case Capsule::ADDRESS_ASSIGN: {
      auto& addressAssignCapsule =
          *static_cast<AddressAssignCapsule*>(capsule.get());
      break;
    }
    case Capsule::ADDRESS_REQUEST: {
      auto& addressRequestCapsule =
          *static_cast<AddressRequestCapsule*>(capsule.get());
      auto& properties = std::get<QuicStream::IPProperties>(stream->properties);
      // 1) address assignment
      AddressAssignCapsule assignCapsule;
      for (const auto& address : addressRequestCapsule.addresses) {
        assignCapsule.addresses.push_back(
            {.requestID = address.requestID,
             .ipAddress =
                 IPAddress::createNetwork(properties.assignedIP.str())});
      }
      httpTransaction->sendBody(assignCapsule.toBuffer());
      // 2) route advertisement
      RouteAdvertisementCapsule routeCapsule;
      routeCapsule.ranges.push_back({.startIP = IPAddressV4("0.0.0.0"),
                                     .endIP = IPAddressV4("255.255.255.255")});
      httpTransaction->sendBody(routeCapsule.toBuffer());
      break;
    }
    case Capsule::ROUTE_ADVERTISEMENT: {
      auto& routeAdvertisementCapsule =
          *static_cast<RouteAdvertisementCapsule*>(capsule.get());
      break;
    }
  }
}

void DatagramTransactionHandler::onEOM() noexcept {
  LOG(INFO) << "onEOM()";
}

void DatagramTransactionHandler::onError(
    const proxygen::HTTPException& error) noexcept {
  LOG(ERROR) << error.what();
}

void DatagramTransactionHandler::onDatagram(
    unique_ptr<IOBuf> datagram) noexcept {
  // EASY_FUNCTION();
  // EASY_BLOCK("DatagramTransactionHandler::onDatagram (1)");
  // the stream id is already stripped
  const auto streamID = httpTransaction->getID();
  auto stream = getStream(streamID);
  // LOG(INFO) << "Stream " << streamID << " received datagram with size "
  //          << datagram->computeChainDataLength() << " bytes";
  // https://www.rfc-editor.org/rfc/rfc9297.html#name-http-3-datagrams
  // https://datatracker.ietf.org/doc/html/rfc9298#name-http-datagram-payload-forma
  datagram->coalesce();
  auto parsedContextID =
      MasqueService::parseContextID(datagram->data(), datagram->length());
  if (!parsedContextID || parsedContextID->first != 0x00) {
    return;
  }
  // EASY_END_BLOCK;
  //
  // EASY_BLOCK("DatagramTransactionHandler::onDatagram (2)");
  datagram->trimStart(parsedContextID->second);
  if (std::holds_alternative<QuicStream::UDPProperties>(stream->properties)) {
    auto& properties = std::get<QuicStream::UDPProperties>(stream->properties);
    properties.socket->write(properties.target, move(datagram));
  } else {
    auto& properties = std::get<QuicStream::IPProperties>(stream->properties);
    // EASY_BLOCK("DatagramTransactionHandler::onDatagram
    // (2.1)");
    /*
    Tins::IP packet;
    try {
      packet =
          Tins::RawPDU(datagram->data(), datagram->length()).to<Tins::IP>();
    } catch (...) {
      // TODO: check if we can avoid the exception
      return;
    }
     */
    // EASY_END_BLOCK;
    // EASY_BLOCK("DatagramTransactionHandler::onDatagram
    // (2.2)");
    // auto serializedPacket = packet.serialize();
    tunDevice->write(datagram->writableData(), datagram->length());
    // EASY_END_BLOCK;
  }
}

///////////////
// upstream: //
///////////////

MasqueCallback::MasqueCallback(EventBase* eventBase,
                               QuicStream* upstream,
                               HTTPTransaction* downstreamTransaction)
    : eventBase(eventBase),
      upstream(upstream),
      downstreamTransaction(downstreamTransaction) {
  CHECK(downstreamTransaction);
  CHECK(upstream);
}

IPAddressV4 MasqueCallback::getClientIP() const {
  CHECK(downstreamTransaction);
  const auto ip = downstreamTransaction->getPeerAddress().getIPAddress();
  if (ip.isV4()) {
    return ip.asV4();
  }
  if (ip.isV6() && ip.asV6().isIPv4Mapped()) {
    return ip.asV6().createIPv4();
  }
  throw folly::IPAddressFormatException("couldn't get IPv4");
}

///////////////////////////
// upstream: CONNECT-UDP //
///////////////////////////

ConnectUDPCallback::ConnectUDPCallback(EventBase* eventBase,
                                       QuicStream* upstream,
                                       HTTPTransaction* downstreamTransaction)
    : MasqueCallback(eventBase, upstream, downstreamTransaction) {
}

QuicStream::UDPProperties& ConnectUDPCallback::getProperties() {
  return std::get<QuicStream::UDPProperties>(upstream->properties);
}

void ConnectUDPCallback::getReadBuffer(void** buf, size_t* len) noexcept {
  *buf = readBuffer.data();
  *len = readBuffer.size();
}

void ConnectUDPCallback::onDataAvailable(
    const SocketAddress& address,
    size_t len,
    bool truncated,
    OnDataAvailableParams onDataAvailableParams) noexcept {
  // EASY_FUNCTION();
  // context id
  auto payloadBuffer = MasqueService::prependContextID(
      IOBuf::copyBuffer(readBuffer.data(), len), 0x00);
  payloadBuffer->coalesce();
  // forward to downstream
  auto unwrappedPayloadBuffer = move(*payloadBuffer);
  eventBase->runInEventBaseThread(
      [this, unwrappedPayloadBuffer = move(unwrappedPayloadBuffer)]() {
        // EASY_BLOCK("ConnectUDPCallback::onPacket lambda");
        auto payloadBuffer = make_unique<IOBuf>(move(unwrappedPayloadBuffer));
        auto res = downstreamTransaction->sendDatagram(move(payloadBuffer));
        if (!res) {
          LOG(ERROR) << "Failure to write: " << std::strerror(errno);
        }
      });
}

void ConnectUDPCallback::onReadError(const AsyncSocketException& ex) noexcept {
  LOG(ERROR) << ex.what();
}

void ConnectUDPCallback::onReadClosed() noexcept {
  LOG(ERROR) << __func__;
  // TODO: cleanup
}

void ConnectUDPCallback::errMessage(const cmsghdr& header) noexcept {
  LOG(ERROR) << __func__;
}

void ConnectUDPCallback::errMessageError(
    const AsyncSocketException& ex) noexcept {
  LOG(ERROR) << ex.what();
}

//////////////////////////
// upstream: CONNECT-IP //
//////////////////////////

ConnectIPCallback::ConnectIPCallback(EventBase* eventBase,
                                     QuicStream* upstream,
                                     HTTPTransaction* downstreamTransaction)
    : MasqueCallback(eventBase, upstream, downstreamTransaction) {
}

QuicStream::IPProperties& ConnectIPCallback::getProperties() {
  return std::get<QuicStream::IPProperties>(upstream->properties);
}

void ConnectIPCallback::onPacket(uint8_t* buffer, size_t len) noexcept {
  // LOG(INFO) << __func__ << " len=" << len;
  // EASY_FUNCTION();
  // LOG(INFO) << "Sending " << len << " bytes to stream "
  //          << downstreamTransaction->getID();
  // context id
  auto payloadBuffer =
      MasqueService::prependContextID(IOBuf::copyBuffer(buffer, len), 0x00);
  payloadBuffer->coalesce();
  // forward to downstream
  auto unwrappedPayloadBuffer = move(*payloadBuffer);
  eventBase->runInEventBaseThread(
      [this, unwrappedPayloadBuffer = move(unwrappedPayloadBuffer)]() {
        // EASY_BLOCK("ConnectIPCallback::onPacket lambda");
        auto payloadBuffer = make_unique<IOBuf>(move(unwrappedPayloadBuffer));
        auto res = downstreamTransaction->sendDatagram(move(payloadBuffer));
        if (!res) {
          LOG(ERROR) << "Failure to write: " << std::strerror(errno);
        }
      });
}

/////////////
// server //
////////////

DatagramServer::DatagramServer(Options serverOptions)
    : quicServer(QuicServer::createQuicServer()),
      serverOptions(move(serverOptions)) {
  {
    auto tunDevice = std::make_unique<TunDevice>(TunDevice::uniqueName("tun_s"),
                                                 this->serverOptions.tunNetwork,
                                                 this->serverOptions.tunMTU);
    auto* tunDevicePtr = tunDevice.get();
    sharedTunDevice = std::make_unique<SharedTun>(std::move(tunDevice));
    tunDevicePtr->run();
  }
  MasqueService::setQUICPacketLenV4(this->serverOptions.UDPSendPacketLen);
  quicServer->setBindV6Only(false);
  quicServer->setCongestionControllerFactory(
      make_shared<ServerCongestionControllerFactory>());
  TransportSettings transportSettings;
  MasqueService::applyPerformanceSettingsServer(transportSettings);
  transportSettings.datagramConfig.enabled = true;
  transportSettings.idleTimeout = milliseconds(this->serverOptions.timeout);
  transportSettings.pacingEnabled =
      (this->serverOptions.ccAlgorithm != CongestionControlType::None);
  transportSettings.defaultCongestionController =
      this->serverOptions.ccAlgorithm;
  transportSettings.datagramConfig.framePerPacket =
      this->serverOptions.framePerPacket;
  transportSettings.canIgnorePathMTU = true;
  transportSettings.maxRecvPacketSize = this->serverOptions.maxRecvPacketSize;
  if (this->serverOptions.enableMigration) {
    transportSettings.disableMigration = false;
    transportSettings.maxNumMigrationsAllowed =
        std::numeric_limits<uint16_t>::max();
  }
  quicServer->setTransportSettings(transportSettings);
  quicServer->setSupportedVersion({QuicVersion::MVFST,
                                   QuicVersion::MVFST_EXPERIMENTAL,
                                   QuicVersion::QUIC_V1,
                                   QuicVersion::QUIC_V1_ALIAS,
                                   QuicVersion::QUIC_DRAFT,
                                   QuicVersion::QUIC_V2});
  quicServer->setQuicServerTransportFactory(
      make_unique<DatagramTransportFactory>(
          [this](EventBase* eventBase) {
            return new DatagramTransactionHandler(eventBase,
                                                  this->sharedTunDevice.get());
          },
          this->serverOptions.timeout,
          this->serverOptions.qlogPath));
  quicServer->setQuicUDPSocketFactory(
      make_unique<QuicSharedUDPSocketFactory>());
  {
    samples::HQServerParams serverParams;
    serverParams.txnTimeout = milliseconds(this->serverOptions.timeout);
    quicServer->setFizzContext(samples::createFizzServerContext(serverParams));
  }
  if (!this->serverOptions.qlogPath) {
    MasqueService::SignalHandler::install(SIGTERM, [this](int) {
      LOG(INFO) << "received SIGTERM, shutting down";
      this->shutdown();
      exit(0);
    });
  }
}

DatagramServer::~DatagramServer() {
  shutdown();
}

void DatagramServer::start() {
  size_t THREADS = std::getenv("THREADS") ? atoi(std::getenv("THREADS"))
                                          : std::thread::hardware_concurrency();
  SocketAddress localAddress;
  localAddress.setFromLocalPort(serverOptions.port);
  quicServer->start(localAddress, THREADS);
  // blocks
  quicServer->waitUntilInitialized();
}

void DatagramServer::shutdown() {
}

} // namespace MasqueService

int main(int argc, char* argv[]) {
  // program options
  po::options_description optionsDescription("Allowed options");
  optionsDescription.add_options()("help", "output help message")(
      "timeout",
      po::value<size_t>()->default_value(10000),
      "set timeout in ms")(
      "port", po::value<uint16_t>()->default_value(6666), "set port")(
      "tuntap-network",
      po::value<string>()->default_value("192.168.0.0/24"),
      "set outgoing tuntap subnet (connect-ip)")(
      "cc",
      po::value<string>()->default_value("None"),
      "set congestion control algorithm ('None' | 'Cubic' | 'NewReno' | 'Copa' "
      "| 'Copa2' "
      "| 'BBR' | 'StaticCwnd')")("framePerPacket",
                                 po::value<bool>()->default_value(false),
                                 "force QUIC to use one frame for each packet")(
      "UDPSendPacketLen",
      po::value<uint16_t>(),
      "set UDPSendPacketLen (kDefaultUDPSendPacketLen)")(
      "maxRecvPacketSize", po::value<uint16_t>(), "set maxRecvPacketSize")(
      "qlog", po::value<string>(), "set qlog path")(
      "datagramReadBuf",
      po::value<size_t>()->default_value(16384),
      "set datagram read buffer size")(
      "datagramWriteBuf",
      po::value<size_t>()->default_value(16384),
      "set datagram write buffer size")(
      "tunMTU", po::value<size_t>()->default_value(1500), "set tun MTU");
  po::variables_map variablesMap;
  po::store(po::parse_command_line(argc, argv, optionsDescription),
            variablesMap);
  po::notify(variablesMap);
  if (variablesMap.count("help")) {
    cout << optionsDescription << endl;
    return 1;
  }
  auto subnet =
      IPAddress::tryCreateNetwork(variablesMap["tuntap-network"].as<string>());
  if (subnet.hasError()) {
    cout << optionsDescription << endl;
    return 1;
  }
  if (subnet->second > 24) {
    cout << "subnet mask must be /24 or smaller" << endl;
    return 1;
  }
  if (subnet->first.bytes()[3] != 0) {
    cout << "subnet must be a .0/ subnet" << endl;
    return 1;
  }
  auto subnetV4 =
      std::make_pair(IPAddressV4(subnet->first.str()), subnet->second);
  MasqueService::DatagramServer::Options serverOptions{
      .port = variablesMap["port"].as<uint16_t>(),
      .timeout = variablesMap["timeout"].as<size_t>(),
      .tunNetwork = subnetV4,
      .ccAlgorithm = *quic::congestionControlStrToType(
          boost::algorithm::to_lower_copy(variablesMap["cc"].as<string>())),
      .framePerPacket = variablesMap["framePerPacket"].as<bool>(),
      .UDPSendPacketLen = variablesMap["UDPSendPacketLen"].as<uint16_t>(),
      .maxRecvPacketSize = variablesMap["maxRecvPacketSize"].as<uint16_t>(),
      .enableMigration = false,
      .tunMTU = variablesMap["tunMTU"].as<size_t>()};
  if (variablesMap.count("qlog") &&
      !variablesMap["qlog"].as<string>().empty()) {
    serverOptions.qlogPath = variablesMap["qlog"].as<string>();
  }
  MasqueService::datagramReadBufSize =
      variablesMap["datagramReadBuf"].as<size_t>();
  MasqueService::datagramWriteBufSize =
      variablesMap["datagramWriteBuf"].as<size_t>();
  // google logging
#if FOLLY_HAVE_LIBGFLAGS
  // Enable glog logging to stderr by default.
  gflags::SetCommandLineOptionWithMode(
      "logtostderr", "1", gflags::SET_FLAGS_DEFAULT);
  // gflags::SetCommandLineOptionWithMode(
  //     "minloglevel", "3", gflags::SET_FLAGS_DEFAULT);
#endif
  // folly
  int dummyArgc = 0;
  folly::init(&dummyArgc, &argv, false);
  folly::ssl::init();
  // profiler
  // profiler::startListen();
  // server
  EventBase eventBase;
  MasqueService::DatagramServer datagramServer(move(serverOptions));
  datagramServer.start();
  eventBase.loopForever();
  return 0;
}

// https://github.com/facebook/folly/blob/main/folly/io/async/AsyncSocket.h#L359