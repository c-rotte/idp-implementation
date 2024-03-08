#include "MasqueUpstream.h"

#include <folly/io/IOBuf.h>

using namespace std;
using namespace folly;
using namespace proxygen;

namespace MasqueService {

QuicStream::QuicStream(UDPProperties properties)
    : properties(move(properties)) {
}

QuicStream::QuicStream(IPProperties properties) : properties(move(properties)) {
}

} // namespace MasqueService