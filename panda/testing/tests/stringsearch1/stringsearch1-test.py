#!/usr/bin/python

import os
import sys
import shutil

thisdir = os.path.dirname(os.path.realpath(__file__))
td = os.path.realpath(f"{thisdir}/../..")
sys.path.append(td)

from ptest_utils import *

ss_filename = f"{miscdir}/cat"
run_test_debian(f"-panda stringsearch:name={ss_filename}", "cat", "i386")

os.chdir(tmpoutdir)
shutil.move(f"{miscdir}/cat_string_matches.txt", tmpoutfile)
