import poslib as pos
import glob
from os import environ
from datetime import datetime


def latest_allocation_folder():
    # assume the last folder to be the current allocation
    files = glob.glob(f"/srv/testbed/results/{environ['USER']}/default/*")
    files.sort()
    return files[-1]

def get_calendar_results():
    user = environ["USER"]
    result, _ = pos.calendar.list_all(f"owner={user}", json=True)
    return result


def print_calendar_entry(entry,):
    print(f"id: {entry['id']}")
    print(f"nodes: {', '.join(entry['nodes'])}")
    print(f"start_date: {entry['start_date']}")
    print(f"end_date: {entry['end_date']}")


def find_fitting_calendar_entry(nodes, interactive=False):
    results = get_calendar_results()
    fitting_entry = None
    if nodes is None or len(nodes) == 0:
        if len(results) > 0:
            print("Non nodes specified, using first entry in calendar")
            return results[0]
        else:
            print("No nodes specified and no calendar entries found! Exiting.")
            exit()

    for entry in results:
        entry_nodes = entry['nodes']
        fitting_entry = entry
        for node in nodes:
            if node not in entry_nodes:
                fitting_entry = None
        if fitting_entry:
            break

    if not fitting_entry:
        if not interactive:
            print("No fitting calendar entry found! Exiting.")
            exit()
        print("No fitting calendar entry found! Do you want to create one ASAP? (y/n)")
        answer = input()
        if answer is not 'y':
            return None
        print("How long should the calendar entry be? (in minutes)")
        answer = int(input())
        result = pos.calendar.create(nodes=nodes, start=None,
                                     duration=answer,
                                     asap_after=datetime.now())
        result, _ = pos.calendar.list_all(f"id={result['id']}", json=True)
        fitting_entry = result[0]
    return fitting_entry
