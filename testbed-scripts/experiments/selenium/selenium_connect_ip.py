import time

from experiments import postprocessing
from run_experiment import Experiment, override
import experiments.utils as utils
from experiments.utils import Command, CommandType
import poslib as pos


def selenium_factory(num_hops, num_clients, num_transactions_per_client):
    assert 0 <= num_hops <= 4, "Number of hops must be between 1 and 4"
    assert num_clients >= 1, "Number of clients must be at least 1"
    assert num_transactions_per_client >= 1, "Number of transactions per client must be at least 1"

    CLASS_NAME = f"TunSeleniumIP{num_hops:03d}x{num_clients:03d}x{num_transactions_per_client:03d}"

    class TunSelenium(Experiment):

        def __init__(self, nodes, variables, command_type, cc):
            super().__init__(CLASS_NAME, nodes[0:num_clients + num_hops + 1], variables, command_type, cc)
            assert self.command_type in [CommandType.LOGGING, CommandType.PERF, CommandType.LOGGING_QLOG]
            self.client_nodes = self.nodes[0:num_clients]
            self.proxy_nodes = self.nodes[num_clients:num_clients + num_hops]
            self.server_node = self.nodes[-1]

        @staticmethod
        def needed_node_count():
            return num_clients + num_hops + 1

        @staticmethod
        def transaction_count():
            return num_transactions_per_client

        def _ports(self):
            return [4433 + i for i in range(10)]

        @override
        def _pre(self):
            utils.execute_command_on_nodes_blocking_parallel(utils.Command(
                command=f"rm -f /tmp/experiment.error",
                command_type=utils.CommandType.NONE,
                blocking=True
            ), self.client_nodes)

            utils.start_cpu_logging(self.nodes, self.node_output_dir)
            threads = num_clients * num_transactions_per_client // 4
            command = f"""
            ./host_scripts/http_server_tum.sh
              {self._ports()[0]}
              {self.cc.lower()}
              {1472 - (len(self.proxy_nodes) * 86)}
              {threads}          
            """
            utils.execute_command_on_node(Command(
                command=command,
                command_type=self.command_type,
                blocking=False,
                output_path=f"{self.node_output_dir}/http_server.{self.command_type.name}"
            ), self.server_node, priorize=True)
            if len(self.proxy_nodes) == 0:
                self.http_url = f"https://{self.server_node.address_eno5}:{self._ports()[0]}/index.html"
            else:
                self.http_url = f"https://{self.server_node.address_eno3}:{self._ports()[0]}/index.html"

            time.sleep(3)

            port = self._ports()[1]
            framePerPacket = 0
            UDPSendPacketLen = 1472
            maxRecvPacketSize = 1472
            # start proxy servers
            for i, proxy in enumerate(self.proxy_nodes):
                cc = self.cc if i == 0 else "None"
                command = f'''
                ./binaries_proxygen/masque-server
                  --timeout 9999999999
                  --port {port}
                  --tuntap-network 192.{i}.0.0/16
                  --cc {cc}
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

            time.sleep(5)

            if len(self.proxy_nodes) > 0:
                # start client proxy
                hosts = [self.proxy_nodes[0].address_eno5] + [proxy.address_eno3 for proxy in self.proxy_nodes[1:]]
                port = self._ports()[1]
                ports = list(range(port, port + len(self.proxy_nodes)))  # 4433, 4434, 4435, ...
                paths = ["/.well-known/masque/ip"] * len(self.proxy_nodes)
                UDPSendPacketLens = [1472 - (i * 86) for i in range(len(self.proxy_nodes))]
                maxRecvPacketSizes = [1500] * len(self.proxy_nodes)
                ccs = ["None"] * len(self.proxy_nodes)
                ccs[0] = self.cc
                framePerPackets = [0] * len(self.proxy_nodes)

                connect_ip_port = 31337
                for client_node in self.client_nodes:
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
                      --numTransactions {num_transactions_per_client} 
                      --connect-ip-port {connect_ip_port}
                      {utils.get_qlog_flag(client_node, self.node_output_dir, 'client', self.command_type)}
                    """
                    utils.execute_command_on_node(Command(
                        command=command,
                        command_type=self.command_type,
                        blocking=False,
                        output_path=f"{self.node_output_dir}/client.{self.command_type.name}"
                    ), client_node, priorize=True)
                    connect_ip_port += 1
                time.sleep(15)

            # set the respective tc limits
            self.client_node_names = " ".join([node.name for node in self.client_nodes])
            self.first_server = self.proxy_nodes[0] if len(self.proxy_nodes) > 0 else self.server_node

            # [10]: 10% loss, 200ms rtt, 1Mbit/s bandwidth
            packet_loss = 10
            # we set the latency + bandwidth in chrome
            command = f"""python3 tc.py 
              {self.client_node_names + " " + self.first_server.name}
              --interface eno5
              --packet_loss {packet_loss} 
              """
            utils.execute_command(command=command, blocking=True)

        @override
        def _start(self):
            sleep_until = 10 + utils.system_time(
                self.nodes[0]) + 1 * num_transactions_per_client  # 1 seconds between each transaction
            sleep_until = int(sleep_until)

            tun_device = ""
            if len(self.proxy_nodes) == 0:
                tun_device = "eno5"
            utils.execute_command_on_nodes_blocking_parallel(Command(
                command=f"./host_scripts/selenium.sh {self.http_url} {sleep_until} {num_transactions_per_client} "
                        f"{self.node_output_dir} {tun_device}",
                command_type=CommandType.NONE,
                blocking=True
            ), self.client_nodes, priorize=True)

            for client_node in self.client_nodes:
                if utils.file_exists_on_server(client_node, f"/tmp/experiment.error"):
                    print(f"Found experiment.error on {client_node.name}! Repeating experiment...")
                    self.should_repeat = True

        @override
        def _stop(self):
            qlog = self.command_type == CommandType.LOGGING_QLOG
            utils.kill_all_parallel(self.nodes, qlog)

        @override
        def _post(self):
            utils.stop_cpu_logging(self.nodes)

            # reset the respective tc limits
            command = f"""python3 tc.py 
              {self.client_node_names + " " + self.first_server.name} 
              --interface eno5
              --reset
              """
            utils.execute_command(command=command, blocking=True)

            zipped_nodes = [(node, self.node_output_dir) for node in self.nodes]
            postprocessing.postprocess_nodes(zipped_nodes)
            postprocessing.download_results(zipped_nodes, self.log_dir)
            postprocessing.postprocess_coinbase(self.log_dir)

    TunSelenium.__name__ = CLASS_NAME
    return TunSelenium


MAX_HOPS = 2
MAX_CLIENTS = 8
MAX_TRANSACTIONS = 16

for num_hops in range(0, MAX_HOPS + 1):
    for num_clients in range(1, MAX_CLIENTS + 1):
        for num_transactions in utils.exponential_range(1, MAX_TRANSACTIONS + 1):
            class_ = selenium_factory(num_hops, num_clients, num_transactions)
            globals()[class_.__name__] = selenium_factory(num_hops, num_clients, num_transactions)
