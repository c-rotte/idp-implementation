import os
import json
import shutil
import csv


def extract_experiment_name(exp):
    return exp.split('x')[0][:-2]


def extract_logging_data(path):
    with open(path, 'r') as file:
        reader = csv.reader(file)
        return [tuple(map(float, row)) for row in reader]


def extract_logging_qlog_data(path):
    with open(path, 'r') as file:
        return json.load(file)


def process_experiment_folder(exp_folder, log_dir):
    exp_data = {}

    for cc_algo in os.listdir(os.path.join(log_dir, exp_folder)):
        cc_data = {}

        # Logging data
        logging_path = os.path.join(log_dir, exp_folder, cc_algo, 'LOGGING', 'node_0', 'result.csv')
        if os.path.exists(logging_path):
            cc_data['logging_data'] = extract_logging_data(logging_path)

        # Logging QLOG data
        qlog_path = os.path.join(log_dir, exp_folder, cc_algo, 'LOGGING_QLOG', 'node_0', 'metrics.json')
        if os.path.exists(qlog_path):
            cc_data['logging_qlog_data'] = extract_logging_qlog_data(qlog_path)

        exp_data[cc_algo] = cc_data

    return exp_data


def copy_perf_svgs(exp_folder, cc_algo, log_dir, output_dir):
    node_folders = os.listdir(os.path.join(log_dir, exp_folder, cc_algo, 'PERF'))
    for node_folder in node_folders:
        for file in os.listdir(os.path.join(log_dir, exp_folder, cc_algo, 'PERF', node_folder)):
            if file.endswith('.svg'):
                src = os.path.join(log_dir, exp_folder, cc_algo, 'PERF', node_folder, file)
                dest_folder = os.path.join(output_dir, 'flamegraphs', exp_folder, cc_algo, node_folder)
                os.makedirs(dest_folder, exist_ok=True)
                shutil.copy(src, dest_folder)


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--log-dir', required=True)
    parser.add_argument('--output-dir', required=True)
    args = parser.parse_args()

    log_dir = args.log_dir
    output_dir = args.output_dir

    experiments_data = {}

    for exp_folder in os.listdir(log_dir):
        experiments_data[extract_experiment_name(exp_folder)] = process_experiment_folder(exp_folder, log_dir)

        # For each congestion control algo, copy the flamegraphs
        for cc_algo in os.listdir(os.path.join(log_dir, exp_folder)):
            copy_perf_svgs(exp_folder, cc_algo, log_dir, output_dir)

    # Save the experiment data
    for exp_name, data in experiments_data.items():
        with open(os.path.join(output_dir, f"{exp_name}.json"), 'w') as file:
            json.dump(data, file, indent=4)


if __name__ == "__main__":
    main()
