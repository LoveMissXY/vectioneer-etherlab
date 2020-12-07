/*****************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2007-2009  Florian Pose, Ingenieurgemeinschaft IgH
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
 ****************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h> /* clock_gettime() */
#include <sys/mman.h> /* mlockall() */
#include <sched.h> /* sched_setscheduler() */

/****************************************************************************/

#include "ecrt.h"

/****************************************************************************/

/** Task period in ns. */
#define PERIOD_NS   (1000000)

#define MAX_SAFE_STACK (8 * 1024) /* The maximum stack size which is
                                     guranteed safe to access without
                                     faulting */

/****************************************************************************/

/* Constants */
#define NSEC_PER_SEC (1000000000)
#define FREQUENCY (NSEC_PER_SEC / PERIOD_NS)

/****************************************************************************/

// EtherCAT
static ec_master_t *master = NULL;
static ec_master_state_t master_state = {};

static ec_soe_request_t *soe_req_read = NULL, *soe_req_write = NULL;
/****************************************************************************/

static unsigned int counter = 0;
static unsigned int blink = 0;

/*****************************************************************************/

void check_request(ec_soe_request_t *soe_req, int write) {
    ec_request_state_t req_state;
    const char *verb = write ? "write" : "read";

    uint32_t read_data;
    uint16_t write_data = 3;

    req_state = ecrt_soe_request_state(soe_req);

    if(req_state == EC_REQUEST_ERROR) {
        printf("SOE %s request error\n", verb);
    } else if(req_state == EC_REQUEST_SUCCESS) {
        if(!write) {
            read_data = *(uint32_t*)ecrt_soe_request_data(soe_req);
            printf("SOE %s request success, data=%d\n", verb, read_data);
        } else {
            printf("SOE %s request success\n", verb);
        }
    } else if(req_state == EC_REQUEST_BUSY) {
        printf("SOE %s request busy\n", verb);
    } else if(req_state == EC_REQUEST_UNUSED) {
        printf("SOE %s request unused\n", verb);
    }

    if(req_state != EC_REQUEST_BUSY) {
        printf("Requesting SOE %s\n", verb);
        if(!write){
            memset(ecrt_soe_request_data(soe_req), 0x00, ecrt_soe_request_data_size(soe_req));
            ecrt_soe_request_read(soe_req);
        } else {
            memcpy(ecrt_soe_request_data(soe_req), &write_data, sizeof(write_data));
            ecrt_soe_request_write_with_size(soe_req, sizeof(write_data));
        }
    }
}

/*****************************************************************************/

void cyclic_task()
{
    // receive process data
    ecrt_master_receive(master);

    if (counter) {
        counter--;
    } else { // do this at 1 Hz
        counter = FREQUENCY;
        check_request(soe_req_read, 0);
        check_request(soe_req_write, 1);
        blink++;
    }

    ecrt_master_send(master);
}

/****************************************************************************/

void stack_prefault(void)
{
    unsigned char dummy[MAX_SAFE_STACK];

    memset(dummy, 0, MAX_SAFE_STACK);
}

/****************************************************************************/

/* auto result = */
/*     ecrt_master_read_idn(master, 9, 0, 0x71, (uint8_t *)&max_motor_speed, */
/*                          sizeof(max_motor_speed), &len, &error_code); */

/* Identity: */
/*   Vendor Id:       0x00000002 */
/*   Product code:    0x14566012 */
/*   Revision number: 0x00d50000 */
/*   Serial number:   0x0004e7e1 */

int main(int argc, char **argv)
{
    ec_slave_config_t *sc;
    struct timespec wakeup_time;
    int ret = 0;


    master = ecrt_request_master(0);
    if (!master) {
        return -1;
    }

    if (!(sc = ecrt_master_slave_config(master, 0, 9, 0x00000002, 0x14566012))) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }

    if (!(soe_req_read = ecrt_slave_config_create_soe_request(sc, 0, 0x71, sizeof(uint32_t)))) {
        fprintf(stderr, "Failed to create read soe request.\n");
        return -1;
    }

    if (!(sc = ecrt_master_slave_config(master, 0, 7, 0x00000002, 0x14566012))) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }

    if (!(soe_req_write = ecrt_slave_config_create_soe_request(sc, 0, 0x63, sizeof(uint16_t)))) {
        fprintf(stderr, "Failed to create write soe request.\n");
        return -1;
    }


    printf("Activating master...\n");
    if (ecrt_master_activate(master)) {
        return -1;
    }

    /* Set priority */

    struct sched_param param = {};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);

    printf("Using priority %i\n", param.sched_priority);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("sched_setscheduler failed");
    }

    /* Lock memory */

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        fprintf(stderr, "Warning: Failed to lock memory: %s\n",
                strerror(errno));
    }

    stack_prefault();

    printf("Starting RT task with dt=%u ns.\n", PERIOD_NS);

    clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
    wakeup_time.tv_sec += 1; /* start in future */
    wakeup_time.tv_nsec = 0;

    while (1) {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                &wakeup_time, NULL);
        if (ret) {
            fprintf(stderr, "clock_nanosleep(): %s\n", strerror(ret));
            break;
        }

        cyclic_task();

        wakeup_time.tv_nsec += PERIOD_NS;
        while (wakeup_time.tv_nsec >= NSEC_PER_SEC) {
            wakeup_time.tv_nsec -= NSEC_PER_SEC;
            wakeup_time.tv_sec++;
        }
    }

    return ret;
}

/****************************************************************************/
