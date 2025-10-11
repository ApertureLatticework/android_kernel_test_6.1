// SPDX-License-Identifier: GPL-2.0
/*
 * summer_pockets_abi.c - Summer Pockets ABI Interface Module
 *
 * This module provides a stable ABI (Application Binary Interface) entry point
 * for userspace to query the version of the Summer Pockets kernel interface.
 * It creates a read-only sysfs attribute at:
 *
 *     /sys/module/summer_pockets/version
 *
 * The content of this file is fixed to the string "1.0test\n", as defined by
 * the ABI contract. This allows userspace tools (e.g., configuration utilities,
 * diagnostic scripts) to programmatically detect the presence and version of
 * the Summer Pockets subsystem without relying on module parameters or dmesg.
 *
 * The module is designed to be compiled either as a loadable kernel module (LKM)
 * or as a built-in part of the kernel image. In both cases:
 *   - Initialization occurs during module loading (LKM) or kernel boot (built-in).
 *   - A sysfs directory named "summer_pockets" is created under /sys/module/.
 *   - The "version" attribute is exposed as a read-only file.
 *
 * On module unload (LKM only), the sysfs entries are cleanly removed.
 * For built-in modules, cleanup is not performed (as expected), and the sysfs
 * nodes persist for the lifetime of the system.
 *
 * Design Notes:
 * -------------
 * - We use kobject_create_and_add() to create a dedicated kobject under the
 *   module's own kobject (accessible via &THIS_MODULE->mkobj.kobj). This ensures
 *   the sysfs path is /sys/module/summer_pockets/, which is conventional for
 *   module-specific attributes.
 * - The attribute is defined using the __ATTR_RO() macro for brevity and safety,
 *   which automatically sets the mode to S_IRUGO and provides a const struct.
 * - All sysfs operations are grouped via a struct attribute_group to allow
 *   atomic creation/removal of multiple attributes (extensible in the future).
 * - Error handling follows kernel conventions: log errors via pr_err(), clean up
 *   partially allocated resources, and return negative errno values.
 *
 * Author: Your Name <your.email@example.com>
 * Copyright (C) 2025 Your Organization
 */

#include <linux/module.h>        /* Core module infrastructure: module_init/exit, THIS_MODULE */
#include <linux/kernel.h>        /* printk(), pr_*() macros */
#include <linux/init.h>          /* __init, __exit annotations */
#include <linux/kobject.h>       /* kobject_create_and_add(), kobject_put() */
#include <linux/sysfs.h>         /* sysfs_create_group(), sysfs_remove_group() */
#include <linux/errno.h>         /* Standard error codes (e.g., -ENOMEM) */

/* 
 * ABI version string. This is the canonical version exposed to userspace.
 * Must remain stable across patch releases unless the ABI itself changes.
 * Format: "<major>.<minor><tag>", where <tag> may indicate development status.
 */
#define SUMMER_POCKETS_ABI_VERSION "1.0test"

/*
 * summer_pockets_kobj - Global kobject representing the /sys/module/summer_pockets directory.
 *
 * This kobject is created during initialization and serves as the parent for all
 * sysfs attributes exported by this module. It is anchored under the module's
 * own kobject (via &THIS_MODULE->mkobj.kobj) to ensure proper sysfs hierarchy.
 *
 * Lifetime:
 *   - Allocated in summer_pockets_abi_init().
 *   - Freed in summer_pockets_abi_exit() (LKM only) via kobject_put().
 *   - For built-in modules, this kobject persists until system shutdown.
 */
static struct kobject *summer_pockets_kobj;

/*
 * version_show() - sysfs show callback for the 'version' attribute.
 * @kobj:   Pointer to the kobject representing the sysfs directory.
 * @attr:   Pointer to the kobj_attribute being read (unused here).
 * @buf:    Buffer provided by sysfs to write the attribute value into.
 *
 * This function is invoked whenever userspace reads from:
 *     /sys/module/summer_pockets/version
 *
 * It writes the fixed ABI version string followed by a newline (as required by
 * sysfs conventions) into @buf and returns the number of bytes written.
 *
 * Return: Number of bytes written to @buf (including newline), or negative errno.
 */
static ssize_t version_show(struct kobject *kobj,
                            struct kobj_attribute *attr,
                            char *buf)
{
    /*
     * Use sprintf() to format the version string. The trailing '\n' is
     * mandatory per sysfs documentation (Documentation/ABI/README).
     */
    return sprintf(buf, "%s\n", SUMMER_POCKETS_ABI_VERSION);
}

/*
 * version_attr - Definition of the 'version' sysfs attribute.
 *
 * The __ATTR_RO() macro expands to:
 *   struct kobj_attribute version_attr = {
 *       .attr = { .name = "version", .mode = S_IRUGO },
 *       .show = version_show,
 *   };
 *
 * This creates a read-only attribute visible to all users (mode 0444).
 */
static struct kobj_attribute version_attr = __ATTR_RO(version);

/*
 * summer_pockets_attrs - Null-terminated array of sysfs attributes.
 *
 * This array lists all attributes to be created under the summer_pockets_kobj.
 * Currently only 'version' is exposed, but additional attributes can be added
 * by appending to this array and updating the attribute_group.
 */
static struct attribute *summer_pockets_attrs[] = {
    &version_attr.attr,
    NULL, /* Sentinel: required by sysfs */
};

/*
 * summer_pockets_attr_group - Group of sysfs attributes for atomic management.
 *
 * Using an attribute group allows:
 *   - Creating all attributes in one sysfs_create_group() call.
 *   - Removing all attributes in one sysfs_remove_group() call.
 *   - Future extension without changing core initialization logic.
 */
static struct attribute_group summer_pockets_attr_group = {
    .attrs = summer_pockets_attrs,
};

/*
 * summer_pockets_abi_init() - Module initialization function.
 *
 * This function is called:
 *   - During kernel boot if compiled as built-in (CONFIG_SUMMER_POCKETS_ABI=y).
 *   - During insmod/rmmod if compiled as a loadable module (CONFIG_SUMMER_POCKETS_ABI=m).
 *
 * Responsibilities:
 *   1. Log module initialization start.
 *   2. Create a kobject under /sys/module/summer_pockets.
 *   3. Register the sysfs attribute group.
 *   4. Log success or propagate error on failure.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int __init summer_pockets_abi_init(void)
{
    int retval;

    pr_info("summer_pockets: Fuxk Oplus.\n");

    /*
     * Create the kobject for /sys/module/summer_pockets.
     *
     * We parent it under &THIS_MODULE->mkobj.kobj to ensure it appears under
     * /sys/module/<module_name>. This is the standard location for module-
     * specific sysfs entries (see Documentation/ABI/stable/sysfs-module).
     *
     * kobject_create_and_add() returns NULL on failure (e.g., ENOMEM).
     */
    summer_pockets_kobj = kobject_create_and_add("summer_pockets",
                                                 &THIS_MODULE->mkobj.kobj);
    if (!summer_pockets_kobj) {
        pr_err("summer_pockets: Failed to create summer_pockets kobject under /sys/module/.\n");
        return -ENOMEM;
    }

    /*
     * Create sysfs files by registering the attribute group.
     *
     * sysfs_create_group() creates all attributes listed in the group.
     * On error, it returns a negative errno (e.g., -EEXIST if name collision).
     */
    retval = sysfs_create_group(summer_pockets_kobj, &summer_pockets_attr_group);
    if (retval) {
        pr_err("summer_pockets: Failed to create sysfs attribute group (err=%d).\n", retval);
        /*
         * Clean up the kobject we just created to avoid leaking it.
         * kobject_put() decrements the reference count; since we hold the
         * only reference, this will free the kobject.
         */
        kobject_put(summer_pockets_kobj);
        return retval;
    }

    pr_info("summer_pockets: Created version info at /sys/module/summer_pockets/version\n");
    return 0;
}

/*
 * summer_pockets_abi_exit() - Module cleanup function.
 *
 * This function is called ONLY when the module is compiled as loadable (LKM)
 * and is being removed via rmmod. It is NOT called for built-in modules.
 *
 * Responsibilities:
 *   1. Log module unload start.
 *   2. Remove sysfs attribute group.
 *   3. Release the kobject (which removes the directory).
 *   4. Log successful cleanup.
 *
 * Note: Built-in modules do not have an exit path; their resources persist.
 */
static void __exit summer_pockets_abi_exit(void)
{
    pr_info("summer_pockets: Ciallo.\n");

    /*
     * Defensive check: ensure kobject was created (should always be true,
     * but safe to verify).
     */
    if (summer_pockets_kobj) {
        /*
         * Remove all sysfs attributes in the group. This deletes the
         * 'version' file.
         */
        sysfs_remove_group(summer_pockets_kobj, &summer_pockets_attr_group);

        /*
         * Drop the reference to the kobject. This will remove the
         * /sys/module/summer_pockets directory once the reference count hits zero.
         */
        kobject_put(summer_pockets_kobj);
        summer_pockets_kobj = NULL; /* Prevent accidental reuse */
    }

    pr_info("summer_pockets: Goodbye.\n");
}

/*
 * Register initialization and cleanup functions with the kernel module loader.
 *
 * - module_init() designates the entry point.
 * - module_exit() designates the exit point (ignored for built-in modules).
 */
module_init(summer_pockets_abi_init);
module_exit(summer_pockets_abi_exit);

/*
 * Module metadata. Required for proper identification in /sys/module/,
 * modinfo output, and kernel logs.
 */
MODULE_LICENSE("GPL");                     /* Required: GPL-compatible */
MODULE_AUTHOR("dabao1955 <dabao1955@163.com>");
MODULE_DESCRIPTION("Summer Pockets test kernel");
MODULE_VERSION(SUMMER_POCKETS_ABI_VERSION); /* Exposed via modinfo and /sys/module/.../version */
