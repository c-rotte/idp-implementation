#pragma once

#include <folly/IPAddress.h>
#include <folly/io/IOBuf.h>
#include <memory>
#include <quic/codec/QuicInteger.h>

namespace MasqueService {

struct Capsule {

  enum StreamType { CONNECT_UDP, CONNECT_IP };

  enum Type {
    // https://www.ietf.org/archive/id/draft-ietf-masque-connect-ip-08.html#name-capsule-type-registrations
    DATA = 0x0,
    ADDRESS_ASSIGN = 0x01,      // 0x2575D601
    ADDRESS_REQUEST = 0x02,     // 0x2575D602
    ROUTE_ADVERTISEMENT = 0x03, // 0x2575D603
    UNKNOWN
  };

  Type type;

  explicit Capsule(Type type) : type(type) {
  }
  virtual ~Capsule() = default;

  virtual std::unique_ptr<folly::IOBuf> toBuffer() const = 0;

  static std::unique_ptr<Capsule> parseCapsule(std::unique_ptr<folly::IOBuf>);
};

struct DatagramCapsule : public Capsule {

  std::unique_ptr<folly::IOBuf> data;

  DatagramCapsule() : Capsule(DATA) {
  }

  std::unique_ptr<folly::IOBuf> toBuffer() const override;
};

namespace Address {

struct Address {
  std::size_t requestID;
  folly::CIDRNetwork ipAddress;
  std::unique_ptr<folly::IOBuf> toBuffer() const;
};

struct AddressRange {
  folly::IPAddress startIP, endIP;
  std::unique_ptr<folly::IOBuf> toBuffer() const;
};

} // namespace Address

struct AddressAssignCapsule : public Capsule {

  std::vector<Address::Address> addresses;

  AddressAssignCapsule() : Capsule(ADDRESS_ASSIGN) {
  }

  std::unique_ptr<folly::IOBuf> toBuffer() const override;
};

struct AddressRequestCapsule : public Capsule {

  std::vector<Address::Address> addresses;

  AddressRequestCapsule() : Capsule(ADDRESS_REQUEST) {
  }

  std::unique_ptr<folly::IOBuf> toBuffer() const override;
};

struct RouteAdvertisementCapsule : public Capsule {

  // ipRange, protocol
  std::vector<Address::AddressRange> ranges;

  RouteAdvertisementCapsule() : Capsule(ROUTE_ADVERTISEMENT) {
  }

  std::unique_ptr<folly::IOBuf> toBuffer() const override;
};

struct UnknownCapsule : public Capsule {

  std::uint64_t capsuleType;

  UnknownCapsule() : Capsule(UNKNOWN) {
  }

  std::unique_ptr<folly::IOBuf> toBuffer() const override;
};

} // namespace MasqueService
