"""
DPDK Hot Plug Remove Multi port
"""

# in-built module imports
import time
import sys
from itertools import dropwhile

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
from common.utils.config_file_utils import get_config_dict, get_gnmi_params_simple, get_gnmi_params_hotplug, get_interface_ipv4_dict, get_interface_ipv4_dict_hotplug, get_interface_mac_dict_hotplug, get_interface_ipv4_route_dict_hotplug, create_port_vm_map
from common.utils.gnmi_cli_utils import gnmi_cli_set_and_verify, gnmi_set_params, ip_set_ipv4
from common.lib.telnet_connection import connectionManager


class Dpdk_Hot_Plug(BaseTest):

    def setUp(self):
        BaseTest.setUp(self)
        self.result = unittest.TestResult()
        config["relax"] = True # for verify_packets to ignore other packets received at the interface
        
        test_params = test_params_get()
        config_json = test_params['config_json']
        try:
            self.vm_cred = test_params['vm_cred']
        except KeyError:
            self.vm_cred = ""
        self.dataplane = ptf.dataplane_instance
        ptf.dataplane_instance = ptf.dataplane.DataPlane(config)

        self.config_data = get_config_dict(config_json,vm_location_list=test_params['vm_location_list'])
        print(self.config_data)

        self.gnmicli_params = get_gnmi_params_simple(self.config_data)
        self.gnmicli_hotplug_params = get_gnmi_params_hotplug(self.config_data)
        self.gnmicli_hotplug_delete_params = get_gnmi_params_hotplug(self.config_data,action="del")
        self.interface_ip_list = get_interface_ipv4_dict(self.config_data)

    def runTest(self):
        
        qemu_ver = test_utils.qemu_version("6.1.0")
        print(qemu_ver)
        if not qemu_ver:
            self.result.addFailure(self, sys.exc_info())
            self.fail("\n qemu version must be >=6.1.0 for this TC")

        result, vm_name = test_utils.vm_create_with_hotplug(self.config_data)
        print(result)
        print(vm_name)
        if not result:
            self.result.addFailure(self, sys.exc_info())
            self.fail(f"Failed to create {vm_name}")

        print("Sleeping for 30 seconds for the vms to come up")
        time.sleep(30)
   
        vm=self.config_data['vm'][0]
        conn1 = connectionManager(vm['hotplug']['qemu-socket-ip'],vm['hotplug']['serial-telnet-port'],vm['vm_username'], password=vm['vm_password'])
        
        vm1=self.config_data['vm'][1]
        conn2 = connectionManager(vm1['hotplug']['qemu-socket-ip'],vm1['hotplug']['serial-telnet-port'],vm1['vm_username'], password=vm1['vm_password'])

        vm1_command_list = ["ip a | egrep \"[0-9]*: \" | cut -d ':' -f 2"]
        result = test_utils.sendCmd_and_recvResult(conn1, vm1_command_list)[0]
        result = result.split("\n")
        vm1result1 = list(dropwhile(lambda x: 'lo\r' not in x, result))

        vm2_command_list = ["ip a | egrep \"[0-9]*: \" | cut -d ':' -f 2"]
        result = test_utils.sendCmd_and_recvResult(conn2, vm2_command_list)[0]
        result = result.split("\n")
        vm2result1 = list(dropwhile(lambda x: 'lo\r' not in x, result))

        if not gnmi_cli_set_and_verify(self.gnmicli_params):
            self.result.addFailure(self, sys.exc_info())
            self.fail("Failed to configure gnmi cli ports")

        for i in range(0, 15):
             print("\nIteration:",i)
             if not gnmi_cli_set_and_verify(self.gnmicli_hotplug_params):
                 self.result.addFailure(self, sys.exc_info())
                 self.fail("Failed to configure hotplug through gnmi")
     
             result = test_utils.sendCmd_and_recvResult(conn1, vm1_command_list)[0]
             result = result.split("\n")
             vm1result2 = list(dropwhile(lambda x: 'lo\r' not in x, result))
      
             result = test_utils.sendCmd_and_recvResult(conn2, vm2_command_list)[0]
             result = result.split("\n")
             vm2result2 = list(dropwhile(lambda x: 'lo\r' not in x, result))

             vm1interfaces = list(set(vm1result2) - set(vm1result1))
             vm1interfaces = [x.strip() for x in vm1interfaces]

             vm2interfaces = list(set(vm2result2) - set(vm2result1))
             vm2interfaces = [x.strip() for x in vm2interfaces]
   
        
             if not vm1interfaces:
                self.result.addFailure(self, sys.exc_info())
                self.fail("Fail to add hotplug through gnmi")
        
             if not vm2interfaces:
                 self.result.addFailure(self, sys.exc_info())
                 self.fail("Fail to add hotplug through gnmi")

             print("PASS: Added hotplug interface for vm1 ",vm1interfaces)
             print("PASS: Added hotplug interface for vm2 ",vm2interfaces)

             if not gnmi_set_params(self.gnmicli_hotplug_delete_params):
                 self.result.addFailure(self, sys.exc_info())
                 self.fail("Failed to remove hotplug through gnmi")

             result = test_utils.sendCmd_and_recvResult(conn1, vm1_command_list)[0]
             result = result.split("\n")
             vm1result2 = list(dropwhile(lambda x: 'lo\r' not in x, result))

             result1 = test_utils.sendCmd_and_recvResult(conn2, vm2_command_list)[0]
             result1 = result1.split("\n")
             vm2result2 = list(dropwhile(lambda x: 'lo\r' not in x, result1))


             vm1interfaces2 = list(set(vm1result2) - set(vm1result1))
             vm1interfaces2 = [x.strip() for x in vm1interfaces2]

             if  vm1interfaces2:
                  self.result.addFailure(self, sys.exc_info())
                  self.fail("Fail to del hotplug through gnmi")
        
             vm2interfaces2 = list(set(vm2result2) - set(vm2result1))
             vm2interfaces2 = [x.strip() for x in vm2interfaces2]

             if  vm2interfaces2:
                  self.result.addFailure(self, sys.exc_info())
                  self.fail("Fail to del hotplug through gnmi")

             print("PASS: Deleted hotplug interface from vm1 ",vm1interfaces)
             print("PASS: Deleted hotplug interface from vm2 ",vm2interfaces)
   
        conn1.close()
        conn2.close()

        self.dataplane.kill()


    def tearDown(self):
        if self.result.wasSuccessful():
            print("Test has PASSED")
        else:
            print("Test has FAILED")

