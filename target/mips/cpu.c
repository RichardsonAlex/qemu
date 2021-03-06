/*
 * QEMU MIPS CPU
 *
 * Copyright (c) 2012 SUSE LINUX Products GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "internal.h"
#ifdef TARGET_CHERI
#include "cheri_utils.h"
#endif
#include "kvm_mips.h"
#include "qemu/module.h"
#include "sysemu/kvm.h"
#include "exec/gdbstub.h"
#include "exec/exec-all.h"
#include "qemu/error-report.h"


static void mips_cpu_set_pc(CPUState *cs, vaddr value)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;

    mips_update_pc(env, value & ~(target_ulong)1, /*can_be_unrepresentable=*/false);
    if (value & 1) {
        env->hflags |= MIPS_HFLAG_M16;
    } else {
        env->hflags &= ~(MIPS_HFLAG_M16);
    }
}

static void mips_cpu_synchronize_from_tb(CPUState *cs, TranslationBlock *tb)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;

    mips_update_pc(env, tb->pc, /*can_be_unrepresentable=*/false);
    env->hflags &= ~MIPS_HFLAG_BMASK;
    env->hflags |= tb->flags & MIPS_HFLAG_BMASK;
}

static bool mips_cpu_has_work(CPUState *cs)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;
    bool has_work = false;

    /*
     * Prior to MIPS Release 6 it is implementation dependent if non-enabled
     * interrupts wake-up the CPU, however most of the implementations only
     * check for interrupts that can be taken.
     */
    if ((cs->interrupt_request & CPU_INTERRUPT_HARD) &&
        cpu_mips_hw_interrupts_pending(env)) {
        if (cpu_mips_hw_interrupts_enabled(env) ||
            (env->insn_flags & ISA_MIPS32R6)) {
            has_work = true;
        }
    }

    /* MIPS-MT has the ability to halt the CPU.  */
    if (env->CP0_Config3 & (1 << CP0C3_MT)) {
        /*
         * The QEMU model will issue an _WAKE request whenever the CPUs
         * should be woken up.
         */
        if (cs->interrupt_request & CPU_INTERRUPT_WAKE) {
            has_work = true;
        }

        if (!mips_vpe_active(env)) {
            has_work = false;
        }
    }
    /* MIPS Release 6 has the ability to halt the CPU.  */
    if (env->CP0_Config5 & (1 << CP0C5_VP)) {
        if (cs->interrupt_request & CPU_INTERRUPT_WAKE) {
            has_work = true;
        }
        if (!mips_vp_active(env)) {
            has_work = false;
        }
    }
    return has_work;
}

static void mips_cpu_reset(DeviceState *dev)
{
    CPUState *s = CPU(dev);
    MIPSCPU *cpu = MIPS_CPU(s);
    MIPSCPUClass *mcc = MIPS_CPU_GET_CLASS(cpu);
    CPUMIPSState *env = &cpu->env;

    mcc->parent_reset(dev);

    memset(env, 0, offsetof(CPUMIPSState, end_reset_fields));
#ifdef CONFIG_DEBUG_TCG
    env->active_tc._pc_is_current = true;
#endif

    cpu_state_reset(env);

#ifndef CONFIG_USER_ONLY
    if (kvm_enabled()) {
        kvm_mips_reset_vcpu(cpu);
    }
#endif
}

static void mips_cpu_disas_set_info(CPUState *s, disassemble_info *info)
{
    MIPSCPU *cpu = MIPS_CPU(s);
    CPUMIPSState *env = &cpu->env;

    if (!(env->insn_flags & ISA_NANOMIPS32)) {
#ifdef TARGET_WORDS_BIGENDIAN
        info->print_insn = print_insn_big_mips;
#else
        info->print_insn = print_insn_little_mips;
#endif
    } else {
#if defined(CONFIG_NANOMIPS_DIS)
        info->print_insn = print_insn_nanomips;
#endif
    }
#ifdef TARGET_MIPS64
    // See disas/mips.c
#define bfd_mach_mipsisa64r2           65
    info->mach = bfd_mach_mipsisa64r2;
#endif

}

static void mips_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    MIPSCPU *cpu = MIPS_CPU(dev);
    MIPSCPUClass *mcc = MIPS_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

#ifdef TARGET_MIPS64
    gdb_register_coprocessor(cs, NULL, NULL, 0, "mips64-cp0.xml", 0);
    gdb_register_coprocessor(cs, NULL, NULL, 0, "mips64-fpu.xml", 0);
    gdb_register_coprocessor(cs, mips_gdb_get_sys_reg, mips_gdb_set_sys_reg,
        1, "mips64-sys.xml", 0);
#else
    gdb_register_coprocessor(cs, NULL, NULL, 0, "mips-cp0.xml", 0);
    gdb_register_coprocessor(cs, NULL, NULL, 0, "mips-fpu.xml", 0);
    gdb_register_coprocessor(cs, mips_gdb_get_sys_reg, mips_gdb_set_sys_reg,
        1, "mips-sys.xml", 0);
#endif
#if defined(TARGET_CHERI)
    gdb_register_coprocessor(cs, mips_gdb_get_cheri_reg,
        mips_gdb_set_cheri_reg, 44,
#if defined(CHERI_MAGIC128) || defined(CHERI_128)
        "mips64-cheri-c128.xml",
#else
        "mips64-cheri-c256.xml",
#endif
        0);
#endif

    cpu_mips_realize_env(&cpu->env);

    cpu_reset(cs);
    qemu_init_vcpu(cs);

    mcc->parent_realize(dev, errp);
}

static void mips_cpu_initfn(Object *obj)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    CPUMIPSState *env = &cpu->env;
    MIPSCPUClass *mcc = MIPS_CPU_GET_CLASS(obj);

    cpu_set_cpustate_pointers(cpu);
    env->cpu_model = mcc->cpu_def;
}

static char *mips_cpu_type_name(const char *cpu_model)
{
    return g_strdup_printf(MIPS_CPU_TYPE_NAME("%s"), cpu_model);
}

static ObjectClass *mips_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    typename = mips_cpu_type_name(cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);
    return oc;
}

#if defined(TARGET_CHERI)
static uint64_t start_ns = 0;
static void dump_cpu_ips_on_exit(void) {
    assert(start_ns != 0);
    CPUState *cpu;
    CPU_FOREACH(cpu) {
        CPUMIPSState *env = cpu->env_ptr;
        double duration_s = (get_clock() - start_ns) / 1000000000.0;
        uint64_t inst_total = env->statcounters_icount_kernel + env->statcounters_icount_user;
        info_report("CPU%d executed instructions: %jd (%jd user, %jd kernel) in %.2fs KIPS: %.2f\r\n", cpu->cpu_index,
                    (uintmax_t)inst_total, (uintmax_t)env->statcounters_icount_user,
                    (uintmax_t)env->statcounters_icount_kernel, duration_s,
                    (double)(inst_total / duration_s) / 1000.0);
    }
}
#if defined(DO_CHERI_STATISTICS)
static void dump_stats_on_exit(void)
{
    if (qemu_log_enabled() && qemu_loglevel_mask(CPU_LOG_INSTR | CPU_LOG_CHERI_BOUNDS)) {
        FILE* logf = qemu_log_lock();
        cheri_cpu_dump_statistics_f(NULL, logf, 0);
        qemu_log_unlock(logf);
    } else
        cheri_cpu_dump_statistics_f(NULL, stderr, 0);
}
#endif
#endif

static void mips_cpu_class_init(ObjectClass *c, void *data)
{
    MIPSCPUClass *mcc = MIPS_CPU_CLASS(c);
    CPUClass *cc = CPU_CLASS(c);
    DeviceClass *dc = DEVICE_CLASS(c);

    device_class_set_parent_realize(dc, mips_cpu_realizefn,
                                    &mcc->parent_realize);
    device_class_set_parent_reset(dc, mips_cpu_reset, &mcc->parent_reset);

    cc->class_by_name = mips_cpu_class_by_name;
    cc->has_work = mips_cpu_has_work;
    cc->do_interrupt = mips_cpu_do_interrupt;
    cc->cpu_exec_interrupt = mips_cpu_exec_interrupt;
    cc->dump_state = mips_cpu_dump_state;
    cc->set_pc = mips_cpu_set_pc;
    cc->synchronize_from_tb = mips_cpu_synchronize_from_tb;
    cc->gdb_read_register = mips_cpu_gdb_read_register;
    cc->gdb_write_register = mips_cpu_gdb_write_register;
#ifndef CONFIG_USER_ONLY
    cc->do_transaction_failed = mips_cpu_do_transaction_failed;
    cc->do_unaligned_access = mips_cpu_do_unaligned_access;
    cc->get_phys_page_debug = mips_cpu_get_phys_page_debug;
    cc->vmsd = &vmstate_mips_cpu;
#endif
    cc->disas_set_info = mips_cpu_disas_set_info;
#ifdef CONFIG_TCG
    cc->tcg_initialize = mips_tcg_init;
    cc->tlb_fill = mips_cpu_tlb_fill;
#endif

#if defined(TARGET_MIPS64)
    cc->gdb_core_xml_file = "mips64-cpu.xml";
#else
    cc->gdb_core_xml_file = "mips-cpu.xml";
#endif
    cc->gdb_num_core_regs = 72;
    cc->gdb_stop_before_watchpoint = true;
#if defined(TARGET_CHERI)
    cc->dump_statistics = cheri_cpu_dump_statistics;
    start_ns = get_clock();
    atexit(dump_cpu_ips_on_exit);
#if defined(DO_CHERI_STATISTICS)
    atexit(dump_stats_on_exit);
#endif
#endif

}

static const TypeInfo mips_cpu_type_info = {
    .name = TYPE_MIPS_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(MIPSCPU),
    .instance_init = mips_cpu_initfn,
    .abstract = true,
    .class_size = sizeof(MIPSCPUClass),
    .class_init = mips_cpu_class_init,
};

static void mips_cpu_cpudef_class_init(ObjectClass *oc, void *data)
{
    MIPSCPUClass *mcc = MIPS_CPU_CLASS(oc);
    mcc->cpu_def = data;
}

static void mips_register_cpudef_type(const struct mips_def_t *def)
{
    char *typename = mips_cpu_type_name(def->name);
    TypeInfo ti = {
        .name = typename,
        .parent = TYPE_MIPS_CPU,
        .class_init = mips_cpu_cpudef_class_init,
        .class_data = (void *)def,
    };

    type_register(&ti);
    g_free(typename);
}

static void mips_cpu_register_types(void)
{
    int i;

    type_register_static(&mips_cpu_type_info);
    for (i = 0; i < mips_defs_number; i++) {
        mips_register_cpudef_type(&mips_defs[i]);
    }
}

#ifdef TARGET_CHERI
static inline void set_epc_or_error_epc(CPUMIPSState *env, cap_register_t* epc_or_error_epc, target_ulong new_cursor)
{

    // Setting EPC should clear EPCC.tag if EPCC is sealed or becomes unrepresentable.
    // This will cause exception on instruction fetch following subsequent eret
    if (!cap_is_unsealed(epc_or_error_epc)) {
        error_report("Attempting to modify sealed EPCC/ErrorEPCC: " PRINT_CAP_FMTSTR "\r", PRINT_CAP_ARGS(epc_or_error_epc));
        qemu_log("Attempting to modify sealed EPCC/ErrorEPCC: " PRINT_CAP_FMTSTR "\r", PRINT_CAP_ARGS(epc_or_error_epc));
        // Clear the tag bit and update the cursor:
        cap_mark_unrepresentable(new_cursor, epc_or_error_epc);
    } else if (!is_representable_cap_with_addr(epc_or_error_epc, new_cursor)) {
        error_report("Attempting to set unrepresentable cursor(0x" TARGET_FMT_lx
                    ") on EPCC/ErrorEPCC: " PRINT_CAP_FMTSTR "\r", new_cursor, PRINT_CAP_ARGS(epc_or_error_epc));
        cap_mark_unrepresentable(new_cursor, epc_or_error_epc);
    } else {
        epc_or_error_epc->_cr_cursor = new_cursor;
    }
}
#endif

void set_CP0_EPC(CPUMIPSState *env, target_ulong arg)
{
#ifdef TARGET_CHERI
    set_epc_or_error_epc(env, &env->active_tc.CHWR.EPCC, arg);
#else
    env->CP0_EPC = arg;
#endif
}
void set_CP0_ErrorEPC(CPUMIPSState *env, target_ulong arg)
{
#ifdef TARGET_CHERI
    set_epc_or_error_epc(env, &env->active_tc.CHWR.ErrorEPCC, arg);
#else
    env->CP0_ErrorEPC = arg;
#endif
}

type_init(mips_cpu_register_types)
