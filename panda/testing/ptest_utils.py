
import os
import subprocess as sp
import sys
import re
import shutil 
from colorama import Fore, Style
import tempfile

debug = False

def progress(msg):
    print(
        f'{Fore.GREEN}[ptest.py] {Fore.RESET}{Style.BRIGHT}{msg}{Style.RESET_ALL}'
    )

    print()

def error(msg):
    print(f'{Fore.RED}[ptest.py] {Fore.RESET}{Style.BRIGHT}{msg}{Style.RESET_ALL}')
    print()

def dir_exists(dirname):
    return os.path.exists(dirname) and os.path.isdir(dirname)

def dir_required(dirname):
    if dir_exists(dirname):
        if debug:
            progress(f"Dir found: {dirname}")
    else:
        progress(f"Dir missing: {dirname}")
        sys.exit(1)

# remove all files from this dir
def clear_dir(dirname):
    shutil.rmtree(dirname)
    os.mkdir(dirname)

def file_exists(filename):
    return os.path.exists(filename) and os.path.isfile(filename)

def file_required(filename):
    if file_exists(filename):
        if debug:
            progress(f"File found: {filename}")
    else:
        progress(f"File missing: {filename}")
        sys.exit(1)

def moveit(base1, base2, suff):
    print(f"moving {base1}{suff} to {base2}{suff}")
    shutil.move(base1 + suff, base2 + suff)

def run(cmd):
    if debug:
        progress(f"Cmd = {cmd}")
        sp.check_call([cmd])
    else:
        DEVNULL = open(os.devnull, "w")
        sp.check_call([cmd], stdout=DEVNULL, stderr=DEVNULL)



pandaregressiondir = "PANDA_REGRESSION_DIR"
assert (pandaregressiondir in os.environ)
pandaregressiondir = os.environ[pandaregressiondir]

thisdir = os.path.dirname(os.path.realpath(__file__))
pandadir = os.path.realpath(f"{thisdir}/../..")
pandascriptsdir = os.path.realpath(f"{pandadir}/panda/scripts")
default_build_dir = os.path.join(pandadir, 'build')
panda_build_dir = os.getenv("PANDA_BUILD", default_build_dir)

# import arch data from run_debian
sys.path.append(pandascriptsdir)
from run_debian import SUPPORTED_ARCHES
testingscriptsdir = thisdir

ptest_config = f"{testingscriptsdir}/tests/config.testing"
if not (file_exists(ptest_config)):
    progress(f"ptest_config file missing: {ptest_config}")
    sys.exit(1)

maybe_tests = [test.strip() for test in open(ptest_config).readlines()]
enabled_tests = [test for test in maybe_tests if (not test.startswith("#"))]
disabled_tests = [test for test in maybe_tests if (test.startswith("#"))]

if debug: 
    progress(("%d enabled tests: " % (len(enabled_tests))) + " : " + (str(enabled_tests)))
    progress(("%d disabled tests: " % (len(disabled_tests))) + " : " + (str(disabled_tests)))

replaydir=None
if foo := re.search("([^/]+)-([setup|test]).*.py", sys.argv[0]):
    testname = foo.groups()[0]
    replaydir = os.path.join(pandaregressiondir, "replays", testname)
    blesseddir = os.path.join(pandaregressiondir, "blessed", testname)
    tmpoutdir = os.path.join(pandaregressiondir, "tmpout", testname)
    miscdir = os.path.join(pandaregressiondir, "misc", testname)
    tmpoutfile = f'{os.path.join(tmpoutdir, testname)}.out'
    # dont put this in tmpoutdir since that gets cleared with each test run?
    tmpfulloutfile = os.path.join("/tmp", testname) + '-full.out'
    notsotmpfulloutfile = f'{os.path.join(tmpoutdir, testname)}-full.out'

    search_string_file_pfx = f"{miscdir}/{testname}"
    search_string_file = f"{search_string_file_pfx}_search_strings.txt"


def record_debian(cmds, replayname, arch):
    progress(f"Creating setup recording {replayname} [{cmds}]")
    # create the replay to use for reference / test
    arch_data = SUPPORTED_ARCHES[arch]
    qcow = f"{pandaregressiondir}/qcows/{arch_data.qcow}"
    cmd = f"{pandascriptsdir}/run_debian.py {cmds} --qcow={qcow}"
    # this is where we want the replays to end up
    replaysdir = f"{pandaregressiondir}/replays/{testname}"
    if not (os.path.exists(replaysdir) and os.path.isdir(replaysdir)):
        os.makedirs(replaysdir)
    cmd += f" --replaybase={replaysdir}/{replayname}"
    progress(cmd)
    sp.check_call(cmd.split())

        
# run this replay with these args (plugins)
# and determine if success     
def run_test_debian(replay_args, replayname, arch, rdir = replaydir, clear_tmpout=True):
    progress(f"Running test {testname}")
    arch_data = SUPPORTED_ARCHES[arch]
    qemu = os.path.join(panda_build_dir, arch_data.dir, arch_data.binary)
    cmd = f"{qemu} -replay {rdir}/{replayname} {replay_args}"
    progress(cmd)
    result = "Success"
    try:
        if clear_tmpout:
            clear_dir(tmpoutdir)
        os.chdir(tmpoutdir)
        output = sp.check_output(cmd.split())
        # full output of replay goes here
        with open (tmpfulloutfile, "ab") as out:
            out.write(output)
        msg = f"Test {testname} succeeded"
        progress(msg)
        with open (tmpoutfile, "a") as out:
            out.write(msg)
    except Exception as e:
        msg = f"Test {testname} failed to run "
        progress(msg)
        with open(tmpoutfile, "w") as out:
            out.write(msg)
        raise e


def create_search_string_file(search_string):
    with open(search_string_file, "w") as ssf:
        ssf.write(search_string + "\n")
