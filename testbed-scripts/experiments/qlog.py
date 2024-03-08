import argparse
import json
import gzip


def _load_qlog(filename):
    with gzip.open(filename, "rb") as f:
        file_content = f.read()
    return json.loads(file_content)


def _packet_map(qlog, event_name=None):
    packet_map = {}
    for event in qlog['traces'][0]['events']:
        packet_number = event[3].get('header', {}).get('packet_number', None)
        if not packet_number:
            continue
        if event_name is not None and event[2] != event_name:
            continue
        if packet_number in packet_map:
            continue
        packet_map[packet_number] = event
    return packet_map


def _event_stats(qlog):
    events = {}
    for event in qlog['traces'][0]['events']:
        if event[2] not in events:
            events[event[2]] = 0
        events[event[2]] += 1
    return events


def _calculate_latency_and_jitter(server_qlog, client_qlog):
    """returns average latency and jitter in us"""
    latencies = []
    # 1) server sent, client received
    server_qlog_map = _packet_map(server_qlog, "packet_sent")
    client_qlog_map = _packet_map(client_qlog, "packet_received")
    common_packet_numbers = set(server_qlog_map.keys()).intersection(set(client_qlog_map.keys()))
    for packet_number in common_packet_numbers:
        server_time = int(server_qlog_map[packet_number][0])
        client_time = int(client_qlog_map[packet_number][0])
        latencies.append(abs(server_time - client_time))
    # 2) client sent, server received
    server_qlog_map = _packet_map(server_qlog, "packet_received")
    client_qlog_map = _packet_map(client_qlog, "packet_sent")
    common_packet_numbers = set(server_qlog_map.keys()).intersection(set(client_qlog_map.keys()))
    for packet_number in common_packet_numbers:
        server_time = int(server_qlog_map[packet_number][0])
        client_time = int(client_qlog_map[packet_number][0])
        latencies.append(abs(server_time - client_time))

    avg_latency = sum(latencies) / len(latencies) if latencies else 0

    jitters = []
    for i in range(1, len(latencies)):
        jitters.append(abs(latencies[i] - latencies[i - 1]))

    avg_jitter = sum(jitters) / len(jitters) if jitters else 0

    return avg_latency, avg_jitter


def _calculate_packet_loss(server_qlog, client_qlog):
    # 1) server sent, client received
    server_qlog_map = _packet_map(server_qlog, "packet_sent")
    client_qlog_map = _packet_map(client_qlog, "packet_received")
    all_packet_numbers = set(server_qlog_map.keys()).union(set(client_qlog_map.keys()))
    common_packet_numbers = set(server_qlog_map.keys()).intersection(set(client_qlog_map.keys()))
    # 2) client sent, server received
    server_qlog_map = _packet_map(server_qlog, "packet_received")
    client_qlog_map = _packet_map(client_qlog, "packet_sent")
    all_packet_numbers = all_packet_numbers.union(set(server_qlog_map.keys()).union(set(client_qlog_map.keys())))
    common_packet_numbers = common_packet_numbers.union(
        set(server_qlog_map.keys()).intersection(set(client_qlog_map.keys())))
    # 3) calculate loss
    sent_packets = len(all_packet_numbers)
    received_packets = len(common_packet_numbers)
    lost_packets = sent_packets - received_packets
    loss_rate = lost_packets / sent_packets
    return lost_packets, loss_rate


def _calculate_time_to_first_byte(server_qlog, client_qlog):
    """returns time to first byte in us"""
    client_request_packets = _packet_map(client_qlog, "packet_sent")
    first_request_packet_number = min(client_request_packets, key=client_request_packets.get)
    first_request_time = client_request_packets[first_request_packet_number][0]

    server_response_packets = _packet_map(server_qlog, "packet_sent")
    first_response_packet_number = min(server_response_packets, key=server_response_packets.get)

    first_response_time = _packet_map(client_qlog, "packet_received")[first_response_packet_number][0]

    return int(first_response_time) - int(first_request_time)


def _calculate_rtt(server_qlog, client_qlog):
    """Calculate average RTT in microseconds."""
    rtt_samples = []
    last_rtts = []
    for event in server_qlog['traces'][0]['events']:
        if event[2] != "metric_update":
            continue
        rtt_samples.append(event[3]["smoothed_rtt"])
    last_rtts.append(rtt_samples[-1])
    for event in client_qlog['traces'][0]['events']:
        if event[2] != "metric_update":
            continue
        rtt_samples.append(event[3]["smoothed_rtt"])
    last_rtts.append(rtt_samples[-1])
    return sum(rtt_samples) / len(rtt_samples), sum(last_rtts) / len(last_rtts)


def _calculate_packet_metrics(server_qlog, client_qlog):
    avg_latency, avg_jitter = _calculate_latency_and_jitter(server_qlog, client_qlog)
    lost_packets, loss_rate = _calculate_packet_loss(server_qlog, client_qlog)
    ttfb = _calculate_time_to_first_byte(server_qlog, client_qlog)
    avg_rtt, smoothed_rtt = _calculate_rtt(server_qlog, client_qlog)
    metrics = {
        'latency_ms': avg_latency / 1000,
        'jitter_ms': avg_jitter / 1000,
        'lost_packets': lost_packets,
        'packet_loss_rate': loss_rate,
        'ttfb_ms': ttfb / 1000,
        "avg_rtt_ms": avg_rtt / 1000,
        "smoothed_rtt_ms": smoothed_rtt / 1000,
        "server_stats": _event_stats(server_qlog),
        "client_stats": _event_stats(client_qlog)
    }
    return metrics


def produce_metrics(server_qlog_path: str, client_qlog_path: str, output_path: str):
    server_qlog = _load_qlog(server_qlog_path)
    client_qlog = _load_qlog(client_qlog_path)
    metrics = _calculate_packet_metrics(server_qlog, client_qlog)
    with open(output_path, 'w') as f:
        f.write(json.dumps(metrics, indent=4))


def main():
    parser = argparse.ArgumentParser(description='Process QLOG files and produce network metrics.')
    parser.add_argument('server_qlog', help='Path to the server QLOG file')
    parser.add_argument('client_qlog', help='Path to the client QLOG file')
    parser.add_argument('output_path', help='Path to the output file for the results')
    args = parser.parse_args()
    produce_metrics(args.server_qlog, args.client_qlog, args.output_path)


if __name__ == "__main__":
    main()
