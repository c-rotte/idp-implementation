import os
import pathlib
import time
import sys
import shutil
import urllib.parse
from selenium import webdriver
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.common.by import By


def netlog2qlog(netlog_path, dl_path):
    assert dl_path != "/root", "Please specify a different download path"
    # convert netlog to qlog
    # this is quite hacky, but it works.
    # we first load the netlog into the qvis webapp and then download the qlog again.
    options = Options()
    prefs = {"profile.default_content_settings.popups": 0, "download.default_directory": dl_path}
    options.add_experimental_option("prefs", prefs)
    options.add_argument('--headless=new')
    options.add_argument('--no-sandbox')
    service = Service()
    service.path = pathlib.Path("~/.bind/chromedriver-linux64/chromedriver").expanduser().absolute()
    driver = webdriver.Chrome(options=options, service=service)
    try:
        driver.get("https://qvis.quictools.info/#/files")
        driver.find_element(by=By.ID, value="fileUpload").send_keys(netlog_path)
        driver.find_element(by=By.XPATH,
                            value='//*[@id="FileManagerContainer"]/div[2]/div[2]/div[1]/form/div/div[2]/button').click()
        driver.find_element(by=By.XPATH, value='//*[@id="loadedGroupsContainer"]/div/div[3]/button').click()
        print("Downloaded qlog to", dl_path)
        driver.close()
    finally:
        driver.quit()
    shutil.copy(netlog_path, dl_path)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script.py <netlog path> <dl path>")
        sys.exit(1)
    netlog_path = sys.argv[1]
    dl_path = sys.argv[2]
    netlog2qlog(netlog_path, dl_path)
