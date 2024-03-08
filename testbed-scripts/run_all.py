import os
import signal
import subprocess
import argparse
import re

from experiments import utils

experiment_categories = ["HttpGETConnectIP", "HttpGETConnectUDP", "TunSeleniumIP", "TunHop"]
modes = {
    "TunSeleniumIP": ["LOGGING"],
    "TunHop": ["LOGGING"],
    "HttpGETConnectIP": ["LOGGING"],
    "HttpGETConnectUDP": ["LOGGING"]
}
cc_algorithms = {
    "TunSeleniumIP": ["Cubic"],
    "TunHop": ["None"],
    "HttpGETConnectIP": ["Cubic"],
    "HttpGETConnectUDP": ["Cubic"]
}
# timeouts in seconds
timeouts = {
    "TunSeleniumIP": 60 * 20,  # 20 minutes
}


def experiments_gen(filter_pattern=None):
    all_experiments = subprocess.check_output(
        ["python3", "run_experiment.py", "--list"]
    ).decode('utf-8').splitlines()
    all_experiments = list(dict.fromkeys(all_experiments))  # remove duplicates
    for category in experiment_categories:
        category_experiments = [e for e in all_experiments if category in e]
        for experiment_name in category_experiments:
            for cc in cc_algorithms[category]:
                for mode in modes[category]:
                    if filter_pattern is None or re.search(filter_pattern, experiment_name):
                        yield category, experiment_name, cc, mode


def restart_servers():
    print("###############################################")
    print("Restarting servers")
    print("###############################################")
    subprocess.run("yes | python3 setup.py", shell=True)


def run_experiment(category, name, cc, mode, nodes, node_addresses_eno3, node_addresses_eno5, index):
    common_args = [
        "python3", "run_experiment.py", name,
        "--nodes", *nodes,
        "--node-addresses-eno3", *node_addresses_eno3,
        "--node-addresses-eno5", *node_addresses_eno5,
        "--variables", "{}",
        "--cc", cc,
        "--index", str(index),
        "--repeat"
    ]
    print("###############################################")
    print(f"{name} - {cc} - {mode}")
    print("###############################################")
    final_cmd = common_args + ["--mode", mode]
    print(" ".join(final_cmd))
    print("###############################################")
    while True:
        p = subprocess.Popen(final_cmd, shell=False)
        try:
            if category in timeouts:
                p.wait(timeout=timeouts[category])
            else:
                p.wait()
            return 0
        except subprocess.TimeoutExpired:
            p.send_signal(signal.SIGINT)
            utils.send_to_discord(f"[Timeout] {name} - {cc} - {mode}")
            print("Process timed out")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nodes", nargs='+', required=True, help="Nodes as arguments.")
    parser.add_argument("--filter", type=str, help="Regex pattern to filter experiments.")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.realpath(__file__))
    os.chdir(script_dir)

    os.system("rm -rf ~/results/*")
    os.system("rm -rf ~/logs/*")

    node_addresses_eno3 = [f"4.0.{i}.3" for i in range(0, len(args.nodes))]
    node_addresses_eno5 = [f"5.0.0.{i + 1}" for i in range(0, len(args.nodes))]

    experiment_filter = args.filter
    print("###############################################")
    print("Running the following experiments:")
    print("###############################################")
    for category, experiment_name, cc, mode in experiments_gen(experiment_filter):
        print(category, experiment_name, cc, mode)
    i = 0
    for category, experiment_name, cc, mode in experiments_gen(experiment_filter):
        run_experiment(category, experiment_name, cc, mode, args.nodes, node_addresses_eno3,
                       node_addresses_eno5, i)
        i += 1


if __name__ == "__main__":
    main()
