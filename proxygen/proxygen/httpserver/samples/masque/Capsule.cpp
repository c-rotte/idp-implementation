#include "Capsule.h"

#include <folly/io/Cursor.h>
#include <proxygen/lib/utils/Logging.h>
#include <quic/codec/QuicInteger.h>

using namespace std;
using namespace folly;

namespace MasqueService {

namespace {

unique_ptr<IOBuf> toBuffer(unique_ptr<IOBuf> data, Capsule::Type type)
// takes in data and a capsule type and turns them into a capsule
{
  auto result = IOBuf::create(0);
  quic::BufAppender appender(result.get(), 32);
  quic::encodeQuicInteger(type, [&](auto val) { appender.writeBE(val); });
  quic::encodeQuicInteger(data->length(),
                          [&](auto val) { appender.writeBE(val); });
  appender.insert(move(data));
  return result;
}

pair<uint64_t, CIDRNetwork> parseAddress(io::Cursor& cursor)
// requestID, IP
{
  CHECK(!cursor.isAtEnd());
  auto requestID = quic::decodeQuicInteger(cursor);
  auto ipVersion = cursor.read<unsigned char>();
  CHECK(ipVersion == 4 || ipVersion == 6);
  IPAddress ipAddress;
  if (ipVersion == 4) {
    // ipv4
    ByteArray4 byteArray;
    cursor.pull(byteArray.data(), 4);
    ipAddress = IPAddressV4(byteArray);
  } else {
    // ipv6
    ByteArray16 byteArray;
    cursor.pull(byteArray.data(), 16);
    ipAddress = IPAddressV6(byteArray);
  }
  auto prefixLength = cursor.read<unsigned char>();
  return make_pair(requestID->first, make_pair(ipAddress, prefixLength));
}

} // namespace

unique_ptr<Capsule> Capsule::parseCapsule(unique_ptr<IOBuf> datagram) {
  io::Cursor cursor(datagram.get());
  auto capsuleType = quic::decodeQuicInteger(cursor);
  auto capsuleLength = quic::decodeQuicInteger(cursor);
  if (!capsuleType || !capsuleLength) {
    return nullptr;
  }
  datagram->trimStart(capsuleType->second + capsuleLength->second);
  switch (capsuleType->first) {
    case Type::DATA: {
      // https://datatracker.ietf.org/doc/html/rfc9298#name-http-datagram-payload-forma
      auto contextID = quic::decodeQuicInteger(cursor);
      if (!contextID) {
        return nullptr;
      }
      datagram->trimStart(contextID->second);
      auto capsule = make_unique<DatagramCapsule>();
      capsule->data = move(datagram);
      return capsule;
    }
    case Type::ADDRESS_ASSIGN: {
      // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-05.html#name-address_assign-capsule
      auto capsule = make_unique<AddressAssignCapsule>();
      while (!cursor.isAtEnd()) {
        auto [requestID, ipAddress] = parseAddress(cursor);
        // TODO: check requestID
        capsule->addresses.push_back(
            Address::Address{.requestID = requestID, .ipAddress = ipAddress});
      }
      return capsule;
    }
    case Type::ADDRESS_REQUEST: {
      // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-05.html#name-address_request-capsule
      auto capsule = make_unique<AddressRequestCapsule>();
      while (!cursor.isAtEnd()) {
        auto [requestID, ipAddress] = parseAddress(cursor);
        CHECK(requestID != 0);
        // TODO: handle requestID
        capsule->addresses.push_back(
            Address::Address{.requestID = requestID, .ipAddress = ipAddress});
      }
      CHECK(!capsule->addresses.empty());
      return capsule;
    }
    case Type::ROUTE_ADVERTISEMENT: {
      auto capsule = make_unique<RouteAdvertisementCapsule>();
      while (!cursor.isAtEnd()) {
        auto ipVersion = cursor.read<unsigned char>();
        CHECK(ipVersion == 4 || ipVersion == 6);
        IPAddress ipAddressA, ipAddressB;
        if (ipVersion == 4) {
          // ipv4
          ByteArray4 byteArray;
          cursor.pull(byteArray.data(), 4);
          ipAddressA = IPAddressV4(byteArray);
          cursor.pull(byteArray.data(), 4);
          ipAddressB = IPAddressV4(byteArray);
        } else {
          // ipv6
          ByteArray16 byteArray;
          cursor.pull(byteArray.data(), 16);
          ipAddressA = IPAddressV6(byteArray);
          cursor.pull(byteArray.data(), 16);
          ipAddressB = IPAddressV6(byteArray);
        }
        auto protocol = cursor.read<unsigned char>();
        CHECK(protocol == 4 || protocol == 6);
        capsule->ranges.emplace_back(
            Address::AddressRange{.startIP = ipAddressA, .endIP = ipAddressB});
      }
      CHECK(is_sorted(
          capsule->ranges.begin(),
          capsule->ranges.end(),
          [](const auto& rangeA, const auto& rangeB) {
            if (rangeA.startIP.isV4() != rangeB.startIP.isV4()) {
              // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-05.html#section-4.6.3-9.1
              return rangeA.startIP.isV4();
            }
            if (rangeA.startIP.version() != rangeB.startIP.version()) {
              // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-05.html#section-4.6.3-9.2
              return rangeA.startIP.version() < rangeB.startIP.version();
            }
            // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-05.html#section-4.6.3-9.3
            return rangeA.endIP < rangeB.startIP;
          }));
      return capsule;
    }
    default: {
      // https://www.rfc-editor.org/rfc/rfc9297.html#section-3.2-7
      auto capsule = make_unique<UnknownCapsule>();
      capsule->capsuleType = capsuleType->first;
      return capsule;
    }
  }
}

unique_ptr<IOBuf> DatagramCapsule::toBuffer() const {
  auto buf = make_unique<IOBuf>(*data);
  return MasqueService::toBuffer(move(buf), type);
}

unique_ptr<IOBuf> Address::Address::toBuffer() const {
  auto buf = IOBuf::create(0);
  quic::BufAppender appender(buf.get(), 32);
  quic::encodeQuicInteger(requestID, [&](auto val) { appender.writeBE(val); });
  appender.writeBE(ipAddress.first.version());
  // TODO: support ipv6
  appender.writeBE(ipAddress.first.asV4().toLongHBO());
  appender.writeBE(ipAddress.second);
  return buf;
}

unique_ptr<IOBuf> Address::AddressRange::toBuffer() const {
  auto buf = IOBuf::create(0);
  quic::BufAppender appender(buf.get(), 32);
  CHECK(startIP.version() == endIP.version());
  appender.writeBE(startIP.version());
  // TODO: support ipv6
  appender.writeBE(startIP.asV4().toLong());
  appender.writeBE(endIP.asV4().toLong());
  appender.writeBE(endIP.version());
  return buf;
}

unique_ptr<IOBuf> AddressAssignCapsule::toBuffer() const {
  auto buf = IOBuf::create(0);
  quic::BufAppender appender(buf.get(), 32);
  for (const auto& address : addresses) {
    appender.insert(address.toBuffer());
  }
  return MasqueService::toBuffer(move(buf), type);
}

unique_ptr<IOBuf> AddressRequestCapsule::toBuffer() const {
  auto buf = IOBuf::create(0);
  quic::BufAppender appender(buf.get(), 32);
  for (const auto& address : addresses) {
    appender.insert(address.toBuffer());
  }
  return MasqueService::toBuffer(move(buf), type);
}

unique_ptr<IOBuf> RouteAdvertisementCapsule::toBuffer() const {
  auto buf = IOBuf::create(0);
  quic::BufAppender appender(buf.get(), 32);
  for (const auto& range : ranges) {
    appender.insert(range.toBuffer());
  }
  return MasqueService::toBuffer(move(buf), type);
}

unique_ptr<IOBuf> UnknownCapsule::toBuffer() const {
  return MasqueService::toBuffer(IOBuf::create(0), type);
}

} // namespace MasqueService