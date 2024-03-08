import poslib as pos
import shlex
import argparse


def execute(node: str, cmd: str):
    split_command = shlex.split(cmd)
    _, data = pos.commands.launch(
        node, name="command", command=split_command, blocking=True, queued=False
    )


def configure_interface(node: str, interface: str, packet_loss: str = None, rtt: str = None, bandwidth: str = None):
    cmd = f"tc qdisc add dev {interface} root handle 1:0 netem"
    if packet_loss:
        cmd += f" loss {packet_loss}%"
    if rtt:
        cmd += f" delay {rtt}ms"
    if bandwidth:
        cmd += f" rate {bandwidth}mbit"

    execute(node, cmd)


def reset_interface(node: str, interface: str):
    cmd = f"tc qdisc del dev {interface} root"
    execute(node, cmd)


def main():
    parser = argparse.ArgumentParser(description='Configure or reset network settings on nodes.')
    parser.add_argument('nodes', metavar='N', type=str, nargs='+', help='a list of node names (space-separated)')
    parser.add_argument('--interface', type=str, required=True, help='name of the network interface')
    parser.add_argument('--packet_loss', type=str, help='packet loss in percent')
    parser.add_argument('--rtt', type=str, help='RTT in ms')
    parser.add_argument('--bandwidth', type=str, help='bandwidth in mbit/s')
    parser.add_argument('--reset', action='store_true', help='flag to reset the interface settings')

    args = parser.parse_args()

    if args.reset:
        for node in args.nodes:
            reset_interface(node, args.interface)
    else:
        for node in args.nodes:
            configure_interface(node, args.interface, args.packet_loss, args.rtt, args.bandwidth)


if __name__ == "__main__":
    main()
