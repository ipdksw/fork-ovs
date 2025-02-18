"""
DPDK L3 Exact Match (match fields, actions) with vHost

"""

# in-built module imports
import time
import sys

# Unittest related imports
import unittest

# ptf related imports
import ptf
import ptf.dataplane as dataplane
from ptf.base_tests import BaseTest
from ptf.testutils import *
from ptf import config

# scapy related imports
from scapy.packet import *
from scapy.fields import *
from scapy.all import *

# framework related imports
import common.utils.ovsp4ctl_utils as ovs_p4ctl
import common.utils.test_utils as test_utils
from common.utils.config_file_utils import get_config_dict, get_gnmi_params_simple, get_interface_ipv4_dict
from common.utils.gnmi_cli_utils import gnmi_cli_set_and_verify, gnmi_set_params, ip_set_ipv4
from common.lib.telnet_connection import connectionManager


class L3_Action_Selector_Vhost(BaseTest):

    def setUp(self):
        BaseTest.setUp(self)
        self.result = unittest.TestResult()
        
        test_params = test_params_get()
        config_json = test_params['config_json']
        
        try:
            self.vm_cred = test_params['vm_cred']
        except KeyError:
            self.vm_cred = ""

        self.config_data = get_config_dict(config_json,vm_location_list=test_params['vm_location_list'],vm_cred=self.vm_cred)
        self.gnmicli_params = get_gnmi_params_simple(self.config_data)

    def runTest(self):

        if not test_utils.gen_dep_files_p4c_ovs_pipeline_builder(self.config_data):
            self.result.addFailure(self, sys.exc_info())
            self.fail("Failed to generate P4C artifacts or pb.bin")

        if not gnmi_cli_set_and_verify(self.gnmicli_params):
            self.result.addFailure(self, sys.exc_info())
            self.fail("Failed to configure gnmi cli ports")


        # set pipe line
        if not ovs_p4ctl.ovs_p4ctl_set_pipe(self.config_data['switch'], self.config_data['pb_bin'], self.config_data['p4_info']):
            self.result.addFailure(self, sys.exc_info())
            self.fail("Failed to set pipe")

        
        # create VMs
        result, vm_name = test_utils.vm_create(self.config_data['vm_location_list'])
        if not result:
            self.result.addFailure(self, sys.exc_info())
            self.fail(f"VM creation failed for {vm_name}")
      
        # create telnet instance for VMs created
        time.sleep(30)
        vm_id = 0
        for vm, port in zip(self.config_data['vm'], self.config_data['port']):
           globals()["conn"+str(vm_id+1)] = connectionManager("127.0.0.1", f"655{vm_id}", vm['vm_username'], vm['vm_password'], timeout=30)
           globals()["vm"+str(vm_id+1)+"_command_list"] = [f"ip addr add {port['ip']} dev {port['interface']}", f"ip link set dev {port['interface']} address {port['mac']}" , f"ip route add 0.0.0.0/0 via {vm['dst_gw']} dev {port['interface']}"]
           vm_id+=1
        

        # add action_selector rules
        
        table = self.config_data['table'][0]

        print(f"##########  Scenario : {table['description']} ##########")

        print("Add action profile members")
        for member in table['member_details']:
            ovs_p4ctl.ovs_p4ctl_add_member_and_verify(table['switch'],table['name'],member)

        print("Adding action selector groups")
        group_count = 0
        for group in table['group_details']:
            ovs_p4ctl.ovs_p4ctl_add_group_and_verify(table['switch'],table['name'],group)
            group_count+=1

        print(f"Setting up rule for : {table['description']}")
        table = self.config_data['table'][1]
        for match_action in table['match_action']:
            ovs_p4ctl.ovs_p4ctl_add_entry(table['switch'],table['name'], match_action)

        # configuring VMs
        print("Configuring VM0 ....")
        test_utils.configure_vm(conn1, vm1_command_list)

        print("Configuring VM1 ....")
        test_utils.configure_vm(conn2, vm2_command_list)


        # verify whether traffic hits group-1
        print("Verify whether traffic hits group-3 from VM0 to VM1")
        dst_ip=self.config_data['traffic']['in_pkt_header']['ip_dst'][0]
        result = test_utils.vm_to_vm_ping_test(conn1, dst_ip)
        if not result:
            self.result.addFailure(self, sys.exc_info())
            print("FAIL: Traffic test failed for group-1")

        # verify whether traffic hits group-2
        print("Verify whether traffic hits group-3 from VM0 to VM1")
        dst_ip=self.config_data['traffic']['in_pkt_header']['ip_dst'][1]
        result = test_utils.vm_to_vm_ping_test(conn1, dst_ip)
        if not result:
            self.result.addFailure(self, sys.exc_info())
            print("FAIL: Traffic test failed for group-2")

        # verify whether traffic hits group-3
        print("Verify whether traffic hits group-3 from VM0 to VM1")
        dst_ip=self.config_data['traffic']['in_pkt_header']['ip_dst'][2]
        result = test_utils.vm_to_vm_ping_test(conn1, dst_ip)
        if not result:
            self.result.addFailure(self, sys.exc_info())
            print("FAIL: Traffic test failed for group-3")
        

        # close telnet connections
        conn1.close()
        conn2.close()


    def tearDown(self):

        table = self.config_data['table'][1]

        print(f"Deleting rules")
        for del_action in table['del_action']:
            ovs_p4ctl.ovs_p4ctl_del_entry(table['switch'], table['name'], del_action)

        table = self.config_data['table'][0]
        print("Deleting groups")
        for del_group in table['del_group']:
            ovs_p4ctl.ovs_p4ctl_del_group(table['switch'],table['name'],del_group)

        print("Deleting members")
        for del_member in table['del_member']:
            ovs_p4ctl.ovs_p4ctl_del_member(table['switch'],table['name'],del_member)


