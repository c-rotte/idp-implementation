import time

from experiments import postprocessing
from run_experiment import Experiment, override
import experiments.utils as utils
from experiments.utils import Command, CommandType
import poslib as pos


def http_connect_ip_factory(num_hops, num_transactions):
    assert 0 <= num_hops <= 3, "Number of hops must be between 0 and 3"

    CLASS_NAME = f"HttpGETConnectIP{num_hops:03d}x{num_transactions:03d}"

    class HttpClient(Experiment):

        def __init__(self, nodes, variables, command_type, cc):
            super().__init__(CLASS_NAME, nodes[0:1 + num_hops + 1], variables, command_type, cc)
            assert self.command_type in [CommandType.LOGGING, CommandType.PERF, CommandType.LOGGING_QLOG]
            self.client_node = self.nodes[0]
            self.proxy_nodes = self.nodes[1:-1]
            self.server_node = self.nodes[-1]

        @staticmethod
        def needed_node_count():
            return 1 + num_hops + 1

        @staticmethod
        def transaction_count():
            return num_transactions

        @override
        def _pre(self):
            utils.toggle_hyperthreading_parallel(self.nodes, False)
            utils.start_cpu_logging(self.nodes, self.node_output_dir)
            # start http server
            self.http_server_pos_id = utils.execute_command_on_node(Command(
                command=f"./host_scripts/http_server.sh "
                        "1337 "
                        f"{self.cc.lower()} "
                        f"{1472 - (len(self.proxy_nodes) * 86)} "
                        "0 "  # threads = nCPU
                        f"{utils.get_qlog_dir(self.server_node, self.node_output_dir, 'http_server', self.command_type)}",
                command_type=CommandType.LOGGING,
                blocking=False,
                output_path=f"{self.node_output_dir}/http_server.{self.command_type.name}"
            ), self.server_node, priorize=True)

            time.sleep(3)  # wait for server to start

            port = 4433
            framePerPacket = 0
            UDPSendPacketLen = 1472
            maxRecvPacketSize = 1472
            # start proxy servers
            for i, proxy in enumerate(self.proxy_nodes):
                command = f'''
                ./binaries_proxygen/masque-server
                  --timeout 9999999999 
                  --port {port} 
                  --tuntap-network 192.168.{i}.0/16
                  --cc {self.cc} 
                  --framePerPacket {framePerPacket}
                  --UDPSendPacketLen {UDPSendPacketLen}
                  --maxRecvPacketSize {maxRecvPacketSize}
                  {utils.get_qlog_flag(proxy, self.node_output_dir, 'server', self.command_type)}
                  --datagramReadBuf {16834 * 64}
                  --datagramWriteBuf {16834 * 64}
                '''

                utils.execute_command_on_node(Command(
                    command=command,
                    command_type=self.command_type,
                    blocking=False,
                    output_path=f"{self.node_output_dir}/server_{i}.{self.command_type.name}"
                ), proxy, priorize=True)

                port += 1
                UDPSendPacketLen -= 86
                maxRecvPacketSize -= 86

            time.sleep(5)

        @override
        def _start(self):
            hosts = [proxy.address_eno3 for proxy in self.proxy_nodes]
            ports = list(range(4433, 4433 + len(self.proxy_nodes)))
            paths = ["/.well-known/masque/ip"] * len(self.proxy_nodes)
            UDPSendPacketLens = [1472 - (i * 86) for i in range(len(self.proxy_nodes))]
            maxRecvPacketSizes = [1472 - (i * 86) for i in range(len(self.proxy_nodes))]
            ccs = ["None"] * len(self.proxy_nodes)
            if ccs:
                ccs[0] = self.cc
            framePerPackets = [0] * len(self.proxy_nodes)

            command = f"""
            ./binaries_proxygen/masque-http-client
              --ip {self.server_node.address_eno3}
              --port 1337
              --method GET
              --timeout 9999999999
              --UDPSendPacketLen {1472 - (len(self.proxy_nodes) * 86)}
              --maxRecvPacketSize {1472 - (len(self.proxy_nodes) * 86)}
              --numTransactions {num_transactions}
              {utils.get_qlog_flag(self.client_node, self.node_output_dir, 'http_client', self.command_type)}
            """
            if len(hosts) > 0:
                command += f"""
                  --cc None
                  --modes {' '.join(["connect-ip"] * len(hosts))} 
                  --hosts {' '.join(hosts)} 
                  --ports {' '.join(map(str, ports))} 
                  --paths {' '.join(paths)} 
                  --UDPSendPacketLens {' '.join(map(str, UDPSendPacketLens))} 
                  --maxRecvPacketSizes {' '.join(map(str, maxRecvPacketSizes))} 
                  --ccs {' '.join(ccs)} 
                  --framePerPackets {' '.join(map(str, framePerPackets))} 
                """
            else:
                command += f"""
                  --cc {self.cc}
                """
            utils.execute_command_on_node(Command(
                command=command,
                command_type=self.command_type,
                blocking=True,
                output_path=f"{self.node_output_dir}/http_client.{self.command_type.name}"
            ), self.client_node, priorize=True)

        @override
        def _stop(self):
            if self.command_type == CommandType.LOGGING_QLOG:
                utils.await_command(self.http_server_pos_id)
            utils.kill_all_parallel(self.nodes)
            if self.command_type == CommandType.LOGGING_QLOG:
                # check if the qlog files were saved
                client_qlog_exists = utils.file_exists_on_server(self.client_node,
                                                                 f"{self.node_output_dir}/http_client/*.qlog.gz")
                server_qlog_exists = utils.file_exists_on_server(self.server_node,
                                                                 f"{self.node_output_dir}/http_server/*.qlog.gz")
                if not client_qlog_exists or not server_qlog_exists:
                    self.should_repeat = True

        @override
        def _post(self):
            utils.stop_cpu_logging(self.nodes)
            zipped_nodes = [(node, self.node_output_dir) for node in self.nodes]
            postprocessing.postprocess_nodes(zipped_nodes)
            postprocessing.download_results(zipped_nodes, self.log_dir)
            postprocessing.postprocess_coinbase(self.log_dir)

    HttpClient.__name__ = CLASS_NAME
    return HttpClient


# Create all combinations:
MAX_HOPS = 3
MAX_TRANSACTIONS = 256

for num_hops in range(0, MAX_HOPS + 1):
    for num_transactions in utils.exponential_range(1, MAX_TRANSACTIONS + 1):
        class_name = f"HttpConnectIP{num_hops}x{num_transactions}"
        globals()[class_name] = http_connect_ip_factory(num_hops, num_transactions)
