/*
 * Copyright (c) 2022 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <openvswitch/vlog.h>
#include <config.h>
#include <bf_types/bf_types.h>
#include <port_mgr/dpdk/bf_dpdk_port_if.h>

#include "tdi/common/tdi_defs.h"
#include "tdi/common/c_frontend/tdi_init.h"
#include "tdi/common/c_frontend/tdi_session.h"
#include "tdi/common/c_frontend/tdi_info.h"
#include "switch_pd_utils.h"
#include "switch_internal.h"
#include "switch_fdb.h"
#include "switch_rif_int.h"
#include "switch_base_types.h"
#include "switch_pd_p4_name_mapping.h"

VLOG_DEFINE_THIS_MODULE(switch_pd_fdb);

switch_status_t switch_pd_l2_tx_forward_table_entry(
    switch_device_t device,
    const switch_api_l2_info_t *api_l2_tx_info,
    const switch_api_tunnel_info_t *api_tunnel_info,
    bool entry_add) {

    tdi_status_t status;

    tdi_id_t field_id = 0;
    tdi_id_t action_id = 0;
    tdi_id_t data_field_id = 0;

    tdi_dev_id_t dev_id = device;

    const tdi_flags_hdl *flags_hdl = NULL;
    tdi_target_hdl *target_hdl = NULL;
    tdi_device_hdl *dev_hdl = NULL;
    tdi_session_hdl *session = NULL;
    tdi_info_hdl *info_hdl = NULL;
    tdi_table_key_hdl *key_hdl = NULL;
    tdi_table_data_hdl *data_hdl = NULL;
    const tdi_table_hdl *table_hdl = NULL;
    const tdi_table_info_hdl *table_info_hdl = NULL;
    uint32_t network_byte_order;

    VLOG_DBG("%s", __func__);

    // TODO: if session creation fails, need to clean up flags_create
    // and target_create
    status = tdi_flags_create(0, &flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create flags handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_device_get(dev_id, &dev_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get device handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_target_create(dev_hdl, &target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create target handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }
    
    status = tdi_session_create(dev_hdl, &session);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create tdi session, error: %d", status);
        return status;
    }

    status = tdi_info_get(dev_id, PROGRAM_NAME, &info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get tdi info handle, error: %d", status);
        goto dealloc_handle_session;
    }

    status = tdi_table_from_name_get(info_hdl,
                                     LNW_L2_FWD_TX_TABLE,
                                     &table_hdl);
    if (status != TDI_SUCCESS || !table_hdl) {
        VLOG_ERR("Unable to get table handle for: %s, error: %d",
                 LNW_L2_FWD_TX_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_key_allocate(table_hdl, &key_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to allocate key handle for: %s, error: %d",
                 LNW_L2_FWD_TX_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_info_get(table_hdl, &table_info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to get table info handle for table, error: %d", status);
        goto dealloc_handle_session;
    }

    status = tdi_key_field_id_get(table_info_hdl,
                                  LNW_L2_FWD_TX_TABLE_KEY_DST_MAC,
                                  &field_id);
    if (status != TDI_SUCCESS) {
      VLOG_ERR("Unable to get field ID for key: %s, error: %d",
                LNW_L2_FWD_TX_TABLE_KEY_DST_MAC, status);
        goto dealloc_handle_session;
    }

    status = tdi_key_field_set_value_ptr(key_hdl, field_id, 
                                         (const uint8_t *)
                                         &api_l2_tx_info->dst_mac.mac_addr,
                                         SWITCH_MAC_LENGTH);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to set value for key ID: %d, error: %d",
                 field_id, status);
        goto dealloc_handle_session;
    }

    if (entry_add &&
        api_l2_tx_info->learn_from == SWITCH_L2_FWD_LEARN_TUNNEL_INTERFACE) {

        VLOG_INFO("Populate set_tunnel action in %s for tunnel "
                  "interface %x", LNW_L2_FWD_TX_TABLE,
                  (unsigned int)api_l2_tx_info->rif_handle);

        status = tdi_action_name_to_id(table_info_hdl,
                                       LNW_L2_FWD_TX_TABLE_ACTION_SET_TUNNEL,
                                       &action_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator ID for: %s, error: %d",
                     LNW_L2_FWD_TX_TABLE_ACTION_SET_TUNNEL, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_action_data_allocate(table_hdl, action_id,
                                                &data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator for ID: %d, "
                     "error: %d", action_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_SET_TUNNEL_PARAM_TUNNEL_ID,
                                                   action_id,
                                                   &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                    LNW_ACTION_SET_TUNNEL_PARAM_TUNNEL_ID, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_set_value_ptr(data_hdl,
                                              data_field_id,
                                              0,
                                              sizeof(uint32_t));
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_SET_TUNNEL_PARAM_DST_ADDR,
                                                   action_id,
                                                   &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                     LNW_ACTION_SET_TUNNEL_PARAM_DST_ADDR, status);
            goto dealloc_handle_session;
        }

        network_byte_order = ntohl(api_tunnel_info->dst_ip.ip.v4addr);
        status = tdi_data_field_set_value_ptr(data_hdl, data_field_id,
                                              (const uint8_t *)
                                              &network_byte_order,
                                              sizeof(uint32_t));
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_entry_add(table_hdl, session, target_hdl,
                                     flags_hdl, key_hdl, data_hdl);
        if (status != TDI_SUCCESS) {
          VLOG_ERR("Unable to add %s table entry, error: %d", 
                   LNW_L2_FWD_TX_TABLE, status);
            goto dealloc_handle_session;
        }

    } else if (entry_add &&
               api_l2_tx_info->learn_from ==
               SWITCH_L2_FWD_LEARN_VLAN_INTERFACE) {

        VLOG_INFO("Populate l2_fwd action in %s "
                  "for VLAN netdev: vlan%d",
                  LNW_L2_FWD_TX_TABLE,
                  api_l2_tx_info->port_id+1);

        status = tdi_action_name_to_id(table_info_hdl,
                                       LNW_L2_FWD_TX_TABLE_ACTION_L2_FWD,
                                       &action_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator ID for: %s, error: %d",
                     LNW_L2_FWD_TX_TABLE_ACTION_L2_FWD, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_action_data_allocate(table_hdl,
                                                action_id,
                                                &data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator for ID: %d, "
                     "error: %d", action_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_L2_FWD_PARAM_PORT,
                                                   action_id,
                                                   &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                     LNW_ACTION_L2_FWD_PARAM_PORT, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_set_value(data_hdl, data_field_id,
                                          api_l2_tx_info->port_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_entry_add(table_hdl, session, target_hdl,
                                     flags_hdl, key_hdl, data_hdl);
        if (status != TDI_SUCCESS) {
          VLOG_ERR("Unable to add %s table entry, error: %d", 
                    LNW_L2_FWD_TX_TABLE, status);
            goto dealloc_handle_session;
        }

    } else if (entry_add &&
               api_l2_tx_info->learn_from ==
               SWITCH_L2_FWD_LEARN_PHYSICAL_INTERFACE) {

        VLOG_INFO("Populate l2_fwd action in %s "
                  "for physical port: %d",
                  LNW_L2_FWD_TX_TABLE, api_l2_tx_info->port_id);
        
        status = tdi_action_name_to_id(table_info_hdl,
                                       LNW_L2_FWD_TX_TABLE_ACTION_L2_FWD,
                                       &action_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator ID for: %s, error: %d",
                     LNW_L2_FWD_TX_TABLE_ACTION_L2_FWD, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_action_data_allocate(table_hdl, action_id,
                                                &data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator for ID: %d, "
                     "error: %d", action_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_L2_FWD_PARAM_PORT,
                                                   action_id,
                                                   &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                     LNW_ACTION_L2_FWD_PARAM_PORT, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_set_value(data_hdl, data_field_id,
                                          api_l2_tx_info->port_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_entry_add(table_hdl, session, target_hdl,
                                     flags_hdl, key_hdl, data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to add %s entry, error: %d",
                      LNW_L2_FWD_TX_TABLE, status);
            goto dealloc_handle_session;
        }
    } else {
        /* Delete an entry from target */
        status = tdi_table_entry_del(table_hdl, session, target_hdl, 
                                     flags_hdl, key_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to delete %s table entry, error: %d", 
                     LNW_L2_FWD_TX_TABLE, status);
            goto dealloc_handle_session;
        }
    }

dealloc_handle_session:

    status = tdi_flags_delete(flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate flags handle, error: %d", status);
    }

    status = tdi_target_delete(target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate target handle, error: %d", status);
    }

    status = tdi_switch_pd_deallocate_handle_session(key_hdl, data_hdl,
                                                     session, entry_add);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate session and handles, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    return switch_pd_tdi_status_to_status(status);
}

switch_status_t switch_pd_l2_rx_forward_table_entry(
    switch_device_t device,
    const switch_api_l2_info_t *api_l2_rx_info,
    bool entry_add) {

    tdi_status_t status;

    tdi_id_t field_id = 0;
    tdi_id_t action_id = 0;
    tdi_id_t data_field_id = 0;

    tdi_dev_id_t dev_id = device;

    const tdi_flags_hdl *flags_hdl = NULL;
    tdi_target_hdl *target_hdl = NULL;
    tdi_device_hdl *dev_hdl = NULL;
    tdi_session_hdl *session = NULL;
    tdi_info_hdl *info_hdl = NULL;
    tdi_table_key_hdl *key_hdl = NULL;
    tdi_table_data_hdl *data_hdl = NULL;
    const tdi_table_hdl *table_hdl = NULL;
    const tdi_table_info_hdl *table_info_hdl = NULL;

    switch_handle_t rif_handle;
    switch_rif_info_t *rif_info = NULL;
    switch_port_t port_id;

    VLOG_DBG("%s", __func__);

    status = tdi_flags_create(0, &flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create flags handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_device_get(dev_id, &dev_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get device handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_target_create(dev_hdl, &target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create target handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_session_create(dev_hdl, &session);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create tdi session, error: %d", status);
        return status;
    }

    status = tdi_info_get(dev_id, PROGRAM_NAME, &info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get tdi info handle, error: %d", status);
        goto dealloc_handle_session;
    }

    status = tdi_table_from_name_get(info_hdl,
                                     LNW_L2_FWD_RX_TABLE,
                                     &table_hdl);
    if (status != TDI_SUCCESS || !table_hdl) {
        VLOG_ERR("Unable to get table handle for: %s, error: %d",
                 LNW_L2_FWD_RX_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_key_allocate(table_hdl, &key_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to allocate key handle for: %s, error: %d",
                 LNW_NEXTHOP_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_info_get(table_hdl, &table_info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to get table info handle for table, error: %d", status);
        goto dealloc_handle_session;
    }

    status = tdi_key_field_id_get(table_info_hdl,
                                  LNW_L2_FWD_RX_TABLE_KEY_DST_MAC,
                                  &field_id);
    if (status != TDI_SUCCESS) {
      VLOG_ERR("Unable to get field ID for key: %s, error: %d",
               LNW_L2_FWD_RX_TABLE_KEY_DST_MAC, status);
        goto dealloc_handle_session;
    }

    status = tdi_key_field_set_value_ptr(key_hdl, field_id, 
                                         (const uint8_t *)
                                         &api_l2_rx_info->dst_mac.mac_addr, 
                                         SWITCH_MAC_LENGTH);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to set value for key ID: %d, error: %d",
                 field_id, status);
        goto dealloc_handle_session;
    }

    if (entry_add) {
        /* Add an entry to target */
        VLOG_INFO("Populate l2_fwd action in %s for rif handle "
                  "%x ", LNW_L2_FWD_RX_TABLE,
                  (unsigned int) api_l2_rx_info->rif_handle);

        status = tdi_action_name_to_id(table_info_hdl,
                                       LNW_L2_FWD_RX_TABLE_ACTION_L2_FWD,
                                       &action_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator ID for: %s, error: %d",
                     LNW_L2_FWD_RX_TABLE_ACTION_L2_FWD, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_action_data_allocate(table_hdl, action_id,
                                                &data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator for ID: %d, "
                     "error: %d", action_id, status);
            goto dealloc_handle_session;
        }

        rif_handle = api_l2_rx_info->rif_handle;
        switch_status_t switch_status = switch_rif_get(device,
                                                       rif_handle,
                                                       &rif_info);
        if (switch_status != SWITCH_STATUS_SUCCESS) {
            VLOG_ERR("Unable to get rif info, error: %d", switch_status);
            goto dealloc_handle_session;
        }

        if (rif_info->api_rif_info.port_id == -1) {
          switch_pd_to_get_port_id(&(rif_info->api_rif_info));
        }

        /* While matching l2_fwd_rx_table should receive packet on phy-port
         * and send to control port. */
        port_id = rif_info->api_rif_info.port_id;

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_L2_FWD_PARAM_PORT,
                                                   action_id,
                                                   &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                     LNW_ACTION_L2_FWD_PARAM_PORT, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_set_value(data_hdl, data_field_id, port_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_entry_add(table_hdl, session, target_hdl,
                                     flags_hdl, key_hdl, data_hdl);
        if (status != TDI_SUCCESS) {
          VLOG_ERR("Unable to add %s table entry, error: %d", 
                   LNW_L2_FWD_RX_TABLE, status);
            goto dealloc_handle_session;
        }
    } else {
        /* Delete an entry from target */
        status = tdi_table_entry_del(table_hdl, session, target_hdl, 
                                    flags_hdl, key_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to delete %s entry, error: %d", 
                     LNW_L2_FWD_RX_TABLE, status);
            goto dealloc_handle_session;
        }
    }

dealloc_handle_session:

    status = tdi_flags_delete(flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate flags handle, error: %d", status);
    }

    status = tdi_target_delete(target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate target handle, error: %d", status);
    }

    status = tdi_switch_pd_deallocate_handle_session(key_hdl, data_hdl,
                                                     session, entry_add);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate session and handles, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    return switch_pd_tdi_status_to_status(status);
}

switch_status_t switch_pd_l2_rx_forward_with_tunnel_table_entry(
    switch_device_t device,
    const switch_api_l2_info_t *api_l2_rx_info,
    bool entry_add) {

    tdi_status_t status;

    tdi_id_t field_id = 0;
    tdi_id_t action_id = 0;
    tdi_id_t data_field_id = 0;

    tdi_dev_id_t dev_id = device;

    const tdi_flags_hdl *flags_hdl = NULL;
    tdi_target_hdl *target_hdl = NULL;
    tdi_device_hdl *dev_hdl = NULL;
    tdi_session_hdl *session = NULL;
    tdi_info_hdl *info_hdl = NULL;
    tdi_table_key_hdl *key_hdl = NULL;
    tdi_table_data_hdl *data_hdl = NULL;
    const tdi_table_hdl *table_hdl = NULL;
    const tdi_table_info_hdl *table_info_hdl = NULL;

    VLOG_DBG("%s", __func__);

    status = tdi_flags_create(0, &flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create flags handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_device_get(dev_id, &dev_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get device handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_target_create(dev_hdl, &target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create target handle, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    status = tdi_session_create(dev_hdl, &session);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to create tdi session, error: %d", status);
        return status;
    }

    status = tdi_info_get(dev_id, PROGRAM_NAME, &info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Failed to get tdi info handle, error: %d", status);
        goto dealloc_handle_session;
    }

    status = tdi_table_from_name_get(info_hdl,
                                     LNW_L2_FWD_RX_WITH_TUNNEL_TABLE,
                                     &table_hdl);
    if (status != TDI_SUCCESS || !table_hdl) {
        VLOG_ERR("Unable to get table handle for: %s, error: %d",
                 LNW_L2_FWD_RX_WITH_TUNNEL_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_key_allocate(table_hdl, &key_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to allocate key handle for: %s, error: %d",
                 LNW_L2_FWD_RX_WITH_TUNNEL_TABLE, status);
        goto dealloc_handle_session;
    }

    status = tdi_table_info_get(table_hdl, &table_info_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to get table info handle for table");
        goto dealloc_handle_session;
    }

    status = tdi_key_field_id_get(table_info_hdl,
                                  LNW_L2_FWD_RX_WITH_TUNNEL_TABLE_KEY_DST_MAC,
                                  &field_id);
    if (status != TDI_SUCCESS) {
      VLOG_ERR("Unable to get field ID for key: %s, error: %d",
               LNW_L2_FWD_RX_WITH_TUNNEL_TABLE_KEY_DST_MAC, status);
        goto dealloc_handle_session;
    }

    status = tdi_key_field_set_value_ptr(key_hdl, field_id, 
                                         (const uint8_t *)
                                         &api_l2_rx_info->dst_mac.mac_addr, 
                                         SWITCH_MAC_LENGTH);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to set value for key ID: %d", field_id);
        goto dealloc_handle_session;
    }

    if (entry_add) {
        /* Add an entry to target */
        VLOG_INFO("Populate l2_fwd action in %s for rif handle %x",
                  LNW_L2_FWD_RX_WITH_TUNNEL_TABLE,
                  (unsigned int) api_l2_rx_info->rif_handle);

        status = tdi_action_name_to_id(table_info_hdl,
                                       LNW_L2_FWD_RX_WITH_TUNNEL_TABLE_ACTION_L2_FWD,
                                       &action_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator ID for: %s, error: %d",
                     LNW_L2_FWD_RX_WITH_TUNNEL_TABLE_ACTION_L2_FWD, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_action_data_allocate(table_hdl, action_id, &data_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get action allocator for ID: %d, "
                    "error: %d", action_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_id_with_action_get(table_info_hdl,
                                                   LNW_ACTION_L2_FWD_PARAM_PORT,
                                                   action_id, &data_field_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to get data field id param for: %s, error: %d",
                     LNW_ACTION_L2_FWD_PARAM_PORT, status);
            goto dealloc_handle_session;
        }

        status = tdi_data_field_set_value(data_hdl, data_field_id,
                                          api_l2_rx_info->port_id);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to set action value for ID: %d, error: %d",
                     data_field_id, status);
            goto dealloc_handle_session;
        }

        status = tdi_table_entry_add(table_hdl, session, target_hdl,
                                     flags_hdl, key_hdl, data_hdl);
        if (status != TDI_SUCCESS) {
          VLOG_ERR("Unable to add %s entry, error: %d", 
                   LNW_L2_FWD_RX_WITH_TUNNEL_TABLE, status);
            goto dealloc_handle_session;
        }
    } else {
        /* Delete an entry from target */
        status = tdi_table_entry_del(table_hdl, session, target_hdl, 
                                     flags_hdl, key_hdl);
        if (status != TDI_SUCCESS) {
            VLOG_ERR("Unable to delete %s entry, error: %d", 
                     LNW_L2_FWD_RX_WITH_TUNNEL_TABLE, status);
            goto dealloc_handle_session;
        }
    }

dealloc_handle_session:

    status = tdi_flags_delete(flags_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate flags handle, error: %d", status);
    }

    status = tdi_target_delete(target_hdl);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate target handle, error: %d", status);
    }

    status = tdi_switch_pd_deallocate_handle_session(key_hdl, data_hdl,
                                                     session, entry_add);
    if (status != TDI_SUCCESS) {
        VLOG_ERR("Unable to deallocate session and handles, error: %d", status);
        return switch_pd_tdi_status_to_status(status);
    }

    return switch_pd_tdi_status_to_status(status);
}
