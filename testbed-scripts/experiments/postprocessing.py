import os
from pathlib import Path

from experiments import utils
from experiments.utils import Command, CommandType
from run_experiment import Node
import glob
from experiments import qlog


def _delete_all_files(nodes: list[tuple], file_name: str):
    pos_ids = []
    for node, node_output_dir in nodes:
        pos_id = utils.execute_command_on_node(Command(
            command=f'find {node_output_dir} -name {file_name} -delete',
            command_type=CommandType.NONE,
            blocking=False
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        utils.await_command(pos_id)


def _delete_all_files_local(log_dir: str, file_name: str):
    utils.execute_command(f"find {log_dir} -name {file_name} -delete", blocking=True)


def _extract_logging_throughput(nodes: list[tuple]):
    pos_ids = []
    for node, node_output_dir in nodes:
        pos_id = utils.execute_command_on_node(Command(
            command=f"./host_scripts/throughput.sh {node_output_dir}",
            command_type=CommandType.NONE,
            blocking=False,
            output_path=f"/tmp/throughput.log"
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        utils.await_command(pos_id)


def _generate_flamegraphs(nodes: list[tuple]):
    pos_ids = []
    for node, node_output_dir in nodes:
        pos_id = utils.execute_command_on_node(Command(
            command=f"./host_scripts/flamegraph.sh {node_output_dir}",
            command_type=CommandType.LOGGING,
            blocking=False,
            output_path=f"/tmp/flamegraph.log"
        ), node)
        pos_ids.append(pos_id)
    for pos_id in pos_ids:
        utils.await_command(pos_id)


def postprocess_nodes(nodes: list[tuple]):
    _extract_logging_throughput(nodes)
    # _delete_all_files(nodes, "*.LOGGING")

    # _delete_all_files(nodes, "*.LOGGING_QLOG")

    _generate_flamegraphs(nodes)
    _delete_all_files(nodes, "*.PERF")
    _delete_all_files(nodes, "*.PERF.folded")


def download_results(nodes: list[tuple], log_dir: str):
    for i, (node, node_output_dir) in enumerate(nodes):
        utils.execute_command(f"mkdir -p {log_dir}/node_{i}", blocking=True)
        utils.scp_r(node, f"{node_output_dir}/*", f"{log_dir}/node_{i}/.")
    # delete empty dirs
    utils.execute_command(f"find {log_dir} -type d -empty -delete", blocking=True)


def _convert_qlog_files(logs_path: str):
    if os.path.basename(logs_path) != "LOGGING_QLOG":
        return
    node_dirs = sorted([d for d in Path(logs_path).iterdir() if d.is_dir()])
    http_client_path = None
    for node_dir in node_dirs:
        if Path(f"{node_dir}/http_client").exists():
            http_client_path = f"{node_dir}/http_client"
            break
    if not Path(http_client_path).exists():
        raise Exception(f"Could not find http_client dir in {logs_path}")
    http_server_path = None
    for node_dir in node_dirs:
        if Path(f"{node_dir}/http_server").exists():
            http_server_path = f"{node_dir}/http_server"
            break
    if not Path(http_server_path).exists():
        raise Exception(f"Could not find http_server dir in {logs_path}")
    client_qlog = [f for f in Path(http_client_path).iterdir()
                   if str(f).endswith('.qlog.gz')][0]
    server_qlog = [f for f in Path(http_server_path).iterdir()
                   if str(f).endswith('.qlog.gz')][0]
    qlog.produce_metrics(str(server_qlog.absolute()),
                         str(client_qlog.absolute()),
                         f"{node_dirs[0]}/metrics.json")
    # delete http_client and http_server dirs
    utils.execute_command(f"rm -rf {http_client_path}", blocking=True)
    utils.execute_command(f"rm -rf {http_server_path}", blocking=True)


def postprocess_coinbase(logs_dir: str):
    print(f"Postprocessing coinbase logs in {logs_dir}")
    _convert_qlog_files(logs_dir)

    _delete_all_files_local(logs_dir, "*.qlog.gz")
