import os
import json
import ast
import re
from datetime import datetime

import matplotlib as mpl

mpl.rcParams['figure.dpi'] = 300

import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import numpy as np
from cycler import cycler

from matplotlib.backends.backend_pdf import PdfPages


def set_size(width, fraction=1, subplots=(1, 1)):
    """Set figure dimensions to avoid scaling in LaTeX.

    Parameters
    ----------
    width: float or string
            Document width in points, or string of predined document type
    fraction: float, optional
            Fraction of the width which you wish the figure to occupy
    subplots: array-like, optional
            The number of rows and columns of subplots.
    Returns
    -------
    fig_dim: tuple
            Dimensions of figure in inches
    """
    if width == 'thesis':
        width_pt = 426.79135
    elif width == 'beamer':
        width_pt = 307.28987
    else:
        width_pt = width

    # Width of figure (in pts)
    fig_width_pt = width_pt * fraction
    # Convert from pt to inches
    inches_per_pt = 1 / 72.27

    # Golden ratio to set aesthetic figure height
    # https://disq.us/p/2940ij3
    golden_ratio = (5 ** .5 - 1) / 2

    # Figure width in inches
    fig_width_in = fig_width_pt * inches_per_pt
    # Figure height in inches
    fig_height_in = fig_width_in * golden_ratio * (subplots[0] / subplots[1])

    return (fig_width_in, fig_height_in)


# use it before plotting
def rc_setting():
    # Direct input

    linestyle_cycler = (cycler("color", plt.cm.viridis(np.linspace(0, 1, 5))) + cycler('linestyle',
                                                                                       ['-', '--', ':', '-.', '-']))
    # Options
    return {
        # Use LaTeX to write all text
        "text.usetex": True,
        "font.family": "serif",
        # Use 10pt font in plots, to match 10pt font in document
        "axes.labelsize": 9,
        "font.size": 9,
        # Make the legend/label fonts a little smaller
        "legend.fontsize": 9,
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "axes.prop_cycle": linestyle_cycler
    }


plt.rcParams.update(rc_setting())
PLOT_SIZE = set_size(width='thesis')


def show_plot(plt, title, show=True):
    print(f"Showing {title}")
    title = re.sub(r'(?<!^)(?=[A-Z])', '_', title).lower()
    plt.savefig(f"/home/chrissi/Documents/TUM/WiSe_22/IDP/coinbase/plots/{title}.png", dpi=300)
    if show:
        plt.show()


class PlotGenerator:

    def __init__(self, results_dir, output_dir):
        self.results_dir = results_dir
        self.output_dir = output_dir
        self.data = {}
        os.makedirs(self.output_dir, exist_ok=True)

    def dirs(self, selected_base_name, selected_mode):
        for experiment_name in sorted(os.listdir(self.results_dir)):
            params_str = experiment_name.lstrip("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
            params = [int(x) for x in params_str.split("x") if x != ""]
            base_name = experiment_name.replace(params_str, "")
            if base_name != selected_base_name:
                continue
            for cc in os.listdir(f"{self.results_dir}/{experiment_name}"):
                for mode in os.listdir(f"{self.results_dir}/{experiment_name}/{cc}"):
                    if mode != selected_mode:
                        continue
                    dir = f"{self.results_dir}/{experiment_name}/{cc}/{mode}"
                    yield base_name, params, cc, mode, dir

    def plot(self):
        raise NotImplementedError()


class CpuPlotter(PlotGenerator):
    def __init__(self, results_dir, output_dir):
        target_dir = f"{output_dir}/cpu_usage"
        os.system(f"rm -rf {target_dir} && mkdir -p {target_dir}")
        print(f"Target dir: {target_dir}")
        super().__init__(results_dir, target_dir)

    def _iterate_log_files(self):

        def split_into_sublists(l, val):
            result = []
            sublist = []
            for i in l:
                if i == val:
                    result.append(sublist)
                    sublist = []
                else:
                    sublist.append(i)
            if sublist:
                result.append(sublist)
            return result

        for root, _, files in os.walk(self.results_dir):
            for file in files:
                if "uptime.log" not in file:
                    continue
                with open(os.path.join(root, file), 'r') as f:
                    lines = f.readlines()
                result = []
                for sublist in split_into_sublists(lines, '\n'):
                    if len(sublist) <= 2:
                        continue
                    if "CPU" not in sublist[0]:
                        continue
                    data = []
                    for line in sublist[2:]:
                        cpu = line.split()[2]
                        idle = float(line.split()[-1])
                        data.append((cpu, idle))
                    #
                    result.append(data)
                yield root, result

    def plot(self):
        for root, result in self._iterate_log_files():
            name = root.split('/')[-4] + "_" + root.split('/')[-3] + "_" + root.split('/')[-2] + "_" + root.split('/')[
                -1]
            plt.figure(figsize=PLOT_SIZE)
            cpu_map = {}
            cpu_time_map = {}
            for t, data in enumerate(result):
                for cpu, idle in data:
                    if cpu not in cpu_map:
                        cpu_map[cpu] = []
                        cpu_time_map[cpu] = []
                    cpu_map[cpu].append(100 - idle)
                    cpu_time_map[cpu].append(t)
            for cpu, cpu_data in cpu_map.items():
                plt.plot(cpu_time_map[cpu], cpu_data, label=f'CPU {cpu}')
            plt.xlabel('Time (s)')
            plt.ylabel('CPU Usage (\%)')
            plt.ylim(0, 105)
            # plt.title(f'CPU Usage Over Time')
            plt.legend()
            plt.grid(True)
            plt.tight_layout()
            show_plot(plt, f"{os.path.basename(self.output_dir)}/{name}", show=False)


class Http(PlotGenerator):

    def __init__(self, results_dir, output_dir, connect_ip: bool):
        if connect_ip:
            self.name = "HttpGETConnectIP"
        else:
            self.name = "HttpGETConnectUDP"
        super().__init__(results_dir, f" {output_dir}/{self.name}")

    def _read_csv(self, file_path):
        with open(file_path, 'r') as file:
            throughput_list = next(file).strip()
        return ast.literal_eval(throughput_list)

    def _read_ttfb_csv(self, file_path):
        ttfb_pattern = re.compile(r'TTFB=(\d+)ms')
        with open(file_path, 'r') as file:
            content = file.read().strip()
            ttfb_list = []
            for line in content.splitlines():
                match = ttfb_pattern.search(line)
                if not match:
                    continue
                ttfb_ms = match.groups()
                ttfb_list.append(int(ttfb_ms[0]))
        return ttfb_list

    def _metrics_plot(self, metric_name, title, y_label):
        for cc in self.data:
            plt.figure(figsize=PLOT_SIZE)
            for num_hops in self.data[cc]:
                metric_data = [metric[metric_name] for metric in self.data[cc][num_hops]["metrics"]]
                num_transactions_log2 = [f"{2 ** i}" for i in range(len(metric_data))]
                plt.plot(num_transactions_log2, metric_data, label=f'Hops: {num_hops}')
            plt.xlabel('Number of Transactions (log2)')
            plt.ylabel(y_label)
            plt.ylim(ymin=0)
            # plt.title(f"[{self.name}] {title} (cc={cc})")
            plt.legend()
            plt.grid(True)
            plt.tight_layout()
            show_plot(plt, f"{self.name}_{metric_name}_{cc}")

    def _plot(self):
        # 1) throughput
        for cc in self.data:
            plt.figure(figsize=PLOT_SIZE)
            for num_hops in self.data[cc]:
                throughput_data = self.data[cc][num_hops]["throughput_list"]
                acc_throughputs = [sum(throughput_list) for throughput_list in throughput_data]
                num_transactions_log2 = [f"{2 ** i}" for i in range(len(throughput_data))]
                plt.plot(num_transactions_log2, acc_throughputs, label=f'Hops: {num_hops}')
            plt.xlabel('Number of Transactions (log2)')
            plt.ylabel('Throughput in Mbit/s')
            plt.ylim(ymin=0)
            # plt.title(f'[{self.name}] Acc. Throughput vs. Number of Transactions (cc={cc})')
            plt.legend()
            plt.grid(True)
            plt.tight_layout()
            show_plot(plt, f"{self.name}_throughput_{cc}")

        # 2) metrics
        self._metrics_plot('latency_ms', 'Latency Distribution', 'Latency (ms)')
        self._metrics_plot('jitter_ms', 'Jitter Distribution', 'Jitter (ms)')
        self._metrics_plot('ttfb_ms', 'TTFB Distribution', 'TTFB (ms)')
        self._metrics_plot('avg_rtt_ms', 'Average RTT Distribution', 'Average RTT (ms)')

        # 3) unique ttfb
        for cc in self.data:
            fig = plt.figure(figsize=PLOT_SIZE)
            rows = 2
            cols = 3 if len(self.data[cc]) > 4 else 2
            for i, num_hops in enumerate(self.data[cc]):
                ttfb_data = self.data[cc][num_hops]["unique_ttfb"]
                num_transactions_log2 = [(f"{2 ** i}" if i % 2 == 0 else "") for i in range(len(ttfb_data))]
                splot = plt.subplot(rows, cols, i + 1)
                splot.set_title(f"Hops: {num_hops}")
                boxplot = plt.boxplot(ttfb_data, labels=num_transactions_log2)
                for median in boxplot['medians']:
                    median.set(color='red', linewidth=1)
                plt.grid(True)
                plt.yscale('log')
                plt.ylim(ymin=0)
            fig.supxlabel('Number of Transactions (log2)')
            fig.supylabel('Time to First Byte (ms) (log)')
            fig.tight_layout()
            # plt.title(f'[{self.name}] TTFB (per Transaction) (cc={cc})')
            show_plot(fig, f"{self.name}_ttfb_{cc}")

    def plot(self):
        for _, params, cc, _, dir in self.dirs(self.name, "LOGGING_QLOG"):
            num_hops, num_transactions = params
            metrics_path = f"{dir}/node_0/metrics.json"
            if not os.path.exists(metrics_path):
                raise ValueError(f"metrics.json is missing for {metrics_path}")
            metrics_dict = json.load(open(metrics_path, "r"))
            if not cc in self.data:
                self.data[cc] = {}
            if not num_hops in self.data[cc]:
                self.data[cc][num_hops] = {
                    "metrics": [],
                    "throughput_list": [],
                    "unique_ttfb": []
                }
            self.data[cc][num_hops]["metrics"].append(metrics_dict)

        for _, params, cc, _, dir in self.dirs(self.name, "LOGGING"):
            num_hops, num_transactions = params
            csv_path = f"{dir}/node_0/result.csv"
            logging_path = f"{dir}/node_0/http_client.LOGGING"
            if not os.path.exists(csv_path):
                raise ValueError(f"result.csv is missing for {csv_path}")
            througput_csv, ttfb_csv = self._read_csv(csv_path), self._read_ttfb_csv(logging_path)
            if not througput_csv or not ttfb_csv:
                raise ValueError(f"Empty: {csv_path}")
            throughput_list = [bits / time for bits, time in througput_csv]
            throughput_list = [bits / 1000 for bits in throughput_list]
            self.data[cc][num_hops]["throughput_list"].append(throughput_list)
            ttfb_list = ttfb_csv
            self.data[cc][num_hops]["unique_ttfb"].append(ttfb_list)

        self._plot()


class TunHop(PlotGenerator):

    def __init__(self, results_dir, output_dir):
        super().__init__(results_dir, f"{output_dir}/TunConnectIP")

    def _read_csv(self, file_path):
        with open(file_path, 'r') as file:
            content = file.read().strip()
        try:
            return ast.literal_eval(content)
        except:
            print(f"Could not parse {file_path}")

    def _plot(self):
        for cc in self.data:
            plt.figure(figsize=PLOT_SIZE)
            for num_hops in self.data[cc]:
                throughput_data = self.data[cc][num_hops]
                acc_throughputs = [sum(throughput_list) for throughput_list in throughput_data]
                num_transactions_log2 = [f"{2 ** i}" for i in range(len(throughput_data))]
                plt.plot(num_transactions_log2, acc_throughputs, label=f'Hops: {num_hops}')
            plt.xlabel('Number of Transactions (log2)')
            plt.ylabel('Throughput in Mbit/s')
            plt.ylim(ymin=0)
            # plt.title(f'[TunConnectIP] Acc. Throughput vs. Number of Transactions (cc={cc})')
            plt.legend()
            plt.grid(True)
            plt.tight_layout()
            show_plot(plt, f"TunConnectIP_throughput_{cc}")

    def plot(self):
        for _, params, cc, _, dir in self.dirs("TunHop", "LOGGING"):
            num_hops, num_transactions = params
            csv_path = f"{dir}/node_0/result.csv"
            if not os.path.exists(csv_path):
                raise ValueError(f"result.csv is missing for {csv_path}")
            csv_data = self._read_csv(csv_path)
            if not csv_data:
                raise ValueError(f"result.csv is empty for {csv_path}")
            throughput_list = [bits / time for bits, time in csv_data]
            throughput_list = [bits / 1000 for bits in throughput_list]
            if not cc in self.data:
                self.data[cc] = {}
            if not num_hops in self.data[cc]:
                self.data[cc][num_hops] = []
            self.data[cc][num_hops].append(throughput_list)
        self._plot()


class TunSeleniumIP(PlotGenerator):

    def __init__(self, results_dir, output_dir):
        super().__init__(results_dir, f"{output_dir}/TunSeleniumIP")

    def _plot(self):
        for cc in self.data:
            for num_hops in self.data[cc]:
                # Combined box plot for page load time
                w, h = PLOT_SIZE
                h = h * 1.5 if len(self.data[cc][num_hops]) > 6 else h
                fig = plt.figure(figsize=(w, h))
                rows = 3 if len(self.data[cc][num_hops]) > 6 else 2
                cols = 3
                for num_clients in self.data[cc][num_hops]:
                    loading_time_data = self.data[cc][num_hops][num_clients]["page_load_time_list"]
                    num_transactions_log2 = [(f"{2 ** i}" if i % 2 == 0 else "") for i in range(len(loading_time_data))]
                    splot = plt.subplot(rows, cols, num_clients)
                    splot.set_title(f"Clients: {num_clients}")
                    boxplot = plt.boxplot(loading_time_data, labels=num_transactions_log2)
                    for median in boxplot['medians']:
                        median.set(color='red', linewidth=1)
                    plt.grid(True)
                    plt.yscale('log')
                    plt.ylim(ymin=0)
                fig.supxlabel('Number of Selenium Instances Per Client (log2)')
                fig.supylabel('Loading Time (ms) (log)')
                fig.tight_layout()
                # plt.title(f'[TunSeleniumIP] Page Loading Time (num_hops={num_hops}, cc={cc})')
                show_plot(fig, f"TunSeleniumIP_page_{cc}_{num_hops}")

                ####################

                # Combined box plot for TTFB
                w, h = PLOT_SIZE
                h = h * 1.5 if len(self.data[cc][num_hops]) > 6 else h
                fig = plt.figure(figsize=(w, h))
                rows = 3 if len(self.data[cc][num_hops]) > 6 else 2
                cols = 3
                for num_clients in self.data[cc][num_hops]:
                    ttfb_data = self.data[cc][num_hops][num_clients]["ttfb_list"]
                    num_transactions_log2 = [(f"{2 ** i}" if i % 2 == 0 else "") for i in range(len(ttfb_data))]
                    splot = plt.subplot(rows, cols, num_clients)
                    splot.set_title(f"Clients: {num_clients}")
                    boxplot = plt.boxplot(ttfb_data, labels=num_transactions_log2)
                    for median in boxplot['medians']:
                        median.set(color='red', linewidth=1)
                    plt.grid(True)
                    plt.yscale('log')
                    plt.ylim(ymin=0)
                fig.supxlabel('Number of Selenium Instances Per Client (log2)')
                fig.supylabel('TTFB (ms) (log)')
                fig.tight_layout()
                # plt.title(f'[TunSeleniumIP] TTFB (num_hops={num_hops}, cc={cc})')
                show_plot(fig, f"TunSeleniumIP_ttfb_{cc}_{num_hops}")

    def plot(self):
        for _, params, cc, _, dir in self.dirs("TunSeleniumIP", "LOGGING"):
            num_hops, num_clients, num_transactions = params
            page_load_time_list = []
            ttfb_list = []
            for i in range(num_clients):
                client_dir = f"{dir}/node_{i}"
                if not os.path.exists(client_dir):
                    raise ValueError(f"node_0_dir is missing for {client_dir}")
                for transaction in range(num_transactions):
                    print(f"Processing {client_dir} transaction {transaction}")
                    transaction_log = f"{client_dir}/transaction_{transaction}.LOGGING"
                    if not os.path.exists(transaction_log):
                        raise ValueError(f"transaction_{transaction}.LOGGING is missing for {transaction_log}")
                    page_load_time = None
                    ttfb = None
                    with open(transaction_log, "r") as file:
                        content = file.read().strip()
                        match = re.search(r"page loaded in ([\d.]+)ms", content)
                        if not match:
                            raise ValueError(f"Could not find page load time in {transaction_log}")
                        page_load_time = float(match.group(1))
                        match = re.search(r"time to first byte \(ttfb\): (-?[\d.]+)ms", content)
                        if not match:
                            raise ValueError(f"Could not find TTFB in {transaction_log}")
                        ttfb = float(match.group(1))
                    if ttfb > 0:
                        page_load_time_list.append(ttfb + page_load_time)
                        ttfb_list.append(ttfb)
                    else:
                        raise ValueError(f"Invalid TTFB {ttfb} or page load time {page_load_time}")
            if not cc in self.data:
                self.data[cc] = {}
            if not num_hops in self.data[cc]:
                self.data[cc][num_hops] = {}
            if not num_clients in self.data[cc][num_hops]:
                self.data[cc][num_hops][num_clients] = {
                    "page_load_time_list": [],
                    "ttfb_list": [],
                }
            self.data[cc][num_hops][num_clients]["page_load_time_list"].append(page_load_time_list)
            self.data[cc][num_hops][num_clients]["ttfb_list"].append(ttfb_list)
        self._plot()


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--results-dir', required=True)
    parser.add_argument('--output-dir', required=True)
    args = parser.parse_args()

    # 1) TunSeleniumIP
    selenium_plotter = TunSeleniumIP(args.results_dir, args.output_dir)
    selenium_plotter.plot()
    # 2) HttpConnectIP
    # http_plotter = Http(args.results_dir, args.output_dir, connect_ip=True)
    # http_plotter.plot()
    # 3) HttpConnectUDP
    # http_plotter = Http(args.results_dir, args.output_dir, connect_ip=False)
    # http_plotter.plot()
    # 4) TunHop
    # hop_plotter = TunHop(args.results_dir, args.output_dir)
    # hop_plotter.plot()
    # 5) CPU
    # cpu_plotter = CpuPlotter(args.results_dir, args.output_dir)
    # cpu_plotter.plot()


if __name__ == "__main__":
    main()
