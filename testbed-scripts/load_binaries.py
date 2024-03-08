import gitlab
import os
import zipfile

project_id_proxygen_mvfst = 129859
project_id_cloudflare_quiche = 130503


def download_binaries_proxygen_mvfst():
    with open(os.path.join(os.path.dirname(__file__), 'gitlab_access_token'), 'r') as f:
        token = f.read().strip()

    gitlab_session = gitlab.Gitlab(url="https://gitlab.lrz.de", private_token=token)
    project = gitlab_session.projects.get(project_id_proxygen_mvfst)
    print("Found project proxygen_mvfst")

    with open("binaries.zip", "wb") as f:
        project.artifacts.download(ref_name="idp/sample-server", job="artifact_job", streamed=True, action=f.write)
    print("Downloaded binaries.zip")

    with zipfile.ZipFile("binaries.zip", "r") as zip_ref:
        zip_ref.printdir()
        zip_ref.extractall("./binaries_proxygen")
    os.remove("binaries.zip")

    print("Extracted binaries and removed zip file")


def download_binaries_cloudflare_quiche():
    with open(os.path.join(os.path.dirname(__file__), 'gitlab_access_token'), 'r') as f:
        token = f.read().strip()

    gitlab_session = gitlab.Gitlab(url="https://gitlab.lrz.de", private_token=token)
    project = gitlab_session.projects.get(project_id_cloudflare_quiche)
    print("Found project cloudflare_quiche")

    with open("binaries.zip", "wb") as f:
        project.artifacts.download(ref_name="crotte", job="build:cargo", streamed=True, action=f.write)
    print("Downloaded binaries.zip")

    with zipfile.ZipFile("binaries.zip", "r") as zip_ref:
        zip_ref.printdir()
        zip_ref.extractall("./binaries_quiche")
    os.remove("binaries.zip")

    print("Extracted binaries and removed zip file")
