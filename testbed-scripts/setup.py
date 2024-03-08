from logging import error
from sys import argv
import os
import poslib as pos
import subprocess
import load_binaries
import datetime
import time
from poscalendar import find_fitting_calendar_entry, print_calendar_entry

image = 'debian-bullseye'

NODES = ["bitcoin", "bitcoincash", "bitcoingold",
         "ether", "ethercash", "ethergold",
         "dogecoin", "dogecoincash", "dogecoingold"]


def get_possible_nodes(node_list):
    sub_len = len(node_list)
    sub_set = set(node_list)
    for i in range(len(NODES) - sub_len + 1):
        if NODES[i] in sub_set:
            if set(NODES[i:i + sub_len]) == sub_set:
                return NODES[i:i + sub_len]
    return None


def start_nodes():
    data, _ = pos.allocations.list_all(
        f"owner={os.environ['USER']}", json=True)
    allocated_nodes = []
    reboot = True
    if len(data) > 0:
        allocated_nodes = data[0]['nodes']
    if set(allocated_nodes) == set(nodes):
        print("Nodes already allocated! Do you want to free and reallocate them? (y/N)")
        answer = input()
        if answer != 'y':
            print("Skipping allocation and setup!")
            return
        print("Do you want to reboot the machines? (y/N)")
        answer = input()
        if answer != 'y':
            reboot = False

    print("Freeing nodes")
    for node in nodes:
        pos.allocations.free(node)

    print("Allocating nodes")
    pos.allocations.allocate(nodes)

    print("Setting image")
    for node in nodes:
        pos.nodes.image(node, image)

    reboot_ids = []
    if reboot:
        print("Rebooting")
        for node in nodes:
            _, id = pos.nodes.reset(node, blocking=False)
            id = id['id']
            if not id:
                error("No data returned!")
                exit()
            reboot_ids.append(id)

    print("Downloading binaries while waiting for reboot to finish.")
    load_binaries.download_binaries_proxygen_mvfst()
    load_binaries.download_binaries_cloudflare_quiche()
    print("Downloading binaries finished")
    subprocess.run(
        "chmod +x binaries_*/* binaries_*/target/release/*", shell=True)

    print("Waiting for reboot to finish")
    for id in reboot_ids:
        pos.commands.await_id(id)

    print("Updating status")
    for node in nodes:
        pos.nodes.update_status(node)

    print("Bootstrapping")
    for node in nodes:
        pos.nodes.bootstrap(node)


def setup_nodes():
    processes = []
    print(f"Copying binaries and certs to nodes")
    for node in nodes:
        p = subprocess.Popen(
            f"scp -p -r ./resources ./binaries_proxygen ./binaries_quiche ./host_scripts ./experiments {node}:.",
            shell=True)
        processes.append(p)

    for p in processes:
        p.wait()

    on_host_ids = []
    for i, node in enumerate(nodes):
        i = i + 1
        _, data = pos.commands.launch(
            node, name='install', command=['./host_scripts/on_host_setup.sh', str(i)], blocking=False, queued=True)
        on_host_ids.append(data['id'])
        print(f"Launched interface setup on {node}: eno3=4.0.{i - 1}.3 eno4=4.0.{i}.4 eno5=5.0.0.{i}")

    print("Waiting for on host setup to finish")
    for id in on_host_ids:
        pos.commands.await_id(id)


if __name__ == "__main__":
    nodes = argv[1:]
    calendar_entry = find_fitting_calendar_entry(nodes, interactive=True)
    print_calendar_entry(calendar_entry)
    nodes = calendar_entry['nodes']
    start_date = calendar_entry['start_date']
    start_time = datetime.datetime.strptime(start_date, "%Y-%m-%d %H:%M:%S")

    current_time = datetime.datetime.now()
    time_difference = (start_time - current_time).total_seconds()

    nodes = get_possible_nodes(nodes)
    if not nodes:
        print(f"Could not find a fitting node combination for {nodes}")
        exit(1)
    print(f"Using nodes {nodes}")

    if time_difference > 0:
        print(f"To wait for the calendar entry, we need to sleep for minutes {int(time_difference / 60)}. OK? (Y/n)")
        answer = input()
        if answer == 'n':
            print(f"Nothing to do. Exiting.")
            exit(1)
        time.sleep(time_difference + 1)
    print(f"Starting setup!")
    start_nodes()
    setup_nodes()
    print("Setup finished! You may now run")
    node_string = " ".join(nodes)
    print(f"python3 run_all.py --nodes {node_string}")
