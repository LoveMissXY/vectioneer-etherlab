/******************************************************************************
 *
 *  Copyright (C) 2006-2020  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *  vim: expandtab
 *
 *****************************************************************************/

/**
   \file
   EtherCAT master methods.
*/

/*****************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>
#include <linux/freezer.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <uapi/linux/sched/types.h> // struct sched_param
#include <linux/sched/types.h> // sched_setscheduler
#include <linux/sched/signal.h> // signal_pending
#endif

#include "globals.h"
#include "slave.h"
#include "slave_config.h"
#include "device.h"
#include "datagram.h"
#include "mailbox.h"
#ifdef EC_EOE
#include "ethernet.h"
#endif
#include "master.h"

/*****************************************************************************/

/** Set to 1 to enable external datagram injection debugging.
 */
#define DEBUG_INJECT 0

/** Always output corrupted frames.
 */
#define FORCE_OUTPUT_CORRUPTED 0

#ifdef EC_HAVE_CYCLES

/** Frame timeout in cycles.
 */
static cycles_t timeout_cycles;

/** Timeout for external datagram injection [cycles].
 */
static cycles_t ext_injection_timeout_cycles;

#else

/** Frame timeout in jiffies.
 */
static unsigned long timeout_jiffies;

/** Timeout for external datagram injection [jiffies].
 */
static unsigned long ext_injection_timeout_jiffies;

#endif

/** List of intervals for statistics [s].
 */
const unsigned int rate_intervals[] = {
    1, 10, 60
};

/*****************************************************************************/

void ec_master_clear_slave_configs(ec_master_t *);
void ec_master_clear_domains(ec_master_t *);
static int ec_master_idle_thread(void *);
static int ec_master_operation_thread(void *);
#ifdef EC_EOE
static int ec_master_eoe_thread(void *);
#endif
void ec_master_find_dc_ref_clock(ec_master_t *);
void ec_master_clear_device_stats(ec_master_t *);
void ec_master_update_device_stats(ec_master_t *);

/*****************************************************************************/

/** Static variables initializer.
*/
void ec_master_init_static(void)
{
#ifdef EC_HAVE_CYCLES
    timeout_cycles = (cycles_t) EC_IO_TIMEOUT /* us */ * (cpu_khz / 1000);
    ext_injection_timeout_cycles =
        (cycles_t) EC_SDO_INJECTION_TIMEOUT /* us */ * (cpu_khz / 1000);
#else
    // one jiffy may always elapse between time measurement
    timeout_jiffies = max(EC_IO_TIMEOUT * HZ / 1000000, 1);
    ext_injection_timeout_jiffies =
        max(EC_SDO_INJECTION_TIMEOUT * HZ / 1000000, 1);
#endif
}

/*****************************************************************************/

/**
   Master constructor.
   \return 0 in case of success, else < 0
*/

int ec_master_init(ec_master_t *master, /**< EtherCAT master */
        unsigned int index, /**< master index */
        const uint8_t *main_mac, /**< MAC address of main device */
        const uint8_t *backup_mac, /**< MAC address of backup device */
        dev_t device_number, /**< Character device number. */
        struct class *class, /**< Device class. */
        unsigned int debug_level /**< Debug level (module parameter). */
        )
{
    int ret;
    unsigned int dev_idx, i;

    master->index = index;
    master->reserved = 0;

    ec_lock_init(&master->master_sem);

    for (dev_idx = EC_DEVICE_MAIN; dev_idx < EC_MAX_NUM_DEVICES; dev_idx++) {
        master->macs[dev_idx] = NULL;
    }

    master->macs[EC_DEVICE_MAIN] = main_mac;

#if EC_MAX_NUM_DEVICES > 1
    master->macs[EC_DEVICE_BACKUP] = backup_mac;
    master->num_devices = 1 + !ec_mac_is_zero(backup_mac);
#else
    if (!ec_mac_is_zero(backup_mac)) {
        EC_MASTER_WARN(master, "Ignoring backup MAC address!");
    }
#endif

    ec_master_clear_device_stats(master);

    ec_lock_init(&master->device_sem);

    master->phase = EC_ORPHANED;
    master->active = 0;
    master->config_changed = 0;
    master->injection_seq_fsm = 0;
    master->injection_seq_rt = 0;
    master->reboot = 0;

    master->slaves = NULL;
    master->slave_count = 0;

    INIT_LIST_HEAD(&master->configs);
    INIT_LIST_HEAD(&master->domains);
    INIT_LIST_HEAD(&master->sii_images);

    master->app_time = 0ULL;
    master->dc_ref_time = 0ULL;
    master->dc_offset_valid = 0;

    master->scan_busy = 0;
    master->allow_scan = 1;
    ec_lock_init(&master->scan_sem);
    init_waitqueue_head(&master->scan_queue);

    master->config_busy = 0;
    ec_lock_init(&master->config_sem);
    init_waitqueue_head(&master->config_queue);

    INIT_LIST_HEAD(&master->datagram_queue);
    master->datagram_index = 0;

    INIT_LIST_HEAD(&master->ext_datagram_queue);
    ec_lock_init(&master->ext_queue_sem);

    master->ext_ring_idx_rt = 0;
    master->ext_ring_idx_fsm = 0;
    master->rt_slave_requests = 0;
    master->rt_slaves_available = 0;

    // init external datagram ring
    for (i = 0; i < EC_EXT_RING_SIZE; i++) {
        ec_datagram_t *datagram = &master->ext_datagram_ring[i];
        ec_datagram_init(datagram);
        snprintf(datagram->name, EC_DATAGRAM_NAME_SIZE, "ext-%u", i);
    }

    // send interval in IDLE phase
    ec_master_set_send_interval(master, 1000000 / HZ);

    master->fsm_slave = NULL;
    INIT_LIST_HEAD(&master->fsm_exec_list);
    master->fsm_exec_count = 0U;

    master->debug_level = debug_level;
    master->stats.timeouts = 0;
    master->stats.corrupted = 0;
    master->stats.unmatched = 0;
    master->stats.output_jiffies = 0;

    // set up pcap debugging
    if (pcap_size > 0) {
        master->pcap_data = vmalloc(pcap_size);
    } else {
        master->pcap_data = NULL;
    }
    master->pcap_curr_data = master->pcap_data;

    master->thread = NULL;

#ifdef EC_EOE
    master->eoe_thread = NULL;
    INIT_LIST_HEAD(&master->eoe_handlers);
#endif

    ec_lock_init(&master->io_sem);
    master->send_cb = NULL;
    master->receive_cb = NULL;
    master->cb_data = NULL;
    master->app_send_cb = NULL;
    master->app_receive_cb = NULL;
    master->app_cb_data = NULL;

    INIT_LIST_HEAD(&master->sii_requests);
    INIT_LIST_HEAD(&master->emerg_reg_requests);

    init_waitqueue_head(&master->request_queue);

    // init devices
    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        ret = ec_device_init(&master->devices[dev_idx], master);
        if (ret < 0) {
            goto out_clear_devices;
        }
    }

    // init state machine datagram
    ec_datagram_init(&master->fsm_datagram);
    snprintf(master->fsm_datagram.name, EC_DATAGRAM_NAME_SIZE, "master-fsm");
    ret = ec_datagram_prealloc(&master->fsm_datagram, EC_MAX_DATA_SIZE);
    if (ret < 0) {
        ec_datagram_clear(&master->fsm_datagram);
        EC_MASTER_ERR(master, "Failed to allocate FSM datagram.\n");
        goto out_clear_devices;
    }

    // create state machine object
    ec_fsm_master_init(&master->fsm, master, &master->fsm_datagram);

    // alloc external datagram ring
    for (i = 0; i < EC_EXT_RING_SIZE; i++) {
        ec_datagram_t *datagram = &master->ext_datagram_ring[i];
        ret = ec_datagram_prealloc(datagram, EC_MAX_DATA_SIZE);
        if (ret) {
            EC_MASTER_ERR(master, "Failed to allocate external"
                    " datagram %u.\n", i);
            goto out_clear_ext_datagrams;
        }
    }

    // init reference sync datagram
    ec_datagram_init(&master->ref_sync_datagram);
    snprintf(master->ref_sync_datagram.name, EC_DATAGRAM_NAME_SIZE,
            "refsync");
    ret = ec_datagram_prealloc(&master->ref_sync_datagram, 4);
    if (ret < 0) {
        ec_datagram_clear(&master->ref_sync_datagram);
        EC_MASTER_ERR(master, "Failed to allocate reference"
                " synchronisation datagram.\n");
        goto out_clear_ext_datagrams;
    }

    // init sync datagram
    ec_datagram_init(&master->sync_datagram);
    snprintf(master->sync_datagram.name, EC_DATAGRAM_NAME_SIZE, "sync");
    ret = ec_datagram_prealloc(&master->sync_datagram, 4);
    if (ret < 0) {
        ec_datagram_clear(&master->sync_datagram);
        EC_MASTER_ERR(master, "Failed to allocate"
                " synchronisation datagram.\n");
        goto out_clear_ref_sync;
    }

    // init sync64 datagram
    ec_datagram_init(&master->sync64_datagram);
    snprintf(master->sync64_datagram.name, EC_DATAGRAM_NAME_SIZE, "sync64");
    ret = ec_datagram_prealloc(&master->sync64_datagram, 8);
    if (ret < 0) {
        ec_datagram_clear(&master->sync_datagram);
        EC_MASTER_ERR(master, "Failed to allocate 64bit ref slave"
                " system clock datagram.\n");
        goto out_clear_sync;
    }

    // init sync monitor datagram
    ec_datagram_init(&master->sync_mon_datagram);
    snprintf(master->sync_mon_datagram.name, EC_DATAGRAM_NAME_SIZE,
            "syncmon");
    ret = ec_datagram_brd(&master->sync_mon_datagram, 0x092c, 4);
    if (ret < 0) {
        ec_datagram_clear(&master->sync_mon_datagram);
        EC_MASTER_ERR(master, "Failed to allocate sync"
                " monitoring datagram.\n");
        goto out_clear_sync64;
    }

    master->dc_ref_config = NULL;
    master->dc_ref_clock = NULL;

    // init character device
    ret = ec_cdev_init(&master->cdev, master, device_number);
    if (ret)
        goto out_clear_sync_mon;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
    master->class_device = device_create(class, NULL,
            MKDEV(MAJOR(device_number), master->index), NULL,
            "EtherCAT%u", master->index);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 26)
    master->class_device = device_create(class, NULL,
            MKDEV(MAJOR(device_number), master->index),
            "EtherCAT%u", master->index);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 15)
    master->class_device = class_device_create(class, NULL,
            MKDEV(MAJOR(device_number), master->index), NULL,
            "EtherCAT%u", master->index);
#else
    master->class_device = class_device_create(class,
            MKDEV(MAJOR(device_number), master->index), NULL,
            "EtherCAT%u", master->index);
#endif
    if (IS_ERR(master->class_device)) {
        EC_MASTER_ERR(master, "Failed to create class device!\n");
        ret = PTR_ERR(master->class_device);
        goto out_clear_cdev;
    }

#ifdef EC_RTDM
    // init RTDM device
    ret = ec_rtdm_dev_init(&master->rtdm_dev, master);
    if (ret) {
        goto out_unregister_class_device;
    }
#endif

    return 0;

#ifdef EC_RTDM
out_unregister_class_device:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 26)
    device_unregister(master->class_device);
#else
    class_device_unregister(master->class_device);
#endif
#endif
out_clear_cdev:
    ec_cdev_clear(&master->cdev);
out_clear_sync_mon:
    ec_datagram_clear(&master->sync_mon_datagram);
out_clear_sync64:
    ec_datagram_clear(&master->sync64_datagram);
out_clear_sync:
    ec_datagram_clear(&master->sync_datagram);
out_clear_ref_sync:
    ec_datagram_clear(&master->ref_sync_datagram);
out_clear_ext_datagrams:
    for (i = 0; i < EC_EXT_RING_SIZE; i++) {
        ec_datagram_clear(&master->ext_datagram_ring[i]);
    }
    ec_fsm_master_clear(&master->fsm);
    ec_datagram_clear(&master->fsm_datagram);
out_clear_devices:
    for (; dev_idx > 0; dev_idx--) {
        ec_device_clear(&master->devices[dev_idx - 1]);
    }
    return ret;
}

/*****************************************************************************/

/** Destructor.
*/
void ec_master_clear(
        ec_master_t *master /**< EtherCAT master */
        )
{
    unsigned int dev_idx, i;

#ifdef EC_RTDM
    ec_rtdm_dev_clear(&master->rtdm_dev);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 26)
    device_unregister(master->class_device);
#else
    class_device_unregister(master->class_device);
#endif

    ec_cdev_clear(&master->cdev);

    ec_master_slaves_not_available(master);
#ifdef EC_EOE
    // free all EoE handlers
    ec_master_clear_eoe_handlers(master, 1);
#endif
    ec_master_clear_domains(master);
    ec_master_clear_slave_configs(master);
    ec_master_clear_slaves(master);
    ec_master_clear_sii_images(master);

    ec_datagram_clear(&master->sync_mon_datagram);
    ec_datagram_clear(&master->sync64_datagram);
    ec_datagram_clear(&master->sync_datagram);
    ec_datagram_clear(&master->ref_sync_datagram);

    for (i = 0; i < EC_EXT_RING_SIZE; i++) {
        ec_datagram_clear(&master->ext_datagram_ring[i]);
    }

    ec_fsm_master_clear(&master->fsm);
    ec_datagram_clear(&master->fsm_datagram);

    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        ec_device_clear(&master->devices[dev_idx]);
    }

    if (master->pcap_data) {
        vfree(master->pcap_data);
        master->pcap_data = NULL;
        master->pcap_curr_data = NULL;
    }
}

/*****************************************************************************/

#ifdef EC_EOE
/** Clear and free auto created EoE handlers.
 * Clear the slave reference from manually created EoE handlers.
 */
void ec_master_clear_eoe_handlers(
        ec_master_t *master, /**< EtherCAT master */
        unsigned int free_all /**< free auto and manual EoE handlers */
        )
{
    ec_eoe_t *eoe, *next;

    list_for_each_entry_safe(eoe, next, &master->eoe_handlers, list) {
        if (free_all || eoe->auto_created) {
            // free_all or auto created eoe: clear and free
            list_del(&eoe->list);
            ec_eoe_clear(eoe);
            kfree(eoe);
        } else {
            // manaully created eoe: clear slave ref
            ec_eoe_clear_slave(eoe);
        }
    }
}
#endif

/*****************************************************************************/

/** Clear all slave configurations.
 */
void ec_master_clear_slave_configs(ec_master_t *master)
{
    ec_slave_config_t *sc, *next;

    master->dc_ref_config = NULL;

    list_for_each_entry_safe(sc, next, &master->configs, list) {
        list_del(&sc->list);
        ec_slave_config_clear(sc);
        kfree(sc);
    }
}

/*****************************************************************************/

/** Clear the SII data.
 */
void ec_sii_image_clear(ec_sii_image_t *sii_image)
{
    unsigned int i;
    ec_pdo_t *pdo, *next_pdo;

    // free all sync managers
    if (sii_image->sii.syncs) {
        for (i = 0; i < sii_image->sii.sync_count; i++) {
            ec_sync_clear(&sii_image->sii.syncs[i]);
        }
        kfree(sii_image->sii.syncs);
        sii_image->sii.syncs = NULL;
    }

    // free all strings
    if (sii_image->sii.strings) {
        for (i = 0; i < sii_image->sii.string_count; i++)
            kfree(sii_image->sii.strings[i]);
        kfree(sii_image->sii.strings);
    }

    // free all SII PDOs
    list_for_each_entry_safe(pdo, next_pdo, &sii_image->sii.pdos, list) {
        list_del(&pdo->list);
        ec_pdo_clear(pdo);
        kfree(pdo);
    }

    if (sii_image->words) {
        kfree(sii_image->words);
    }
}

/*****************************************************************************/

/** Clear the SII data applied during bus scanning.
 */
void ec_master_clear_sii_images(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_sii_image_t *sii_image, *next;

    list_for_each_entry_safe(sii_image, next, &master->sii_images, list) {
#ifdef EC_SII_CACHE
        if ((master->phase != EC_OPERATION) ||
           ((sii_image->sii.serial_number == 0) && (sii_image->sii.alias == 0)))
#endif
        {
            list_del(&sii_image->list);
            ec_sii_image_clear(sii_image);
            kfree(sii_image);
        }
    }
}

/*****************************************************************************/

/** Set flag to say that the slaves are not available for slave request
 * processing.
 *
 * Called from master fsm, which is processed inside the master_sem lock
 */
void ec_master_slaves_not_available(ec_master_t *master)
{
    master->rt_slaves_available = 0;
}

/*****************************************************************************/

/** Set flag to say that the slaves are now available for slave request
 * processing.
 *
 * Called from master fsm, which is processed inside the master_sem lock
 */
void ec_master_slaves_available(ec_master_t *master)
{
    master->rt_slaves_available = 1;
}

/*****************************************************************************/

/** Clear all slaves.
 */
void ec_master_clear_slaves(ec_master_t *master)
{
    ec_slave_t *slave;

    master->dc_ref_clock = NULL;

    // External requests are obsolete, so we wake pending waiters and remove
    // them from the list.

    while (!list_empty(&master->sii_requests)) {
        ec_sii_write_request_t *request =
            list_entry(master->sii_requests.next,
                    ec_sii_write_request_t, list);
        list_del_init(&request->list); // dequeue
        EC_MASTER_WARN(master, "Discarding SII request, slave %s-%u about"
                " to be deleted.\n", ec_device_names[request->slave->device_index!=0],
                request->slave->ring_position);
        request->state = EC_INT_REQUEST_FAILURE;
        wake_up_all(&master->request_queue);
    }

    master->fsm_slave = NULL;
    INIT_LIST_HEAD(&master->fsm_exec_list);
    master->fsm_exec_count = 0;

    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_slave_clear(slave);
    }

    if (master->slaves) {
        kfree(master->slaves);
        master->slaves = NULL;
    }

    master->slave_count = 0;
}

/*****************************************************************************/

/** Clear all domains.
 */
void ec_master_clear_domains(ec_master_t *master)
{
    ec_domain_t *domain, *next;

    list_for_each_entry_safe(domain, next, &master->domains, list) {
        list_del(&domain->list);
        ec_domain_clear(domain);
        kfree(domain);
    }
}

/*****************************************************************************/

void ec_master_reset_slave_fsms(ec_master_t *master) {
    ec_slave_t *slave;
    for(slave = master->slaves;
        slave < master->slaves + master->slave_count;
        slave++) {
        ec_fsm_slave_reset(&slave->fsm);
    }
}

/*****************************************************************************/

/** Clear the configuration applied by the application.
 */
void ec_master_clear_config(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_lock_down(&master->master_sem);
    ec_master_clear_domains(master);
    ec_master_clear_slave_configs(master);
    ec_master_reset_slave_fsms(master);
    ec_lock_up(&master->master_sem);
}

/*****************************************************************************/

/** Internal sending callback.
 */
void ec_master_internal_send_cb(
        void *cb_data /**< Callback data. */
        )
{
    ec_master_t *master = (ec_master_t *) cb_data;
    ec_lock_down(&master->io_sem);
    ecrt_master_send_ext(master);
    ec_lock_up(&master->io_sem);
}

/*****************************************************************************/

/** Internal receiving callback.
 */
void ec_master_internal_receive_cb(
        void *cb_data /**< Callback data. */
        )
{
    ec_master_t *master = (ec_master_t *) cb_data;
    ec_lock_down(&master->io_sem);
    ecrt_master_receive(master);
    ec_lock_up(&master->io_sem);
}

/*****************************************************************************/

/** Starts the master thread.
 *
 * \retval  0 Success.
 * \retval <0 Error code.
 */
int ec_master_thread_start(
        ec_master_t *master, /**< EtherCAT master */
        int (*thread_func)(void *), /**< thread function to start */
        const char *name /**< Thread name. */
        )
{
    EC_MASTER_INFO(master, "Starting %s thread.\n", name);
    master->thread = kthread_run(thread_func, master, name);
    if (IS_ERR(master->thread)) {
        int err = (int) PTR_ERR(master->thread);
        EC_MASTER_ERR(master, "Failed to start master thread (error %i)!\n",
                err);
        master->thread = NULL;
        return err;
    }

    return 0;
}

/*****************************************************************************/

/** Stops the master thread.
 */
void ec_master_thread_stop(
        ec_master_t *master /**< EtherCAT master */
        )
{
    unsigned long sleep_jiffies;

    if (!master->thread) {
        EC_MASTER_WARN(master, "%s(): Already finished!\n", __func__);
        return;
    }

    EC_MASTER_DBG(master, 1, "Stopping master thread.\n");

    kthread_stop(master->thread);
    master->thread = NULL;
    EC_MASTER_INFO(master, "Master thread exited.\n");

    if (master->fsm_datagram.state != EC_DATAGRAM_SENT) {
        return;
    }

    // wait for FSM datagram
    sleep_jiffies = max(HZ / 100, 1); // 10 ms, at least 1 jiffy
    schedule_timeout(sleep_jiffies);
}

/*****************************************************************************/

/** Transition function from ORPHANED to IDLE phase.
 *
 * \return Zero on success, otherwise a negative error code.
 */
int ec_master_enter_idle_phase(
        ec_master_t *master /**< EtherCAT master */
        )
{
    int ret;
    ec_device_index_t dev_idx;
#ifdef EC_EOE
    int i;
    int master_index = 0;
    uint16_t alias, ring_position = 0;
#endif

    EC_MASTER_DBG(master, 1, "ORPHANED -> IDLE.\n");

    master->send_cb = ec_master_internal_send_cb;
    master->receive_cb = ec_master_internal_receive_cb;
    master->cb_data = master;

    master->phase = EC_IDLE;

    // reset number of responding slaves to trigger scanning
    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        master->fsm.slaves_responding[dev_idx] = 0;
    }

#ifdef EC_EOE
    // create eoe interfaces for this master on startup
    // Note: needs the masters main device to be configured to init the
    //   eoe's mac address
    for (i = 0; i < eoe_count; i++) {
        ret = ec_eoe_parse(eoe_interfaces[i], &master_index, &alias,
                &ring_position);

        if ((ret == 0) && (master_index == master->index)) {
            EC_MASTER_INFO(master, "Adding EOE iface \"%s\" for master %d, "
                    "alias %u, ring position %u.\n",
                    eoe_interfaces[i], master_index, alias, ring_position);
            ecrt_master_eoe_addif(master, alias, ring_position);
        }
    }
#endif

    ret = ec_master_thread_start(master, ec_master_idle_thread,
            "EtherCAT-IDLE");
    if (ret)
        master->phase = EC_ORPHANED;

    return ret;
}

/*****************************************************************************/

/** Transition function from IDLE to ORPHANED phase.
 */
void ec_master_leave_idle_phase(ec_master_t *master /**< EtherCAT master */)
{
    EC_MASTER_DBG(master, 1, "IDLE -> ORPHANED.\n");

    master->phase = EC_ORPHANED;

    ec_master_slaves_not_available(master);
#ifdef EC_EOE
    ec_master_eoe_stop(master);
#endif
    ec_master_thread_stop(master);

    ec_lock_down(&master->master_sem);
    ec_master_clear_slaves(master);
    ec_master_clear_sii_images(master);
    ec_lock_up(&master->master_sem);

    ec_fsm_master_reset(&master->fsm);
}

/*****************************************************************************/

/** Transition function from IDLE to OPERATION phase.
 *
 * \return Zero on success, otherwise a negative error code.
 */
int ec_master_enter_operation_phase(
        ec_master_t *master /**< EtherCAT master */
        )
{
    int ret = 0;
    ec_slave_t *slave;

    EC_MASTER_DBG(master, 1, "IDLE -> OPERATION.\n");

    ec_lock_down(&master->config_sem);
    if (master->config_busy) {
        ec_lock_up(&master->config_sem);

        // wait for slave configuration to complete
        ret = wait_event_interruptible(master->config_queue,
                    !master->config_busy);
        if (ret) {
            EC_MASTER_INFO(master, "Finishing slave configuration"
                    " interrupted by signal.\n");
            goto out_return;
        }

        EC_MASTER_DBG(master, 1, "Waiting for pending slave"
                " configuration returned.\n");
    } else {
        ec_lock_up(&master->config_sem);
    }

    ec_lock_down(&master->scan_sem);
    master->allow_scan = 0; // 'lock' the slave list
    if (!master->scan_busy) {
        ec_lock_up(&master->scan_sem);
    } else {
        ec_lock_up(&master->scan_sem);

        // wait for slave scan to complete
        ret = wait_event_interruptible(master->scan_queue,
                !master->scan_busy);
        if (ret) {
            EC_MASTER_INFO(master, "Waiting for slave scan"
                    " interrupted by signal.\n");
            goto out_allow;
        }

        EC_MASTER_DBG(master, 1, "Waiting for pending"
                " slave scan returned.\n");
    }

    // set states for all slaves
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_slave_request_state(slave, EC_SLAVE_STATE_PREOP);
    }

    master->phase = EC_OPERATION;
    master->app_send_cb = NULL;
    master->app_receive_cb = NULL;
    master->app_cb_data = NULL;
    return ret;

out_allow:
    master->allow_scan = 1;
out_return:
    return ret;
}

/*****************************************************************************/

/** Transition function from OPERATION to IDLE phase.
 */
void ec_master_leave_operation_phase(
        ec_master_t *master /**< EtherCAT master */
        )
{
    if (master->active) {
        ecrt_master_deactivate(master); // also clears config
    } else {
        ec_master_clear_config(master);
    }

    /* Re-allow scanning for IDLE phase. */
    master->allow_scan = 1;

    EC_MASTER_DBG(master, 1, "OPERATION -> IDLE.\n");

    master->phase = EC_IDLE;
}

/*****************************************************************************/

/** Injects external datagrams that fit into the datagram queue.
 */
void ec_master_inject_external_datagrams(
        ec_master_t *master /**< EtherCAT master */
        )
{
    ec_datagram_t *datagram;
    size_t queue_size = 0, new_queue_size = 0;
#if DEBUG_INJECT
    unsigned int datagram_count = 0;
#endif

    if (master->ext_ring_idx_rt == master->ext_ring_idx_fsm) {
        // nothing to inject
        return;
    }

    list_for_each_entry(datagram, &master->datagram_queue, queue) {
        if (datagram->state == EC_DATAGRAM_QUEUED) {
            queue_size += datagram->data_size;
        }
    }

#if DEBUG_INJECT
    EC_MASTER_DBG(master, 1, "Injecting datagrams, queue_size=%zu\n",
            queue_size);
#endif

    while (master->ext_ring_idx_rt != master->ext_ring_idx_fsm) {
        datagram = &master->ext_datagram_ring[master->ext_ring_idx_rt];

        if (datagram->state != EC_DATAGRAM_INIT) {
            // skip datagram
            master->ext_ring_idx_rt =
                (master->ext_ring_idx_rt + 1) % EC_EXT_RING_SIZE;
            continue;
        }

        new_queue_size = queue_size + datagram->data_size;
        if (new_queue_size <= master->max_queue_size) {
#if DEBUG_INJECT
            EC_MASTER_DBG(master, 1, "Injecting datagram %s"
                    " size=%zu, queue_size=%zu\n", datagram->name,
                    datagram->data_size, new_queue_size);
            datagram_count++;
#endif
#ifdef EC_HAVE_CYCLES
            datagram->cycles_sent = 0;
#endif
            datagram->jiffies_sent = 0;
            ec_master_queue_datagram(master, datagram);
            queue_size = new_queue_size;
        }
        else if (datagram->data_size > master->max_queue_size) {
            datagram->state = EC_DATAGRAM_ERROR;
            EC_MASTER_ERR(master, "External datagram %s is too large,"
                    " size=%zu, max_queue_size=%zu\n",
                    datagram->name, datagram->data_size,
                    master->max_queue_size);
        }
        else { // datagram does not fit in the current cycle
#ifdef EC_HAVE_CYCLES
            cycles_t cycles_now = get_cycles();

            if (cycles_now - datagram->cycles_sent
                    > ext_injection_timeout_cycles)
#else
            if (jiffies - datagram->jiffies_sent
                    > ext_injection_timeout_jiffies)
#endif
            {
#if defined EC_RT_SYSLOG || DEBUG_INJECT
                unsigned int time_us;
#endif

                datagram->state = EC_DATAGRAM_ERROR;

#if defined EC_RT_SYSLOG || DEBUG_INJECT
#ifdef EC_HAVE_CYCLES
                time_us = (unsigned int)
                    ((cycles_now - datagram->cycles_sent) * 1000LL)
                    / cpu_khz;
#else
                time_us = (unsigned int)
                    ((jiffies - datagram->jiffies_sent) * 1000000 / HZ);
#endif
                EC_MASTER_ERR(master, "Timeout %u us: Injecting"
                        " external datagram %s size=%zu,"
                        " max_queue_size=%zu\n", time_us, datagram->name,
                        datagram->data_size, master->max_queue_size);
#endif
            }
            else {
#if DEBUG_INJECT
                EC_MASTER_DBG(master, 1, "Deferred injecting"
                        " external datagram %s size=%u, queue_size=%u\n",
                        datagram->name, datagram->data_size, queue_size);
#endif
                break;
            }
        }

        master->ext_ring_idx_rt =
            (master->ext_ring_idx_rt + 1) % EC_EXT_RING_SIZE;
    }

#if DEBUG_INJECT
    EC_MASTER_DBG(master, 1, "Injected %u datagrams.\n", datagram_count);
#endif
}

/*****************************************************************************/

/** Sets the expected interval between calls to ecrt_master_send
 * and calculates the maximum amount of data to queue.
 */
void ec_master_set_send_interval(
        ec_master_t *master, /**< EtherCAT master */
        unsigned int send_interval /**< Send interval */
        )
{
    master->send_interval = send_interval;
    master->max_queue_size =
        (send_interval * 1000) / EC_BYTE_TRANSMISSION_TIME_NS;
    master->max_queue_size -= master->max_queue_size / 10;
}

/*****************************************************************************/

/** Requests that all slaves on this master be rebooted (if supported).
 */
void ec_master_reboot_slaves(
        ec_master_t *master /**< EtherCAT master */
        )
{
    master->reboot = 1;
}

/*****************************************************************************/

/** Searches for a free datagram in the external datagram ring.
 *
 * \return Next free datagram, or NULL.
 */
ec_datagram_t *ec_master_get_external_datagram(
        ec_master_t *master /**< EtherCAT master */
        )
{
    if ((master->ext_ring_idx_fsm + 1) % EC_EXT_RING_SIZE !=
            master->ext_ring_idx_rt) {
        ec_datagram_t *datagram =
            &master->ext_datagram_ring[master->ext_ring_idx_fsm];
        /* Record the queued time for ec_master_inject_external_datagrams */
#ifdef EC_HAVE_CYCLES
        datagram->cycles_sent = get_cycles();
#endif
        datagram->jiffies_sent = jiffies;

        return datagram;
    }
    else {
        return NULL;
    }
}

/*****************************************************************************/

/** Places a datagram in the datagram queue.
 */
void ec_master_queue_datagram(
        ec_master_t *master, /**< EtherCAT master */
        ec_datagram_t *datagram /**< datagram */
        )
{
    ec_datagram_t *queued_datagram;

    /* It is possible, that a datagram in the queue is re-initialized with the
     * ec_datagram_<type>() methods and then shall be queued with this method.
     * In that case, the state is already reset to EC_DATAGRAM_INIT. Check if
     * the datagram is queued to avoid duplicate queuing (which results in an
     * infinite loop!). Set the state to EC_DATAGRAM_QUEUED again, probably
     * causing an unmatched datagram. */
    list_for_each_entry(queued_datagram, &master->datagram_queue, queue) {
        if (queued_datagram == datagram) {
            datagram->skip_count++;
#ifdef EC_RT_SYSLOG
            EC_MASTER_DBG(master, 1,
                    "Datagram %p already queued (skipping).\n", datagram);
#endif
            datagram->state = EC_DATAGRAM_QUEUED;
            return;
        }
    }

    if (datagram->state != EC_DATAGRAM_INVALID) {
        list_add_tail(&datagram->queue, &master->datagram_queue);
        datagram->state = EC_DATAGRAM_QUEUED;
    }
}

/*****************************************************************************/

/** Places a datagram in the non-application datagram queue.
 */
void ec_master_queue_datagram_ext(
        ec_master_t *master, /**< EtherCAT master */
        ec_datagram_t *datagram /**< datagram */
        )
{
    ec_lock_down(&master->ext_queue_sem);
    list_add_tail(&datagram->queue, &master->ext_datagram_queue);
    ec_lock_up(&master->ext_queue_sem);
}

/*****************************************************************************/

static int index_in_use(ec_master_t *master, uint8_t index)
{
    ec_datagram_t *datagram;
    list_for_each_entry(datagram, &master->datagram_queue, queue)
        if (datagram->state == EC_DATAGRAM_SENT && datagram->index == index)
            return 1;
    return 0;
}

/** Sends the datagrams in the queue for a certain device.
 *
 */
size_t ec_master_send_datagrams(
        ec_master_t *master, /**< EtherCAT master */
        ec_device_index_t device_index /**< Device index. */
        )
{
    ec_datagram_t *datagram, *next;
    size_t datagram_size;
    uint8_t *frame_data, *cur_data = NULL;
    void *follows_word;
#ifdef EC_HAVE_CYCLES
    cycles_t cycles_start, cycles_sent, cycles_end;
#endif
    unsigned long jiffies_sent;
    unsigned int frame_count, more_datagrams_waiting;
    struct list_head sent_datagrams;
    size_t sent_bytes = 0;
    uint8_t last_index;

#ifdef EC_HAVE_CYCLES
    cycles_start = get_cycles();
#endif
    frame_count = 0;
    INIT_LIST_HEAD(&sent_datagrams);

    EC_MASTER_DBG(master, 2, "%s(device_index = %u)\n",
            __func__, device_index);

    do {
        frame_data = NULL;
        follows_word = NULL;
        more_datagrams_waiting = 0;

        // fill current frame with datagrams
        list_for_each_entry(datagram, &master->datagram_queue, queue) {
            if (datagram->state != EC_DATAGRAM_QUEUED ||
                    datagram->device_index != device_index) {
                continue;
            }

            if (!frame_data) {
                // fetch pointer to transmit socket buffer
                frame_data =
                    ec_device_tx_data(&master->devices[device_index]);
                cur_data = frame_data + EC_FRAME_HEADER_SIZE;
            }

            // does the current datagram fit in the frame?
            datagram_size = EC_DATAGRAM_HEADER_SIZE + datagram->data_size
                + EC_DATAGRAM_FOOTER_SIZE;
            if (cur_data - frame_data + datagram_size > ETH_DATA_LEN) {
                more_datagrams_waiting = 1;
                break;
            }

            // do not reuse the index of a pending datagram to avoid confusion
            // in ec_master_receive_datagrams()
            last_index = master->datagram_index;
            while (index_in_use(master, master->datagram_index)) {
                if (++master->datagram_index == last_index) {
                    EC_MASTER_ERR(master, "No free datagram index, sending delayed\n");
                    goto break_send;
                }
            }
            datagram->index = master->datagram_index++;

            list_add_tail(&datagram->sent, &sent_datagrams);

            EC_MASTER_DBG(master, 2, "Adding datagram 0x%02X\n",
                    datagram->index);

            // set "datagram following" flag in previous datagram
            if (follows_word) {
                EC_WRITE_U16(follows_word,
                        EC_READ_U16(follows_word) | 0x8000);
            }

            // EtherCAT datagram header
            EC_WRITE_U8 (cur_data, datagram->type);
            EC_WRITE_U8 (cur_data + 1, datagram->index);
            memcpy(cur_data + 2, datagram->address, EC_ADDR_LEN);
            EC_WRITE_U16(cur_data + 6, datagram->data_size & 0x7FF);
            EC_WRITE_U16(cur_data + 8, 0x0000);
            follows_word = cur_data + 6;
            cur_data += EC_DATAGRAM_HEADER_SIZE;

            // EtherCAT datagram data
            memcpy(cur_data, datagram->data, datagram->data_size);
            cur_data += datagram->data_size;

            // EtherCAT datagram footer
            EC_WRITE_U16(cur_data, 0x0000); // reset working counter
            cur_data += EC_DATAGRAM_FOOTER_SIZE;
        }

break_send:
        if (list_empty(&sent_datagrams)) {
            EC_MASTER_DBG(master, 2, "nothing to send.\n");
            break;
        }

        // EtherCAT frame header
        EC_WRITE_U16(frame_data, ((cur_data - frame_data
                        - EC_FRAME_HEADER_SIZE) & 0x7FF) | 0x1000);

        // pad frame
        while (cur_data - frame_data < ETH_ZLEN - ETH_HLEN)
            EC_WRITE_U8(cur_data++, 0x00);

        EC_MASTER_DBG(master, 2, "frame size: %zu\n", cur_data - frame_data);

        // send frame
        ec_device_send(&master->devices[device_index],
                cur_data - frame_data);
        /* preamble and inter-frame gap */
        sent_bytes += ETH_HLEN + cur_data - frame_data + ETH_FCS_LEN + 20;
#ifdef EC_HAVE_CYCLES
        cycles_sent = get_cycles();
#endif
        jiffies_sent = jiffies;

        // set datagram states and sending timestamps
        list_for_each_entry_safe(datagram, next, &sent_datagrams, sent) {
            datagram->state = EC_DATAGRAM_SENT;
#ifdef EC_HAVE_CYCLES
            datagram->cycles_sent = cycles_sent;
#endif
            datagram->jiffies_sent = jiffies_sent;
            datagram->app_time_sent = master->app_time;
            list_del_init(&datagram->sent); // empty list of sent datagrams
        }

        frame_count++;
    }
    while (more_datagrams_waiting && frame_count < EC_TX_RING_SIZE);

#ifdef EC_HAVE_CYCLES
    if (unlikely(master->debug_level > 1)) {
        cycles_end = get_cycles();
        EC_MASTER_DBG(master, 0, "%s()"
                " sent %u frames in %uus.\n", __func__, frame_count,
               (unsigned int) (cycles_end - cycles_start) * 1000 / cpu_khz);
    }
#endif
    return sent_bytes;
}

/*****************************************************************************/

/** Processes a received frame.
 *
 * This function is called by the network driver for every received frame.
 *
 * \return 0 in case of success, else < 0
 */
void ec_master_receive_datagrams(
        ec_master_t *master, /**< EtherCAT master */
        ec_device_t *device, /**< EtherCAT device */
        const uint8_t *frame_data, /**< frame data */
        size_t size /**< size of the received data */
        )
{
    size_t frame_size, data_size;
    uint8_t datagram_type, datagram_index, datagram_mbox_prot;
#ifdef EC_EOE
    uint8_t eoe_type;
#endif
    unsigned int cmd_follows, datagram_slave_addr, datagram_offset_addr, datagram_wc, matched;
    const uint8_t *cur_data;
    ec_datagram_t *datagram;
    ec_slave_t *slave;

    if (unlikely(size < EC_FRAME_HEADER_SIZE)) {
        if (master->debug_level || FORCE_OUTPUT_CORRUPTED) {
            EC_MASTER_DBG(master, 0, "Corrupted frame received"
                    " on %s (size %zu < %u byte):\n",
                    device->dev->name, size, EC_FRAME_HEADER_SIZE);
            ec_print_data(frame_data, size);
        }
        master->stats.corrupted++;
#ifdef EC_RT_SYSLOG
        ec_master_output_stats(master);
#endif
        return;
    }

    cur_data = frame_data;

    // check length of entire frame
    frame_size = EC_READ_U16(cur_data) & 0x07FF;
    cur_data += EC_FRAME_HEADER_SIZE;

    if (unlikely(frame_size > size)) {
        if (master->debug_level || FORCE_OUTPUT_CORRUPTED) {
            EC_MASTER_DBG(master, 0, "Corrupted frame received"
                    " on %s (invalid frame size %zu for "
                    "received size %zu):\n", device->dev->name,
                    frame_size, size);
            ec_print_data(frame_data, size);
        }
        master->stats.corrupted++;
#ifdef EC_RT_SYSLOG
        ec_master_output_stats(master);
#endif
        return;
    }

    cmd_follows = 1;
    while (cmd_follows) {
        // process datagram header
        datagram_type  = EC_READ_U8 (cur_data);
        datagram_index = EC_READ_U8 (cur_data + 1);
        datagram_slave_addr  = EC_READ_U16(cur_data + 2);
        datagram_offset_addr = EC_READ_U16(cur_data + 4);
        data_size      = EC_READ_U16(cur_data + 6) & 0x07FF;
        cmd_follows    = EC_READ_U16(cur_data + 6) & 0x8000;
        cur_data += EC_DATAGRAM_HEADER_SIZE;

        if (unlikely(cur_data - frame_data
                     + data_size + EC_DATAGRAM_FOOTER_SIZE > size)) {
            if (master->debug_level || FORCE_OUTPUT_CORRUPTED) {
                EC_MASTER_DBG(master, 0, "Corrupted frame received"
                        " on %s (invalid data size %zu):\n",
                        device->dev->name, data_size);
                ec_print_data(frame_data, size);
            }
            master->stats.corrupted++;
#ifdef EC_RT_SYSLOG
            ec_master_output_stats(master);
#endif
            return;
        }

        // search for matching datagram in the queue
        matched = 0;
        list_for_each_entry(datagram, &master->datagram_queue, queue) {
            if (datagram->index == datagram_index
                && datagram->state == EC_DATAGRAM_SENT
                && datagram->type == datagram_type
                && datagram->data_size == data_size) {
                matched = 1;
                break;
            }
        }

        // no matching datagram was found
        if (!matched) {
            master->stats.unmatched++;
#ifdef EC_RT_SYSLOG
            ec_master_output_stats(master);
#endif

            if (unlikely(master->debug_level > 0)) {
                EC_MASTER_DBG(master, 0, "UNMATCHED datagram:\n");
                ec_print_data(cur_data - EC_DATAGRAM_HEADER_SIZE,
                        EC_DATAGRAM_HEADER_SIZE + data_size
                        + EC_DATAGRAM_FOOTER_SIZE);
#ifdef EC_DEBUG_RING
                ec_device_debug_ring_print(&master->devices[EC_DEVICE_MAIN]);
#endif
            }

            cur_data += data_size + EC_DATAGRAM_FOOTER_SIZE;
            continue;
        }

        if (datagram->type != EC_DATAGRAM_APWR &&
                datagram->type != EC_DATAGRAM_FPWR &&
                datagram->type != EC_DATAGRAM_BWR &&
                datagram->type != EC_DATAGRAM_LWR) {

            // common mailbox dispatcher for mailboxes read using the physical slave address
            if (datagram->type == EC_DATAGRAM_FPRD) {
                datagram_wc = EC_READ_U16(cur_data + data_size);
                if (datagram_wc) {
                    if (master->slaves != NULL) {
                        for (slave = master->slaves; slave < master->slaves + master->slave_count; slave++) {
                            if (slave->station_address == datagram_slave_addr) {
                                break;
                            }
                        }
                        if (slave->station_address == datagram_slave_addr) {
                            if (slave->configured_tx_mailbox_offset != 0) {
                                if (datagram_offset_addr == slave->configured_tx_mailbox_offset) {
                                    if (slave->valid_mbox_data) {
                                        // check if the mailbox header slave address is the
                                        // MBox Gateway addr offset above the slave position, and
                                        // a valid MBox Gateway address
                                        // Note: the datagram station address is the slave position + 1
                                        // Note: the EL6614 EoE module does not fill in the MailBox Header
                                        //   Address value in the EoE response.  Other modules / protocols
                                        //   may do the same.
                                        if (unlikely(
                                                (EC_READ_U16(cur_data + 2) == datagram_slave_addr + EC_MBG_SLAVE_ADDR_OFFSET - 1) &&
                                                (EC_READ_U16(cur_data + 2) >= EC_MBG_SLAVE_ADDR_OFFSET) )) {
                                            // EtherCAT Mailbox Gateway response
                                            if ((slave->mbox_mbg_data.data) && (data_size <= slave->mbox_mbg_data.data_size)) {
                                                memcpy(slave->mbox_mbg_data.data, cur_data, data_size);
                                                slave->mbox_mbg_data.payload_size = data_size;
                                            }
                                        } else {
                                            datagram_mbox_prot = EC_READ_U8(cur_data + 5) & 0x0F;
                                            switch (datagram_mbox_prot) {
#ifdef EC_EOE
                                            case EC_MBOX_TYPE_EOE:
                                                    // check EOE type and store in correct handlers mbox data cache
                                                    eoe_type = EC_READ_U8(cur_data + 6) & 0x0F;

                                                    switch (eoe_type) {

                                                    case EC_EOE_TYPE_FRAME_FRAG:
                                                        // EoE Frame Fragment handler
                                                        if ((slave->mbox_eoe_frag_data.data) && (data_size <= slave->mbox_eoe_frag_data.data_size)) {
                                                            memcpy(slave->mbox_eoe_frag_data.data, cur_data, data_size);
                                                            slave->mbox_eoe_frag_data.payload_size = data_size;
                                                        }
                                                        break;
                                                    case EC_EOE_TYPE_INIT_RES:
                                                        // EoE Init / Set IP response handler
                                                        if ((slave->mbox_eoe_init_data.data) && (data_size <= slave->mbox_eoe_init_data.data_size)) {
                                                            memcpy(slave->mbox_eoe_init_data.data, cur_data, data_size);
                                                            slave->mbox_eoe_init_data.payload_size = data_size;
                                                        }
                                                        break;
                                                    default:
                                                        EC_MASTER_DBG(master, 1, "Unhandled EoE protocol type from slave: %u Protocol: %u, Type: %x\n",
                                                                datagram_slave_addr, datagram_mbox_prot, eoe_type);
                                                        // copy instead received data into the datagram memory.
                                                        memcpy(datagram->data, cur_data, data_size);
                                                        break;
                                                }
                                                break;
#endif
                                            case EC_MBOX_TYPE_COE:
                                                if ((slave->mbox_coe_data.data) && (data_size <= slave->mbox_coe_data.data_size)) {
                                                    memcpy(slave->mbox_coe_data.data, cur_data, data_size);
                                                    slave->mbox_coe_data.payload_size = data_size;
                                                }
                                                break;
                                            case EC_MBOX_TYPE_FOE:
                                                if ((slave->mbox_foe_data.data) && (data_size <= slave->mbox_foe_data.data_size)) {
                                                    memcpy(slave->mbox_foe_data.data, cur_data, data_size);
                                                    slave->mbox_foe_data.payload_size = data_size;
                                                }
                                                break;
                                            case EC_MBOX_TYPE_SOE:
                                                if ((slave->mbox_soe_data.data) && (data_size <= slave->mbox_soe_data.data_size)) {
                                                    memcpy(slave->mbox_soe_data.data, cur_data, data_size);
                                                    slave->mbox_soe_data.payload_size = data_size;
                                                }
                                                break;
                                            case EC_MBOX_TYPE_VOE:
                                                if ((slave->mbox_voe_data.data) && (data_size <= slave->mbox_voe_data.data_size)) {
                                                    memcpy(slave->mbox_voe_data.data, cur_data, data_size);
                                                    slave->mbox_voe_data.payload_size = data_size;
                                                }
                                                break;
                                            default:
                                                EC_MASTER_DBG(master, 1, "Unknown mailbox protocol from slave: %u Protocol: %u\n", datagram_slave_addr, datagram_mbox_prot);
                                                // copy instead received data into the datagram memory.
                                                memcpy(datagram->data, cur_data, data_size);
                                                break;
                                            }
                                        }
                                    } else {
                                        // copy instead received data into the datagram memory.
                                        memcpy(datagram->data, cur_data, data_size);
                                    }
                                } else {
                                    // copy instead received data into the datagram memory.
                                    memcpy(datagram->data, cur_data, data_size);
                                }
                            } else {
                                // copy instead received data into the datagram memory.
                                memcpy(datagram->data, cur_data, data_size);
                            }
                        } else {
                            EC_MASTER_DBG(master, 1, "No slave matching datagram slave address: %u\n", datagram_slave_addr);
                        }
                    } else {
                        EC_MASTER_DBG(master, 1, "No configured slaves!\n");
                        // copy instead received data into the datagram memory.
                        memcpy(datagram->data, cur_data, data_size);
                    }
                } else {
                    // copy instead received data into the datagram memory.
                    memcpy(datagram->data, cur_data, data_size);
                }
            } else {
                // copy instead received data into the datagram memory.
                memcpy(datagram->data, cur_data, data_size);
            }
        }
        cur_data += data_size;

        // set the datagram's working counter
        datagram->working_counter = EC_READ_U16(cur_data);
        cur_data += EC_DATAGRAM_FOOTER_SIZE;

#ifdef EC_HAVE_CYCLES
        datagram->cycles_received =
            master->devices[EC_DEVICE_MAIN].cycles_poll;
#endif
        datagram->jiffies_received =
            master->devices[EC_DEVICE_MAIN].jiffies_poll;

        barrier(); /* reordering might lead to races */

        // dequeue the received datagram
        datagram->state = EC_DATAGRAM_RECEIVED;
        list_del_init(&datagram->queue);
    }
}

/*****************************************************************************/

/** Output master statistics.
 *
 * This function outputs statistical data on demand, but not more often than
 * necessary. The output happens at most once a second.
 */
void ec_master_output_stats(ec_master_t *master /**< EtherCAT master */)
{
    if (unlikely(jiffies - master->stats.output_jiffies >= HZ)) {
        if (!master->scan_busy || (master->debug_level > 0)) {
            master->stats.output_jiffies = jiffies;
            if (master->stats.timeouts) {
                EC_MASTER_WARN(master, "%u datagram%s TIMED OUT!\n",
                        master->stats.timeouts,
                        master->stats.timeouts == 1 ? "" : "s");
                master->stats.timeouts = 0;
            }
            if (master->stats.corrupted) {
                EC_MASTER_WARN(master, "%u frame%s CORRUPTED!\n",
                        master->stats.corrupted,
                        master->stats.corrupted == 1 ? "" : "s");
                master->stats.corrupted = 0;
            }
            if (master->stats.unmatched) {
                EC_MASTER_WARN(master, "%u datagram%s UNMATCHED!\n",
                        master->stats.unmatched,
                        master->stats.unmatched == 1 ? "" : "s");
                master->stats.unmatched = 0;
            }
        }
    }
}

/*****************************************************************************/

/** Clears the common device statistics.
 */
void ec_master_clear_device_stats(
        ec_master_t *master /**< EtherCAT master */
        )
{
    unsigned int i;

    // zero frame statistics
    master->device_stats.tx_count = 0;
    master->device_stats.last_tx_count = 0;
    master->device_stats.rx_count = 0;
    master->device_stats.last_rx_count = 0;
    master->device_stats.tx_bytes = 0;
    master->device_stats.last_tx_bytes = 0;
    master->device_stats.rx_bytes = 0;
    master->device_stats.last_rx_bytes = 0;
    master->device_stats.last_loss = 0;

    for (i = 0; i < EC_RATE_COUNT; i++) {
        master->device_stats.tx_frame_rates[i] = 0;
        master->device_stats.rx_frame_rates[i] = 0;
        master->device_stats.tx_byte_rates[i] = 0;
        master->device_stats.rx_byte_rates[i] = 0;
        master->device_stats.loss_rates[i] = 0;
    }

    master->device_stats.jiffies = 0;
}

/*****************************************************************************/

/** Updates the common device statistics.
 */
void ec_master_update_device_stats(
        ec_master_t *master /**< EtherCAT master */
        )
{
    ec_device_stats_t *s = &master->device_stats;
    s32 tx_frame_rate, rx_frame_rate, tx_byte_rate, rx_byte_rate, loss_rate;
    u64 loss;
    unsigned int i, dev_idx;

    // frame statistics
    if (likely(jiffies - s->jiffies < HZ)) {
        return;
    }

    tx_frame_rate = (s->tx_count - s->last_tx_count) * 1000;
    rx_frame_rate = (s->rx_count - s->last_rx_count) * 1000;
    tx_byte_rate = s->tx_bytes - s->last_tx_bytes;
    rx_byte_rate = s->rx_bytes - s->last_rx_bytes;
    loss = s->tx_count - s->rx_count;
    loss_rate = (loss - s->last_loss) * 1000;

    /* Low-pass filter:
     *      Y_n = y_(n - 1) + T / tau * (x - y_(n - 1))   | T = 1
     *   -> Y_n += (x - y_(n - 1)) / tau
     */
    for (i = 0; i < EC_RATE_COUNT; i++) {
        s32 n = rate_intervals[i];
        s->tx_frame_rates[i] += (tx_frame_rate - s->tx_frame_rates[i]) / n;
        s->rx_frame_rates[i] += (rx_frame_rate - s->rx_frame_rates[i]) / n;
        s->tx_byte_rates[i] += (tx_byte_rate - s->tx_byte_rates[i]) / n;
        s->rx_byte_rates[i] += (rx_byte_rate - s->rx_byte_rates[i]) / n;
        s->loss_rates[i] += (loss_rate - s->loss_rates[i]) / n;
    }

    s->last_tx_count = s->tx_count;
    s->last_rx_count = s->rx_count;
    s->last_tx_bytes = s->tx_bytes;
    s->last_rx_bytes = s->rx_bytes;
    s->last_loss = loss;

    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        ec_device_update_stats(&master->devices[dev_idx]);
    }

    s->jiffies = jiffies;
}

/*****************************************************************************/

#ifdef EC_USE_HRTIMER

/*
 * Sleep related functions:
 */
static enum hrtimer_restart ec_master_nanosleep_wakeup(struct hrtimer *timer)
{
    struct hrtimer_sleeper *t =
        container_of(timer, struct hrtimer_sleeper, timer);
    struct task_struct *task = t->task;

    t->task = NULL;
    if (task)
        wake_up_process(task);

    return HRTIMER_NORESTART;
}

/*****************************************************************************/

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)

/* compatibility with new hrtimer interface */
static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
    return timer->expires;
}

/*****************************************************************************/

static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time)
{
    timer->expires = time;
}

#endif

/*****************************************************************************/

void ec_master_nanosleep(const unsigned long nsecs)
{
    struct hrtimer_sleeper t;
    enum hrtimer_mode mode = HRTIMER_MODE_REL;

    hrtimer_init(&t.timer, CLOCK_MONOTONIC, mode);
    t.timer.function = ec_master_nanosleep_wakeup;
    t.task = current;
#ifdef CONFIG_HIGH_RES_TIMERS
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 24)
    t.timer.cb_mode = HRTIMER_CB_IRQSAFE_NO_RESTART;
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 26)
    t.timer.cb_mode = HRTIMER_CB_IRQSAFE_NO_SOFTIRQ;
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 28)
    t.timer.cb_mode = HRTIMER_CB_IRQSAFE_UNLOCKED;
#endif
#endif
    hrtimer_set_expires(&t.timer, ktime_set(0, nsecs));

    do {
        set_current_state(TASK_INTERRUPTIBLE);
        hrtimer_start(&t.timer, hrtimer_get_expires(&t.timer), mode);

        if (likely(t.task))
            schedule();

        hrtimer_cancel(&t.timer);
        mode = HRTIMER_MODE_ABS;

    } while (t.task && !signal_pending(current));
}

/*****************************************************************************/

/** Sleep timer.
 */
static ktime_t ec_master_nanosleep_timer(struct hrtimer_sleeper *t, ktime_t ideal_time, const unsigned long nsecs)
{
    t->task = current;
    ideal_time = ktime_add_ns(ideal_time, nsecs);
    hrtimer_set_expires(&t->timer, ideal_time);

    do {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
        set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
        hrtimer_start_expires(&t->timer, HRTIMER_MODE_ABS);

        if (likely(t->task))
            schedule();
#else
        set_current_state(TASK_INTERRUPTIBLE);
        hrtimer_start_expires(&t->timer, HRTIMER_MODE_ABS);

        if (likely(t->task))
            freezable_schedule();
#endif
        hrtimer_cancel(&t->timer);

    } while (t->task && !signal_pending(current));
    __set_current_state(TASK_RUNNING);

    return ideal_time;
}

/*****************************************************************************/

/** Create time with usec precision.
 */
static inline ktime_t us_to_ktime(u64 us)
{
	static const ktime_t ktime_zero = 0;
	return ktime_add_us(ktime_zero, us);
}

#endif // EC_USE_HRTIMER

/*****************************************************************************/

/* compatibility for priority changes */
static inline void set_normal_priority(struct task_struct *p, int nice)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    sched_set_normal(p, nice);
#else
    struct sched_param param = { .sched_priority = 0 };
    sched_setscheduler(p, SCHED_NORMAL, &param);
    set_user_nice(p, nice);
#endif
}

/*****************************************************************************/

/** Execute slave FSMs.
 */
void ec_master_exec_slave_fsms(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_datagram_t *datagram;
    ec_fsm_slave_t *fsm, *next;
    unsigned int count = 0;

    list_for_each_entry_safe(fsm, next, &master->fsm_exec_list, list) {
        if (!fsm->datagram) {
            EC_MASTER_WARN(master, "Slave %s-%u FSM has zero datagram."
                    "This is a bug!\n", ec_device_names[fsm->slave->device_index!=0],
                    fsm->slave->ring_position);
            list_del_init(&fsm->list);
            master->fsm_exec_count--;
            return;
        }

        if (fsm->datagram->state == EC_DATAGRAM_INIT ||
                fsm->datagram->state == EC_DATAGRAM_QUEUED ||
                fsm->datagram->state == EC_DATAGRAM_SENT) {
            // previous datagram was not sent or received yet.
            // wait until next thread execution
            return;
        }

        datagram = ec_master_get_external_datagram(master);
        if (!datagram) {
            // no free datagrams at the moment
            EC_MASTER_WARN(master, "No free datagram during"
                    " slave FSM execution. This is a bug!\n");
            continue;
        }

#if DEBUG_INJECT
        EC_MASTER_DBG(master, 1, "Executing slave %s-%u FSM.\n",
                ec_device_names[fsm->slave->device_index!=0],
                fsm->slave->ring_position);
#endif
        if (ec_fsm_slave_exec(fsm, datagram)) {
            if (datagram->state != EC_DATAGRAM_INVALID) {
                // FSM consumed datagram
#if DEBUG_INJECT
                EC_MASTER_DBG(master, 1, "FSM consumed datagram %s\n",
                        datagram->name);
#endif
                master->ext_ring_idx_fsm =
                    (master->ext_ring_idx_fsm + 1) % EC_EXT_RING_SIZE;
            }
        }
        else {
            // FSM finished
            list_del_init(&fsm->list);
            master->fsm_exec_count--;
#if DEBUG_INJECT
            EC_MASTER_DBG(master, 1, "FSM finished. %u remaining.\n",
                    master->fsm_exec_count);
#endif
        }
    }

    while (master->fsm_exec_count < EC_EXT_RING_SIZE / 2
            && count < master->slave_count) {

        if (ec_fsm_slave_is_ready(&master->fsm_slave->fsm)) {
            datagram = ec_master_get_external_datagram(master);

            if (ec_fsm_slave_exec(&master->fsm_slave->fsm, datagram)) {
                if (datagram->state != EC_DATAGRAM_INVALID) {
                    master->ext_ring_idx_fsm =
                        (master->ext_ring_idx_fsm + 1) % EC_EXT_RING_SIZE;
                }
                list_add_tail(&master->fsm_slave->fsm.list,
                        &master->fsm_exec_list);
                master->fsm_exec_count++;
#if DEBUG_INJECT
                EC_MASTER_DBG(master, 1, "New slave %s-%u FSM"
                        " consumed datagram %s, now %u FSMs in list.\n",
                        ec_device_names[master->fsm_slave->device_index!=0],
                        master->fsm_slave->ring_position, datagram->name,
                        master->fsm_exec_count);
#endif
            }
        }

        master->fsm_slave++;
        if (master->fsm_slave >= master->slaves + master->slave_count) {
            master->fsm_slave = master->slaves;
        }
        count++;
    }
}

/*****************************************************************************/

/** Master kernel thread function for IDLE phase.
 */
static int ec_master_idle_thread(void *priv_data)
{
    ec_master_t *master = (ec_master_t *) priv_data;
    int fsm_exec;
    size_t sent_bytes;

    // send interval in IDLE phase
    ec_master_set_send_interval(master, 1000000 / HZ);

    EC_MASTER_DBG(master, 1, "Idle thread running with send interval = %u us,"
            " max data size=%zu\n", master->send_interval,
            master->max_queue_size);

    while (!kthread_should_stop()) {
        ec_datagram_output_stats(&master->fsm_datagram);

        // receive
        ec_lock_down(&master->io_sem);
        ecrt_master_receive(master);
        ec_lock_up(&master->io_sem);

        // execute master & slave state machines
        if (ec_lock_down_interruptible(&master->master_sem)) {
            break;
        }

        fsm_exec = ec_fsm_master_exec(&master->fsm);

        // idle thread will still be in charge of calling the slave requests
        ec_master_exec_slave_fsms(master);

        ec_lock_up(&master->master_sem);

        // queue and send
        ec_lock_down(&master->io_sem);
        if (fsm_exec) {
            ec_master_queue_datagram(master, &master->fsm_datagram);
        }
        sent_bytes = ecrt_master_send(master);
        ec_lock_up(&master->io_sem);

        if (ec_fsm_master_idle(&master->fsm)) {
#ifdef EC_USE_HRTIMER
            ec_master_nanosleep(master->send_interval * 1000);
#else
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(1);
#endif
        } else {
#ifdef EC_USE_HRTIMER
            ec_master_nanosleep(
                    sent_bytes * EC_BYTE_TRANSMISSION_TIME_NS * 6 / 5);
#else
            schedule();
#endif
        }
    }

    EC_MASTER_DBG(master, 1, "Master IDLE thread exiting...\n");

    return 0;
}

/*****************************************************************************/

/** Master kernel thread function for OPERATION phase.
 */
static int ec_master_operation_thread(void *priv_data)
{
    ec_master_t *master = (ec_master_t *) priv_data;
#ifdef EC_USE_HRTIMER
    struct hrtimer_sleeper t;
    ktime_t ideal_time;
    s64 start_time;
#endif

    EC_MASTER_DBG(master, 1, "Operation thread running"
            " with fsm interval = %u us, max data size=%zu\n",
            master->send_interval, master->max_queue_size);

#ifdef EC_USE_HRTIMER
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
    hrtimer_init_sleeper(&t, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
#else
    hrtimer_init_sleeper(&t, CLOCK_MONOTONIC, HRTIMER_MODE_ABS, current);
#endif
    // wait till the next millisecond, before entering operation loop
    ideal_time = t.timer.base->get_time();
    start_time = ktime_to_us(ideal_time);
    ideal_time = us_to_ktime(start_time + 1);
    ec_master_nanosleep_timer(&t, ideal_time, 0);
#endif

    while (!kthread_should_stop()) {
        ec_datagram_output_stats(&master->fsm_datagram);

        if (master->injection_seq_rt == master->injection_seq_fsm) {
            // output statistics
            ec_master_output_stats(master);

            // execute master & slave state machines
            if (ec_lock_down_interruptible(&master->master_sem)) {
                break;
            }

            if (ec_fsm_master_exec(&master->fsm)) {
                // Inject datagrams (let the RT thread queue them, see
                // ecrt_master_send())
                master->injection_seq_fsm++;
            }

            // if rt_slave_requests is true and the slaves are available
            // this will be handled by the app explicitly calling
            // ecrt_master_exec_slave_request()
            if (!master->rt_slave_requests || !master->rt_slaves_available) {
                ec_master_exec_slave_fsms(master);
            }

            ec_lock_up(&master->master_sem);
        }

#ifdef EC_USE_HRTIMER
        // the op thread should not work faster than the sending RT thread
        // ec_master_nanosleep(master->send_interval * 1000);
        ideal_time = ec_master_nanosleep_timer(&t, ideal_time, master->send_interval * 1000);
#else
        if (ec_fsm_master_idle(&master->fsm)) {
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(1);
        }
        else {
            schedule();
        }
#endif
    }

    EC_MASTER_DBG(master, 1, "Master OP thread exiting...\n");
    return 0;
}

/*****************************************************************************/

#ifdef EC_EOE
/** Starts Ethernet over EtherCAT processing on demand.
 */
void ec_master_eoe_start(ec_master_t *master /**< EtherCAT master */)
{
    if (master->eoe_thread) {
        EC_MASTER_WARN(master, "EoE already running!\n");
        return;
    }

    if (list_empty(&master->eoe_handlers)) {
        return;
    }

    if (!master->send_cb || !master->receive_cb) {
        EC_MASTER_WARN(master, "EoE External processing"
                " required!\n");
        return;
    }

    EC_MASTER_INFO(master, "Starting EoE thread.\n");

    master->eoe_thread = kthread_run(ec_master_eoe_thread, master,
            "EtherCAT-EoE");
    if (IS_ERR(master->eoe_thread)) {
        int err = (int) PTR_ERR(master->eoe_thread);
        EC_MASTER_ERR(master, "Failed to start EoE thread (error %i)!\n",
                err);
        master->eoe_thread = NULL;
        return;
    }

    set_normal_priority(master->eoe_thread, 0);
}

/*****************************************************************************/

/** Stops the Ethernet over EtherCAT processing.
 */
void ec_master_eoe_stop(ec_master_t *master /**< EtherCAT master */)
{
    if (master->eoe_thread) {
        EC_MASTER_INFO(master, "Stopping EoE thread.\n");

        kthread_stop(master->eoe_thread);
        master->eoe_thread = NULL;
        EC_MASTER_INFO(master, "EoE thread exited.\n");
    }
}

/*****************************************************************************/

#ifdef EC_RTDM

/** Check if any EOE handlers are open.
 *
 * \return 1 if any eoe handlers are open, zero if not,
 *   otherwise a negative error code.
 */
int ec_master_eoe_is_open(ec_master_t *master /**< EtherCAT master */)
{
    ec_eoe_t *eoe;

    // check that eoe is not already being processed by the master
    // and that we can currently process EoE
    if ( (master->phase != EC_OPERATION) || master->eoe_thread ||
            !master->rt_slaves_available ) {
        // protocol not available
        return -ENOPROTOOPT;
    }

    ec_lock_down(&master->master_sem);
    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        if (ec_eoe_is_open(eoe)) {
            ec_lock_up(&master->master_sem);
            return 1;
        }
    }
    ec_lock_up(&master->master_sem);

    return 0;
}

/*****************************************************************************/

/** Check if any EOE handlers are open.
 *
 * \return 1 if something to send +
 *   2 if an eoe handler has something still pending
 */
int ec_master_eoe_process(ec_master_t *master /**< EtherCAT master */)
{
    ec_eoe_t *eoe;
    int sth_to_send = 0;
    int sth_pending = 0;

    // check that eoe is not already being processed by the master
    if (master->eoe_thread) {
        return 0;
    }

     // actual EoE processing
    ec_lock_down(&master->master_sem);
    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        if ( eoe->slave &&
             ( (eoe->slave->current_state == EC_SLAVE_STATE_PREOP) ||
               (eoe->slave->current_state == EC_SLAVE_STATE_SAFEOP) ||
               (eoe->slave->current_state == EC_SLAVE_STATE_OP) ) ) {
            ec_eoe_run(eoe);
            if (eoe->queue_datagram) {
                sth_to_send = EOE_STH_TO_SEND;
            }
            if (!ec_eoe_is_idle(eoe)) {
                sth_pending = EOE_STH_PENDING;
            }
        }
    }

    if (sth_to_send) {
        list_for_each_entry(eoe, &master->eoe_handlers, list) {
            ec_eoe_queue(eoe);
        }
    }
    ec_lock_up(&master->master_sem);

    return sth_to_send + sth_pending;
}

#endif

/*****************************************************************************/

/** Does the Ethernet over EtherCAT processing.
 */
static int ec_master_eoe_thread(void *priv_data)
{
    ec_master_t *master = (ec_master_t *) priv_data;
    ec_eoe_t *eoe;
    unsigned int none_open, sth_to_send, all_idle;

    EC_MASTER_DBG(master, 1, "EoE thread running.\n");

    while (!kthread_should_stop()) {
        none_open = 1;
        all_idle = 1;

        ec_lock_down(&master->master_sem);
        list_for_each_entry(eoe, &master->eoe_handlers, list) {
            if (ec_eoe_is_open(eoe)) {
                none_open = 0;
                break;
            }
        }
        ec_lock_up(&master->master_sem);

        if (none_open) {
            goto schedule;
        }

        // receive datagrams
        master->receive_cb(master->cb_data);

        // actual EoE processing
        ec_lock_down(&master->master_sem);
        sth_to_send = 0;
        list_for_each_entry(eoe, &master->eoe_handlers, list) {
            if ( eoe->slave &&
                 ( (eoe->slave->current_state == EC_SLAVE_STATE_PREOP) ||
                   (eoe->slave->current_state == EC_SLAVE_STATE_SAFEOP) ||
                   (eoe->slave->current_state == EC_SLAVE_STATE_OP) ) ) {
                ec_eoe_run(eoe);
                if (eoe->queue_datagram) {
                    sth_to_send = 1;
                }
                if (!ec_eoe_is_idle(eoe)) {
                    all_idle = 0;
                }
            }
        }
        ec_lock_up(&master->master_sem);

        if (sth_to_send) {
            ec_lock_down(&master->master_sem);
            list_for_each_entry(eoe, &master->eoe_handlers, list) {
                ec_eoe_queue(eoe);
            }
            ec_lock_up(&master->master_sem);

            // (try to) send datagrams
            master->send_cb(master->cb_data);
        }

schedule:
        if (all_idle) {
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(1);
        } else {
            schedule();
        }
    }

    EC_MASTER_DBG(master, 1, "EoE thread exiting...\n");
    return 0;
}
#endif

/*****************************************************************************/

/** Attaches the slave configurations to the slaves.
 */
void ec_master_attach_slave_configs(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_slave_config_t *sc;

    list_for_each_entry(sc, &master->configs, list) {
        ec_slave_config_attach(sc);
    }
}

/*****************************************************************************/

/** Abort active requests for slave configs without attached slaves.
 */
void ec_master_expire_slave_config_requests(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_slave_config_t *sc;

    list_for_each_entry(sc, &master->configs, list) {
        ec_slave_config_expire_disconnected_requests(sc);
    }
}

/*****************************************************************************/

/** Common implementation for ec_master_find_slave()
 * and ec_master_find_slave_const().
 */
#define EC_FIND_SLAVE \
    do { \
        if (alias) { \
            for (; slave < master->slaves + master->slave_count; \
                    slave++) { \
                if (slave->effective_alias == alias) \
                break; \
            } \
            if (slave == master->slaves + master->slave_count) \
            return NULL; \
        } \
        \
        slave += position; \
        if (slave < master->slaves + master->slave_count) { \
            return slave; \
        } else { \
            return NULL; \
        } \
    } while (0)

/** Finds a slave in the bus, given the alias and position.
 *
 * \return Search result, or NULL.
 */
ec_slave_t *ec_master_find_slave(
        ec_master_t *master, /**< EtherCAT master. */
        uint16_t alias, /**< Slave alias. */
        uint16_t position /**< Slave position. */
        )
{
    ec_slave_t *slave = master->slaves;
    EC_FIND_SLAVE;
}

/** Finds a slave in the bus, given the alias and position.
 *
 * Const version.
 *
 * \return Search result, or NULL.
 */
const ec_slave_t *ec_master_find_slave_const(
        const ec_master_t *master, /**< EtherCAT master. */
        uint16_t alias, /**< Slave alias. */
        uint16_t position /**< Slave position. */
        )
{
    const ec_slave_t *slave = master->slaves;
    EC_FIND_SLAVE;
}

/*****************************************************************************/

/** Get the number of slave configurations provided by the application.
 *
 * \return Number of configurations.
 */
unsigned int ec_master_config_count(
        const ec_master_t *master /**< EtherCAT master. */
        )
{
    const ec_slave_config_t *sc;
    unsigned int count = 0;

    list_for_each_entry(sc, &master->configs, list) {
        count++;
    }

    return count;
}

/*****************************************************************************/

/** Common implementation for ec_master_get_config()
 * and ec_master_get_config_const().
 */
#define EC_FIND_CONFIG \
    do { \
        list_for_each_entry(sc, &master->configs, list) { \
            if (pos--) \
                continue; \
            return sc; \
        } \
        return NULL; \
    } while (0)

/** Get a slave configuration via its position in the list.
 *
 * \return Slave configuration or \a NULL.
 */
ec_slave_config_t *ec_master_get_config(
        const ec_master_t *master, /**< EtherCAT master. */
        unsigned int pos /**< List position. */
        )
{
    ec_slave_config_t *sc;
    EC_FIND_CONFIG;
}

/** Get a slave configuration via its position in the list.
 *
 * Const version.
 *
 * \return Slave configuration or \a NULL.
 */
const ec_slave_config_t *ec_master_get_config_const(
        const ec_master_t *master, /**< EtherCAT master. */
        unsigned int pos /**< List position. */
        )
{
    const ec_slave_config_t *sc;
    EC_FIND_CONFIG;
}

/*****************************************************************************/

/** Get the number of domains.
 *
 * \return Number of domains.
 */
unsigned int ec_master_domain_count(
        const ec_master_t *master /**< EtherCAT master. */
        )
{
    const ec_domain_t *domain;
    unsigned int count = 0;

    list_for_each_entry(domain, &master->domains, list) {
        count++;
    }

    return count;
}

/*****************************************************************************/

/** Common implementation for ec_master_find_domain() and
 * ec_master_find_domain_const().
 */
#define EC_FIND_DOMAIN \
    do { \
        list_for_each_entry(domain, &master->domains, list) { \
            if (index--) \
                continue; \
            return domain; \
        } \
        \
        return NULL; \
    } while (0)

/** Get a domain via its position in the list.
 *
 * \return Domain pointer, or \a NULL if not found.
 */
ec_domain_t *ec_master_find_domain(
        ec_master_t *master, /**< EtherCAT master. */
        unsigned int index /**< Domain index. */
        )
{
    ec_domain_t *domain;
    EC_FIND_DOMAIN;
}

/** Get a domain via its position in the list.
 *
 * Const version.
 *
 * \return Domain pointer, or \a NULL if not found.
 */
const ec_domain_t *ec_master_find_domain_const(
        const ec_master_t *master, /**< EtherCAT master. */
        unsigned int index /**< Domain index. */
        )
{
    const ec_domain_t *domain;
    EC_FIND_DOMAIN;
}

/*****************************************************************************/

#ifdef EC_EOE

/** Get the number of EoE handlers.
 *
 * \return Number of EoE handlers.
 */
uint16_t ec_master_eoe_handler_count(
        const ec_master_t *master /**< EtherCAT master. */
        )
{
    const ec_eoe_t *eoe;
    unsigned int count = 0;

    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        count++;
    }

    return count;
}

/*****************************************************************************/

/** Get an EoE handler via its position in the list.
 *
 * Const version.
 *
 * \return EoE handler pointer, or \a NULL if not found.
 */
const ec_eoe_t *ec_master_get_eoe_handler_const(
        const ec_master_t *master, /**< EtherCAT master. */
        uint16_t index /**< EoE handler index. */
        )
{
    const ec_eoe_t *eoe;

    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        if (index--)
            continue;
        return eoe;
    }

    return NULL;
}

#endif

/*****************************************************************************/

/** Set the debug level.
 *
 * \retval       0 Success.
 * \retval -EINVAL Invalid debug level.
 */
int ec_master_debug_level(
        ec_master_t *master, /**< EtherCAT master. */
        unsigned int level /**< Debug level. May be 0, 1 or 2. */
        )
{
    if (level > 2) {
        EC_MASTER_ERR(master, "Invalid debug level %u!\n", level);
        return -EINVAL;
    }

    if (level != master->debug_level) {
        master->debug_level = level;
        EC_MASTER_INFO(master, "Master debug level set to %u.\n",
                master->debug_level);
    }

    return 0;
}

/*****************************************************************************/

/** Finds the DC reference clock.
 */
void ec_master_find_dc_ref_clock(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_slave_t *slave, *ref = NULL;

    if (master->dc_ref_config) {
        // Check application-selected reference clock
        slave = master->dc_ref_config->slave;

        if (slave) {
            if (slave->base_dc_supported && slave->has_dc_system_time) {
                ref = slave;
                EC_MASTER_INFO(master, "Using slave %s-%u as application selected"
                        " DC reference clock.\n", ec_device_names[slave->device_index!=0],
                        ref->ring_position);
            }
            else {
                EC_MASTER_WARN(master, "Application selected slave %s-%u can not"
                        " act as a DC reference clock!", ec_device_names[slave->device_index!=0],
                        slave->ring_position);
            }
        }
        else {
            EC_MASTER_WARN(master, "Application selected DC reference clock"
                    " config (%u-%u) has no slave attached!\n",
                    master->dc_ref_config->alias,
                    master->dc_ref_config->position);
        }
    }

    if (!ref) {
        // Use first slave with DC support as reference clock
        for (slave = master->slaves;
                slave < master->slaves + master->slave_count;
                slave++) {
            if (slave->base_dc_supported && slave->has_dc_system_time) {
                ref = slave;
                break;
            }
        }

    }

    master->dc_ref_clock = ref;

    if (ref) {
        EC_MASTER_INFO(master, "Using slave %s-%u as DC reference clock.\n",
                ec_device_names[ref->device_index!=0], ref->ring_position);
    }
    else {
        EC_MASTER_INFO(master, "No DC reference clock found.\n");
    }

    // These calls always succeed, because the
    // datagrams have been pre-allocated.
    ec_datagram_fpwr(&master->ref_sync_datagram,
            ref ? ref->station_address : 0xffff, 0x0910, 4);
    ec_datagram_frmw(&master->sync_datagram,
            ref ? ref->station_address : 0xffff, 0x0910, 4);
    ec_datagram_fprd(&master->sync64_datagram,
            ref ? ref->station_address : 0xffff, 0x0910, 8);
}

/*****************************************************************************/

/** Calculates the bus topology; recursion function.
 *
 * \return Zero on success, otherwise a negative error code.
 */
int ec_master_calc_topology_rec(
        ec_master_t *master, /**< EtherCAT master. */
        ec_slave_t *upstream_slave, /**< Slave at upstream port. */
        unsigned int *slave_position /**< Slave position. */
        )
{
    ec_slave_t *slave = master->slaves + *slave_position;
    unsigned int port_index;
    int ret;

    static const unsigned int next_table[EC_MAX_PORTS] = {
        3, 2, 0, 1
    };

    slave->ports[slave->upstream_port].next_slave = upstream_slave;

    port_index = next_table[slave->upstream_port];
    while (port_index != slave->upstream_port) {
        if (!slave->ports[port_index].link.loop_closed) {
            *slave_position = *slave_position + 1;
            if (*slave_position < master->slave_count) {
                slave->ports[port_index].next_slave =
                    master->slaves + *slave_position;
                ret = ec_master_calc_topology_rec(master,
                        slave, slave_position);
                if (ret) {
                    return ret;
                }
            } else {
                return -1;
            }
        }

        port_index = next_table[port_index];
    }

    return 0;
}

/*****************************************************************************/

/** Calculates the bus topology.
 */
void ec_master_calc_topology(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    unsigned int slave_position = 0;
    ec_slave_t *slave;

    if (master->slave_count == 0)
        return;

    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_slave_calc_upstream_port(slave);
    }

    if (ec_master_calc_topology_rec(master, NULL, &slave_position))
        EC_MASTER_ERR(master, "Failed to calculate bus topology.\n");
}

/*****************************************************************************/

/** Calculates the bus transmission delays.
 */
void ec_master_calc_transmission_delays(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    ec_slave_t *slave;

    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_slave_calc_port_delays(slave);
    }

    if (master->dc_ref_clock) {
        uint32_t delay = 0;
        ec_slave_calc_transmission_delays_rec(master->dc_ref_clock, &delay);
    }
}

/*****************************************************************************/

/** Distributed-clocks calculations.
 */
void ec_master_calc_dc(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    // find DC reference clock
    ec_master_find_dc_ref_clock(master);

    // calculate bus topology
    ec_master_calc_topology(master);

    ec_master_calc_transmission_delays(master);
}

/*****************************************************************************/

/** Request OP state for configured slaves.
 */
void ec_master_request_op(
        ec_master_t *master /**< EtherCAT master. */
        )
{
    unsigned int i;
    ec_slave_t *slave;

    if (!master->active)
        return;

    EC_MASTER_DBG(master, 1, "Requesting OP...\n");

    // request OP for all configured slaves
    for (i = 0; i < master->slave_count; i++) {
        slave = master->slaves + i;
        if (slave->config) {
            ec_slave_request_state(slave, EC_SLAVE_STATE_OP);
        }
    }

#ifdef EC_REFCLKOP
    // always set DC reference clock to OP
    if (master->dc_ref_clock) {
        ec_slave_request_state(master->dc_ref_clock, EC_SLAVE_STATE_OP);
    }
#endif
}

/*****************************************************************************/

int ec_master_dict_upload(ec_master_t *master, uint16_t slave_position)
{
    ec_dict_request_t request;
    ec_slave_t *slave;
    int ret = 0;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p, slave_position = %u\n",
            __func__, master, slave_position);

    ec_dict_request_init(&request);
    ec_dict_request_read(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling dictionary upload request.\n");

    // schedule request.
    list_add_tail(&request.list, &slave->dict_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    if (request.state != EC_INT_REQUEST_SUCCESS) {
        ret = -EIO;
    }
    return ret;
}

/******************************************************************************
 *  Application interface
 *****************************************************************************/

/** Same as ecrt_master_create_domain(), but with ERR_PTR() return value.
 *
 * \return New domain, or ERR_PTR() return value.
 */
ec_domain_t *ecrt_master_create_domain_err(
        ec_master_t *master /**< master */
        )
{
    ec_domain_t *domain, *last_domain;
    unsigned int index;

    EC_MASTER_DBG(master, 1, "ecrt_master_create_domain(master = 0x%p)\n",
            master);

    if (!(domain =
                (ec_domain_t *) kmalloc(sizeof(ec_domain_t), GFP_KERNEL))) {
        EC_MASTER_ERR(master, "Error allocating domain memory!\n");
        return ERR_PTR(-ENOMEM);
    }

    ec_lock_down(&master->master_sem);

    if (list_empty(&master->domains)) {
        index = 0;
    } else {
        last_domain = list_entry(master->domains.prev, ec_domain_t, list);
        index = last_domain->index + 1;
    }

    ec_domain_init(domain, master, index);
    list_add_tail(&domain->list, &master->domains);

    ec_lock_up(&master->master_sem);

    EC_MASTER_DBG(master, 1, "Created domain %u.\n", domain->index);

    return domain;
}

/*****************************************************************************/

ec_domain_t *ecrt_master_create_domain(
        ec_master_t *master /**< master */
        )
{
    ec_domain_t *d = ecrt_master_create_domain_err(master);
    return IS_ERR(d) ? NULL : d;
}

/*****************************************************************************/

int ecrt_master_setup_domain_memory(ec_master_t *master)
{
	// not currently supported
    return -ENOMEM;  // FIXME
}

/*****************************************************************************/

int ecrt_master_activate(ec_master_t *master)
{
    uint32_t domain_offset;
    ec_domain_t *domain;
    int ret;
#ifdef EC_EOE
    int eoe_was_running;
#endif

    EC_MASTER_DBG(master, 1, "ecrt_master_activate(master = 0x%p)\n", master);

    if (master->active) {
        EC_MASTER_WARN(master, "%s: Master already active!\n", __func__);
        return 0;
    }

    ec_lock_down(&master->master_sem);

    // finish all domains
    domain_offset = 0;
    list_for_each_entry(domain, &master->domains, list) {
        ret = ec_domain_finish(domain, domain_offset);
        if (ret < 0) {
            ec_lock_up(&master->master_sem);
            EC_MASTER_ERR(master, "Failed to finish domain 0x%p!\n", domain);
            return ret;
        }
        domain_offset += domain->data_size;
    }

    ec_lock_up(&master->master_sem);

    // restart EoE process and master thread with new locking

    ec_master_thread_stop(master);
#ifdef EC_EOE
    eoe_was_running = (master->eoe_thread != NULL);
    ec_master_eoe_stop(master);
#endif

    EC_MASTER_DBG(master, 1, "FSM datagram is %p.\n", &master->fsm_datagram);

    master->injection_seq_fsm = 0;
    master->injection_seq_rt = 0;

    master->send_cb = master->app_send_cb;
    master->receive_cb = master->app_receive_cb;
    master->cb_data = master->app_cb_data;

#ifdef EC_EOE
    if (eoe_was_running) {
        ec_master_eoe_start(master);
    }
#endif
    ret = ec_master_thread_start(master, ec_master_operation_thread,
                "EtherCAT-OP");
    if (ret < 0) {
        EC_MASTER_ERR(master, "Failed to start master thread!\n");
        return ret;
    }

    /* Allow scanning after a topology change. */
    master->allow_scan = 1;

    master->active = 1;

    // notify state machine, that the configuration shall now be applied
    master->config_changed = 1;
    master->dc_offset_valid = 0;

    return 0;
}

/*****************************************************************************/

void ecrt_master_deactivate_slaves(ec_master_t *master)
{
    ec_slave_t *slave;
    ec_slave_config_t *sc, *next;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p)\n", __func__, master);

    if (!master->active) {
        EC_MASTER_WARN(master, "%s: Master not active.\n", __func__);
        return;
    }


    // clear dc settings on all slaves
    list_for_each_entry_safe(sc, next, &master->configs, list) {
        if (sc->dc_assign_activate) {
            ecrt_slave_config_dc(sc, 0x0000, 0, 0, 0, 0);
        }
    }


    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {

        // set states for all slaves
        ec_slave_request_state(slave, EC_SLAVE_STATE_PREOP);

        // mark for reconfiguration, because the master could have no
        // possibility for a reconfiguration between two sequential operation
        // phases.
        slave->force_config = 1;
    }
}

/*****************************************************************************/

void ecrt_master_deactivate(ec_master_t *master)
{
    ec_slave_t *slave;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p)\n", __func__, master);

    if (!master->active) {
        EC_MASTER_WARN(master, "%s: Master not active.\n", __func__);
        ec_master_clear_config(master);
        return;
    }

    ec_master_thread_stop(master);
#ifdef EC_EOE
    ec_master_eoe_stop(master);
#endif

    master->send_cb = ec_master_internal_send_cb;
    master->receive_cb = ec_master_internal_receive_cb;
    master->cb_data = master;

    ec_master_clear_config(master);

    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {

        // set states for all slaves
        ec_slave_request_state(slave, EC_SLAVE_STATE_PREOP);

        // clear read_mbox_busy flag in case slave CoE FSM were interrupted
        ec_read_mbox_lock_clear(slave);

        // mark for reconfiguration, because the master could have no
        // possibility for a reconfiguration between two sequential operation
        // phases.
        slave->force_config = 1;
    }

    master->app_time = 0ULL;
    master->dc_ref_time = 0ULL;
    master->dc_offset_valid = 0;

    /* Disallow scanning to get into the same state like after a master
     * request (after ec_master_enter_operation_phase() is called). */
    master->allow_scan = 0;

    master->active = 0;

#ifdef EC_EOE
    ec_master_eoe_start(master);
#endif
    if (ec_master_thread_start(master, ec_master_idle_thread,
                "EtherCAT-IDLE")) {
        EC_MASTER_WARN(master, "Failed to restart master thread!\n");
    }
}

/*****************************************************************************/

size_t ecrt_master_send(ec_master_t *master)
{
    ec_datagram_t *datagram, *n;
    ec_device_index_t dev_idx;
    size_t sent_bytes = 0;


    if (master->injection_seq_rt != master->injection_seq_fsm) {
        // inject datagram produced by master FSM
        ec_master_queue_datagram(master, &master->fsm_datagram);
        master->injection_seq_rt = master->injection_seq_fsm;
    }

    ec_master_inject_external_datagrams(master);

    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        if (unlikely(!master->devices[dev_idx].link_state)) {
            // link is down, no datagram can be sent
            list_for_each_entry_safe(datagram, n,
                    &master->datagram_queue, queue) {
                if (datagram->device_index == dev_idx) {
                    datagram->state = EC_DATAGRAM_ERROR;
                    list_del_init(&datagram->queue);
                }
            }

            if (!master->devices[dev_idx].dev) {
                continue;
            }

            // query link state
            ec_device_poll(&master->devices[dev_idx]);

            // clear frame statistics
            ec_device_clear_stats(&master->devices[dev_idx]);
            continue;
        }

        // send frames
        sent_bytes = max(sent_bytes,
            ec_master_send_datagrams(master, dev_idx));
    }

    return sent_bytes;
}

/*****************************************************************************/

void ecrt_master_receive(ec_master_t *master)
{
    unsigned int dev_idx;
    ec_datagram_t *datagram, *next;

    // receive datagrams
    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        ec_device_poll(&master->devices[dev_idx]);
    }
    ec_master_update_device_stats(master);

    // dequeue all datagrams that timed out
    list_for_each_entry_safe(datagram, next, &master->datagram_queue, queue) {
        if (datagram->state != EC_DATAGRAM_SENT) continue;

#ifdef EC_HAVE_CYCLES
        if (master->devices[EC_DEVICE_MAIN].cycles_poll -
                datagram->cycles_sent > timeout_cycles) {
#else
        if (master->devices[EC_DEVICE_MAIN].jiffies_poll -
                datagram->jiffies_sent > timeout_jiffies) {
#endif
            list_del_init(&datagram->queue);
            datagram->state = EC_DATAGRAM_TIMED_OUT;
            master->stats.timeouts++;

#ifdef EC_RT_SYSLOG
            ec_master_output_stats(master);

            if (unlikely(master->debug_level > 0)) {
                unsigned int time_us;
#ifdef EC_HAVE_CYCLES
                time_us = (unsigned int)
                    (master->devices[EC_DEVICE_MAIN].cycles_poll -
                        datagram->cycles_sent) * 1000 / cpu_khz;
#else
                time_us = (unsigned int)
                    ((master->devices[EC_DEVICE_MAIN].jiffies_poll -
                            datagram->jiffies_sent) * 1000000 / HZ);
#endif
                EC_MASTER_DBG(master, 0, "TIMED OUT datagram %p,"
                        " index %02X waited %u us.\n",
                        datagram, datagram->index, time_us);
            }
#endif /* RT_SYSLOG */
        }
    }
}

/*****************************************************************************/

size_t ecrt_master_send_ext(ec_master_t *master)
{
    ec_datagram_t *datagram, *next;

    ec_lock_down(&master->ext_queue_sem);

    list_for_each_entry_safe(datagram, next, &master->ext_datagram_queue,
            queue) {
        list_del(&datagram->queue);
        ec_master_queue_datagram(master, datagram);
    }
    ec_lock_up(&master->ext_queue_sem);

    return ecrt_master_send(master);
}

/*****************************************************************************/

/** Same as ecrt_master_slave_config(), but with ERR_PTR() return value.
 */
ec_slave_config_t *ecrt_master_slave_config_err(ec_master_t *master,
        uint16_t alias, uint16_t position, uint32_t vendor_id,
        uint32_t product_code)
{
    ec_slave_config_t *sc;
    unsigned int found = 0;


    EC_MASTER_DBG(master, 1, "ecrt_master_slave_config(master = 0x%p,"
            " alias = %u, position = %u, vendor_id = 0x%08x,"
            " product_code = 0x%08x)\n",
            master, alias, position, vendor_id, product_code);

    list_for_each_entry(sc, &master->configs, list) {
        if (sc->alias == alias && sc->position == position) {
            found = 1;
            break;
        }
    }

    if (found) { // config with same alias/position already existing
        if (sc->vendor_id != vendor_id || sc->product_code != product_code) {
            EC_MASTER_ERR(master, "Slave type mismatch. Slave was"
                    " configured as 0x%08X/0x%08X before. Now configuring"
                    " with 0x%08X/0x%08X.\n", sc->vendor_id, sc->product_code,
                    vendor_id, product_code);
            return ERR_PTR(-ENOENT);
        }
    } else {
        EC_MASTER_DBG(master, 1, "Creating slave configuration for %u:%u,"
                " 0x%08X/0x%08X.\n",
                alias, position, vendor_id, product_code);

        if (!(sc = (ec_slave_config_t *) kmalloc(sizeof(ec_slave_config_t),
                        GFP_KERNEL))) {
            EC_MASTER_ERR(master, "Failed to allocate memory"
                    " for slave configuration.\n");
            return ERR_PTR(-ENOMEM);
        }

        ec_slave_config_init(sc, master,
                alias, position, vendor_id, product_code);

        ec_lock_down(&master->master_sem);

        // try to find the addressed slave
        ec_slave_config_attach(sc);
        ec_slave_config_load_default_sync_config(sc);
        list_add_tail(&sc->list, &master->configs);

        ec_lock_up(&master->master_sem);
    }

    return sc;
}

/*****************************************************************************/

ec_slave_config_t *ecrt_master_slave_config(ec_master_t *master,
        uint16_t alias, uint16_t position, uint32_t vendor_id,
        uint32_t product_code)
{
    ec_slave_config_t *sc = ecrt_master_slave_config_err(master, alias,
            position, vendor_id, product_code);
    return IS_ERR(sc) ? NULL : sc;
}

/*****************************************************************************/

int ecrt_master_select_reference_clock(ec_master_t *master,
        ec_slave_config_t *sc)
{
    master->dc_ref_config = sc;

    if (master->dc_ref_config) {
        EC_MASTER_INFO(master, "Application selected DC reference clock"
                " config (%u-%u) set by application.\n",
                master->dc_ref_config->alias,
                master->dc_ref_config->position);
    }
    else {
        EC_MASTER_INFO(master, "Application selected DC reference clock"
                " config cleared by application.\n");
    }

    // update dc datagrams
    ec_master_find_dc_ref_clock(master);

    return 0;
}

/*****************************************************************************/

int ecrt_master(ec_master_t *master, ec_master_info_t *master_info)
{
    EC_MASTER_DBG(master, 1, "ecrt_master(master = 0x%p,"
            " master_info = 0x%p)\n", master, master_info);

    master_info->slave_count = master->slave_count;
    master_info->link_up = master->devices[EC_DEVICE_MAIN].link_state;
    master_info->scan_busy = master->scan_busy;
    master_info->app_time = master->app_time;
    memcpy(master_info->address, master->macs[EC_DEVICE_MAIN], sizeof(master_info->address));
    return 0;
}

/*****************************************************************************/

int ecrt_master_get_slave(ec_master_t *master, uint16_t slave_position,
        ec_slave_info_t *slave_info)
{
    const ec_slave_t *slave;
    unsigned int i;
    int ret = 0;

    if (ec_lock_down_interruptible(&master->master_sem)) {
        return -EINTR;
    }

    slave = ec_master_find_slave_const(master, 0, slave_position);

    if (slave == NULL) {
       ret = -ENOENT;
       goto out_get_slave;
    }

    if (slave->sii_image == NULL) {
        EC_MASTER_WARN(master, "Cannot access SII data from slave position %s-%u",
                ec_device_names[slave->device_index!=0], slave->ring_position);
        ret = -ENOENT;
        goto out_get_slave;
    }

    slave_info->position = slave->ring_position;
    slave_info->vendor_id = slave->sii_image->sii.vendor_id;
    slave_info->product_code = slave->sii_image->sii.product_code;
    slave_info->revision_number = slave->sii_image->sii.revision_number;
    slave_info->serial_number = slave->sii_image->sii.serial_number;
    slave_info->alias = slave->effective_alias;
    slave_info->current_on_ebus = slave->sii_image->sii.current_on_ebus;

    for (i = 0; i < EC_MAX_PORTS; i++) {
        slave_info->ports[i].desc = slave->ports[i].desc;
        slave_info->ports[i].link.link_up = slave->ports[i].link.link_up;
        slave_info->ports[i].link.loop_closed =
            slave->ports[i].link.loop_closed;
        slave_info->ports[i].link.signal_detected =
            slave->ports[i].link.signal_detected;
        slave_info->ports[i].receive_time = slave->ports[i].receive_time;
        if (slave->ports[i].next_slave) {
            slave_info->ports[i].next_slave =
                slave->ports[i].next_slave->ring_position;
        } else {
            slave_info->ports[i].next_slave = 0xffff;
        }
        slave_info->ports[i].delay_to_next_dc =
            slave->ports[i].delay_to_next_dc;
    }
    slave_info->upstream_port = slave->upstream_port;

    slave_info->al_state = slave->current_state;
    slave_info->error_flag = slave->error_flag;
    slave_info->scan_required = slave->scan_required;
    slave_info->ready = ec_fsm_slave_is_ready(&slave->fsm);
    slave_info->sync_count = slave->sii_image->sii.sync_count;
    slave_info->sdo_count = ec_slave_sdo_count(slave);
    if (slave->sii_image->sii.name) {
        strncpy(slave_info->name, slave->sii_image->sii.name, EC_MAX_STRING_LENGTH);
    } else {
        slave_info->name[0] = 0;
    }

out_get_slave:
    ec_lock_up(&master->master_sem);

    return ret;
}

/*****************************************************************************/

void ecrt_master_callbacks(ec_master_t *master,
        void (*send_cb)(void *), void (*receive_cb)(void *), void *cb_data)
{
    EC_MASTER_DBG(master, 1, "ecrt_master_callbacks(master = 0x%p,"
            " send_cb = 0x%p, receive_cb = 0x%p, cb_data = 0x%p)\n",
            master, send_cb, receive_cb, cb_data);

    master->app_send_cb = send_cb;
    master->app_receive_cb = receive_cb;
    master->app_cb_data = cb_data;
}

/*****************************************************************************/

void ecrt_master_state(const ec_master_t *master, ec_master_state_t *state)
{
    ec_device_index_t dev_idx;

    state->slaves_responding = 0U;
    state->al_states = 0;
    state->link_up = 0U;
    state->scan_busy = master->scan_busy ? 1U : 0U;

    for (dev_idx = EC_DEVICE_MAIN; dev_idx < ec_master_num_devices(master);
            dev_idx++) {
        /* Announce sum of responding slaves on all links. */
        state->slaves_responding += master->fsm.slaves_responding[dev_idx];

        /* Binary-or slave states of all links. */
        state->al_states |= master->fsm.slave_states[dev_idx];

        /* Signal link up if at least one device has link. */
        state->link_up |= master->devices[dev_idx].link_state;
    }
}

/*****************************************************************************/

int ecrt_master_link_state(const ec_master_t *master, unsigned int dev_idx,
        ec_master_link_state_t *state)
{
    if (dev_idx >= ec_master_num_devices(master)) {
        return -EINVAL;
    }

    state->slaves_responding = master->fsm.slaves_responding[dev_idx];
    state->al_states = master->fsm.slave_states[dev_idx];
    state->link_up = master->devices[dev_idx].link_state;

    return 0;
}

/*****************************************************************************/

void ecrt_master_application_time(ec_master_t *master, uint64_t app_time)
{
    master->app_time = app_time;

    if (unlikely(!master->dc_ref_time)) {
        master->dc_ref_time = app_time;
    }
}

/*****************************************************************************/

int ecrt_master_reference_clock_time(ec_master_t *master, uint32_t *time)
{
    if (!master->dc_ref_clock) {
        return -ENXIO;
    }

    if (master->sync_datagram.state != EC_DATAGRAM_RECEIVED) {
        return -EIO;
    }

    if (!master->dc_offset_valid) {
        return -EAGAIN;
    }

    // Get returned datagram time, transmission delay removed.
    *time = EC_READ_U32(master->sync_datagram.data) -
        master->dc_ref_clock->transmission_delay;

    return 0;
}

/*****************************************************************************/

void ecrt_master_sync_reference_clock(ec_master_t *master)
{
    if (master->dc_ref_clock && master->dc_offset_valid) {
        EC_WRITE_U32(master->ref_sync_datagram.data, master->app_time);
        ec_master_queue_datagram(master, &master->ref_sync_datagram);
    }
}

/*****************************************************************************/

void ecrt_master_sync_reference_clock_to(
        ec_master_t *master,
        uint64_t sync_time
        )
{
    if (master->dc_ref_clock) {
        EC_WRITE_U32(master->ref_sync_datagram.data, sync_time);
        ec_master_queue_datagram(master, &master->ref_sync_datagram);
    }
}

/*****************************************************************************/

void ecrt_master_sync_slave_clocks(ec_master_t *master)
{
    if (master->dc_ref_clock && master->dc_offset_valid) {
        ec_datagram_zero(&master->sync_datagram);
        ec_master_queue_datagram(master, &master->sync_datagram);
    }
}

/*****************************************************************************/

void ecrt_master_64bit_reference_clock_time_queue(ec_master_t *master)
{
    if (master->dc_ref_clock && master->dc_offset_valid) {
        ec_datagram_zero(&master->sync64_datagram);
        ec_master_queue_datagram(master, &master->sync64_datagram);
    }
}

/*****************************************************************************/

int ecrt_master_64bit_reference_clock_time(ec_master_t *master, uint64_t *time)
{
    if (!master->dc_ref_clock) {
        return -ENXIO;
    }

    if (master->sync64_datagram.state != EC_DATAGRAM_RECEIVED) {
        return -EIO;
    }

    if (!master->dc_offset_valid) {
    	return -EAGAIN;
    }

    // Get returned datagram time, transmission delay removed.
    *time = EC_READ_U64(master->sync64_datagram.data) -
        master->dc_ref_clock->transmission_delay;

    return 0;
}

/*****************************************************************************/

void ecrt_master_sync_monitor_queue(ec_master_t *master)
{
    ec_datagram_zero(&master->sync_mon_datagram);
    ec_master_queue_datagram(master, &master->sync_mon_datagram);
}

/*****************************************************************************/

uint32_t ecrt_master_sync_monitor_process(ec_master_t *master)
{
    if (master->sync_mon_datagram.state == EC_DATAGRAM_RECEIVED) {
        return EC_READ_U32(master->sync_mon_datagram.data) & 0x7fffffff;
    } else {
        return 0xffffffff;
    }
}

/*****************************************************************************/

int ecrt_master_sdo_download(ec_master_t *master, uint16_t slave_position,
        uint16_t index, uint8_t subindex, const uint8_t *data,
        size_t data_size, uint32_t *abort_code)
{
    ec_sdo_request_t request;
    ec_slave_t *slave;
    int ret;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p,"
            " slave_position = %u, index = 0x%04X, subindex = 0x%02X,"
            " data = 0x%p, data_size = %zu, abort_code = 0x%p)\n",
            __func__, master, slave_position, index, subindex,
            data, data_size, abort_code);

    if (!data_size) {
        EC_MASTER_ERR(master, "Zero data size!\n");
        return -EINVAL;
    }

    ec_sdo_request_init(&request);
    ecrt_sdo_request_index(&request, index, subindex);
    ret = ec_sdo_request_alloc(&request, data_size);
    if (ret) {
        ec_sdo_request_clear(&request);
        return ret;
    }

    memcpy(request.data, data, data_size);
    request.data_size = data_size;
    ecrt_sdo_request_write(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_sdo_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        ec_sdo_request_clear(&request);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SDO download request.\n");

    // schedule request.
    list_add_tail(&request.list, &slave->sdo_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_sdo_request_clear(&request);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    *abort_code = request.abort_code;

    if (request.state == EC_INT_REQUEST_SUCCESS) {
        ret = 0;
    } else if (request.errno) {
        ret = -request.errno;
    } else {
        ret = -EIO;
    }

    ec_sdo_request_clear(&request);
    return ret;
}

/*****************************************************************************/

int ecrt_master_sdo_download_complete(ec_master_t *master,
        uint16_t slave_position, uint16_t index, const uint8_t *data,
        size_t data_size, uint32_t *abort_code)
{
    ec_sdo_request_t request;
    ec_slave_t *slave;
    int ret;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p,"
            " slave_position = %u, index = 0x%04X,"
            " data = 0x%p, data_size = %zu, abort_code = 0x%p)\n",
            __func__, master, slave_position, index, data, data_size,
            abort_code);

    if (!data_size) {
        EC_MASTER_ERR(master, "Zero data size!\n");
        return -EINVAL;
    }

    ec_sdo_request_init(&request);
    ecrt_sdo_request_index_complete(&request, index);
    ret = ec_sdo_request_alloc(&request, data_size);
    if (ret) {
        ec_sdo_request_clear(&request);
        return ret;
    }

    memcpy(request.data, data, data_size);
    request.data_size = data_size;
    ecrt_sdo_request_write(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_sdo_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        ec_sdo_request_clear(&request);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SDO download request"
            " (complete access).\n");

    // schedule request.
    list_add_tail(&request.list, &slave->sdo_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_sdo_request_clear(&request);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    *abort_code = request.abort_code;

    if (request.state == EC_INT_REQUEST_SUCCESS) {
        ret = 0;
    } else if (request.errno) {
        ret = -request.errno;
    } else {
        ret = -EIO;
    }

    ec_sdo_request_clear(&request);
    return ret;
}

/*****************************************************************************/

int ecrt_master_sdo_upload(ec_master_t *master, uint16_t slave_position,
        uint16_t index, uint8_t subindex, uint8_t *target,
        size_t target_size, size_t *result_size, uint32_t *abort_code)
{
    ec_sdo_request_t request;
    ec_slave_t *slave;
    int ret = 0;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p,"
            " slave_position = %u, index = 0x%04X, subindex = 0x%02X,"
            " target = 0x%p, target_size = %zu, result_size = 0x%p,"
            " abort_code = 0x%p)\n",
            __func__, master, slave_position, index, subindex,
            target, target_size, result_size, abort_code);

    ec_sdo_request_init(&request);
    ecrt_sdo_request_index(&request, index, subindex);
    ecrt_sdo_request_read(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_sdo_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        ec_sdo_request_clear(&request);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SDO upload request.\n");

    // schedule request.
    list_add_tail(&request.list, &slave->sdo_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_sdo_request_clear(&request);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    *abort_code = request.abort_code;

    if (request.state != EC_INT_REQUEST_SUCCESS) {
        *result_size = 0;
        if (request.errno) {
            ret = -request.errno;
        } else {
            ret = -EIO;
        }
    } else {
        if (request.data_size > target_size) {
            EC_SLAVE_ERR(slave, "%s(): Buffer too small.\n", __func__);
            ret = -EOVERFLOW;
        }
        else {
            memcpy(target, request.data, request.data_size);
            *result_size = request.data_size;
            ret = 0;
        }
    }

    ec_sdo_request_clear(&request);
    return ret;
}

/*****************************************************************************/

int ecrt_master_sdo_upload_complete(ec_master_t *master, uint16_t slave_position,
        uint16_t index, uint8_t *target,
        size_t target_size, size_t *result_size, uint32_t *abort_code)
{
    ec_sdo_request_t request;
    ec_slave_t *slave;
    int ret = 0;

    EC_MASTER_DBG(master, 1, "%s(master = 0x%p,"
            " slave_position = %u, index = 0x%04X,"
            " target = 0x%p, target_size = %zu, result_size = 0x%p,"
            " abort_code = 0x%p)\n",
            __func__, master, slave_position, index,
            target, target_size, result_size, abort_code);

    ec_sdo_request_init(&request);
    ecrt_sdo_request_index_complete(&request, index);
    ecrt_sdo_request_read(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_sdo_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        ec_sdo_request_clear(&request);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SDO upload request (complete access).\n");

    // schedule request.
    list_add_tail(&request.list, &slave->sdo_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_sdo_request_clear(&request);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    *abort_code = request.abort_code;

    if (request.state != EC_INT_REQUEST_SUCCESS) {
        *result_size = 0;
        if (request.errno) {
            ret = -request.errno;
        } else {
            ret = -EIO;
        }
    } else {
        if (request.data_size > target_size) {
            EC_MASTER_ERR(master, "Buffer too small.\n");
            ret = -EOVERFLOW;
        }
        else {
            memcpy(target, request.data, request.data_size);
            *result_size = request.data_size;
            ret = 0;
        }
    }

    ec_sdo_request_clear(&request);
    return ret;
}

/*****************************************************************************/

int ecrt_master_write_idn(ec_master_t *master, uint16_t slave_position,
        uint8_t drive_no, uint16_t idn, uint8_t *data, size_t data_size,
        uint16_t *error_code)
{
    ec_soe_request_t request;
    ec_slave_t *slave;
    int ret;

    if (drive_no > 7) {
        EC_MASTER_ERR(master, "Invalid drive number!\n");
        return -EINVAL;
    }

    ec_soe_request_init(&request);
    ec_soe_request_set_drive_no(&request, drive_no);
    ec_soe_request_set_idn(&request, idn);

    ret = ec_soe_request_alloc(&request, data_size);
    if (ret) {
        ec_soe_request_clear(&request);
        return ret;
    }

    memcpy(request.data, data, data_size);
    request.data_size = data_size;
    ec_soe_request_write(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_soe_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n",
                slave_position);
        ec_soe_request_clear(&request);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SoE write request.\n");

    // schedule SoE write request.
    list_add_tail(&request.list, &slave->soe_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            // abort request
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_soe_request_clear(&request);
            return -EINTR;
        }
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    if (error_code) {
        *error_code = request.error_code;
    }
    ret = request.state == EC_INT_REQUEST_SUCCESS ? 0 : -EIO;
    ec_soe_request_clear(&request);

    return ret;
}

/*****************************************************************************/

int ecrt_master_read_idn(ec_master_t *master, uint16_t slave_position,
        uint8_t drive_no, uint16_t idn, uint8_t *target, size_t target_size,
        size_t *result_size, uint16_t *error_code)
{
    ec_soe_request_t request;
    ec_slave_t *slave;
    int ret;

    if (drive_no > 7) {
        EC_MASTER_ERR(master, "Invalid drive number!\n");
        return -EINVAL;
    }

    ec_soe_request_init(&request);
    ec_soe_request_set_drive_no(&request, drive_no);
    ec_soe_request_set_idn(&request, idn);
    ec_soe_request_read(&request);

    if (ec_lock_down_interruptible(&master->master_sem)) {
        ec_soe_request_clear(&request);
        return -EINTR;
    }

    if (!(slave = ec_master_find_slave(master, 0, slave_position))) {
        ec_lock_up(&master->master_sem);
        ec_soe_request_clear(&request);
        EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_position);
        return -EINVAL;
    }

    EC_SLAVE_DBG(slave, 1, "Scheduling SoE read request.\n");

    // schedule request.
    list_add_tail(&request.list, &slave->soe_requests);

    ec_lock_up(&master->master_sem);

    // wait for processing through FSM
    if (wait_event_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        // interrupted by signal
        ec_lock_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_lock_up(&master->master_sem);
            ec_soe_request_clear(&request);
            return -EINTR;
        }
        // request already processing: interrupt not possible.
        ec_lock_up(&master->master_sem);
    }

    // wait until master FSM has finished processing
    wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    if (error_code) {
        *error_code = request.error_code;
    }

    if (request.state != EC_INT_REQUEST_SUCCESS) {
        if (result_size) {
            *result_size = 0;
        }
        ret = -EIO;
    } else { // success
        if (request.data_size > target_size) {
            EC_SLAVE_ERR(slave, "%s(): Buffer too small.\n", __func__);
            ret = -EOVERFLOW;
        }
        else { // data fits in buffer
            if (result_size) {
                *result_size = request.data_size;
            }
            memcpy(target, request.data, request.data_size);
            ret = 0;
        }
    }

    ec_soe_request_clear(&request);
    return ret;
}

/*****************************************************************************/

int ecrt_master_rt_slave_requests(ec_master_t *master,
        unsigned int rt_slave_requests)
{
    // set flag as to whether the master or the external application
    // should be handling processing the slave request
    master->rt_slave_requests = rt_slave_requests;

    if (master->rt_slave_requests) {
        EC_MASTER_INFO(master, "Application selected to process"
                " slave request by the application.\n");
    }
    else {
        EC_MASTER_INFO(master, "Application selected to process"
                " slave request by the master.\n");
    }

    return 0;
}

/*****************************************************************************/

void ecrt_master_exec_slave_requests(ec_master_t *master)
{
    // execute slave state machines
    if (ec_lock_down_interruptible(&master->master_sem)) {
        return;
    }

    // ignore this call if the master is not operational or not set to
    // handle the slave requests from the application
    if (master->rt_slave_requests && master->rt_slaves_available &&
        (master->phase == EC_OPERATION)) {
        ec_master_exec_slave_fsms(master);
    }

    ec_lock_up(&master->master_sem);
}

/*****************************************************************************/

#ifdef EC_EOE

int ecrt_master_eoe_addif(ec_master_t *master,
        uint16_t alias, uint16_t posn)
{
    ec_eoe_t *eoe;
    char name[EC_DATAGRAM_NAME_SIZE];
    int res;

    // check if the name already exists
    if (alias) {
        snprintf(name, EC_DATAGRAM_NAME_SIZE, "eoe%ua%u", master->index, alias);
    } else {
        snprintf(name, EC_DATAGRAM_NAME_SIZE, "eoe%us%u", master->index, posn);
    }

    ec_lock_down(&master->master_sem);
    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        if ((eoe->slave == NULL) &&
                (strncmp(name, ec_eoe_name(eoe), EC_DATAGRAM_NAME_SIZE) == 0)) {
            ec_lock_up(&master->master_sem);
            return -EADDRINUSE;
        }
    }

    // none found, create one
    if (!(eoe = kmalloc(sizeof(ec_eoe_t), GFP_KERNEL))) {
        EC_MASTER_ERR(master, "Failed to allocate EoE handler memory!\n");
        ec_lock_up(&master->master_sem);
        return -EFAULT;
    }

    if ((res = ec_eoe_init(master, eoe, alias, posn))) {
        EC_MASTER_ERR(master, "Failed to init EoE handler!\n");
        kfree(eoe);
        ec_lock_up(&master->master_sem);
        return res;
    }

    list_add_tail(&eoe->list, &master->eoe_handlers);
    ec_lock_up(&master->master_sem);

    return 0;
}

/*****************************************************************************/

int ecrt_master_eoe_delif(ec_master_t *master,
        uint16_t alias, uint16_t posn)
{
    ec_eoe_t *eoe;
    char name[EC_DATAGRAM_NAME_SIZE];

    if (alias) {
        snprintf(name, EC_DATAGRAM_NAME_SIZE, "eoe%ua%u", master->index, alias);
    } else {
        snprintf(name, EC_DATAGRAM_NAME_SIZE, "eoe%us%u", master->index, posn);
    }

    ec_lock_down(&master->master_sem);
    list_for_each_entry(eoe, &master->eoe_handlers, list) {
        if (strncmp(name, ec_eoe_name(eoe), EC_DATAGRAM_NAME_SIZE) == 0) {
            list_del(&eoe->list);
            ec_eoe_clear(eoe);
            kfree(eoe);
            ec_lock_up(&master->master_sem);
            return 0;
        }
    }
    ec_lock_up(&master->master_sem);

    return -EFAULT;
}

#endif

/*****************************************************************************/

int ec_master_obj_dict(ec_master_t *master, uint8_t *data,
        size_t *data_size, size_t buff_size)
{
    uint8_t     sdo_req_cmd;
    uint8_t     sdo_resp_cmd;
    uint16_t    sdo_index;
    uint8_t     sdo_sub_index;
    uint16_t    slave_posn;
    ec_slave_t *slave;
    char        value[32];
    size_t      value_size;
    size_t      total_value_size;
    int         i;
    uint8_t     link_status;
    uint32_t    offset;
    uint32_t    abort_code;
    uint8_t     resp_error = 0;


    EC_MASTER_DBG(master, 1, "MBox Gateway request for Master Information.\n");

    // check the mailbox header type is CoE
    if ( (*data_size < EC_MBOX_HEADER_SIZE) ||
            ((EC_READ_U16(data + 5) & 0x0F) != EC_MBOX_TYPE_COE) ) {
        EC_MASTER_ERR(master, "Master does not support requested mailbox type!\n");
        return -EPROTONOSUPPORT;
    }

    // ensure the CoE Header service is an SDO request
    offset = EC_MBOX_HEADER_SIZE;
    if ( (*data_size < EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE + 4) ||
            (EC_READ_U16(data + offset) >> 12 != 0x2) ) {
        EC_MASTER_ERR(master, "Master only supports SDO requests!\n");
        return -EINVAL;
    }

    // get the SDO cmd, index, subindex
    offset = EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE;
    sdo_req_cmd   = EC_READ_U8(data + offset) >> 5;
    sdo_index     = EC_READ_U16(data + offset + 1);
    sdo_sub_index = EC_READ_U8(data + offset + 3);

    // get the master lock
    if (ec_lock_down_interruptible(&master->master_sem)) {
        return -EINTR;
    }

    // handle SDO request
    // See ETG.5001.3 for required object dictionary entries to support
    if ( (sdo_index >= 0x8000) && (sdo_index < 0x8000 + master->slave_count) ) {
        // readonly commands
        if (sdo_req_cmd != 0x02) {
            ec_lock_up(&master->master_sem);
            EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                    " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
            return -EPROTONOSUPPORT;
        }

        // calc slave position (slaves start at position 0)
        slave_posn = sdo_index - 0x8000;

        // get the slave information
        if (!(slave = ec_master_find_slave(master, 0, slave_posn))) {
            ec_lock_up(&master->master_sem);
            EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_posn);
            return -EINVAL;
        }

        switch (sdo_sub_index) {
            case 0 : {
                // length of this object (uint8)
                // Note: this may need to be 35 (there is a gap between 8 and 33)
                value_size = sizeof(uint8_t);
                EC_WRITE_U8(value, 35);
            } break;
            case 1 : {
                // slave index (uint16)
                // slave posn + MBG slave address offset
                value_size = sizeof(uint16_t);
                EC_WRITE_U16(value, slave_posn + EC_MBG_SLAVE_ADDR_OFFSET);
            } break;
            case 2 : {
                // slave type (visiblestring[16])
                value_size = 16;
                memset(value, 0x00, value_size);
                if (slave->sii_image) {
                    snprintf(value, value_size, "%s", slave->sii_image->sii.order);
                }
            } break;
            case 3 : {
                // slave name (visiblestring[32])
                value_size = 32;
                memset(value, 0x00, value_size);
                if (slave->sii_image) {
                    snprintf(value, value_size, "%s", slave->sii_image->sii.name);
                }
            } break;
            case 4 : {
                // device type (uint32) (Modular Device Profile)
                // need ESI's for this information or a slave that can
                // supports mailbox with SDO's (read 0x1000:00)
                if (!(slave->sii_image) ||
                    !(slave->sii_image->sii.mailbox_protocols & EC_MBOX_COE) ||
                    (ecrt_master_sdo_upload(master, slave_posn, 0x1000, 0x00,
                        value, sizeof(uint32_t), &value_size, &abort_code) != 0)) {
                    // return 0 by default
                    value_size = sizeof(uint32_t);
                    EC_WRITE_U32(value, 0x00000000);
                }
            } break;
            case 5 : {
                // vendor id (uint32)
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, slave->config->vendor_id);
            } break;
            case 6 : {
                // product code (uint32)
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, slave->config->product_code);
            } break;
            case 7 : {
                // revision number (uint32)
                value_size = sizeof(uint32_t);
                if (slave->sii_image) {
                    EC_WRITE_U32(value, slave->sii_image->sii.revision_number);
                } else {
                    EC_WRITE_U32(value, 0x00000000);
                }
            } break;
            case 8 : {
                // serial number (uint32)
                value_size = sizeof(uint32_t);
                if (slave->sii_image) {
                    EC_WRITE_U32(value, slave->sii_image->sii.serial_number);
                } else {
                    EC_WRITE_U32(value, 0x00000000);
                }
            } break;
            case 9 ... 32 : {
                // Data can not be read or stored
                resp_error = 1;
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x08000020);
            } break;
            case 33 : {
                // mailbox out size (uint16)
                value_size = sizeof(uint16_t);
                if (slave->sii_image) {
                    EC_WRITE_U16(value, slave->sii_image->sii.std_rx_mailbox_size);
                } else {
                    EC_WRITE_U16(value, 0x0000);
                }
            } break;
            case 34 : {
                // mailbox in size (uint16)
                value_size = sizeof(uint16_t);
                if (slave->sii_image) {
                    EC_WRITE_U16(value, slave->sii_image->sii.std_tx_mailbox_size);
                } else {
                    EC_WRITE_U16(value, 0x0000);
                }
            } break;
            case 35 : {
                // link status (uint8), bits 4..7 of register 0x0110:0x0111
                link_status = 0;
                for (i = 0; i < EC_MAX_PORTS; i++) {
                    if (slave->ports[i].link.link_up) {
                        link_status += 1 << (4 + i);
                    }
                }
                value_size = sizeof(uint8_t);
                EC_WRITE_U8(value, link_status);
            } break;
            default :
            {
                // Subindex does not exist error
                resp_error = 1;
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x06090011);
            } break;
        }

    } else if ( (sdo_index >= 0xA000) && (sdo_index < 0xA000 + master->slave_count) ) {
        // Note: this is meant to be optional, but the TwinSAFE_Loader.exe seems to
        //   want to have it

        // calc slave position (slaves start at position 0)
        slave_posn = sdo_index - 0xA000;

        switch (sdo_sub_index) {
            case 0 : {
                // readonly command
                if (sdo_req_cmd != 0x02) {
                    ec_lock_up(&master->master_sem);
                    EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                            " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
                    return -EPROTONOSUPPORT;
                }

                // length of this object (uint8)
                value_size = sizeof(uint8_t);
                EC_WRITE_U8(value, 2);
            } break;
            case 1 : {
                // AL Status, register 0x130-0x131 (uint16)
                // current state

                // readonly command
                if (sdo_req_cmd != 0x02) {
                    ec_lock_up(&master->master_sem);
                    EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                            " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
                    return -EPROTONOSUPPORT;
                }

                // get the slave information
                if (!(slave = ec_master_find_slave(master, 0, slave_posn))) {
                    ec_lock_up(&master->master_sem);
                    EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_posn);
                    return -EINVAL;
                }

                // return the cached slaves current state
                // Note: the master only stores the first state byte
                //   whereas the AL Status register is two bytes
                //   (second byte reserved)
                value_size = sizeof(uint16_t);
                EC_WRITE_U16(value, slave->current_state);
            } break;
            case 2 : {
                // AL Control, register 0x120-0x121 (uint16)
                // requested state

                // readwrite command
                if ( (sdo_req_cmd != 0x02) && (sdo_req_cmd != 0x00) ) {
                    ec_lock_up(&master->master_sem);
                    EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                            " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
                    return -EPROTONOSUPPORT;
                }

                // get the slave information
                if (!(slave = ec_master_find_slave(master, 0, slave_posn))) {
                    ec_lock_up(&master->master_sem);
                    EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_posn);
                    return -EINVAL;
                }

                if (sdo_req_cmd == 0x02) {
                    // read
                    // return the cached slaves requested state
                    // Note: the master only stores the first state byte
                    //   whereas the AL Control register is two bytes
                    //   (second byte reserved)
                    value_size = sizeof(uint16_t);
                    EC_WRITE_U16(value, slave->requested_state);

                } else {
                    // write (sdo_req_cmd == 0x00)
                    uint8_t *write_data = data + offset + 4;
                    size_t   write_size;
                    uint8_t  size_specified = EC_READ_U8(data + offset) & 0x01;
                    if (size_specified) {
                        write_size = 4 - ((EC_READ_U8(data + offset) & 0x0C) >> 2);
                    } else {
                        write_size = 4;
                    }

                    // check write size
                    if (write_size != 2) {
                        ec_lock_up(&master->master_sem);
                        EC_MASTER_ERR(master, "Master, unexpected SDO write data size"
                                " %zu (expected %u) on 0x%04X:%02X!\n",
                                write_size, 2, sdo_index, sdo_sub_index);
                        return -EPROTONOSUPPORT;
                    }

                    // request the slave AL state
                    ec_slave_request_state(slave, EC_READ_U16(write_data));

                    // set blank download response
                    value_size = sizeof(uint32_t);
                    memset(value, 0x00, 4);
                }
            } break;
            default : {
                // Subindex does not exist error
                resp_error = 1;
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x06090011);
            } break;
        }

    } else if ( (sdo_index >= 0xF020) && (sdo_index < 0xF030) ) {
        // readonly commands
        if (sdo_req_cmd != 0x02) {
            ec_lock_up(&master->master_sem);
            EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                    " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
            return -EPROTONOSUPPORT;
        }

        if (sdo_sub_index == 0) {
            uint64_t index = master->slave_count;
            uint32_t remainder;

            // length of this object (uint8)
            value_size = sizeof(uint8_t);

            // calc index and remainder from slave count
            remainder = do_div(index, 255);

            if (sdo_index - 0xF020 < index) {
                EC_WRITE_U8(value, 255);
            } else if ( (sdo_index - 0xF020 == index) && (remainder > 0) ) {
                EC_WRITE_U8(value, remainder);
            } else {
                EC_WRITE_U8(value, 0);
            }
        } else {
            // calc slave position
            slave_posn = ((sdo_index - 0xF020) << 8) + sdo_sub_index - (sdo_index - 0xF020 + 1);

            if (slave_posn < master->slave_count) {
                // slave index (uint16)
                // slave posn + MBG slave address offset
                value_size = sizeof(uint16_t);
                EC_WRITE_U16(value, slave_posn + EC_MBG_SLAVE_ADDR_OFFSET);
            } else {
                // Object Not Found error
                resp_error = 1;
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x06020000);
            }
        }

    } else if (sdo_index == 0xF000) {
        // readonly commands
        if (sdo_req_cmd != 0x02) {
            ec_lock_up(&master->master_sem);
            EC_MASTER_ERR(master, "Master, unsupported SDO Command %hhu on"
                    " 0x%04X:%02X!\n", sdo_req_cmd, sdo_index, sdo_sub_index);
            return -EPROTONOSUPPORT;
        }

        // Modular Device Profile
        switch (sdo_sub_index) {
            case 0 : {
                // length of this object (uint8)
                value_size = sizeof(uint8_t);
                EC_WRITE_U8(value, 4);
            } break;
            case 1 : {
                // module index distance (uint16)
                value_size = sizeof(uint16_t);
                EC_WRITE_U16(value, 0x0001);
            } break;
            case 2 : {
                // maximum number of modules (uint16)
                // Gateway information limit
                value_size = sizeof(uint16_t);
                EC_WRITE_U16(value, 4080);
            } break;
            case 3 : {
                // general configuration (uint32)
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x000000FF);
            } break;
            case 4 : {
                // general information (uint32)
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x00000000);
            } break;
            default : {
                // Subindex does not exist error
                resp_error = 1;
                value_size = sizeof(uint32_t);
                EC_WRITE_U32(value, 0x06090011);
            } break;
        }

    } else {
        // Object Not Found error
        resp_error = 1;
        value_size = sizeof(uint32_t);
        EC_WRITE_U32(value, 0x06020000);
    }

    ec_lock_up(&master->master_sem);


    // do we need to allow room for a complete size field?
    if ( (value_size > 0) && (value_size <= 4) ) {
        total_value_size = value_size;
    } else {
        total_value_size = value_size + sizeof(uint32_t);
    }

    // set reply
    if (EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE + 4 + total_value_size > buff_size) {
        EC_MASTER_ERR(master, "Buffer too small.\n");
        return -EOVERFLOW;
    }
    else {
        // update data_size
        *data_size = EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE + 4 + total_value_size;

        // update Mailbox Header Length (length of CoE Header onwards)
        EC_WRITE_U16(data, *data_size - EC_MBOX_HEADER_SIZE);

        // update CoE service response or SDO command specifier on error
        if (resp_error) {
            // not happy, return abort SDO transfer request
            offset = EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE;
            EC_WRITE_U8(data + offset, 0x04 << 5);

            // set abort value
            memcpy(data + offset + 4, value, value_size);
        } else {
            // happy, return service code 3 (SDO response)
            offset = EC_MBOX_HEADER_SIZE;
            EC_WRITE_U16(data + offset, 0x03 << 12);

            // set SDO command specifier
            if (sdo_req_cmd == 0x02) {
                // upload response
                sdo_resp_cmd = 0x02;
            } else {
                // download response
                sdo_resp_cmd = 0x01;
            }

            // set value size
            offset = EC_MBOX_HEADER_SIZE + EC_COE_HEADER_SIZE;
            if ( (value_size > 0) && (value_size <= 4) ) {
                // upload response, expedited size specified
                // bit 0      1 = size specified
                // bit 1      1 = expedited
                // bit 2..3   4 - size
                // bit 5..7   command specifier
                EC_WRITE_U8(data + offset, (sdo_resp_cmd << 5) +
                        ((4 - value_size) << 2) + 0x02 + 0x01);

                // set offset to data
                offset += 4;
            } else {
                // upload response, size specified
                EC_WRITE_U8(data + offset, (sdo_resp_cmd << 5) + 0x01);

                // set value size
                offset += 4;
                EC_WRITE_U32(data + offset, value_size);

                // set offset to value
                offset += sizeof(uint32_t);
            }

            // set value
            memcpy(data + offset, value, value_size);
        }
    }


    return 0;
}

/*****************************************************************************/

int ec_master_mbox_gateway(ec_master_t *master, uint8_t *data,
        size_t *data_size, size_t buff_size)
{
    ec_mbg_request_t request;
    uint16_t slave_posn;
    ec_slave_t *slave;
    int ret = 0;

    // get the slave address
    slave_posn = EC_READ_U16(data + 2);

    // check if slave address is zero (master object dictionary request)
    if (slave_posn == 0)
    {
        // request for master information
        ret = ec_master_obj_dict(master, data, data_size, buff_size);
    }
    else if (slave_posn >= EC_MBG_SLAVE_ADDR_OFFSET)
    {
        // calculate the slave position address
        slave_posn -= EC_MBG_SLAVE_ADDR_OFFSET;

        // pass on request to slave
        ec_mbg_request_init(&request);
        ret = ec_mbg_request_copy_data(&request, data, *data_size);
        *data_size = 0;
        if (ret) {
            ec_mbg_request_clear(&request);
            return ret;
        }
        ec_mbg_request_run(&request);

        if (ec_lock_down_interruptible(&master->master_sem)) {
            ec_mbg_request_clear(&request);
            return -EINTR;
        }

        // check for a valid slave request
        if (!(slave = ec_master_find_slave(master, 0, slave_posn))) {
            ec_lock_up(&master->master_sem);
            ec_mbg_request_clear(&request);
            EC_MASTER_ERR(master, "Slave %u does not exist!\n", slave_posn);
            return -EINVAL;
        }

        EC_SLAVE_DBG(slave, 1, "Scheduling MBox Gateway request.\n");

        // schedule request.
        list_add_tail(&request.list, &slave->mbg_requests);

        ec_lock_up(&master->master_sem);

        // wait for processing through FSM
        if (wait_event_interruptible(master->request_queue,
                    request.state != EC_INT_REQUEST_QUEUED)) {
            // interrupted by signal
            ec_lock_down(&master->master_sem);
            if (request.state == EC_INT_REQUEST_QUEUED) {
                list_del(&request.list);
                ec_lock_up(&master->master_sem);
                ec_mbg_request_clear(&request);
                return -EINTR;
            }
            // request already processing: interrupt not possible.
            ec_lock_up(&master->master_sem);
        }

        // wait until master FSM has finished processing
        wait_event(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

        if (request.state != EC_INT_REQUEST_SUCCESS) {
            if (request.error_code) {
                ret = -request.error_code;
            } else {
                ret = -EIO;
            }
        } else {
            if (request.data_size > buff_size) {
                EC_MASTER_ERR(master, "Buffer too small.\n");
                ret = -EOVERFLOW;
            }
            else {
                memcpy(data, request.data, request.data_size);
                *data_size = request.data_size;
                ret = 0;
            }
        }

        ec_mbg_request_clear(&request);
    } else {
        EC_MASTER_ERR(master, "MBox Gateway: Invalid slave offset address %u!\n", slave_posn);
        return -EINVAL;
    }



    return ret;
}

/*****************************************************************************/

void ecrt_master_reset(ec_master_t *master)
{
    ec_slave_config_t *sc;

    list_for_each_entry(sc, &master->configs, list) {
        if (sc->slave) {
            ec_slave_request_state(sc->slave, EC_SLAVE_STATE_OP);
        }
    }
}

/*****************************************************************************/

/** \cond */

EXPORT_SYMBOL(ecrt_master_create_domain);
EXPORT_SYMBOL(ecrt_master_setup_domain_memory);
EXPORT_SYMBOL(ecrt_master_activate);
EXPORT_SYMBOL(ecrt_master_deactivate_slaves);
EXPORT_SYMBOL(ecrt_master_deactivate);
EXPORT_SYMBOL(ecrt_master_send);
EXPORT_SYMBOL(ecrt_master_send_ext);
EXPORT_SYMBOL(ecrt_master_receive);
EXPORT_SYMBOL(ecrt_master_callbacks);
EXPORT_SYMBOL(ecrt_master);
EXPORT_SYMBOL(ecrt_master_get_slave);
EXPORT_SYMBOL(ecrt_master_slave_config);
EXPORT_SYMBOL(ecrt_master_select_reference_clock);
EXPORT_SYMBOL(ecrt_master_state);
EXPORT_SYMBOL(ecrt_master_link_state);
EXPORT_SYMBOL(ecrt_master_application_time);
EXPORT_SYMBOL(ecrt_master_sync_reference_clock);
EXPORT_SYMBOL(ecrt_master_sync_reference_clock_to);
EXPORT_SYMBOL(ecrt_master_sync_slave_clocks);
EXPORT_SYMBOL(ecrt_master_reference_clock_time);
EXPORT_SYMBOL(ecrt_master_64bit_reference_clock_time_queue);
EXPORT_SYMBOL(ecrt_master_64bit_reference_clock_time);
EXPORT_SYMBOL(ecrt_master_sync_monitor_queue);
EXPORT_SYMBOL(ecrt_master_sync_monitor_process);
EXPORT_SYMBOL(ecrt_master_sdo_download);
EXPORT_SYMBOL(ecrt_master_sdo_download_complete);
EXPORT_SYMBOL(ecrt_master_sdo_upload);
EXPORT_SYMBOL(ecrt_master_sdo_upload_complete);
EXPORT_SYMBOL(ecrt_master_write_idn);
EXPORT_SYMBOL(ecrt_master_read_idn);
EXPORT_SYMBOL(ecrt_master_rt_slave_requests);
EXPORT_SYMBOL(ecrt_master_exec_slave_requests);
#ifdef EC_EOE
EXPORT_SYMBOL(ecrt_master_eoe_addif);
EXPORT_SYMBOL(ecrt_master_eoe_delif);
#endif
EXPORT_SYMBOL(ecrt_master_reset);

/** \endcond */

/*****************************************************************************/
