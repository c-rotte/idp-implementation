#include "ConnectClient.h"

using namespace std;
using namespace folly;
using namespace proxygen;

namespace MasqueService {

void TunReadCallback::getReadBuffer(void** buf, size_t* len) noexcept {
  *buf = readBuffer.data();
  *len = readBuffer.size();
}

void TunReadCallback::onReadError(const AsyncSocketException& ex) noexcept {
  LOG(ERROR) << ex.what();
}

void TunReadCallback::onReadClosed() noexcept {
  LOG(WARNING) << __func__;
}

} // namespace MasqueService
