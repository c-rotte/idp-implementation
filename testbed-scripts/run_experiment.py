import argparse
import copy
import json
import tempfile
from abc import abstractmethod, ABC
import poslib as pos
import importlib.util
import os
import pathlib
from dataclasses import dataclass

from experiments import utils


@dataclass
class Node:
    name: str
    address_eno3: str
    address_eno5: str


class Experiment(ABC):

    def __init__(self, CLASS_NAME: str, nodes: [Node], variables, command_type, cc):
        assert len(nodes) >= self.needed_node_count(), "Insufficient nodes for the experiment"
        self.CLASS_NAME = CLASS_NAME
        self.nodes = nodes
        self.variables = variables
        self.command_type = utils.str_to_command_type(command_type)
        self.cc = cc
        self.node_output_dir = f"/tmp/output"  # the dir on the node where the output is stored
        self.should_repeat = True
        self.should_repeat_flag = False
        self.experiment_index = 0
        utils.execute_command_on_nodes_blocking_parallel(utils.Command(
            command=f"rm -rf {self.node_output_dir}",
            command_type=utils.CommandType.NONE,
            blocking=True
        ), self.nodes)
        utils.execute_command_on_nodes_blocking_parallel(utils.Command(
            command=f"mkdir -p {self.node_output_dir}",
            command_type=utils.CommandType.NONE,
            blocking=True
        ), self.nodes)
        # the dir on the host where the output is collected
        self.log_dir = f"{utils.logs_path()}/{CLASS_NAME}/{cc}/{command_type}"

    def _ensure_logging_disabled(self):
        # disable logging (keep this, trust me)
        utils.execute_command_on_nodes_blocking_parallel(utils.Command(
            command=f"./host_scripts/disable_logging.sh",
            command_type=utils.CommandType.NONE,
            blocking=True
        ), self.nodes)

    def _set_experiment_variables(self):

        def set_variables_with_pos_on_machine(node: Node, dictionary: dict):
            """
            loads the variables and their value given in as a dict on to the
            host using pos allocations.
            These can later be used on the host using
            pos command scripts.
            """
            print(f"Setting the pos variables:\n{dictionary}\non the host {node.name}")
            # Create a temporary json file to store the dict in,
            # as pos currently only allows to read vars using a data file.
            tmp_file = tempfile.NamedTemporaryFile(dir="/tmp", prefix="masque-temp-pos-data-", mode="w+")
            json.dump(dictionary, tmp_file, ensure_ascii=False, indent=4)
            tmp_file.flush()
            tmp_file.seek(0)
            pos.allocations.set_variables(
                allocation=node.name,
                datafile=tmp_file,
                extension="json",
                as_global=None,
                as_loop=None,
                print_variables=None
            )
            tmp_file.close()

        print("Setting experiment variables:", self.variables, "\n")
        for node in self.nodes:
            set_variables_with_pos_on_machine(node, self.variables)

    @staticmethod
    def is_experiment():
        return True

    @staticmethod
    def needed_node_count():
        raise NotImplementedError

    @staticmethod
    def transaction_count():
        raise NotImplementedError

    @abstractmethod
    def _pre(self):
        raise NotImplementedError

    @abstractmethod
    def _start(self):
        raise NotImplementedError

    @abstractmethod
    def _stop(self):
        raise NotImplementedError

    @abstractmethod
    def _post(self):
        raise NotImplementedError

    def run(self):
        utils.send_to_discord(f"[Starting] {self.CLASS_NAME} in mode {self.command_type.name} with cc {self.cc}")
        if self.should_repeat_flag:
            utils.send_to_discord("Note: This experiment will be repeated until it succeeds")
        absolute_timestamp_error_ms = utils.absolute_timestamp_error_ms(self.nodes)
        print(f"Absolute timestamp error: {absolute_timestamp_error_ms}ms")
        while self.should_repeat:
            self.should_repeat = False
            utils.execute_command(f"rm -rf {self.log_dir}", blocking=True)
            os.makedirs(self.log_dir, exist_ok=True)
            print(f"Starting Experiment '{self.__class__.__name__}' [{self.experiment_index}] on the following nodes: "
                  f"{[node.name for node in self.nodes]}")
            self._set_experiment_variables()
            utils.kill_all_parallel(self.nodes)
            print("Starting Preprocessing")
            self._pre()
            try:
                self._ensure_logging_disabled()
                print("Starting Experiment")
                utils.execute_command_on_nodes_blocking_parallel(utils.Command(
                    command=f"truncate -s 0 {self.node_output_dir}/uptime.log",
                    command_type=utils.CommandType.NONE,
                    blocking=True
                ), self.nodes)
                self._start()
            except Exception as e:
                print(e)
            finally:
                print("Stopping Experiment")
                self._stop()
                print("Starting Postprocessing")
                self._post()
            if self.should_repeat:
                if not self.should_repeat_flag:
                    utils.send_to_discord("[Repeating] Experiment failed, but not repeating")
                    return 2
                utils.send_to_discord("[Repeating] Retrying experiment...")
        utils.send_to_discord(f"[Done] {self.CLASS_NAME} in mode {self.command_type.name} with cc {self.cc}")
        return 0


# only used for hinting
def override(f):
    return f


def collect_experiment_subclasses(base_dir):
    def is_experiment_class(obj):
        return hasattr(obj, "is_experiment") and obj.is_experiment()

    for root, dirs, files in os.walk(base_dir):
        for filename in files:
            if filename.endswith(".py"):
                module_name = filename[:-3]  # remove .py extension
                module_path = os.path.join(root, filename)
                spec = importlib.util.spec_from_file_location(module_name, module_path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                for name in dir(module):
                    obj = getattr(module, name)
                    if isinstance(obj, type) and is_experiment_class(obj) and obj.__name__ != "Experiment":
                        yield obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run an experiment.')
    parser.add_argument('--list-experiments', action='store_true', help='List all available experiments.')
    parser.add_argument('experiment', type=str, nargs='?', help='The name of the experiment to run.')
    parser.add_argument('--nodes', type=str, nargs='*', help='The nodes to run the experiment on.')
    parser.add_argument('--node-addresses-eno3', type=str, nargs='*',
                        help='IP addresses corresponding to each node on eno3.')
    parser.add_argument('--node-addresses-eno5', type=str, nargs='*',
                        help='IP addresses corresponding to each node on eno5.')
    parser.add_argument('--mode', type=str, nargs='?',
                        help='The mode to run the experiment in. (LOGGING | PERF | LOGGING_QLOG)')
    parser.add_argument('--cc', type=str, nargs='?',
                        help='The congestion control algorithm to use. (None | Cubic | NewReno | Copa | Copa2 | BBR)')
    parser.add_argument('--repeat', action='store_true', help='Repeat the experiment until it succeeds.')
    parser.add_argument('--variables', type=str, nargs='?', help='The variables to set for the experiment.')
    parser.add_argument('--index', type=int, nargs='?', const=0, help='The index of the experiment to run.')
    args = parser.parse_args()


    def sorted_experiment_names():
        experiment_classes = list(collect_experiment_subclasses(
            pathlib.Path(__file__).parent.resolve().as_posix() + "/experiments"))
        experiment_classes.sort(key=lambda x: (x.needed_node_count(), x.transaction_count(), x.__name__), reverse=True)
        experiment_class_names = [e.__name__ for e in experiment_classes]
        return experiment_class_names


    if args.list_experiments:
        for experiment_name in sorted_experiment_names():
            print(experiment_name)
        exit()

    node_names = args.nodes
    variables = json.loads(args.variables)
    command_mode = args.mode
    if command_mode is None or command_mode not in ["LOGGING", "PERF", "LOGGING_QLOG"]:
        print("Invalid mode!")
        exit(1)
    cc = args.cc
    if cc is None or cc not in ["None", "Cubic", "NewReno", "Copa", "Copa2", "BBR"]:
        print("Invalid congestion control algorithm!")
        exit(1)

    # Load the experiment
    experiment_classes = collect_experiment_subclasses(
        pathlib.Path(__file__).parent.resolve().as_posix() + "/experiments")
    experiment = None
    for ExperimentSubclass in experiment_classes:
        if ExperimentSubclass.__name__ == args.experiment:
            nodes = [Node(name, address_eno3, address_eno5) for name, address_eno3, address_eno5 in
                     zip(node_names, args.node_addresses_eno3, args.node_addresses_eno5)]
            experiment = ExperimentSubclass(nodes, variables, command_mode, cc)
    if experiment is None:
        print(f"Experiment {args.experiment} not found!")
        exit(2)
    experiment.experiment_index = args.index
    experiment.should_repeat_flag = args.repeat
    exit_code = experiment.run()
    exit(exit_code)
