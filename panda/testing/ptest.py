#!/usr/bin/python


USAGE = """

NB: you need to set the PANDA_REGRESSION_DIR env variable for any
of this to work.  This is where all your regression testing will happen

You should be able to run the following

ptest.py init         (initializes testing, downloads some qcows)
ptest.py setup         (runs setup.py for all enabled tests, doesn't download qcows)
ptest.py bless        (runs all enabled tests, blesses and saves outputs)
ptest.py test         (re-runs all enabled tests and checks output against blessed)

also, 

ptest.py              which is equiv to ptests.py test

each of these will print out lots of info about how the operation is
proceeding, and, at the end, provide a summary of which enabled tests
the operation succeeded for and which it failed for.  The very last
line will tell you if, overall, the operation succeeded.


Details.

./config.testing file contains list of tests that are currently
enabled.  each line in that file should be a directory under tests.
If you put '#' at the front of a line that will disable the test.

"""

import os
import sys
import argparse
import shutil
import subprocess as sp
import filecmp

if len(sys.argv) == 1:
    mode = 'test'
    all_targets = True
elif len(sys.argv) > 1:
    mode = sys.argv[1]
    all_targets = True
    targets = None
    if len(sys.argv) == 3:
        targets = sys.argv[2:]
        all_targets = (sys.argv[2] == 'all')

from ptest_utils import *

if mode not in ['init', 'setup', 'bless', 'test']:
    exit(USAGE)

testsdir = f"{testingscriptsdir}/tests"

# make sure all enabled tests have min requirements
# of a setup and a test script
if debug: progress("checking min requirements")
for testname in enabled_tests:
    testdir = f"{testsdir}/{testname}"
    dir_required(testdir)
    file_required(f"{testdir}/{testname}-setup.py")
    file_required(f"{testdir}/{testname}-test.py")


def setup(testname):
    progress(f"Setup {testname}")
    try:
        os.chdir(testsdir)
        run(f"{testsdir}/{testname}/{testname}-setup.py")
        progress(f"Setup {testname} succeeded")
        return True
    except Exception as e:
        error(f"Setup {testname} failed")
        print(e)
        return False

def bless(testname):
    progress(f"Bless {testname}")
    try:
        # Run test script
        run(f"{testsdir}/{testname}/{testname}-test.py")

        # Copy output files to blessed directory
        blesseddir = os.path.join(pandaregressiondir, "blessed", testname)
        clear_dir(blesseddir)
        tmpoutdir = os.path.join(pandaregressiondir, "tmpout", testname)
        files = os.listdir(tmpoutdir)
#        print tmpoutdir

        for f in files:
            progress(f"Moving blessed file {f}")
            shutil.move(os.path.join(tmpoutdir, f), os.path.join(blesseddir, f))
        progress(f"Bless {testname} succeeded")
        return True
    except Exception as e:
        error(f"Bless {testname} failed")
        print(e)
        return False

def test(testname):
    progress(f"Test {testname}")
    try:
        run(f"{testsdir}/{testname}/{testname}-test.py")

        blesseddir = os.path.join(pandaregressiondir, "blessed", testname)
        tmpoutdir = os.path.join(pandaregressiondir, "tmpout", testname)
        files = os.listdir(tmpoutdir)

        for f in files:
            tof = os.path.join(tmpoutdir, f)
            if not (file_exists(tof)):
                error(f"tmp out for {testname} missing: {tof}")
                return False

            bf = os.path.join(blesseddir, f)
            if not (file_exists(bf)):
                error(f"blessed output for {testname} missing: {bf}")
                return False

            progress(f"tof = {tof}")
            progress(f"bf =  {bf}")
            if filecmp.cmp(tof, bf):
                progress(f"New output for {testname} agrees with blessed")
                progress(f"Test {testname} succeeded")
            else:
                error(f"New output for {testname} DISAGREES with blessed")
                error(f"{tof} != {bf}")
                return False
        return True
    except Exception as e:
        error(f"Test {testname} failed")
        print(e)
        return False

def do_all(do_fn):
    return {testname: do_fn(testname) for testname in enabled_tests}

def run_mode(the_mode, do_fn):
    res = {}
    if all_targets:
        res = do_all(do_fn)
    else:
        for testname in targets:
            res[testname] = do_fn(testname)
    progress(f"Summary results for {the_mode}")
    all_res = True
    for i, testname in enumerate(res):
        if res[testname]:
            progress("  test %d %20s : %s succeeded" % (i, testname, the_mode))
        else:
            error("  test %d %20s : %s FAILED" % (i, testname, the_mode))
        all_res &= res[testname]
    if all_res:
        progress(f"All {the_mode} succeeded")
    else:
        error(f"XXX Some {the_mode} failed")
        sys.exit(100)


if __name__ == "__main__":
    if mode == 'init':
        progress(f"Initializing panda regression test dir in {pandaregressiondir}")
        progress(f"Removing {pandaregressiondir}")
        shutil.rmtree(pandaregressiondir, ignore_errors=True)
        os.makedirs(pandaregressiondir)
        for dirname in ['qcows', 'replays', 'blessed', 'tmpout', 'misc']:
            for testname in enabled_tests:
                os.makedirs(os.path.join( pandaregressiondir, dirname, testname))

        os.chdir(f"{pandaregressiondir}/qcows")
        sp.check_call(["wget", "http://panda.moyix.net/~moyix/wheezy_panda2.qcow2", "-O", "wheezy_panda2.qcow2"])
        run_mode('setup', setup)

    if mode == 'setup':
        run_mode(mode, setup)

    if mode == 'bless':
        run_mode(mode, bless)

    if mode == 'test':
        run_mode(mode, test)




