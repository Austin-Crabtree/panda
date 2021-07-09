/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "panda/panda_api.h"
#include "syscalls2/syscalls_ext_typedefs.h"
#include "syscalls2/syscalls2_ext.h"
#include "osi/osi_types.h"
#include "osi/osi_ext.h"
#include "capstone/capstone.h"

#include <sys/time.h>
#include <set>
#include <vector>
#include <map>
#include <strings.h>

/*
phase1: get the current task struct, pid, and tgid
    - get the current GS offset (per_cpu_offset_0_addr)
    - start a getpid syscall
        - look at all loads from GS, store all the offsets in a vector
        - at each call instruction:
            - option 1 (works on x64) :
                - check if readmem(GS+offset) == RDI
                    - if true, then add offset to a vector of likely candidates
                - if number of candidates == 1, then set current_task_addr = offset
                - else fail
            - option 2 (new experiment) :
                - store the value of RDI in a list
                - on return from getpid, check each of these values by searching for the retval
                - search all of memory for the value of current, and store the locations where it exists
                - start a second getpid syscall and see which of those locations get read from
    - on return from getpid:
        - get the retval
        - iterate over current with step of 4 bytes
        - check if readmem(base+step) == retval
            - if true, store step in a list
        - check if list has length 2
            - if true, set task.pid and task.tgid
        - TODO: verify that task.pid and task.tgid are not swapped

phase 2: parent search
    - get the ppid by replacing another syscall
    - step through current with step size of 4
    - if the value is not null:
        - read memory at (value + pid offset)
        - if the value there equals the ppid retrieved earlier
            - you've found the pointer to the parent, success
    - if no values are found:
        - fail
phase 3: find the pointer to cred
    - use the setuid syscall to set a new non-zero uid
    - every cred struct has the same layout, so it's easy
    - iterate through every field in the task_struct
        - if you reach a valid pointer, add 4 and dereference
        - if it matches the uid that you set, then you win
phase 3a: find the task list
    - it is a circular linked list, so check by dereferencing pointers,
        then dereferencing the value of the last dereference, etc
    - check that the number of items in the linked list is relatively large, to avoid self-references, or the siblings/children list

phase 4: find comm
    - search through every task for the string "swapper/"

phase n-1: find the group leader
    
phase n: generate kernelconf file and load OSI
*/


//todo: add some basic primitives (read ulong, get_all_procs_base, read reg, set reg, etc)
//  - clean up a little
//  - reorder some steps (get the list of all procs early, 
//  - redo the pid and tgid search


// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
void time_of_day_enter(CPUState* cpu, target_ulong pc, uint64_t tv, uint64_t tz);
void time_of_day_return(CPUState* cpu, target_ulong pc, uint64_t tv, uint64_t tz);
bool translate_callback(CPUState* cpu, target_ulong pc);
int insn_exec_callback(CPUState* cpu, target_ulong pc);
void virt_mem_before_read_callback(CPUState *cpu, target_ptr_t pc, target_ptr_t addr, size_t size);
void before_block_exec(CPUState *cpu, TranslationBlock *tb);

void getpid_enter(CPUState *cpu, target_ulong pc);
void getuid_return(CPUState *cpu, target_ulong pc);
bool check_if_string(CPUState* cpu, target_ptr_t addr);
void current_task_addr_search(CPUState* cpu);
void pid_tgid_search(CPUState* cpu, uint64_t pid);
void parent_search(CPUState* cpu, uint64_t ppid);
void comm_search(CPUState* cpu);
void uid_search(CPUState* cpu, uint64_t new_uid);
void tasks_search(CPUState* cpu);
void group_leader_search(CPUState* cpu);
void thread_group_search(CPUState* cpu);
void pgd_search(CPUState* cpu);

void print_results(void *);
void print_regstate(CPUState* cpu);
target_ulong read_target_ulong(CPUState* cpu, target_ulong addr, target_ulong res);
uint64_t read_uint64(CPUState* cpu, target_ulong addr, target_ulong res);
uint32_t read_uint32(CPUState* cpu, target_ulong addr, target_ulong res);
int read_register(CPUState* cpu, char* name, target_ulong* res);
void write_register(CPUState* cpu, target_ulong val);
void get_all_proc_base_addrs(CPUState* cpu, uint64_t list_offset, std::vector<target_ulong>&);
//void stack_search(CPUState* cpu);
//void execve_enter(CPUState* cpu, target_ulong pc, uint64_t filename, uint64_t argv, uint64_t envp);
}

const char* regnames[] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};

bool phase1 = true;
bool phase2 = false;
bool phase3 = false;
bool phase4 = false;
bool in_syscall = false;
bool check_mem_reads = true;
int call_count = 0;

FILE* fptr = NULL;
uint64_t current = 0;
uint64_t tmp = 0;
std::set<uint64_t> current_candidates;

//uint64_t task_per_cpu_offsets_addr = 0;   - TODO
uint64_t task_per_cpu_offset_0_addr = 0;
uint64_t task_current_task_addr = 0;
//uint64_t task_init_addr = 0;              - TODO

//uint64_t task_size = 0                    - TODO
uint64_t task_tasks_offset = 0;
uint64_t task_pid_offset = 0;
uint64_t task_tgid_offset = 0;
//uint64_t task_group_leader_offset = 0;    - TODO
uint64_t task_thread_group_offset = 0;
uint64_t task_real_parent_offset = 0;
uint64_t task_parent_offset = 0;
uint64_t task_mm_offset = 0;
uint64_t task_stack_offset = 0;             //non-essential
uint64_t task_real_cred_offset = 0;
uint64_t task_cred_offset = 0;
uint64_t task_comm_offset = 0;
uint64_t task_comm_size = 16;
//uint64_t task_files_offset = 0;           - TODO

uint64_t cred_uid_offset = 4;
uint64_t cred_gid_offset = 8;
uint64_t cred_euid_offset = 20;
uint64_t cred_egid_offset = 24;

//uint64_t mm_size = 0;                     - TODO
//uint64_t mm_mmap_offset = 0;              - TODO
uint64_t mm_pgd_offset = 0;
//uint64_t mm_arg_start_offset = 0;         - TODO
//uint64_t mm_start_brk_offset = 0;         - TODO
//uint64_t mm_brk_offset = 0;               - TODO
//uint64_t mm_start_stack_offset = 0;       - TODO

//uint64_t vma_size = 0;                    - TODO
//uint64_t vma_vm_mm_offset = 0;            - TODO
//uint64_t vma_vm_start_offset = 0;         - TODO
//uint64_t vma_vm_end_offset = 0;           - TODO
//uint64_t vma_vm_next_offset = 0;          - TODO
//uint64_t vma_vm_flags_offset = 0;         - TODO
//uint64_t vma_vm_file_offset = 0;          - TODO

//uint64_t fs_f_path_dentry_offset = 0;     - TODO
//uint64_t fs_f_path_mnt_offset = 0;        - TODO
//uint64_t fs_f_pos_offset = 0;             - TODO
//uint64_t fs_fdt_offset = 0;               - TODO
//uint64_t fs_fdtab_offset = 0;             - TODO
//uint64_t fs_fd_offset = 0;                - TODO

//uint64_t qstr_size = 0;                   - TODO
//uint64_t qstr_name_offset = 0;            - TODO

//uint64_t path_d_name_offset = 0;          - TODO
//uint64_t path_d_iname_offset = 0;         - TODO
//uint64_t path_d_parent_offset = 0;        - TODO
//uint64_t path_d_op_offset = 0;            - TODO
//uint64_t path_d_dname_offset = 0;         - TODO
//uint64_t path_mnt_root_offset = 0;        - TODO
//uint64_t path_mnt_parent_offset = 0;      - TODO
//uint64_t path_mnt_mountpoint_offset = 0;  - TODO


//UNFINISHED
int read_register(CPUState* cpu, char* name, target_ulong* res) {
    const char* regnames[] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};

    //int num = 0;

    for(int i = 0; i < 16; i++) {
        if(strcasecmp(name, regnames[i]) == 0) {
            *res = ((CPUX86State*) cpu->env_ptr)->regs[i];
        }
    }

    return 0;
}

//UNTESTED
int read_target_ulong(CPUState* cpu, target_ulong addr, target_ulong* res) {
    uint8_t data[8] = {0};
    target_ulong tmp = 0;
    
    tmp = panda_virtual_memory_read(cpu, addr, data, 8);
    if(tmp == -1) {
        printf("Couldn't read memory at 0x" TARGET_PTR_FMT "\n", addr);
        return -1;
    }
    
    *res = *((target_ulong*) &data[0]);
    return 0;
}

//UNTESTED
int read_uint64(CPUState* cpu, target_ulong addr, target_ulong* res) {
    uint8_t data[8] = {0};
    target_ulong tmp = 0;
    
    tmp = panda_virtual_memory_read(cpu, addr, data, 8);
    if(tmp == -1) {
        printf("Couldn't read memory at 0x" TARGET_PTR_FMT "\n", addr);
        return -1;
    }
    
    *res = *((uint64_t*) &data[0]);
    return 0;
}

//UNTESTED
int read_uint32(CPUState* cpu, target_ulong addr, target_ulong* res) {
    uint8_t data[8] = {0};
    target_ulong tmp = 0;
    
    tmp = panda_virtual_memory_read(cpu, addr, data, 8);
    if(tmp == -1) {
        printf("Couldn't read memory at 0x" TARGET_PTR_FMT "\n", addr);
        return -1;
    }
    
    *res = *((uint32_t*) &data[0]);
    return 0;
}

//made this for demo, delete later
void print_results() {
    printf("\n\n***** RESULTS *****\n");

    printf("task_per_cpu_offset_0_addr %lu\n", task_per_cpu_offset_0_addr);
    printf("task_current_task_addr %lu\n", task_current_task_addr);
    printf("current %lu\n", current);

    printf("\ntask_tasks_offset %lu\n", task_tasks_offset);
    printf("task_pid_offset %lu\n", task_pid_offset);
    printf("task_tgid_offset %lu\n", task_tgid_offset);
    printf("task_thread_group_offset %lu\n", task_thread_group_offset);
    printf("task_real_parent_offset %lu\n", task_real_parent_offset);
    printf("task_parent_offset %lu\n", task_parent_offset);
    printf("task_mm_offset %lu\n", task_mm_offset);
    printf("task_stack_offset %lu\n", task_stack_offset);
    printf("task_real_cred_offset %lu\n", task_real_cred_offset);
    printf("task_cred_offset %lu\n", task_cred_offset);
    printf("task_comm_offset %lu\n", task_comm_offset);
    printf("task_comm_size %lu\n", task_comm_size);

    printf("\ncred_uid_offset %lu\n", cred_uid_offset);
    printf("cred_gid_offset %lu\n", cred_gid_offset);
    printf("cred_euid_offset %lu\n", cred_euid_offset);
    printf("cred_egid_offset %lu\n", cred_egid_offset);

    printf("\nmm_pgd_offset %lu\n", mm_pgd_offset);
}

void print_regstate(CPUState* cpu) {
    for (int i = 0; i < 16; i++) {
        if (i == 1) {
            printf("%s: 0x%lx\n", regnames[3], ((CPUX86State*) cpu->env_ptr)->regs[3]);
        } else if (i == 2) {
            printf("%s: 0x%lx\n", regnames[1], ((CPUX86State*) cpu->env_ptr)->regs[1]);
        } else if (i == 3) {
            printf("%s: 0x%lx\n", regnames[2], ((CPUX86State*) cpu->env_ptr)->regs[2]);
        } else {
            printf("%s: 0x%lx\n", regnames[i], ((CPUX86State*) cpu->env_ptr)->regs[i]);
        }
    }
}

bool check_if_string(CPUState* cpu, target_ptr_t addr) {
    int num_letters = 0;
    uint8_t data[16] = {0};
    int ret = panda_virtual_memory_read(cpu, addr, data, 16);
    if (ret == -1) {
        printf("couldn't read memory at 0x%lx\n", addr);
        return false;
    }

    if (strlen( (const char*) data) < 3) {
        return false;
    }
    for(int i = 0; i < strlen( (const char*) data); i++) {
        if (data[i] > 127) {
            return false;
        }
        if ((data[i] >= 48 && data[i] <= 57) || (data[i] >= 65 && data[i] <= 90) || (data[i] >= 97 && data[i] <= 122)) {
            num_letters++;
        }
    }

    if(num_letters < 3) {
        return false;
    }
        
    return true;
}

void comm_search(CPUState* cpu) {
    printf("Searching for comm offset\n");
//    int offset = 0;
//    while (offset < 10000) {
//        if (check_if_string(cpu, current + offset)) {
//            uint8_t data[16] = {0};
//            panda_virtual_memory_read(cpu, current + offset, data, 16);
//            printf("String found at offset %d", offset);
//            printf(" --> %s\n", data);
//            offset += (strlen((const char*)data)/4)*4;
//        }
//            
//        offset += 4;
//    }

    //refresh current
    uint64_t new_current;
    uint8_t data[8] = {0};
    uint8_t comm[16] = {0};
    int offset = 0;
    int ret = 0;
    bool found = false;

    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    //get address of tasks list
    uint64_t tasks_addr = new_current + task_tasks_offset;
    uint64_t next_addr = tasks_addr;
    uint64_t task_base = 0;
    printf("tasks list addr is 0x%lx\n", tasks_addr);

    //for each task in the list, strncmp "swapper/"
    do {
        task_base = next_addr - task_tasks_offset;
        printf("task_base is: 0x%lx\n", task_base);
        offset = 0;

        while(offset < 10000) {
            ret = panda_virtual_memory_read(cpu, task_base + offset, comm, 16);
            if (ret == -1) {
                printf("couldn't read memory at 0x%lx\n", current + offset);
                break;
            }

            if(strncmp("swapper", (char*)comm, 7) == 0) {
                printf("FOUND!\n");
                printf("offset: %d - comm: %s\n", offset, (char*) comm);
                task_comm_offset = offset;
                found = true;
                break;
            }

            offset += 4;
        }
        panda_virtual_memory_read(cpu, next_addr, data, 8);
        next_addr = *((uint64_t*) &data[0]);
        printf("next_addr is 0x%lx\n", next_addr);
        if (found) {
            break;
        }
    } while (next_addr != tasks_addr);

    printf("finished the loop!\n");
    if(found) {
        printf("task_comm_offset: %lu\n", task_comm_offset);
        printf("task_comm_size: %lu\n", task_comm_size);
    }
    //break when found
}

void pid_tgid_search(CPUState* cpu, uint64_t pid) {
    printf("pid searching\n");

    printf("current: 0x%lx,  pid: %lu\n", current, pid);
    int offset = 0;
    std::vector<uint32_t> solutions;

    while(offset < 10000) {
        uint8_t data[8] = {0};
        int ret = panda_virtual_memory_read(cpu, current + offset, data, 8);
        if (ret == -1) {
            printf("couldn't read memory at 0x%lx\n", current + offset);
            break;
        }

        //printf("0x%lx -> 0x%lx\n", current+offset, *((uint64_t*) &data[0]));

        if(*((uint32_t*) &data[0]) == pid) {
            //printf("found match at offset %d\n", offset);
            solutions.push_back(offset);
        }

        offset += 4;
    }

    printf("Found %lu solutions\n", solutions.size());
    if (solutions.size() == 2) {
        task_pid_offset = solutions[0];
        printf("task_pid_offset: %u\n", solutions[0]);
        task_tgid_offset = solutions[1];
        printf("task_tgid_offset: %u\n", solutions[1]);
    } else if (solutions.size() < 2) {
        printf("couldn't find enough solutions\n");
    } else {
        printf("found too many solutions\n");
    }
}

void parent_search(CPUState* cpu, uint64_t ppid) {
    printf("parent searching... \n");

    //reload current
    uint64_t new_current;
    uint8_t data[8] = {0};
    int offset = 0;

    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    std::vector<uint64_t> solutions;

    while(offset < 10000) {
        int ret = panda_virtual_memory_read(cpu, new_current + offset, data, 8);
        if (ret == -1) {
            printf("couldn't read memory at 0x%lx\n", current + offset);
            break;
        }

        uint64_t parent_candidate = *((uint64_t*) &data[0]);

        //printf("read value %u at offset %d\n", *((uint32_t*) &data[0]), offset);

        if(parent_candidate != 0) {
            ret = panda_virtual_memory_read(cpu, parent_candidate + task_pid_offset, data, 8);
            if (ret == -1) {
                //printf("couldn't read memory at parent candidate\n");
            }

            if(*((uint32_t*) &data[0]) == ppid) {
                printf("found match!\n");
                solutions.push_back(offset);
            }
        }

        offset += 4;
    }

    printf("found %lu solutions\n", solutions.size());
    if(solutions.size() == 2) {
        //THIS MIGHT BE BROKEN FIX THIS LATER
        task_real_parent_offset = solutions[0];
        task_parent_offset = solutions[1];
        printf("task_real_parent offset: %lu\n", task_real_parent_offset);
        printf("task_parent_offset: %lu\n", task_parent_offset);
    } else if (solutions.size() > 2) {
        printf("found too many solutions!\n");
    } else {
        printf("found no solutions\n");
    }

}

void current_task_addr_search(CPUState* cpu) {
    int ret = 0;
    uint8_t data[8] = {0};
    std::vector<uint64_t> solutions;

    for(std::set<uint64_t>::iterator it = current_candidates.begin(); it != current_candidates.end(); it++) {
        ret = panda_virtual_memory_read(cpu, *it, data, 8);
        if (ret == -1) {
            printf("failed to read from address " TARGET_PTR_FMT "\n", *it);
            break;
        }

        if(*((uint64_t*) &data[0]) == ((CPUX86State*) cpu->env_ptr)->regs[7]) {
            solutions.push_back(*it);
        }
    }

    printf("found %lu solutions\n", solutions.size());

    if(solutions.size() == 1) {
        task_current_task_addr = solutions[0] - task_per_cpu_offset_0_addr;
        printf("current_task_addr is 0x%lx\n", task_current_task_addr);
    } else if (solutions.size() == 0) {
        printf("found no solutions\n");
    } else {
        printf("found too many solutions\n");
    }
}

void uid_search(CPUState* cpu, uint64_t new_uid) {
    printf("searching for new uid...\n");

    //reload current
    uint64_t new_current;
    uint8_t data[8] = {0};
    int offset = 0;

    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    std::vector<uint64_t> solutions;
    while(offset < 10000) {
        int ret = panda_virtual_memory_read(cpu, new_current + offset, data, 8);
        if (ret == -1) {
            printf("couldn't read memory at 0x%lx\n", new_current + offset);
            break;
        }

        uint64_t ptr = *((uint64_t*) &data[0]);

        ret = panda_virtual_memory_read(cpu, ptr+4, data, 8);

        uint64_t tmp = *((uint64_t*) &data[0]);

        if(tmp == new_uid) {
            printf("match found at %d\n", offset);
            solutions.push_back(offset);
        }

        offset += 4;
    }

    printf("found %lu solutions\n", solutions.size());
    if(solutions.size() == 2) {
        //THIS MIGHT BE BROKEN FIX THIS LATER
        task_real_cred_offset = solutions[0];
        task_cred_offset = solutions[1];
        printf("task_real_cred_offset: %lu\n", task_real_cred_offset);
        printf("task_cred_offset: %lu\n", task_cred_offset);
    } else if (solutions.size() > 2) {
        printf("found too many solutions!\n");
    } else {
        printf("found no solutions\n");
    }
}

void tasks_search(CPUState* cpu) {
    printf("\nsearching for tasks...\n");

    //reload current
    uint64_t new_current;
    uint8_t data[8] = {0};
    int offset = 0;

    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    offset = 0;
    std::vector<std::pair<uint64_t, uint64_t> > solutions;

    while (offset < 10000) {
        int ret = panda_virtual_memory_read(cpu, new_current + offset, data, 8);
        if (ret == -1) {
            printf("couldn't read memory at 0x%lx\n", new_current + offset);
            //break;
        }

        uint64_t res = *((uint64_t*) &data[0]);

        //printf("trying offset %d\n", offset);

        int count = 0;
        while (res != new_current + offset) {
            ret = panda_virtual_memory_read(cpu, res, data, 8);
            if (ret == -1) {
                //printf("couldn't read memory at 0x%lx\n", res);
                break;
            }
            res = *((uint64_t*) &data[0]);
            //printf("res is 0x%lx\n", res);

            if (count > 5000) {
                //printf("infinite loop - breaking\n");
                break;
            }
            count++;
        }

        if ((res == new_current + offset) && (count > 10)) {
            solutions.push_back(std::make_pair(offset, count));
            printf("success! candidate is %d\n", offset);
        }

        offset += 4;
    }

    if (solutions.size() == 0) {
        printf("found no solutions. returning\n");
        return;
    } else {
        printf("found %lu solutions!\n", solutions.size());
    }

    uint64_t num = 0;
    for(int i = 0; i < solutions.size(); i++) {
        printf("solution %d:  %lu : %lu\n", i, solutions[i].first, solutions[i].second);
        if (i == 0) {
            num = solutions[i].second;
        } else if (solutions[i].second != num) {
            printf("not all solutions valid\n");
            return;
        }
    }

    task_tasks_offset = solutions[0].first;
    printf("all solutions valid!\n");
    printf("task_tasks_offset is %lu\n", task_tasks_offset);
}

void group_leader_search(CPUState* cpu) {
    printf("\nsearching for group leader...\n");

    //reload current
    uint64_t new_current;
    uint8_t data[8] = {0};
    int offset = 2280;

    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    // manually verify group leader
    panda_virtual_memory_read(cpu, new_current + offset, data, 8);
    uint64_t group_leader = *((uint64_t*) &data[0]);
    printf("group leader is 0x%lx\n", group_leader);

    panda_virtual_memory_read(cpu, group_leader + task_cred_offset, data, 8);
    uint64_t group_leader_cred = *((uint64_t*) &data[0]);

    panda_virtual_memory_read(cpu, group_leader_cred + cred_gid_offset, data, 8);
    uint32_t gid = *((uint32_t*) &data[0]);

    printf("group leader gid: %u\n", gid);
    panda_virtual_memory_read(cpu, group_leader + task_pid_offset, data, 8);
    uint32_t pid = *((uint32_t*) &data[0]);
    printf("group leader pid: %u\n", pid);
    

}

void thread_group_search(CPUState* cpu) {
    //find circular linked list candidates
    //for each candidate, keep only if the tgids all match
    //check each process for these candidates

    //2392

    //reload current
    std::vector<int> candidates;
    uint64_t new_current;
    //uint64_t task_base = 0;
    uint8_t data[8] = {0};
    uint8_t comm[16] = {0};
    int offset = 0;
    int count = 0;
    int ret = 0;

    printf("searching for thread group\n");
    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    new_current = *((uint64_t*) &data[0]);

    printf("current is %lx\n", new_current);

    while (offset < 10000) {
        //uint64_t secret_known_offset = 2392;
        uint64_t main_thread_ptr = new_current + offset;
        uint64_t current_thread_ptr = main_thread_ptr;
        //uint32_t last_tgid = -1;
        count = 0;

        ret = panda_virtual_memory_read(cpu, main_thread_ptr, data, 8);
        if (ret == -1) {
            offset += 4;
            continue;
        }
        current_thread_ptr = *((uint64_t*) &data[0]);

        //printf("main_thread_ptr 0x%lx\n", main_thread_ptr);
        //printf("current_thread_ptr 0x%lx\n", current_thread_ptr);

        while(current_thread_ptr != main_thread_ptr) {
//            task_base = current_thread_ptr - offset;
//            //check tgid
//            ret = panda_virtual_memory_read(cpu, task_base + task_tgid_offset, data, 8);
//            if (ret == -1) {
//                printf("COULD NOT READ THE TGID!!!\n");
//                break;
//            }
//
//            if (last_tgid == -1) {
//                last_tgid = *((uint32_t*) &data[0]);
//            } else {
//                if(*((uint32_t*) &data[0]) != last_tgid) {
//                    printf("tgid is different\n");
//                    break;
//                }
//            }

            ret = panda_virtual_memory_read(cpu, current_thread_ptr, data, 8);
            if (ret == -1) {
                printf("couldn't read memory at 0x%lx\n", current_thread_ptr);
                break;
            }
            current_thread_ptr = *((uint64_t*) &data[0]);
            //printf("read another thread\n");

            count++;
            if (count > 1000) break;
        }

        if (current_thread_ptr == main_thread_ptr) {
            candidates.push_back(offset);
        }

        printf("finished looping!\n");
        offset += 4;
    }

    for (int i = 0; i < candidates.size(); i++) {
        printf("candidate: %d\n", candidates[i]);
    }
    //iterate over the list of tasks



    //create a map of candidate offsets -> vector<int>
    //for each process
        //for each candidate offset

            //count the number of tasks in the list at the candidate offset
            //if the number is >= 2:
                //if the TGIDs don't match
                    //delete the candidate offset from the map
            //if the offset hasn't been deleted:
                //add the number of elements in the list to the mapped vector

    uint64_t tasks_addr = new_current + task_tasks_offset;
    uint64_t next_addr = tasks_addr;
    uint64_t task_base = 0;
    printf("tasks list addr is 0x%lx\n", tasks_addr);

    std::map<int, std::vector<int> > lists;

    for (int i = 0; i < candidates.size(); i++) {
        lists.insert(std::pair<int, std::vector<int> >(candidates[i], std::vector<int>()));
    }
    std::map<int, std::vector<int> >::iterator it;

    std::set<int> finalists;

    //for each task in the list
    printf("iterating through task list!\n");
    do {
        task_base = next_addr - task_tasks_offset;
        //printf("task_base is: 0x%lx\n", task_base);

        ret = panda_virtual_memory_read(cpu, task_base + task_comm_offset, comm, 16);
        if (ret == -1) {
            printf("couldn't read comm string at 0x%lx\n", task_base + task_comm_offset);
            break;
        }

        printf("proc: %s\n", (char*) comm);

        //for each candidate offset
        for(it = lists.begin(); it != lists.end(); ++it) {
            //count the number of tasks in the list at the candidate offset
            count = 0;
            uint64_t task_base_inner = 0;
            uint64_t candidate_addr = task_base + it->first;
            uint64_t next_addr_inner = candidate_addr;
            int64_t last_tgid = -1;
            uint32_t tgid = 0;
            //printf("first loop: candidate_addr: 0x%lx, next_addr_inner: 0x%lx, task_base: 0x%lx\n", candidate_addr, next_addr_inner, task_base);

            do {

                ret = panda_virtual_memory_read(cpu, next_addr_inner, data, 8);
                if (ret == -1) {
                    //printf("BREAKING - couldn't read the next pointer\n");
                    break;
                }

                next_addr_inner = *((uint64_t*) &data[0]);
                count++;
                if (count > 500) {
                    //printf("infinite loop! breaking...\n");
                    break;
                }

            } while(next_addr_inner != candidate_addr);
            count -= 1;
            printf("finished inner candidate %d loop - number of tasks: %d\n", it->first, count);
            //lists[it->first].push_back(count);

            //if there are more than two tasks in the loop
            if(count >= 2 && count < 500) {
                candidate_addr = task_base + it->first;
                next_addr_inner = candidate_addr;
                count = 0;
                bool mismatch = false;
                //printf("second loop: candidate_addr: 0x%lx, next_addr_inner: 0x%lx, task_base: 0x%lx\n", candidate_addr, next_addr_inner, task_base);
                do {
                    task_base_inner = next_addr_inner - it->first;

                    ret = panda_virtual_memory_read(cpu, task_base_inner + task_comm_offset, comm, 16);
                    if (ret == -1) {
                        printf("couldn't read comm string at 0x%lx\n", task_base_inner + task_comm_offset);
                        break;
                    }
                    printf("inner proc: %s\n", (char*) comm);

                    //check tgid
                    ret = panda_virtual_memory_read(cpu, task_base_inner + task_tgid_offset, data, 4);
                    if (ret == -1) {
                        printf("COULDN'T READ THE TGID - candidate %d\n", it->first);
                        break;
                    }
                    tgid = *((uint32_t*) &data[0]);
                    printf("tgid is %u\n", tgid);
                    if(last_tgid == -1) {
                        last_tgid = (int64_t) tgid;
                    } else if(tgid != last_tgid) {
                        mismatch = true;
                    }

                    //get next addr
                    panda_virtual_memory_read(cpu, next_addr_inner, data, 8);
                    next_addr_inner = *((uint64_t*) &data[0]);
                    count++;
                    if (count > 500) {
                        //printf("infinite loop! breaking...\n");
                        break;
                    }
                } while(next_addr_inner != candidate_addr);

                if(!mismatch) {
                    finalists.insert(it->first);
                    printf("found a solution!\n");
                }
            }
        }

        //break;


        panda_virtual_memory_read(cpu, next_addr, data, 8);
        next_addr = *((uint64_t*) &data[0]);
        //printf("next_addr is 0x%lx\n", next_addr);
    } while (next_addr != tasks_addr); 

//    for(it = lists.begin(); it != lists.end(); ++it) {
//        printf("%d: [", it->first);
//        for(int i = 0; i < it->second.size(); i++) {
//            printf("%d, ", it->second[i]);
//        }
//        printf("]\n");
//    }

    printf("FINAL CANDIDATES\n");
    std::set<int>::iterator it2;
    for(it2 = finalists.begin(); it2 != finalists.end(); ++it2) {
        printf("%d\n", *it2);
    }

    if(finalists.size() == 1) {
        printf("found one valid solution!\n");
        task_thread_group_offset = *(finalists.begin());
        printf("task_thread_group_offset: %lu\n\n", task_thread_group_offset);
    }
    //printf("finished looping through tasks!\n");
}

void pgd_search(CPUState* cpu) {
    uint8_t data[8] = {0};
    uint64_t mm_addr = 0;
    uint64_t pgd = 0;
    uint64_t pgd_phys = 0;
    target_ulong asid = 0;
    int offset_mm = 0;
    int offset_pgd = 0;
    int ret = 0;
    bool match = false;

    fflush(stdout);
    printf("searching for mm_struct and page global directory offsets\n");
    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
    current = *((uint64_t*) &data[0]);
    printf("current is %lx\n", current);

    asid = panda_current_asid(cpu);
    printf("current asid (from panda) is: " TARGET_FMT_lx "\n", asid);

    while(offset_mm < 10000) {
        //printf("trying offset %d\n", offset_mm);
        ret = panda_virtual_memory_read(cpu, current + offset_mm, data, 8);
        if (ret == -1) {
            //printf("couldn't read offset at potential mm offset %d\n", offset_mm);
            offset_mm += 4;
            continue;
        }
        mm_addr = *((uint64_t*) &data[0]);
        offset_pgd = 0;

        while(offset_pgd < 3000) { //switch back to 3000
            ret = panda_virtual_memory_read(cpu, mm_addr + offset_pgd, data, 8);
            if(ret == -1) {
                //printf("couldn't read potential pgd offset %d\n", offset_pgd);
                offset_pgd += 4;
                continue;
            }
            pgd = *((uint64_t*) &data[0]);
            pgd_phys = panda_virt_to_phys(cpu, pgd);

            if(pgd_phys == asid) {
                match = true;
                break;
            }

            offset_pgd += 4;
        }

        if(match) {
            break;
        }
        
        offset_mm += 4;
    }

    if(match) {
        printf("matches found!\n");
        task_mm_offset = offset_mm;
        mm_pgd_offset = offset_pgd;

        printf("task_mm_offset: %lu\n", task_mm_offset);
        printf("mm_pgd_offset: %lu\n", mm_pgd_offset);
        return;
    }
}

//void stack_search(CPUState* cpu) {
//    uint8_t data[8] = {0};
//    uint64_t stack_ptr = 0;
//    uint64_t thread_info_pointer = 0;
//    uint64_t ksp = 0;
//    uint64_t sp = 0;
//    int ret = 0;
//
//    printf("\nsearching for stack offset\n");
//
//    panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
//    current = *((uint64_t*) &data[0]);
//    printf("current is %lx\n", current);
//
//    //initial test
//    int stack_offset = 24;
//
//    ret = panda_virtual_memory_read(cpu, current + stack_offset, data, 8);
//    if(ret == -1) {
//        printf("couldn't read stack\n");
//        return;
//    }
//    stack_ptr = *((uint64_t*) &data[0]);
//
//    printf("stack_ptr: 0x%lx\n", stack_ptr);
//    stack_ptr = panda_virt_to_phys(cpu, stack_ptr);
//    printf("stack_ptr (phys): 0x%lx\n", stack_ptr);
//
//    //printf("esp 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[4]);
//    ret = panda_virtual_memory_read(cpu, current + 0, data, 8);
//    if(ret == -1) {
//        printf("couldn't read thread info\n");
//        return;
//    }
//    thread_info_pointer = *((uint64_t*) &data[0]);
//
//    printf("thread_info_pointer: 0x%lx\n", thread_info_pointer);
//
//    ksp = panda_current_ksp(cpu);
//    printf("ksp: 0x%lx\n", ksp);
//    ksp = panda_virt_to_phys(cpu, ksp);
//    printf("ksp (phys): 0x%lx\n", ksp);
//
//    sp = panda_current_sp(cpu);
//    printf("sp: 0x%lx\n", sp);
//    sp = panda_virt_to_phys(cpu, sp);
//    printf("sp (phys): 0x%lx\n", sp);
//}

bool translate_callback(CPUState* cpu, target_ulong pc){
//    if (phase1 && in_syscall) {
    unsigned char byte[9];
    int res = panda_virtual_memory_read(cpu, pc, (uint8_t *) &byte, 9);
    if (res == -1) return false;  // really should not happen

    if (byte[0] == 0xe8) {
        //printf("translated a call!\n");
        return true;
    }
//    }
    return false;
}

int insn_exec_callback(CPUState *cpu, target_ulong pc) {
    if (phase1 && in_syscall) {
        printf("At a call instruction! - %d\n", call_count);

        csh handle;
        cs_insn *insn;
        size_t count;
        uint8_t* code = NULL; 
        int ret = 0;

        //printf("tb size: %x\n", tb->size);
        code = (uint8_t*) calloc(10, sizeof(uint8_t));
        ret = panda_virtual_memory_read(cpu, pc, code, 10);
        if (ret == -1) printf("failed to read memory at pc\n");

        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) printf("VERY BAD NOT GOOD\n");
        count = cs_disasm(handle, code, 10, pc, 1, &insn);
        if (count != 1) {
            printf("failed to disassemble 1 instruction\n");
        }

        printf("at instruction %s\t%s\n", insn[0].mnemonic, insn[0].op_str);
        //printf("RDI: %lx\n", ((CPUX86State*) cpu->env_ptr)->regs[7]);
        //print_regstate(cpu);

        //current_candidates.insert(((CPUX86State*) cpu->env_ptr)->regs[7]);
        if(call_count == 3) {
            printf("call count is 3!\n");
            printf("RDI is 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[7]);
            current = ((CPUX86State*) cpu->env_ptr)->regs[7];
            current_task_addr_search(cpu);
        }

//        if (strcmp(insn[0].op_str, "0xffffffff810af950") == 0) {
//            uint8_t data[16];
//            panda_virtual_memory_read(cpu, 0xffff88803be00000+2640, data, 16);
//            printf("proc name? %s - current: 0x%lx\n", data, current);
//        }

        call_count++;
    }
    return 0;
}

void virt_mem_before_read_callback(CPUState *cpu, target_ptr_t pc, target_ptr_t addr, size_t size) {
    if (phase1 && in_syscall && call_count < 3) {
        uint8_t data[8] = {0};

        panda_virtual_memory_read(cpu, addr, data, size);
        printf("reading %lu bytes from " TARGET_PTR_FMT " -> %lx\n", size, addr, *((uint64_t*)(&data[0])));

        current_candidates.insert(addr);
    }

    if (phase4 && in_syscall) {
        uint8_t data[64] = {0};

        panda_virtual_memory_read(cpu, addr, data, size);
        //if(addr - current < 10000) {
            //printf("reading %lu bytes from " TARGET_PTR_FMT " -> %lx\n", size, addr, *((uint64_t*)(&data[0])));
        //}
    }
}

void getpid_enter(CPUState *cpu, target_ulong pc) {
//    printf("entering getpid!\n");
////    phase1 = true;
//    in_syscall = true;
}

void getpid_return(CPUState *cpu, target_ulong pc) {
//    printf("exiting getpid\n");
//    phase1 = false;
//    in_syscall = false;

}

void time_of_day_enter(CPUState* cpu, target_ulong pc, uint64_t tv, uint64_t tz) {
//    OsiProc *current = get_current_process(cpu);
//
//    printf("entering timeofday - process %s with PID %d\n", current->name, current->pid);
//    printf("syscall number (RAX) is: %lu\n", ((CPUX86State*) cpu->env_ptr)->regs[0]);
//    printf("First arg      (RDI) is: 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[7]);
//    printf("Second arg     (RSI) is: 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[6]);
//
//    uint8_t timeval_data[16] = {0};
//    panda_virtual_memory_read(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], timeval_data, 16);
//
//    printf("seconds: %lu\n", *((uint64_t*) &timeval_data[0]));
//    printf("microseconds: %lu\n", *((uint64_t*) &timeval_data[8]));
//
//    printf("setting new syscall number\n");


    if (phase1) {
        printf("phase 1: entering timeofday\n");
        in_syscall = true;
        ((CPUX86State*) cpu->env_ptr)->regs[0] = 39; //getpid
    }

    if (phase2) {
        printf("phase 2: entering timeofday\n");
        in_syscall = true;
        ((CPUX86State*) cpu->env_ptr)->regs[0] = 110; //getppid
    }

    if (phase3) {
        printf("phase 3: entering timeofday\n");
        uint64_t new_uid = 12345;
        in_syscall = true;
        ((CPUX86State*) cpu->env_ptr)->regs[0] = 105; //setuid
        tmp = ((CPUX86State*) cpu->env_ptr)->regs[7]; //store the timeval struct pointer for the return
        ((CPUX86State*) cpu->env_ptr)->regs[7] = new_uid;  //supply our uid as an argument
    }

    if (phase4) {
        printf("\nphase4: entering timeofday\n");
        in_syscall = true;

        ((CPUX86State*) cpu->env_ptr)->regs[0] = 111; //getpgrp

        //load current
        uint64_t new_current;
        uint8_t data[8] = {0};
        panda_virtual_memory_read(cpu, task_per_cpu_offset_0_addr + task_current_task_addr, data, 8);
        new_current = *((uint64_t*) &data[0]);
        current = new_current;
        printf("current is %lx\n", new_current);

        //get current pid
        panda_virtual_memory_read(cpu, new_current + task_pid_offset, data, 8);
        uint32_t pid = *((uint32_t*) &data[0]);
        printf("pid is %u\n", pid);
        //save rdi
        //tmp = ((CPUX86State*) cpu->env_ptr)->regs[7];
        //set rdi to pid
        //((CPUX86State*) cpu->env_ptr)->regs[7] = pid;
    }
}

void time_of_day_return(CPUState* cpu, target_ulong pc, uint64_t tv, uint64_t tz) {
    if (phase1) {
        printf("phase 1: returning from timeofday\n\n");
        OsiProc *current = get_current_process(cpu);

    //    printf("returning from timeofday\n");
    //    printf("retval         (RAX) is: %lu\n", ((CPUX86State*) cpu->env_ptr)->regs[0]);
    //    printf("First arg      (RDI) is: 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[7]);
    //    printf("Second arg     (RSI) is: 0x%lx\n", ((CPUX86State*) cpu->env_ptr)->regs[6]);

        struct timeval t;
        gettimeofday(&t, NULL);
    //    printf("host seconds: %lu\n", t.tv_sec);
    //    printf("host microseconds: %lu\n", t.tv_usec);

        panda_virtual_memory_write(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], (uint8_t*) &t, 16);

    //    uint8_t timeval_data[16] = {0};
    //    panda_virtual_memory_read(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], timeval_data, 16);
    //
    //    printf("seconds: %lu\n", *((uint64_t*) &timeval_data[0]));
    //    printf("microseconds: %lu\n", *((uint64_t*) &timeval_data[8]));
    //    printf("\n");

        assert(((CPUX86State*) cpu->env_ptr)->regs[0] == current->pid);

        //printf("set size: %lu\n", current_candidates.size());

        //current_task_addr_search(cpu);

        pid_tgid_search(cpu, ((CPUX86State*) cpu->env_ptr)->regs[0]);

        //comm_search(cpu);

        ((CPUX86State*) cpu->env_ptr)->regs[0] = 0;

        in_syscall = false;
        phase1 = false;
        phase2 = true;
    } else if (phase2) {
        printf("phase 2: returning from timeofday\n\n");
        OsiProc *current = get_current_process(cpu);

        struct timeval t;
        gettimeofday(&t, NULL);

        panda_virtual_memory_write(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], (uint8_t*) &t, 16);

        assert(((CPUX86State*) cpu->env_ptr)->regs[0] == current->ppid);
        //printf("PPID (from osi): %d\n", current->ppid);
        printf("PPID (from rax): %lu\n", ((CPUX86State*) cpu->env_ptr)->regs[0]);

        //printf("Didn't fail! Yay!\n");

        //parent search
        parent_search(cpu, ((CPUX86State*)cpu->env_ptr)->regs[0]);

        ((CPUX86State*) cpu->env_ptr)->regs[0] = 0;

        in_syscall = false;
        phase2 = false;
        phase3 = true;
    } else if (phase3) {
        printf("phase 3: returning from timeofday\n\n");

        uint64_t retval = ((CPUX86State*) cpu->env_ptr)->regs[0];
        printf("setuid retval: %lu\n", retval);
        ((CPUX86State*) cpu->env_ptr)->regs[7] = tmp; //restore the timeval struct pointer
        ((CPUX86State*) cpu->env_ptr)->regs[0] = 0;   //return success

        struct timeval t;
        gettimeofday(&t, NULL);
        panda_virtual_memory_write(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], (uint8_t*) &t, 16); //write a valid result

        uid_search(cpu, 12345);
        tasks_search(cpu);
        //group_leader_search(cpu);

        phase3 = false;
        in_syscall = false;
        phase4 = true;

    } else if (phase4) {
        printf("\nphase 4: returning from timeofday\n");

        uint64_t retval = ((CPUX86State*) cpu->env_ptr)->regs[0];
        printf("pgid of current proc is %lu\n", retval);

        //((CPUX86State*) cpu->env_ptr)->regs[7] = tmp; //restore the timeval struct pointer
        ((CPUX86State*) cpu->env_ptr)->regs[0] = 0;   //return success

        struct timeval t;
        gettimeofday(&t, NULL);
        panda_virtual_memory_write(cpu, ((CPUX86State*) cpu->env_ptr)->regs[7], (uint8_t*) &t, 16); //write a valid result

        comm_search(cpu);
        thread_group_search(cpu);
        pgd_search(cpu);
        //stack_search(cpu);

        //print_results();

        in_syscall = false;
        phase4 = false;
    }
}


void before_block_exec(CPUState *cpu, TranslationBlock *tb) {
    //if(phase1 && in_syscall) {
    //    csh handle;
    //    cs_insn *insn;
    //    size_t count;
    //    uint8_t* code = NULL; 
    //    int ret = 0;

    //    //printf("tb size: %x\n", tb->size);
    //    code = (uint8_t*) calloc(tb->size, sizeof(uint8_t));
    //    ret = panda_virtual_memory_read(env, tb->pc, code, tb->size);
    //    if (ret == -1) {
    //        printf("couldn't read memory at the pc\n");
    //        return;
    //    }
    //    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) printf("VERY BAD NOT GOOD\n");
    //    count = cs_disasm(handle, code, tb->size, tb->pc, 0, &insn);

    //    fprintf(fptr, "tb size: %d - read %lu instructions\n", tb->size, count);
    //    for(size_t i = 0; i < count; i++) {
    //        fprintf(fptr, "pc: %016lx\t%s\t%s\n", insn[i].address, insn[i].mnemonic, insn[i].op_str);
    //    }


    //    free(code);
    //}

    uint64_t tmp = ((CPUX86State*) cpu->env_ptr)->kernelgsbase;
    if (tmp != 0 && tmp != task_per_cpu_offset_0_addr) {
        task_per_cpu_offset_0_addr = tmp;
        printf("updated task_per_cpu_offset_0_addr updated to 0x%lx\n", tmp);
    }
    return;
}



bool init_plugin(void *self) {
    panda_cb pcb = { .before_block_exec = before_block_exec };
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);

    panda_add_arg("syscalls2", "load-info=1");
    panda_require("syscalls2");
    assert(init_syscalls2_api());

    panda_require("osi");
    assert(init_osi_api());

    //PPP_REG_CB("syscalls2", on_all_sys_return2, sys_return);
    PPP_REG_CB("syscalls2", on_sys_gettimeofday_enter, time_of_day_enter);
    PPP_REG_CB("syscalls2", on_sys_gettimeofday_return, time_of_day_return);

    PPP_REG_CB("syscalls2", on_sys_getpid_enter, getpid_enter);
    PPP_REG_CB("syscalls2", on_sys_getpid_return, getpid_return);

    //PPP_REG_CB("syscalls2", on_sys_execve_enter, execve_enter);

    panda_enable_memcb();
    pcb.insn_translate = translate_callback;
    panda_register_callback(self, PANDA_CB_INSN_TRANSLATE, pcb);
    pcb.insn_exec = insn_exec_callback;
    panda_register_callback(self, PANDA_CB_INSN_EXEC, pcb);
    pcb.virt_mem_before_read = virt_mem_before_read_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_BEFORE_READ, pcb);

    //fptr = fopen("pid_replaced_instruction_log.txt", "w");
    fptr = fopen("null.txt", "w");

    return true;
}

void uninit_plugin(void *self) { 
    fclose(fptr);
}





/*
#define __STDC_FORMAT_MACROS

#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "panda/panda_api.h"
#include "syscalls2/syscalls_ext_typedefs.h"
#include "syscalls2/syscalls2_ext.h"
#include "osi/osi_types.h"
#include "osi/osi_ext.h"
#include "capstone/capstone.h"

bool init_plugin(void *);
void uninit_plugin(void *);
void before_block_exec(CPUState *env, TranslationBlock *tb);
void getpid_enter(CPUState *env, target_ulong pc);
void getpid_exit(CPUState *env, target_ulong pc);
//void getuid_enter(CPUState *env, target_ulong pc);
//void getuid_exit(CPUState *env, target_ulong pc);
bool translate_callback(CPUState* cpu, target_ulong pc);
int insn_exec_callback(CPUState *cpu, target_ulong pc);
bool is_val_in_array(uint32_t val);
void add_val_to_array(uint32_t val);
void find_comm(CPUState *cpu);
int start_taint(void);
int start_tsm(void);

bool print_instr = false;
bool counting = false;
bool checking = false;
int num_calls = 0;
int count = 0;
FILE* fptr;

uint32_t *offsets = NULL;
int idx = 0;

target_ulong gsbase = 0;

//task_struct vals
uint64_t current_task_addr = 0;
uint64_t task_comm_offset = 0;
uint64_t task_pid_offset = 0;
uint64_t task_tgid_offset = 0;

bool is_val_in_array(uint32_t val) {
    for(int i = 0; i < idx; i++) {
        if(offsets[i] == val) {
            return true;
        }
    }
    return false;
}

void add_val_to_array(uint32_t val) {
    if(!is_val_in_array(val)) {
        offsets[idx] = val;
        idx++;
    }
}


int start_taint(void) {
    char **taint_args;
    taint_args = (char**) malloc(3*sizeof(char*));
    for(int i = 0; i < 3; i++) taint_args[i] = (char*) malloc(64*sizeof(char));
    strcpy(taint_args[0], "no_tp=1");
    strcpy(taint_args[1], "max_taintset_compute_number=32");
    strcpy(taint_args[2], "max_taintset_card=8");

    char taint_name[16];
    strcpy(taint_name, "taint2");

    return panda_init_plugin(taint_name, taint_args, 3);
}

int start_tsm(void) {
    char **tsm_args;
    tsm_args = (char**) malloc(4*sizeof(char*));
    for(int i = 0; i < 4; i++) tsm_args[i] = (char*) malloc(64*sizeof(char));
    strcpy(tsm_args[0], "kernel=1");
    strcpy(tsm_args[1], "flows=3");
    strcpy(tsm_args[2], "compute=1");
    strcpy(tsm_args[3], "map_updates=0");

    char tsm_name[16];
    strcpy(tsm_name, "tsm");

    return panda_init_plugin(tsm_name, tsm_args, 4);
}

void getpid_enter(CPUState *env, target_ulong pc) {

    OsiProc *current = get_current_process(env);
    printf("ENTERING GETPID - proc: %s, pid: %d\n", current->name, current->pid);

    if(strcmp(current->name, "pid") == 0) {
        //read the actual pid from the task struct
        uint8_t str[16] = {0};
        uint64_t addr = 18446612683139579904ul + 89088;

        panda_virtual_memory_read(env, addr, str, 8);
        printf("ADDR IS %lx\n", addr);
        printf("STORED MEM IS %lx\n", *((uint64_t*) str));


        printf("Enabling taint2 and tsm...\n");

        //panda_enable_plugin(panda_get_plugin_by_name("taint2"));
        //panda_enable_plugin(panda_get_plugin_by_name("tsm"));

        //start_taint();
        //start_tsm();
        print_instr = true;
        checking = true;
    }

}

void find_comm(CPUState *cpu) {
    uint8_t data[16] = {0};
    int ret = panda_virtual_memory_read(cpu, gsbase + current_task_addr, data, 8);
    if(ret == -1) printf("couldn't read memory at task_struct\n");
    uint64_t current = *((uint64_t*) data);
    printf("found value %lx\n", current); 

    int offset = 0;
    int num_results = 0;
    int last_result = 0;
    memset(data, 0, 8);

    while(offset < 10000) {
        ret = panda_virtual_memory_read(cpu, current + offset, data, 16);
        if(ret == -1) printf("couldn't read memory at offset of task_struct\n");

        if(strncmp((const char*) data, "pid", 3) == 0) {
            printf("found comm in the task struct at offset %d\n", offset);
            num_results++;
            last_result = offset;
        }

        offset += 4;
    }

    if(num_results == 1) {
        printf("only 1 match! you win!\n");
        task_comm_offset = (uint64_t) last_result;
    } else {
        printf("wrong number of comm matches found\n");
    }

    offset = 0;
    num_results = 0;
    int last_result1 = 0;
    int last_result2 = 0;
    memset(data, 0, 8);

    while(offset < 10000) {
        ret = panda_virtual_memory_read(cpu, current + offset, data, 16);
        if(ret == -1) printf("couldn't read memory at offset of task_struct\n");

        uint32_t tmp = *((uint32_t*) data);
        if(tmp == 1698) {
            printf("found pid in the task struct at offset %d\n", offset);

            if(num_results == 0) last_result1 = offset;
            else last_result2 = offset;

            num_results++;
        }

        offset += 4;
    }

    if(num_results == 2) {
        printf("2 matches! you win!\n");
        task_pid_offset = last_result1;
        task_tgid_offset = last_result2;
    } else {
        printf("wrong number of pid/tgid matches found\n");
    }
}

void getpid_exit(CPUState *env, target_ulong pc) {
    OsiProc *current = get_current_process(env);
    printf("EXITING GETPID - proc: %s, pid: %d\n", current->name, current->pid);

    if(strcmp(current->name, "pid") == 0) {

        //panda_disable_plugin(panda_get_plugin_by_name("taint2"));
        //panda_disable_plugin(panda_get_plugin_by_name("tsm"));

        //panda_unload_plugin_by_name("tsm");
        printf("Disabling taint2 and tsm...\n");
        print_instr = false;
        checking = false;

        target_long pid = get_syscall_retval(env);
        printf("pid: %ld\n", pid);

        if(current_task_addr != 0) {
            printf("current_task_addr was found!\n");
            find_comm(env);
        }
    }
}

//void getuid_enter(CPUState *env, target_ulong pc) {
//    OsiProc *current = get_current_process(env);
//    printf("ENTERING GETUID - proc: %s, pid: %d\n", current->name, current->pid);
//
//    if (strcmp(current->name, "uid") == 0) {
//        start_taint();
//        start_tsm();
//
//        counting = true;
//        print_instr = true;
//    }
//}
//
//void getuid_exit(CPUState *env, target_ulong pc) {
//    OsiProc *current = get_current_process(env);
//    printf("EXITING GETUID - proc: %s, pid: %d\n", current->name, current-> pid);
//
//    if (strcmp(current->name, "uid") == 0) {
//        counting = false;
//        print_instr = false;
//
//        printf("counted %d basic blocks\n", count);
//
//        target_long uid = get_syscall_retval(env);
//        printf("uid is %lu\n", uid);
//
//        panda_unload_plugin_by_name("tsm");
//        printf("Disabling taint2 and tsm...\n");
//    }
//}

void before_block_exec(CPUState *env, TranslationBlock *tb) {
    if(print_instr) {
        csh handle;
        cs_insn *insn;
        size_t count;
        uint8_t* code = NULL; 
        int ret = 0;

        //printf("tb size: %x\n", tb->size);
        code = (uint8_t*) calloc(tb->size, sizeof(uint8_t));
        ret = panda_virtual_memory_read(env, tb->pc, code, tb->size);
        if (ret == -1) {
            printf("couldn't read memory at the pc\n");
            return;
        }
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) printf("VERY BAD NOT GOOD\n");
        count = cs_disasm(handle, code, tb->size, tb->pc, 0, &insn);

        fprintf(fptr, "tb size: %d - read %lu instructions\n", tb->size, count);
        for(size_t i = 0; i < count; i++) {
            fprintf(fptr, "pc: %016lx\t%s\t%s\n", insn[i].address, insn[i].mnemonic, insn[i].op_str);
        }


        free(code);
    }

    if(counting) {
        count += 1;
    }

    target_ulong gs = ((CPUX86State*) env->env_ptr)->kernelgsbase;
    if(gs != 0) {
        //printf("gs base changed to %lx\n", gs);
        gsbase = gs;
    }



}

bool translate_callback(CPUState* cpu, target_ulong pc){
    if(checking) {
        unsigned char byte[9];
        int res = panda_virtual_memory_read(cpu, pc, (uint8_t *) &byte, 9);
        if (res == -1) return false;  // really should not happen
        char tmp[5];
        memcpy(tmp, byte, 5); 

        if (byte[0] == 0xe8) {
            printf("translated a call!\n");
            return true;
        }

        if(byte[0] == 0x65 && (byte[1] == 0x48 || byte[1] == 0x4c) && byte[2] == 0x8b) {//if(strcmp(tmp, "\x65\x48\x8B\x3C\x25") == 0) {
            //printf("loaded from gs!\n");

            csh handle;
            cs_insn *insn;
            size_t count;
            uint8_t* code = NULL; 
            int ret = 0;

            //printf("tb size: %x\n", tb->size);
            code = (uint8_t*) calloc(9, sizeof(uint8_t));
            ret = panda_virtual_memory_read(cpu, pc, code, 9);
            if (ret == -1) {
                printf("couldn't read memory at the pc\n");
                return false;
            }
            if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) printf("VERY BAD NOT GOOD\n");
            count = cs_disasm(handle, code, 9, pc, 0, &insn);
            if(count == 0) return false;

            printf("loaded from gs!\t\tpc: %016lx\t%s\t%s\n", insn[0].address, insn[0].mnemonic, insn[0].op_str);

            free(code);


            //get the offset

            uint32_t offset = *((uint32_t*) &byte[5]);
            printf("offset is: %x\n", offset);

            add_val_to_array(offset);
        }
    }

    return false;
}

int insn_exec_callback(CPUState *cpu, target_ulong pc) {

    printf("call %d is being executed!\n", num_calls);

    if(num_calls == 3) {
        printf("THIRD CALL REACHED\n");
        for(int i = 0; i < 16; i++){
            printf("reg%d is: %lx\n", i, ((CPUX86State*) cpu->env_ptr)->regs[i]);
        }
        printf("GS base is: %lx", ((CPUX86State*) cpu->env_ptr)->kernelgsbase);
        printf("\n");

        printf("offset candidates:\n");
        for(int i = 0; i < idx; i++) {
            printf("%x\t", offsets[i]);
        }
        printf("\n");

        int matches = 0;
        uint32_t potential_res = 0;
        for(int i = 0; i < idx; i++) {
            uint8_t data[8] = {0};
            int ret = panda_virtual_memory_read(cpu, gsbase + offsets[i], data, 8);
            if(ret == -1) printf("couldn't read memory at candidate offset\n");
            printf("found value %lx\n", *((uint64_t*) data));
            for(int j = 0; j < 16; j++) {
                if(*((uint64_t*) data) == ((CPUX86State*) cpu->env_ptr)->regs[j]) {
                    printf("MATCH FOUND!  offset is %x\n", offsets[i]);
                    potential_res = offsets[i];
                    matches++;
                }
            }
        }

        if(matches == 0) printf("no matches found\n");
        if(matches > 1) printf("too many matches\n");
        if(matches == 1) {
            printf("exactly one match! you win!\n");
            current_task_addr = potential_res;
        }
        
    }

    num_calls += 1;

    return 0;

}


bool init_plugin(void *self) {
    panda_cb pcb = { .before_block_exec = before_block_exec };
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);

    pcb.insn_translate = translate_callback;
    panda_register_callback(self, PANDA_CB_INSN_TRANSLATE, pcb);
    pcb.insn_exec = insn_exec_callback;
    panda_register_callback(self, PANDA_CB_INSN_EXEC, pcb);

    PPP_REG_CB("syscalls2", on_sys_getpid_enter, getpid_enter);
    PPP_REG_CB("syscalls2", on_sys_getpid_return, getpid_exit);

    //panda_disable_callback(panda_get_plugin_by_name("passive"), PANDA_CB_INSN_TRANSLATE, pcb);
    //panda_disable_callback(panda_get_plugin_by_name("passive"), PANDA_CB_INSN_EXEC, pcb);

    //PPP_REG_CB("syscalls2", on_sys_getuid_enter, getuid_enter);
    //PPP_REG_CB("syscalls2", on_sys_getuid_return, getuid_exit);

    

    panda_require("osi");
    if(!init_osi_api()) return false;
    assert(init_syscalls2_api());

    //fptr = fopen("uid_instruction_log.txt", "w");
    fptr = fopen("null.txt", "w");

    offsets = (uint32_t*) calloc(64, sizeof(uint32_t));

//    char **taint_args;
//    taint_args = (char**) malloc(3*sizeof(char*));
//    taint_args[0] = (char*) malloc(64*sizeof(char));
//    taint_args[1] = (char*) malloc(64*sizeof(char));
//    taint_args[2] = (char*) malloc(64*sizeof(char));
//    strcpy(taint_args[0], "no_tp=1");
//    strcpy(taint_args[1], "max_taintset_compute_number=32");
//    strcpy(taint_args[2], "max_taintset_card=8");
//
//    char taint_name[16];
//    strcpy(taint_name, "taint2");
//
//    if(!panda_init_plugin(taint_name, taint_args, 3)) return false;

    //if(!start_taint()) return false;
    //if(!start_tsm()) return false;

//    panda_disable_plugin(panda_get_plugin_by_name("taint2"));
//    panda_disable_plugin(panda_get_plugin_by_name("tsm"));

    return true;
}

void uninit_plugin(void *self) { 
    fclose(fptr);
    free(offsets);

    printf("\n***** RESULTS *****\n");
    printf("%-25.25s0x%lx\n", "per_cpu_offset_0_addr:", gsbase);
    printf("%-25.25s0x%lx\n", "current_task_addr:", current_task_addr);
    printf("%-25.25s%lu\n", "task.comm_offset:", task_comm_offset);
    printf("%-25.25s%lu\n", "task.pid_offset:", task_pid_offset);
    printf("%-25.25s%lu\n", "task.tgid_offset:", task_tgid_offset);
    printf("\n\n");
}
*/
