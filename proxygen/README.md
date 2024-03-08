# MASQUE Server & Client Build Instructions

## Dependencies

Ensure you have the following dependencies installed on your system:

- Google's logging library, `glog`: [GitHub - glog](https://github.com/google/glog)
- Boost library (>= 1.83): [Boost](https://www.boost.org/) (We need Boost >= 1.83 because of the io_uring backend.)

For other dependencies, refer to the script file located at `proxygen/help/install_dependencies.sh`.

## Build Instructions

```bash
git clone git@gitlab.lrz.de:netintum/teaching/tumi8-theses/masque-theses/proxygen.git
git clone git@gitlab.lrz.de:netintum/teaching/tumi8-theses/masque-theses/mvfst.git
cd mvfst && git checkout idp/main && cd ..
cd proxygen && git checkout idp/main && cd ..
```

To build the project, navigate to the `proxygen` directory and run the `build.sh` script with appropriate flags:

Building may take a while since it builds the entire `mvfst` codebase as well as most of the
dependencies. However, this has the advantage that we can statically link the libraries on Debian-based systems.

```bash
cd proxygen/proxygen && ./build.sh --no-install-dependencies --with-quic --masque --jobs 1
```

Note that we rename `proxygen_h3datagram_client` to `masque-client`, `proxygen_masque_http_client`
to `masque-http-client`, and `hq` to `hq-server` in the CI.

## Server Configuration

### Parameters for Server Commands:

- `timeout`: Time in milliseconds for the server to remain active.
- `port`: The port on which the server listens.
- `cc`: Congestion Control algorithm (`None` | `Cubic` | `NewReno` | `Copa` | `Copa2` | `BBR` | `StaticCwnd`)
- `framePerPacket`: Indicates whether to use one QUIC frame per packet. (0 means don't force, 1 means force)
- `UDPSendPacketLen`: Maximum size of QUIC packets that the server can send.
- `qlog`: Specify the directory to store the qlog files in.

## Client Configuration

### Parameters for Client Commands:

The parameters for the client commands are mostly similar to those for the server, with a few additions:

- `modes`: Connection modes (`connect-ip` | `connect-udp`).
- `hosts`: Proxy server IPs.
- `ports`: Proxy server ports.
- `paths`: Proxy server paths.
- `UDPSendPacketLens`: The maximum length of QUIC packets to send for each hop.
- `maxRecvPacketSizes`: Maximum size of received QUIC packets for each hop.
- `numTransactions`: Number of concurrent HTTP transactions.

## Notes on Parameters:

### UDPSendPacketLen / maxRecvPacketSize

- `UDPSendPacketLen` and `maxRecvPacketSize` specifies the max. size of the QUIC packet. (This follows the naming
  convention used in the `mvfst`
  codebase. Don't ask me why.)

### Largest Sent IP Packet

- The largest sent IP packet size can be calculated as
  follows: `hop[0].UDPSendPacketLen + 20 (IP header size) + 8 (UDP header size)`.

For instance, if `hop[0].UDPSendPacketLen` is set to `1472`, then the largest sent IP packet size would be `1472` (
UDPSendPacketLen) + `20` (IP header size) + `8` (UDP header size) = `1500` bytes.

Thus, we usually start with a QUIC packet size of `1472`.

### Packet Overheads

- In our experiments, we have observed that the total overhead per hop is `86` bytes for Connect-IP and `58` bytes for
  Connect-UDP. But this may change depending on the environment.

## Examples

### TUN-Connect-IP

The following example shows how to run a client in `connect-ip` with two hops.

#### Hop 1 on `ether`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4433 \
  --cc None \
  --tuntap-network 192.168.0.0/24 \
  --framePerPacket 0 \
  --UDPSendPacketLen 1472 \
  --maxRecvPacketSize 1472
```

#### Hop 2 on `ethercash`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4434 \
  --cc None \
  --tuntap-network 192.168.1.0/24
  --framePerPacket 0 \
  --UDPSendPacketLen 1386 \
  --maxRecvPacketSize 1386
```

#### Client on `ethercash`

```bash
./binaries_proxygen/masque-client \
  --timeout 99999999 \
  --modes connect-ip connect-ip \
  --hosts <ether_ip> <ethercash_ip> \
  --ports 4433 4434 \
  --paths /.well-known/masque/ip /.well-known/masque/ip \
  --UDPSendPacketLens 1472 1386 \
  --maxRecvPacketSizes 1500 1500 \
  --ccs None None \
  --framePerPackets 0 0 \
  --numTransactions 1 \
  --tuntap-ip 192.169.1.0
```

This opens a TUN interface on the client called `tun_client_0`.
If you choose to have more transactions, the names of the TUN devices will be of the format `f"tun_client_{i * 4}"`.

### HTTP-Connect-IP

The following example shows how to run a client in `connect-ip` with two hops. This will perform a GET request to
download a 1GB file. Additionally, we will log the packets in qlog format.

#### Hop 1 on `ether`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4433 \
  --cc None \
  --tuntap-network 192.168.0.0/24 \
  --framePerPacket 0 \
  --UDPSendPacketLen 1472 \
  --maxRecvPacketSize 1472
```

#### Hop 2 on `ethercash`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4434 \
  --cc None \
  --tuntap-network 192.168.1.0/24
  --framePerPacket 0 \
  --UDPSendPacketLen 1386 \
  --maxRecvPacketSize 1386
```

#### Server on `ether`

```bash
mkdir -p /tmp/qlog_server

# cert.crt and cert.key are self-created certificates
./binaries_proxygen/hq-server \
    -mode server \
    -cert ./resources/cert.crt \
    -key ./resources/cert.key \
    -host 0.0.0.0 \
    -port 1337 \
    -httpversion "3" \
    -outdir /tmp/www \
    -logdir /tmp/logs \
    -log_response true \
    -qlogger_path /tmp/qlog_server \
    -pretty_json false \
    -congestion None
````

#### Client on `ethercash`

```bash
mkdir -p /tmp/qlog_client

./binaries_proxygen/masque-http-client \
  --ip <ether_ip> \
  --port 1337 \
  --method GET \
  --timeout 9999999999 \
  --UDPSendPacketLen 1300 \
  --maxRecvPacketSize 1300 \
  --cc None \
  --numTransactions 1 \
  --qlog /tmp/qlog_client \
  --modes connect-ip connect-ip \
  --hosts <ether_ip> <ethercash_ip> \
  --ports 4433 4434 \
  --paths /.well-known/masque/ip /.well-known/masque/ip \
  --UDPSendPacketLens 1472 1386 \
  --maxRecvPacketSizes 1472 1386 \
  --ccs None None \
  --framePerPackets 0 0
```

### HTTP-Connect-UDP

The following example shows how to run a client in `connect-udp` with two hops.
This will perform a GET request to download a 1GB file. We parallelize the download by using 4 transactions.

#### Hop 1 on `ether`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4433 \
  --cc None \
  --framePerPacket 0 \
  --UDPSendPacketLen 1472 \
  --maxRecvPacketSize 1472
```

#### Hop 2 on `ethercash`

```bash
./binaries_proxygen/masque-server \
  --timeout 9999999999 \
  --port 4434 \
  --cc None \
  --framePerPacket 0 \
  --UDPSendPacketLen 1414 \
  --maxRecvPacketSize 1414
```

#### Server on `ether`

```bash
# cert.crt and cert.key are self-created certificates
./binaries_proxygen/hq-server \
    -mode server \
    -cert ./resources/cert.crt \
    -key ./resources/cert.key \
    -host 0.0.0.0 \
    -port 1337 \
    -httpversion "3" \
    -outdir /tmp/www \
    -logdir /tmp/logs \
    -log_response true \
    -congestion None
```

#### Client on `ethercash`

```bash
./binaries_proxygen/masque-http-client \
  --ip [HTTP_SERVER_PROXY_ADDRESS] \
  --port 1337 \
  --method GET \
  --timeout 9999999999 \
  --UDPSendPacketLen 1356 \
  --maxRecvPacketSize 1356 \
  --cc None \
  --numTransactions 4 \
  --modes connect-udp connect-udp \
  --hosts <ether_ip> <ethercash_ip> \
  --ports 4433 4434 \
  --paths "/.well-known/masque/udp/<ethercash_ip>/4434" "/.well-known/masque/udp/<ether_ip>/1337" \
  --UDPSendPacketLens 1472 1414 \
  --maxRecvPacketSizes 1472 1414 \
  --ccs [CC_VALUE] [CC_VALUE] \
  --framePerPackets 0 0
```

## Implementation Details

- `masque-server` spawns one TUN device which is shared between the transactions. This may clash when multiple clients
  want to connect. To avoid this, we can create multiple shared TUN devices and assign them to each client. This is not
  implemented yet.