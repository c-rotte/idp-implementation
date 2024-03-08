import time

from experiments import postprocessing
from run_experiment import Experiment, override
import experiments.utils as utils
from experiments.utils import Command, CommandType
import poslib as pos


def tun_hop_factory(num_hops, num_transactions):
    assert 1 <= num_hops <= 4, "Number of hops must be between 1 and 4"

    CLASS_NAME = f"TunHop{num_hops:03d}x{num_transactions:03d}"

    class TunHop(Experiment):

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
            utils.start_cpu_logging(self.nodes, self.node_output_dir)
            # start multiple iperf servers for multiple transactions:
            for i in range(num_transactions):
                port = 5201 + i  # iperf default port is 5201
                utils.execute_command_on_node(Command(
                    command=f"iperf3 -s -p {port}",
                    command_type=CommandType.NONE,
                    blocking=False
                ), self.server_node)
            time.sleep(3)

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

            # start client proxy
            hosts = [proxy.address_eno3 for proxy in self.proxy_nodes]
            ports = list(range(4433, 4433 + len(self.proxy_nodes)))  # 4433, 4434, 4435, ...
            paths = ["/.well-known/masque/ip"] * len(self.proxy_nodes)
            UDPSendPacketLens = [1472 - (i * 86) for i in range(len(self.proxy_nodes))]
            maxRecvPacketSizes = [1500] * len(self.proxy_nodes)
            ccs = ["None"] * len(self.proxy_nodes)
            ccs[0] = self.cc
            framePerPackets = [0] * len(self.proxy_nodes)
            command = f"""
            ./binaries_proxygen/masque-client 
              --timeout 99999999 
              --modes {' '.join(["connect-ip"] * len(hosts))} 
              --hosts {' '.join(hosts)} 
              --ports {' '.join(map(str, ports))} 
              --paths {' '.join(paths)} 
              --UDPSendPacketLens {' '.join(map(str, UDPSendPacketLens))} 
              --maxRecvPacketSizes {' '.join(map(str, maxRecvPacketSizes))} 
              --ccs {' '.join(ccs)} 
              --framePerPackets {' '.join(map(str, framePerPackets))} 
              --numTransactions {num_transactions} 
              {utils.get_qlog_flag(self.client_node, self.node_output_dir, 'client', self.command_type)}
            """
            utils.execute_command_on_node(Command(
                command=command,
                command_type=self.command_type,
                blocking=False,
                output_path=f"{self.node_output_dir}/client.{self.command_type.name}"
            ), self.client_node, priorize=True)

            time.sleep(15)

        @override
        def _start(self):
            # Execute multiple iperf clients for each tun_client
            sleep_until = utils.system_time(
                self.client_node) + 0.5 * num_transactions  # 1 second between each transaction
            sleep_until = int(sleep_until)

            port = 5201
            command = f"""./host_scripts/iperf_clients.sh 
                {self.server_node.address_eno3} 
                {sleep_until} 
                {port} 
                {num_transactions} 
                {self.node_output_dir}"""
            utils.execute_command_on_node(Command(
                command=command,
                command_type=CommandType.NONE,
                blocking=True
            ), self.client_node, priorize=True)

        @override
        def _stop(self):
            qlog = self.command_type == CommandType.LOGGING_QLOG
            utils.kill_all_parallel(self.nodes, qlog)

        @override
        def _post(self):
            utils.stop_cpu_logging(self.nodes)
            zipped_nodes = [(node, self.node_output_dir) for node in self.nodes]
            postprocessing.postprocess_nodes(zipped_nodes)
            postprocessing.download_results(zipped_nodes, self.log_dir)
            postprocessing.postprocess_coinbase(self.log_dir)

    TunHop.__name__ = CLASS_NAME
    return TunHop


MAX_HOPS = 4
MAX_TRANSACTIONS = 256

for num_hops in range(1, MAX_HOPS + 1):
    for num_transactions in utils.exponential_range(1, MAX_TRANSACTIONS + 1):
        class_ = tun_hop_factory(num_hops, num_transactions)
        globals()[class_.__name__] = tun_hop_factory(num_hops, num_transactions)
