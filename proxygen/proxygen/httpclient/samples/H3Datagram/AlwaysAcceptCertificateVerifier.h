#pragma once

#include <fizz/protocol/CertificateVerifier.h>

namespace proxygen {

// This is an insecure certificate verifier that accepts everything
class AlwaysAcceptCertificateVerifier : public fizz::CertificateVerifier {
 public:
  ~AlwaysAcceptCertificateVerifier() override = default;

  std::shared_ptr<const folly::AsyncTransportCertificate> verify(
      const std::vector<std::shared_ptr<const fizz::PeerCert>>&)
      const override {
    // accept everything
    return nullptr;
  }

  std::vector<fizz::Extension> getCertificateRequestExtensions()
      const override {
    return std::vector<fizz::Extension>();
  }
};
} // namespace proxygen
