import json
import os
import pathlib
import random
import threading
import time
import sys
import urllib.parse
from selenium import webdriver
from selenium.webdriver import ActionChains, Keys
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait

# https://github.com/JsBergbau/BindToInterface
BIND_SO_PATH = pathlib.Path("~/.bind/bind.so").expanduser().absolute()


def page_has_loaded(driver, url):
    current_requests = driver.execute_script('return window.performance.getEntries();')
    total_bytes = 0
    for entry in current_requests:
        if entry.get("responseEnd", 0) <= 0:
            continue
        total_bytes += entry.get("transferSize", 0)
    # print(f"total bytes: {total_bytes}")
    # 1870000 bytes for our tum.de mirror
    return total_bytes >= 1870000


def load_page_at_time(url, unix_timestamp, iface):
    # initialize random based on time and the iface name
    random.seed(time.time() + sum([ord(c) for c in iface]))
    service = Service()
    # future note: does not seem to work with domains, only IPv4 addresses
    print(f"Loading {url} at {unix_timestamp} on {iface}")
    profile_path = f"/tmp/{random.randrange(0, 9999999999)}.profile"
    cache_path = f"/tmp/{random.randrange(0, 9999999999)}.cache"
    print(f"profile_path: {profile_path}")
    print(f"cache_path: {cache_path}")
    os.system(f"rm -rf {profile_path}")
    os.system(f"mkdir -p {profile_path}")
    os.system(f"rm -rf {cache_path}")
    os.system(f"mkdir -p {cache_path}")
    options = webdriver.ChromeOptions()
    # https://www.selenium.dev/documentation/webdriver/drivers/options/#pageloadstrategy
    options.page_load_strategy = "eager"
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument(f"--user-data-dir={profile_path}")
    options.add_argument("--disable-cache")
    options.add_argument("--disable-application-cache")
    # https://superuser.com/a/1793916
    options.add_argument("--disk-cache-size=1")
    options.add_argument(f"--disk-cache-dir={cache_path}")
    options.set_capability("goog:loggingPrefs", {"performance": "ALL"})
    options.add_argument('--headless=new')
    # https://stackoverflow.com/a/52985937
    options.add_argument('--no-sandbox')
    # https://stackoverflow.com/a/50827853
    # https://github.com/aiortc/aioquic/tree/main/examples
    options.add_argument("--enable-quic")
    options.add_argument(f"--origin-to-force-quic-on={urllib.parse.urlparse(url).netloc}")
    options.add_argument("--ignore-certificate-errors-spki-list=BSQJ0jkQ7wwhR7KvPZ+DSNk2XTZ/MS6xCbo9qu++VdQ=")
    os.system(f"rm -rf /tmp/{iface}.netlog")
    service.path = pathlib.Path("~/.bind/chromedriver-linux64/chromedriver").expanduser().absolute()
    if iface != "null":
        service.env["BIND_INTERFACE"] = iface
        service.env["BIND_EXCLUDE"] = "127.0.0.1"
        service.env["LD_PRELOAD"] = str(BIND_SO_PATH)
    driver = webdriver.Chrome(options=options, service=service)
    # setup driver
    driver.execute_cdp_cmd("Network.setBlockedURLs", {"urls": ["*tum.de*", "*visitor-analytics.io*", "*.mp4"]})
    driver.execute_cdp_cmd("Network.enable", {})
    driver.execute_cdp_cmd("Network.setCacheDisabled", {"cacheDisabled": True})
    # https://stackoverflow.com/a/46672769
    # [10]: 10% loss, 200ms rtt, 1Mbit/s bandwidth
    # (we set the packet loss in tc beforehand)
    driver.set_network_conditions(offline=False, latency=2 * 200, throughput=1 * 1024 * 1024)
    timing_log = None
    try:
        if unix_timestamp == 0:
            print("Skipping sleep because unix_timestamp is 0")
        else:
            current_time = time.time()
            sleep_time = unix_timestamp - current_time
            if sleep_time > 0:
                print(f"Sleeping for {sleep_time} seconds...")
                time.sleep(sleep_time)
            else:
                # raise Exception("unix_timestamp is in the past!")
                pass
        print(f"Loading {url}")
        now = time.time()
        driver.get(url)
        print(f"get() took {time.time() - now}ms")
        # wait until page has loaded
        while True:
            local_now = time.time()
            loaded = page_has_loaded(driver, url)
            # print(f"checked page in {local_now - now}ms")
            if loaded:
                break
            time.sleep(0.05)
        #
        duration = time.time() - now
        page_source = driver.page_source
        if "ERR_QUIC_HANDSHAKE_FAILED" in page_source:
            raise Exception("ERR_QUIC_HANDSHAKE_FAILED while loading page!")
        if "ERR_QUIC_PROTOCOL_ERROR" in page_source:
            raise Exception("ERR_QUIC_PROTOCOL_ERROR while loading page!")
        print(f"page loaded in {duration * 1000}ms")
        time.sleep(3)
        timing_log = []
        request_id = None
        for entry in driver.get_log("performance"):
            message = json.loads(entry["message"])
            message = message["message"]
            if message["method"] == "Network.requestWillBeSent":
                request_url = message["params"]["request"]["url"].strip("/")
                if request_url == url.strip("/"):
                    timing_log.append(message)
                    request_id = message["params"]["requestId"]
                    print(f"requestWillBeSent: {request_url} -> {request_id}")
            if message["method"] == "Network.dataReceived":
                id = message["params"]["requestId"]
                if id == request_id:
                    timing_log.append(message)
                    print(f"dataReceived: {request_id}")
                    break
    except Exception as e:
        print(e)
    finally:
        try:
            driver.close()
            driver.quit()
        except Exception as e:
            print(e)
        error = True
        if timing_log:
            # ms
            requestWillBeSent = 0
            responseReceived = 0
            for message in timing_log:
                if requestWillBeSent == 0 and message["method"] == "Network.requestWillBeSent":
                    requestWillBeSent = float(message["params"]["timestamp"]) * 1000
                if responseReceived == 0 and message["method"] == "Network.dataReceived":
                    responseReceived = float(message["params"]["timestamp"]) * 1000
            if responseReceived != 0 and requestWillBeSent != 0:
                ttfb = responseReceived - requestWillBeSent
                if ttfb < 0 or ttfb > duration * 1000:
                    raise Exception(f"ttfb out of bounds: {ttfb}ms vs {duration * 1000}ms")
                error = False
                print(f"time to first byte (ttfb): {ttfb}ms")
        os.system(f"rm -rf {profile_path}")
        os.system(f"rm -rf {cache_path}")
        if error:
            raise Exception("Error while loading page!")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python script.py <URL> <Unix Timestamp> <iface>")
        sys.exit(1)
    url = sys.argv[1]
    unix_timestamp = float(sys.argv[2])
    iface = sys.argv[3]
    try:
        load_page_at_time(url, unix_timestamp, iface)
    except Exception as e:
        open(f"/tmp/experiment.error", "w").close()
        print(e)
