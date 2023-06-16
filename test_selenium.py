#!/usr/bin/env python

from selenium import webdriver
import datetime
import logging
from pathlib import Path
import sys
import os

import boto3
from botocore.exceptions import NoCredentialsError


LOGGER = logging.getLogger(__file__)
LOGGER.setLevel(logging.INFO)
AWS_ACCESS_KEY_ID = os.getenv('AWS_ACCESS_KEY_ID')
AWS_SECRET_ACCESS_KEY = os.getenv('AWS_SECRET_ACCESS_KEY')
# Endpoint is something like "https://s3.sbg.cloud.ovh.net/"
S3_ENDPOINT = os.getenv('S3_ENDPOINT')
S3_REGION = os.getenv('S3_REGION')
S3_BUCKET = os.getenv('S3_BUCKET')
url = os.getenv('COMMIT_URL')


remote = "http://162.19.225.144"
port = "4444"
executor = f"{remote}:{port}/wd/hub"

basename = "screenshot"
suffix = datetime.datetime.now().strftime("%y%m%d_%H%M%S")
filename = "_".join([basename, suffix]) # e.g. 'mylogfile_120508_171442'
path = f"/tmp/{filename}.png"

driver = webdriver.Remote(command_executor=executor, desired_capabilities=webdriver.DesiredCapabilities.FIREFOX)

driver.get(url)

driver.get_screenshot_as_file(path)

print(f"Success! Your screenshot is available at {path}")

# Uploading to S3
def upload_to_s3(local_file: Path):
    """Uploads `local_file` to the s3 bucket.
    """
    if not AWS_ACCESS_KEY_ID:
        LOGGER.critical("`AWS_ACCESS_KEY_ID` is not defined")
        sys.exit(1)
    if not AWS_SECRET_ACCESS_KEY:
        LOGGER.critical("`AWS_SECRET_ACCESS_KEY` is not defined")
        sys.exit(1)
    if not S3_BUCKET:
        LOGGER.critical("`S3_BUCKET` is not defined")
        sys.exit(1)

    s3client = boto3.client('s3', endpoint_url=S3_ENDPOINT,
                            region_name=S3_REGION)

    LOGGER.info("Uploading archive to `%s`...", S3_BUCKET)
    try:
        s3client.upload_file(str(local_file), S3_BUCKET, local_file.name)
    except NoCredentialsError:
        LOGGER.exception("Incorrect S3 credentials given")
        raise
    LOGGER.info("Upload Successful")

# Upload to s3 bucket
upload_to_s3(Path(path))
