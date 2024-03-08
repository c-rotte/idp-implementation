/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AlwaysAcceptCertificateVerifier.h"
#include "ConnectClient.h"
#include "ConnectIPClient.h"
#include "ConnectUDPClient.h"
#include "proxygen/httpserver/samples/masque/help/MasqueUtils.h"
#include "proxygen/httpserver/samples/masque/help/SignalHandler.h"
#include <algorithm>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GFlags.h>
#include <folly/ssl/Init.h>
#include <proxygen/lib/transport/H3DatagramAsyncSocket.h>

#include <easy/profiler.h>

using namespace std;
using namespace folly;
using namespace proxygen;
using namespace MasqueService;

namespace po = boost::program_options;

namespace {

vector<OptionPair> generateOptions(
    vector<bool> connectIPModes,
    size_t timeout,
    vector<string> hosts,
    vector<uint16_t> ports,
    vector<string> paths,
    size_t numTransactions,
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
  CHECK(!hosts.empty());
  vector<OptionPair> result;
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
    options.defaultCCType = ccAlgorithms[i];
    options.framePerPacket = framePerPackets[i];
    MasqueService::applyPerformanceSettingsClient(options.transportSettings);
    OptionPair pair{.connectIP = connectIPModes[i],
                    .options = move(options),
                    .UDPSendPacketLen = UDPSendPacketLens[i],
                    .maxRecvPacketSize = maxRecvPacketSizes[i]};
    result.push_back(move(pair));
  }
  if (!result.empty()) {
    // apply the number of transactions to the last hop
    result.back().options.transactions_ = numTransactions;
  }
  return result;
}

void startClient(const po::variables_map& variablesMap, EventBase& eventBase) {
  // args
  vector<bool> connectIPModes;
  {
    auto modes = variablesMap["modes"].as<vector<string>>();
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
  auto timeout = variablesMap["timeout"].as<size_t>();
  auto hosts = variablesMap["hosts"].as<vector<string>>();
  auto ports = variablesMap["ports"].as<vector<uint16_t>>();
  auto paths = variablesMap["paths"].as<vector<string>>();
  auto numTransactions = variablesMap["numTransactions"].as<size_t>();
  auto UDPSendPacketLens =
      variablesMap["UDPSendPacketLens"].as<vector<size_t>>();
  auto maxRecvPacketSizes =
      variablesMap["maxRecvPacketSizes"].as<vector<size_t>>();
  vector<quic::CongestionControlType> ccAlgorithms;
  {
    auto ccs = variablesMap["ccs"].as<vector<string>>();
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
  auto framePerPackets = variablesMap["framePerPackets"].as<vector<bool>>();
  // socket options
  bool lastIsConnectIP = connectIPModes.back();
  string lastPath = paths.back();
  auto options = generateOptions(move(connectIPModes),
                                 timeout,
                                 move(hosts),
                                 move(ports),
                                 move(paths),
                                 numTransactions,
                                 move(UDPSendPacketLens),
                                 move(maxRecvPacketSizes),
                                 move(ccAlgorithms),
                                 move(framePerPackets));
  optional<CIDRNetworkV4> tunNetwork;
  if (variablesMap.count("tuntap-ip")) {
    auto generalNetwork =
        IPAddress::createNetwork(variablesMap["tuntap-ip"].as<string>());
    tunNetwork = make_pair(generalNetwork.first.asV4(), generalNetwork.second);
  }
  optional<string> qlogPath;
  if (variablesMap.count("qlog") &&
      !variablesMap["qlog"].as<string>().empty()) {
    qlogPath = variablesMap["qlog"].as<string>();
  } else {
    MasqueService::SignalHandler::install(SIGTERM, [](int) {
      LOG(INFO) << "received SIGTERM, shutting down";
      exit(0);
    });
  }
  if (lastIsConnectIP) {
    ConnectIPClient client(&eventBase, move(options), tunNetwork, qlogPath);
    client.start();
    eventBase.loopForever();
  } else {
    auto sourcePort = variablesMap["source-port"].as<uint16_t>();
    // get the destination udp port from the path
    vector<string> splitRes;
    folly::split("/", lastPath, splitRes);
    CHECK(tunNetwork);
    ConnectUDPClient::UDPClientTUNOptions tunOptions{
        .tunDeviceNetwork = *tunNetwork,
        .sourcePort = sourcePort,
        .destinationPort = folly::to<uint16_t>(splitRes.back())};
    ConnectUDPClient client(
        &eventBase, move(options), move(tunOptions), qlogPath);
    client.start();
    eventBase.loopForever();
  }
}

} // namespace

int main(int argc, char* argv[]) {
  LOG(INFO) << "Starting client...";
  // program options
  po::options_description optionsDescription("Allowed options");
  optionsDescription.add_options()("help", "output help message")(
      "qlog", po::value<string>(), "set qlog path")(
      "modes",
      po::value<vector<string>>()->multitoken(),
      "'{connect-ip' | 'connect-udp}'")(
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
      "set maxRecvPacketSizes (transportSettings.maxRecvPacketSize) for each "
      "hop")("tuntap-ip",
             po::value<string>(),
             "set tun network for packet input (last hop: optional for "
             "connect-ip, mandatory for connect-udp)")(
      "ccs",
      po::value<vector<string>>()->multitoken(),
      "set congestion control algorithms ('None' | 'Cubic' | 'NewReno' | "
      "'Copa' "
      "| 'Copa2' | 'BBR' | 'StaticCwnd')")(
      "framePerPackets",
      po::value<vector<bool>>()->multitoken(),
      "force QUIC to use one frame for each packet")(
      "source-port",
      po::value<uint16_t>()->default_value(51337),
      "expected source port for incoming udp packets on the tun device (only if "
      "the last hop is connect-udp)")("connect-ip-port", po::value<uint16_t>()->default_value(1337),
                                      "set connect-ip port")(
      "datagramReadBuf", po::value<size_t>()->default_value(16384), "set datagram read buffer size")(
      "datagramWriteBuf", po::value<size_t>()->default_value(16384), "set datagram write buffer size")(
	  "tun-starting-nr", po::value<size_t>()->default_value(0), "set first tun nr (tun_client_<nr>)");
  po::variables_map variablesMap;
  po::store(po::parse_command_line(argc, argv, optionsDescription),
            variablesMap);
  po::notify(variablesMap);
  if (variablesMap.count("help")) {
    cout << optionsDescription << endl;
    return 1;
  }
  if (!variablesMap.count("modes")) {
    cout << optionsDescription << endl;
    return 1;
  }
  auto lastMode = variablesMap["modes"].as<vector<string>>().back();
  if (lastMode != "connect-udp" && lastMode != "connect-ip") {
    cout << optionsDescription << endl;
    return 1;
  }
  const auto checkRequiredArgs = [lastMode, &variablesMap]() {
    static unordered_set<string> requiredArgs;
    if (lastMode == "connect-ip") {
      requiredArgs = {"modes",
                      "timeout",
                      "hosts",
                      "ports",
                      "paths",
                      "numTransactions",
                      "UDPSendPacketLens",
                      "maxRecvPacketSizes",
                      "ccs",
                      "framePerPackets"};
    }
    if (lastMode == "connect-udp") {
      requiredArgs = {"modes",
                      "timeout",
                      "hosts",
                      "ports",
                      "paths",
                      "numTransactions",
                      "UDPSendPacketLens",
                      "maxRecvPacketSizes",
                      "tuntap-ip",
                      "ccs",
                      "framePerPackets",
                      "source-port"};
    }
    return std::all_of(
        requiredArgs.begin(),
        requiredArgs.end(),
        [&variablesMap](const auto& arg) { return variablesMap.count(arg); });
  };
  LayeredConnectIPSocket::MASQUE_UDP_PORT = variablesMap["connect-ip-port"].as<uint16_t>();
  MasqueService::datagramReadBufSize = variablesMap["datagramReadBuf"].as<size_t>();
  MasqueService::datagramWriteBufSize = variablesMap["datagramWriteBuf"].as<size_t>();
  MasqueService::FIRST_TUN_NUMBER = variablesMap["tun-starting-nr"].as<size_t>();
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
  folly::ssl::init();

  // profiler
  profiler::startListen(profiler::DEFAULT_PORT + 1);

  EventBase eventBase;
  if (!checkRequiredArgs()) {
    cout << optionsDescription << endl;
    return 1;
  }
  startClient(variablesMap, eventBase);
  return 0;
}
