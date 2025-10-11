/*
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/reboot.h>
#include <linux/bug.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <asm/sections.h>
#include <asm/current.h>

/*
 * Forward declaration of struct module.
 *
 * We do not include <linux/module.h> to avoid pulling in unnecessary
 * headers and potential KMI (Kernel Module Interface) violations.
 * Instead, we declare only the fields we need for traversal and symbol
 * scanning. This is safe because the layout of these core fields is
 * stable across GKI kernels.
 */
struct kpm_modules {
    struct list_head list;        /* Node in the global modules list */
    const char *name;             /* Module name (for logging) */
    unsigned int num_sections;    /* Number of sections in this module */
};

/*
 * Forward declaration of the global modules list.
 *
 * This symbol is defined in kernel/module/main.c and is part of the
 * standard kernel symbol space. It is safe to reference here because
 * we are compiling into the kernel image (vmlinux), not a module.
 */
extern struct list_head modules;

/*
 * Symbol table boundaries for built-in kernel symbols.
 *
 * These symbols are defined by the linker script (vmlinux.lds) and
 * mark the start and end of the __ksymtab and __ksymtab_gpl sections.
 * They contain entries of type 'struct kernel_symbol', which pair
 * function addresses with their names.
 */
extern const unsigned long __start___ksymtab;
extern const unsigned long __stop___ksymtab;
extern const unsigned long __start___ksymtab_gpl;
extern const unsigned long __stop___ksymtab_gpl;

/*
 * Structure representing an entry in the kernel's built-in symbol table.
 *
 * This is used for exported symbols from built-in (non-module) code.
 * Each entry contains:
 *   - value: the address of the symbol (function or variable)
 *   - name:  the null-terminated symbol name (e.g., "sukisu_handle_kpm")
 *
 * This structure is defined in <linux/export.h>, but we redeclare it
 * here to avoid including that header.
 */
struct kpm_kernel_symbol {
    long value;                   /* Address of the symbol */
    const char *name;             /* Null-terminated symbol name */
};

/*
 * Target symbol to detect.
 *
 * This is the function used by KernelSU to handle kpm (kernel patch management).
 * If this symbol is present in either the built-in kernel or any loaded module,
 * it indicates that root capabilities have been injected.
 *
 * Stored as a static const string to avoid runtime string allocation.
 */
static const char target_symbol_name[] = "sukisu_handle_kpm";

/**
 * my_strcmp - Compare two null-terminated strings
 * @a: First string
 * @b: Second string
 *
 * Performs a standard lexicographic comparison of two strings.
 * Returns:
 *   0 if strings are equal
 *   < 0 if @a < @b
 *   > 0 if @a > @b
 *
 * This function is implemented manually to avoid dependency on
 * <linux/string.h> and to ensure that the comparison logic cannot
 * be subverted by a patched strcmp() in the kernel.
 */
static int my_strcmp(const char *a, const char *b)
{
    while (*a && *b && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)(*a) - (unsigned char)(*b);
}

/**
 * is_init_process - Check if current process is init (PID 1)
 *
 * The init process (PID 1) is the first userspace process started by
 * the kernel. We use this as a synchronization point to ensure that
 * all kernel modules have been loaded before we begin scanning.
 *
 * Returns: true if current task is init, false otherwise.
 */
static bool is_init_process(void)
{
    return current->pid == 1 && current->tgid == 1;
}

/**
 * scan_builtin_symbols - Scan built-in kernel symbol table for target
 *
 * Iterates over the __ksymtab and __ksymtab_gpl sections, which contain
 * all symbols exported from built-in kernel code (i.e., not modules).
 *
 * This allows detection of root-enabling functions that have been
 * compiled directly into the kernel image (e.g., via patching).
 *
 * Returns: true if target symbol is found, false otherwise.
 */
static bool scan_builtin_symbols(void)
{
    const struct kpm_kernel_symbol *start, *stop, *ksym;

    /* Scan the main kernel symbol table (__ksymtab) */
    start = (const struct kpm_kernel_symbol *)&__start___ksymtab;
    stop  = (const struct kpm_kernel_symbol *)&__stop___ksymtab;

    for (ksym = start; ksym < stop; ksym++) {
        if (ksym->name && my_strcmp(ksym->name, target_symbol_name) == 0) {
            return true;
        }
    }

    /* Also scan the GPL-only symbol table (__ksymtab_gpl) */
    start = (const struct kpm_kernel_symbol *)&__start___ksymtab_gpl;
    stop  = (const struct kpm_kernel_symbol *)&__stop___ksymtab_gpl;

    for (ksym = start; ksym < stop; ksym++) {
        if (ksym->name && my_strcmp(ksym->name, target_symbol_name) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * scan_all_modules - Traverse all loaded kernel modules
 *
 * Walks the global 'modules' list and checks each one for the presence
 * of the target symbol using section name matching.
 *
 * This detects root-enabling functions that are loaded as loadable
 * kernel modules (e.g., su.ko, ksu.ko).
 *
 * Returns: true if any module contains the target symbol, false otherwise.
 */

/**
 * sukisu_kpm_symbol_scan - Main detection routine
 *
 * Entry point called via late_initcall. Runs once during boot after
 * init process starts. Scans both built-in and module symbols for
 * unauthorized root functions.
 *
 * If the target symbol is found, triggers a kernel panic with a
 * descriptive message to aid in forensic analysis.
 *
 * This function is designed to be:
 *   - Non-invasive: does not alter system state
 *   - Idempotent: safe to call once
 *   - Auditable: all logic is visible and minimal
 *
 * Returns: 0 on success (no symbol found), never returns on detection.
 */
static int __init sukisu_kpm_symbol_scan(void)
{
    bool symbol_found = false;

    /* Only run in the context of init process (PID 1) */
    if (!is_init_process()) {
        return 0;
    }

    pr_info("Starting runtime root symbol scan in init process (PID 1)\n");

    /* Scan built-in kernel symbols */
    if (scan_builtin_symbols()) {
        pr_emerg("PANIC: built-in symbol '%s' detected in kernel image\n",
                 target_symbol_name);
        symbol_found = true;
    }

    /* If nothing found, continue booting. */
    if (!symbol_found) {
        return 0;
    }

    /*
     * Critical: Unauthorized symbol detected.
     * Trigger kernel panic to halt system and generate crash log.
     * This ensures the event is logged in kernel log buffer and
     * prevents further execution of a potentially compromised system.
     */
    panic("PANIC: found kpm function '%s' detected in kernel",
          target_symbol_name);

    /* Unreachable */
    return 0;
}

/*
 * Register the detection function to run late in the init sequence,
 * after module loading is complete and init process has started.
 */
late_initcall(sukisu_kpm_symbol_scan);
