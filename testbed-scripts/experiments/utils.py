import copy
import sys
import time

import poslib as pos
from enum import Enum
from dataclasses import dataclass
from discord_webhook import DiscordWebhook
import subprocess
import shlex
import os
from concurrent.futures import ThreadPoolExecutor

from run_experiment import Node

POOL = ThreadPoolExecutor(max_workers=999999999)


class CommandType(Enum):
    NONE = "NONE"
    LOGGING = "LOGGING"
    PERF = "PERF"
    LOGGING_QLOG = "LOGGING_QLOG"


def str_to_command_type(command_type: str) -> CommandType:
    if command_type == "NONE":
        return CommandType.NONE
    if command_type == "LOGGING":
        return CommandType.LOGGING
    if command_type == "PERF":
        return CommandType.PERF
    if command_type == "LOGGING_QLOG":
        return CommandType.LOGGING_QLOG
    raise Exception("Unknown command type: " + command_type)


@dataclass
class Command:
    command: str
    command_type: CommandType
    blocking: bool
    output_path: str = None


def execute_command(command: str, blocking: bool, print_output=False):
    command = " ".join(command.split())
    print(f"[{'blocking' if blocking else 'non-blocking'}] Executing '{command}'")
    # Escape any quotes within the command
    process = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if blocking:
        stdout, stderr = process.communicate()
        if print_output:
            print(stdout, file=sys.stdout)
            print(stderr, file=sys.stderr)
        return stdout, stderr, process.returncode
    else:
        return process


def execute_command_on_node(command: Command, node: Node, priorize: bool = False):
    command_backup = copy.deepcopy(command.command)
    if priorize:
        command.command = "nice -n -20 ionice -c 2 -n 0 " + command.command
    command.command = " ".join(command.command.split())
    command.command = command.command.strip().replace("\n", "").replace("\"", "\\\"")
    print(f"[{'blocking' if command.blocking else 'non-blocking'}] Executing '{command.command}' on {node.name}")
    if command.command_type == CommandType.NONE:
        # 1) no logging
        split_command = shlex.split(command.command)
        _, data = pos.commands.launch(
            node.name, name="command", command=split_command, blocking=False, queued=False
        )
    elif command.command_type == CommandType.LOGGING or command.command_type == CommandType.LOGGING_QLOG:
        # 2) output logging
        assert command.output_path is not None
        _, data = pos.commands.launch(
            node.name, name="command",
            command=["./host_scripts/cmd/logging.sh", command.command, command.output_path], blocking=False,
            queued=False
        )
    elif command.command_type == CommandType.PERF:
        # 3) perf measurement
        assert command.output_path is not None
        _, data = pos.commands.launch(
            node.name, name="command",
            command=["./host_scripts/cmd/perf.sh", command.command, command.output_path], blocking=False,
            queued=False
        )
    else:
        raise Exception("Unknown command type")
    command.command = command_backup
    # Fetch the ID of the command
    pos_id = data.get("id", None)
    if not command.blocking:
        return pos_id
    try:
        print(f"Awaiting command {pos_id}")
        pos.commands.await_id(pos_id)
        print(f"Command {pos_id} finished")
    except Exception as e:
        print(f"Error while executing command on node {node.name}: {e}")
        pass
    return None


def await_command(pos_id: str, timeout_s: int = 60 * 20):
    print(f"Awaiting command {pos_id}")
    try:
        pos.commands.await_id(pos_id)
        print(f"Command {pos_id} finished")
    except Exception as e:
        print(f"Error while awaiting command: {e}")
        pass


def send_to_discord(message: str, file_path: str = None):
    content = f"```\n{message}\n```"
    webhook = DiscordWebhook(
        url="https://discord.com/api/webhooks/1148284506407718912/"
            "gAm4UnMp0p7Od0Eo722jjIJOUrumLYpK11U4WoRFyNjUmBlh20X6l6RIFzKFqNq8bwd9",
        content=content)
    if file_path:
        with open(file_path, "rb") as f:
            webhook.add_file(file=f.read(), filename="file.txt")
    webhook.execute()


def kill_all(node: Node, qlog=False):
    qlog_str = "true" if qlog else ""
    execute_command_on_node(Command(
        command=f"./host_scripts/killall.sh {qlog_str}",
        command_type=CommandType.LOGGING,
        blocking=True,
        output_path="/tmp/killall.log"
    ), node)


def kill_all_parallel(nodes, qlog=False):
    qlog_str = "true" if qlog else ""
    pos_ids = []
    for node in nodes:
        pos_ids.append(execute_command_on_node(Command(
            command=f"./host_scripts/killall.sh {qlog_str}",
            command_type=CommandType.LOGGING,
            blocking=False,
            output_path="/tmp/killall.log"
        ), node))
    for pos_id in pos_ids:
        await_command(pos_id)


def logs_path():
    home = os.path.expanduser("~")
    os.makedirs(f"{home}/logs", exist_ok=True)
    return f"{home}/logs"


def results_path():
    home = os.path.expanduser("~")
    os.makedirs(f"{home}/results", exist_ok=True)
    return f"{home}/results"


def system_time(node: Node):
    sysout, _, _ = execute_command(f"ssh {node.name} date +%s", blocking=True)
    return float(sysout)


def scp(node: Node, source: str, destination: str):
    execute_command(f"scp {node.name}:{source} {destination}", blocking=True)


def scp_r(node: Node, source: str, destination: str):
    execute_command(f"scp -r {node.name}:{source} {destination}", blocking=True)


def file_exists_on_server(node: Node, path: str):
    _, _, ret_code = execute_command(f"ssh {node.name} test -f {path}", blocking=True)
    return int(ret_code) == 0


def start_cpu_logging(nodes: [Node], output_path: str):
    stop_cpu_logging(nodes)
    pos_ids = []
    for node in nodes:
        pos_id = execute_command_on_node(Command(
            command=f"./host_scripts/cpu_logger.sh cpu_logger {output_path}",
            command_type=CommandType.NONE,
            blocking=False
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        await_command(pos_id)


def stop_cpu_logging(nodes: [Node]):
    pos_ids = []
    for node in nodes:
        pos_id = execute_command_on_node(Command(
            command="screen -S cpu_logger -X quit",
            command_type=CommandType.NONE,
            blocking=False
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        await_command(pos_id)


def get_qlog_dir(node: Node, output_dir: str, qlog_name: str, command_type: CommandType):
    if command_type == CommandType.LOGGING_QLOG:
        qlog_dir = f"{output_dir}/{qlog_name}"
        execute_command_on_node(Command(
            command=f"mkdir -p {qlog_dir}",
            command_type=CommandType.NONE,
            blocking=True
        ), node)
        return qlog_dir
    return ""


def get_qlog_flag(node: Node, output_dir: str, qlog_name: str, command_type: CommandType):
    qlog_dir = get_qlog_dir(node, output_dir, qlog_name, command_type)
    if qlog_dir:
        return f"--qlog {qlog_dir}"
    return ""


def exponential_range(inclusive_start, exclusive_end):
    i = inclusive_start
    while i < exclusive_end:
        yield i
        i *= 2


def absolute_timestamp_error_ms(nodes: [Node]):
    systimes = []
    now = time.time()
    for node in nodes:
        systime = system_time(node)
        time_since = time.time() - now
        systimes.append((systime - time_since) * 1000)
    return max(systimes) - min(systimes)


def toggle_hyperthreading_parallel(nodes: [Node], enable: bool):
    pos_ids = []
    for node in nodes:
        pos_id = execute_command_on_node(Command(
            command=f"echo {'on' if enable else 'off'} > /sys/devices/system/cpu/smt/control",
            command_type=CommandType.NONE,
            blocking=False
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        await_command(pos_id)


def execute_command_on_nodes_blocking_parallel(command: Command, nodes: [Node], priorize: bool = False):
    assert command.blocking
    command.blocking = False
    pos_ids = []
    for node in nodes:
        command = copy.deepcopy(command)
        pos_id = execute_command_on_node(command, node, priorize)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        await_command(pos_id)
