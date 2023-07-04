/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2006-2014  Florian Pose, Ingenieurgemeinschaft IgH
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
 *****************************************************************************/

/** \file
 * Ethernet-over-EtherCAT request functions.
 */

/*****************************************************************************/

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/slab.h>

#include "eoe_request.h"

/*****************************************************************************/

/** EoE request constructor.
 */
void ec_eoe_request_init(
        ec_eoe_request_t *req /**< EoE request. */
        )
{
    INIT_LIST_HEAD(&req->list);
    req->state = EC_INT_REQUEST_INIT;
    req->jiffies_sent = 0U;

    req->mac_address_included = 0;
    req->ip_address_included = 0;
    req->subnet_mask_included = 0;
    req->gateway_included = 0;
    req->dns_included = 0;
    req->name_included = 0;

    memset(req->mac_address, 0x00, ETH_ALEN);
    req->ip_address = 0;
    req->subnet_mask = 0;
    req->gateway = 0;
    req->dns = 0;
    req->name[0] = 0x00;

    req->result = 0x0000;
}

/*****************************************************************************/

/** Copy another EoE request.
 */

void ec_eoe_request_copy(
        ec_eoe_request_t *req, /**< EoE request. */
        const ec_eoe_request_t *other /**< Other EoE request to copy from. */
)
{
    req->mac_address_included = other->mac_address_included;
    req->ip_address_included = other->ip_address_included;
    req->subnet_mask_included = other->subnet_mask_included;
    req->gateway_included = other->gateway_included;
    req->dns_included = other->dns_included;
    req->name_included = other->name_included;

    memcpy(req->mac_address, other->mac_address, ETH_ALEN);
    req->ip_address = other->ip_address;
    req->subnet_mask = other->subnet_mask;
    req->gateway = other->gateway;
    req->dns = other->dns;
    memcpy(req->name, other->name, EC_MAX_HOSTNAME_SIZE);
}

/*****************************************************************************/

void ec_eoe_request_write(ec_eoe_request_t *req)
{
    req->state = EC_INT_REQUEST_QUEUED;
    req->jiffies_sent = jiffies;

    req->result = 0x0000;
}

