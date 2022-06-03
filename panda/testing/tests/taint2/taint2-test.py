#!/usr/bin/python

import os
import sys
import subprocess as sp
import tempfile

thisdir = os.path.dirname(os.path.realpath(__file__))
td = os.path.realpath(f"{thisdir}/../..")
sys.path.append(td)

from ptest_utils import *

sys.path.append(pandascriptsdir)

from plog_reader import plogiter

# run taint_instr and tainted_branch on file_branch_taint program
testdir = f"{testingscriptsdir}/tests/taint2"
sp.check_call(
    [
        f"{pandascriptsdir}/taint_debian.py",
        f"{testdir}/file_branch_taint",
        f"{testdir}/taint2.input",
    ]
)


ulsm = {}

with open("%s/taint2.out" % tmpoutdir, "w") as out:

    def print_taint_query(label,pc,tqelist):
        just_uls = pc < 0x8000000 or pc >= 0x9000000
        if (not just_uls):
            out.write(f"{label} {hex(pc)} ")
        for tqe in tqelist:
            if (not just_uls):
                out.write("( off=%d tcn=%d" % (tqe.offset, tqe.tcn))
            if tqe.HasField("unique_label_set"):
                uls = tqe.unique_label_set
                ulsm[uls.ptr] = uls.label
            if (not just_uls):
                ptr = tqe.ptr
                for l in ulsm[ptr]:
                    out.write(" %d" % l)
                out.write(")\n")


    for entry in plogiter("taint.plog"):
        if entry.HasField("tainted_instr"):
            print_taint_query("tainted_instr", entry.pc, entry.tainted_instr.taint_query)
        if entry.HasField("tainted_branch"):
            print_taint_query("tainted_branch", entry.pc, entry.tainted_branch.taint_query)

os.chdir(tmpoutdir)
shutil.move("taint2.out", tmpoutfile)
