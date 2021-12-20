/*
 * QEMU MIPS interrupt support
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/mips/cpudevs.h"
#include "cpu.h"
#include "sysemu/kvm.h"
#include "kvm_mips.h"

#ifdef CONFIG_SOFTMMU
#include "panda/rr/rr_log.h"
extern bool in_timer_expire;
#endif

static void cpu_mips_irq_request_internal(void *opaque, int irq, int level);

static void cpu_mips_irq_request_internal(void *opaque, int irq, int level)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;
    CPUState *cs = CPU(cpu);

    if (irq < 0 || irq > 7)
        return;

    if (level) {
        env->CP0_Cause |= 1 << (irq + CP0Ca_IP);

        if (kvm_enabled() && irq == 2) {
            kvm_mips_set_interrupt(cpu, irq, level);
        }

    } else {
        env->CP0_Cause &= ~(1 << (irq + CP0Ca_IP));

        if (kvm_enabled() && irq == 2) {
            kvm_mips_set_interrupt(cpu, irq, level);
        }
    }

    if (rr_in_record() && !in_timer_expire){
        rr_store_cause_record(env->CP0_Cause);
    }
    if (rr_in_replay() && !in_timer_expire){
        rr_skipped_callsite_location = RR_CALLSITE_CPU_PENDING_INTERRUPTS_BEFORE;
        rr_replay_skipped_calls();
    }


    if (env->CP0_Cause & CP0Ca_IP_mask) {
        if (rr_in_record() && !in_timer_expire){
            rr_store_cpu_interrupt_hard();
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
        }
        if (rr_in_replay()){
            if (!in_timer_expire){
                rr_skipped_callsite_location = RR_CALLSITE_CPU_PENDING_INTERRUPTS_AFTER;
                rr_replay_skipped_calls();
            }else{
                cpu_interrupt(cs, CPU_INTERRUPT_HARD);
            }
        }
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void cpu_mips_irq_request(void *opaque, int irq, int level){
    //MIPSCPU *cpu = opaque;
    //CPUMIPSState *env = &cpu->env;
//    RR_DO_RECORD_OR_REPLAY(
    /*action=*/ cpu_mips_irq_request_internal(opaque,irq,level);
 //   /*record=*/rr_input_4((uint32_t*)&env->CP0_Cause),
 //   /*replay=*/rr_input_4((uint32_t*)&env->CP0_Cause),
 //   /*location=*/RR_CALLSITE_CPU_HANDLE_INTERRUPT_BEFORE);
    //if (rr_in_record())
    //    rr_mips_cause_record(env->CP0_Cause);
    

}

void cpu_mips_irq_init_cpu(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;
    qemu_irq *qi;
    int i;

    qi = qemu_allocate_irqs(cpu_mips_irq_request, mips_env_get_cpu(env), 8);
    for (i = 0; i < 8; i++) {
        env->irq[i] = qi[i];
    }
}

void cpu_mips_soft_irq(CPUMIPSState *env, int irq, int level)
{
    if (irq < 0 || irq > 2) {
        return;
    }

    qemu_set_irq(env->irq[irq], level);
}
