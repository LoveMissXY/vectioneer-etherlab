/******************************************************************************
 *  
 * $Id$
 * 
 * Copyright (C) 2006-2008  Florian Pose, Ingenieurgemeinschaft IgH
 * 
 * This file is part of the IgH EtherCAT master userspace library.
 * 
 * The IgH EtherCAT master userspace library is free software; you can
 * redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; version 2.1 of
 * the License.
 *
 * The IgH EtherCAT master userspace library is distributed in the hope that
 * it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the IgH EtherCAT master userspace library. If not, see
 * <http://www.gnu.org/licenses/>.
 * 
 * The right to use EtherCAT Technology is granted and comes free of charge
 * under condition of compatibility of product made by Licensee. People
 * intending to distribute/sell products based on the code, have to sign an
 * agreement to guarantee that products using software based on IgH EtherCAT
 * master stay compatible with the actual EtherCAT specification (which are
 * released themselves as an open standard) as the (only) precondition to have
 * the right to use EtherCAT Technology, IP and trade marks.
 *
 *****************************************************************************/

#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "slave_config.h"
#include "domain.h"
#include "voe_handler.h"
#include "master.h"
#include "master/ioctl.h"

/*****************************************************************************/

int ecrt_slave_config_sync_manager(ec_slave_config_t *sc, uint8_t sync_index,
        ec_direction_t dir)
{
    ec_ioctl_config_t data;
    unsigned int i;

    if (sync_index >= EC_MAX_SYNC_MANAGERS)
        return -1;

    memset(&data, 0x00, sizeof(ec_ioctl_config_t));
    data.config_index = sc->index;
    data.syncs[sync_index].dir = dir;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_SYNC, &data) == -1) {
        fprintf(stderr, "Failed to config sync manager: %s\n",
                strerror(errno));
        return -1; 
    }
    
    return 0;
}

/*****************************************************************************/

int ecrt_slave_config_pdo_assign_add(ec_slave_config_t *sc,
        uint8_t sync_index, uint16_t pdo_index)
{
    ec_ioctl_config_pdo_t data;

    data.config_index = sc->index;
    data.sync_index = sync_index;
    data.index = pdo_index;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_ADD_PDO, &data) == -1) {
        fprintf(stderr, "Failed to add Pdo: %s\n",
                strerror(errno));
        return -1; 
    }
    
    return 0;
}

/*****************************************************************************/

void ecrt_slave_config_pdo_assign_clear(ec_slave_config_t *sc,
        uint8_t sync_index)
{
    ec_ioctl_config_pdo_t data;

    data.config_index = sc->index;
    data.sync_index = sync_index;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_CLEAR_PDOS, &data) == -1) {
        fprintf(stderr, "Failed to clear Pdos: %s\n",
                strerror(errno));
    }
}

/*****************************************************************************/

int ecrt_slave_config_pdo_mapping_add(ec_slave_config_t *sc,
        uint16_t pdo_index, uint16_t entry_index, uint8_t entry_subindex,
        uint8_t entry_bit_length)
{
    ec_ioctl_add_pdo_entry_t data;

    data.config_index = sc->index;
    data.pdo_index = pdo_index;
    data.entry_index = entry_index;
    data.entry_subindex = entry_subindex;
    data.entry_bit_length = entry_bit_length;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_ADD_ENTRY, &data) == -1) {
        fprintf(stderr, "Failed to add Pdo entry: %s\n",
                strerror(errno));
        return -1; 
    }
    
    return 0;
}

/*****************************************************************************/

void ecrt_slave_config_pdo_mapping_clear(ec_slave_config_t *sc,
        uint16_t pdo_index)
{
    ec_ioctl_config_pdo_t data;

    data.config_index = sc->index;
    data.index = pdo_index;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_CLEAR_ENTRIES, &data) == -1) {
        fprintf(stderr, "Failed to clear Pdo entries: %s\n",
                strerror(errno));
    }
}

/*****************************************************************************/

int ecrt_slave_config_pdos(ec_slave_config_t *sc,
        unsigned int n_syncs, const ec_sync_info_t syncs[])
{
    unsigned int i, j, k;
    const ec_sync_info_t *sync_info;
    const ec_pdo_info_t *pdo_info;
    const ec_pdo_entry_info_t *entry_info;

    if (!syncs)
        return 0;

    for (i = 0; i < n_syncs; i++) {
        sync_info = &syncs[i];

        if (sync_info->index == (uint8_t) EC_END)
            break;

        if (sync_info->index >= EC_MAX_SYNC_MANAGERS) {
            fprintf(stderr, "Invalid sync manager index %u!\n",
                    sync_info->index);
            return -1;
        }

        if (ecrt_slave_config_sync_manager(
                    sc, sync_info->index, sync_info->dir))
            return -1;

        if (sync_info->n_pdos && sync_info->pdos) {
            ecrt_slave_config_pdo_assign_clear(sc, sync_info->index);

            for (j = 0; j < sync_info->n_pdos; j++) {
                pdo_info = &sync_info->pdos[j];

                if (ecrt_slave_config_pdo_assign_add(
                            sc, sync_info->index, pdo_info->index))
                    return -1;

                if (pdo_info->n_entries && pdo_info->entries) {
                    ecrt_slave_config_pdo_mapping_clear(sc, pdo_info->index);

                    for (k = 0; k < pdo_info->n_entries; k++) {
                        entry_info = &pdo_info->entries[k];

                        if (ecrt_slave_config_pdo_mapping_add(sc,
                                    pdo_info->index, entry_info->index,
                                    entry_info->subindex,
                                    entry_info->bit_length))
                            return -1;
                    }
                }
            }
        }
    }

    return 0;
}

/*****************************************************************************/

int ecrt_slave_config_reg_pdo_entry(
        ec_slave_config_t *sc,
        uint16_t index,
        uint8_t subindex,
        ec_domain_t *domain,
        unsigned int *bit_position
        )
{
    ec_ioctl_reg_pdo_entry_t data;
    int ret;

    data.config_index = sc->index;
    data.entry_index = index;
    data.entry_subindex = subindex;
    data.domain_index = domain->index;

    ret = ioctl(sc->master->fd, EC_IOCTL_SC_REG_PDO_ENTRY, &data);
    if (ret == -1) {
        fprintf(stderr, "Failed to register Pdo entry: %s\n",
                strerror(errno));
        return -2;
    }

    if (bit_position) {
        *bit_position = data.bit_position;
    } else {
        if (data.bit_position) {
            fprintf(stderr, "Pdo entry 0x%04X:%02X does not byte-align "
                    "in config %u:%u.\n", index, subindex,
                    sc->alias, sc->position);
            return -3;
        }
    }

    return ret;
}

/*****************************************************************************/

int ecrt_slave_config_sdo(ec_slave_config_t *sc, uint16_t index,
        uint8_t subindex, const uint8_t *sdo_data, size_t size)
{
    ec_ioctl_sc_sdo_t data;

    data.config_index = sc->index;
    data.index = index;
    data.subindex = subindex;
    data.data = sdo_data;
    data.size = size;

    if (ioctl(sc->master->fd, EC_IOCTL_SC_REG_PDO_ENTRY, &data) == -1) {
        fprintf(stderr, "Failed to configure Sdo.\n");
        return -1;
    }

    return 0;
}

/*****************************************************************************/

int ecrt_slave_config_sdo8(ec_slave_config_t *sc, uint16_t index,
        uint8_t subindex, uint8_t value)
{
    uint8_t data[1];

    EC_WRITE_U8(data, value);
    return ecrt_slave_config_sdo(sc, index, subindex, data, 1);
}

/*****************************************************************************/

int ecrt_slave_config_sdo16(ec_slave_config_t *sc, uint16_t index,
        uint8_t subindex, uint16_t value)
{
    uint8_t data[2];

    EC_WRITE_U16(data, value);
    return ecrt_slave_config_sdo(sc, index, subindex, data, 2);
}

/*****************************************************************************/

int ecrt_slave_config_sdo32(ec_slave_config_t *sc, uint16_t index,
        uint8_t subindex, uint32_t value)
{
    uint8_t data[4];

    EC_WRITE_U32(data, value);
    return ecrt_slave_config_sdo(sc, index, subindex, data, 4);
}

/*****************************************************************************/

ec_sdo_request_t *ecrt_slave_config_create_sdo_request(ec_slave_config_t *sc,
        uint16_t index, uint8_t subindex, size_t size)
{
    return 0; // TODO
}

/*****************************************************************************/

ec_voe_handler_t *ecrt_slave_config_create_voe_handler(ec_slave_config_t *sc,
        size_t size)
{
    ec_ioctl_voe_t data;
    ec_voe_handler_t *voe;
    unsigned int index;

    voe = malloc(sizeof(ec_voe_handler_t));
    if (!voe) {
        fprintf(stderr, "Failed to allocate memory.\n");
        return 0;
    }

    if (size) {
        voe->data = malloc(size);
        if (!voe->data) {
            fprintf(stderr, "Failed to allocate %u bytes of VoE data"
                    " memory.\n", size);
            free(voe);
            return 0;
        }
    } else {
        voe->data = NULL;
    }

    data.config_index = sc->index;
    data.size = size;
    
    if (ioctl(sc->master->fd, EC_IOCTL_SC_VOE, &data) == -1) {
        fprintf(stderr, "Failed to create VoE handler: %s\n",
                strerror(errno));
        if (voe->data)
            free(voe->data);
        free(voe);
        return NULL; 
    }

    voe->config = sc;
    voe->index = data.voe_index;
    voe->data_size = size;
    voe->mem_size = size;
    return voe;
}

/*****************************************************************************/

void ecrt_slave_config_state(const ec_slave_config_t *sc,
        ec_slave_config_state_t *state)
{
    ec_ioctl_sc_state_t data;

    data.config_index = sc->index;
    data.state = state;
    
    if (ioctl(sc->master->fd, EC_IOCTL_SC_STATE, &data) == -1) {
        fprintf(stderr, "Failed to get slave configuration state: %s\n",
                strerror(errno));
    }
}

/*****************************************************************************/