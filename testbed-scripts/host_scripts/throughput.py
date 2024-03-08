import os
import re
import argparse


def extract_iperf_throughput(directory):
    kbitrates = []
    pattern = re.compile(r'(\d+\.?\d*) KBytes/sec')
    num_transactions = len([f for f in os.listdir(directory) if 'transaction_' in f])
    for i in range(num_transactions):
        with open(f"{directory}/transaction_{i}.LOGGING", "r") as f:
            lines = f.readlines()
        kbits = []
        for line in lines:
            match = pattern.search(line)
            if not match:
                continue
            kbits.append(float(match.group(1)))
        if len(kbits) <= 1:
            print(f"Warning: Expected >=2 measurements, got {len(kbits)}")
            continue
        assert len(kbits) >= 2, f"Expected >=2 measurements, got {len(kbits)}"
        # kybtes to bits
        kbitrates.append((kbits[-2] * 1000 * 8, 1000))
    print(kbitrates)


def extract_log_throughput(directory):
    pattern = re.compile(r'(\d+)bytes received in (\d+)ms')
    ttfb_pattern = re.compile(r'TTFB=(\d+)ms')
    with open(f"{directory}/http_client.LOGGING", "r") as f:
        lines = f.readlines()
    results = []
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        bytes_received, time_ms = match.groups()
        results.append((int(bytes_received) * 8, int(time_ms)))
    print(results)
    results = []
    for line in lines:
        match = ttfb_pattern.search(line)
        if not match:
            continue
        ttfb_ms = match.groups()
        results.append(int(ttfb_ms))
    print(results)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Extract throughput information. [(bits, ms)]')
    parser.add_argument('--dir', type=str, required=True, help='Directory containing the log files.')
    args = parser.parse_args()

    directory = args.dir

    if os.path.exists(f"{directory}/http_client.LOGGING"):
        extract_log_throughput(directory)
    else:
        extract_iperf_throughput(directory)
