#!/usr/bin/env python2.7

# record and then replay boot
# for 32-bit linux

import os
import sys
import shutil
thisdir = os.path.dirname(os.path.realpath(__file__))
td = os.path.realpath(f"{thisdir}/../..")
sys.path.append(td)
from ptest_utils import *
sys.path.append(pandascriptsdir)
from run_guest import create_boot_recording

def both(msg, disp_fn):
    tof.write(msg + "\n")
    disp_fn(msg)

tof = open(tmpoutfile, "w")
try:
    qcow = f"{pandaregressiondir}/qcows/wheezy_32bit.qcow2"
    create_boot_recording(qemu, qcow, replayfile, boot_time=5)
    msg = f"Create boot recording for {testname} succeeded"
    both(msg, progress)
except Exception as e:
    msg = f"Create recording for {testname} FAILED"
    both(msg, error)
    tof.close()
    raise e

try:
    run_test_debian("", "rr-boot-test", "i386")
    msg = f"Replay for {testname} succeeded"
    both(msg, progress)
    tof.close()
except Exception as e:
    msg = f"Replay for {testname} FAILED"
    both(msg, error)
    tof.close()
    raise e



