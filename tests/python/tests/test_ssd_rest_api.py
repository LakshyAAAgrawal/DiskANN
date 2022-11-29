import atexit
import os
import subprocess
import sys
import time
import unittest

import numpy as np
import requests

from tempfile import TemporaryDirectory

from .disk_ann_util import build_ssd_index


_VECTOR_DIMS = 100
_RNG_SEED = 12345

class TestSSDRestApi(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if "DISKANN_REST_SERVER" in os.environ:
            cls._rest_address = os.environ["DISKANN_REST_SERVER"]
            cls._cleanup_lambda = lambda : None
        else:
            if "DISKANN_BUILD_DIR" not in os.environ:
                raise Exception("We require the environment variable DISKANN_BUILD_DIR be set to the diskann build directory on disk")
            diskann_build_dir = os.environ["DISKANN_BUILD_DIR"]

            if "DISKANN_REST_TEST_WORKING_DIR" not in os.environ:
                cls._temp_dir = TemporaryDirectory()
                cls._build_dir = cls._temp_dir.name
            else:
                cls._temp_dir = None
                cls._build_dir = os.environ["DISKANN_REST_TEST_WORKING_DIR"]

            rng = np.random.default_rng(_RNG_SEED)  # adjust seed for new random numbers
            cls._working_vectors = rng.random((1000, _VECTOR_DIMS), dtype=float)
            build_ssd_index(
                diskann_build_dir,
                cls._build_dir,
                cls._working_vectors
            )
            # now we have a built index, we should run the rest server
            rest_port = rng.integers(10000, 10100)
            cls._rest_address = f"http://127.0.0.1:{rest_port}/"

            ssd_server_path = os.path.join(diskann_build_dir, "tests", "restapi", "ssd_server")

            args = [
                ssd_server_path,
                cls._rest_address,
                "float",
                os.path.join(cls._build_dir, "smoke_test"),
                str(_VECTOR_DIMS),
                "1"
            ]

            command_run = " ".join(args)
            print(f"Executing REST server startup command: {command_run}", file=sys.stderr)

            cls._rest_process = subprocess.Popen(args)
            time.sleep(10)

            cls._cleanup_lambda = lambda: cls._rest_process.kill()

            # logically this shouldn't be necessary, but an open port is worse than some random gibberish in the
            # system tmp dir
            atexit.register(cls._cleanup_lambda)

    @classmethod
    def tearDownClass(cls):
        cls._cleanup_lambda()

    def _is_ready(self):
        return self._rest_process.poll() is None  # None means the process has no return status code yet

    def test_server_responds(self):
        rng = np.random.default_rng(_RNG_SEED)
        query = rng.random((_VECTOR_DIMS), dtype=float).tolist()
        json_payload = {
            "Ls": 32,
            "query_id": 1234,
            "query": query,
            "k": 10
        }
        try:
            response = requests.post(self._rest_address, json=json_payload)
            self.assertEqual(200, response.status_code, "Expected a successful request")
        except Exception:
            raise Exception(f"Rest process status code is: {self._rest_process.poll()}")
