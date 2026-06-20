#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/bridge-module.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer swtches;
    
    gpunodes.Create<GPU>(8);
    swtches.Create(6);
    CsmaHelper csma0;
    csma0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma0.SetChannelAttribute("Delay", StringValue("300ns"));
    csma0.SetChannelAttribute("DataRate", StringValue("125Gbps"));
    
    CsmaHelper csma1;
    csma1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma1.SetChannelAttribute("Delay", StringValue("1500ns"));
    csma1.SetChannelAttribute("DataRate", StringValue("9Gbps"));
    
    NodeContainer nc0_0;
    nc0_0.Add(gpunodes.Get(0));
    nc0_0.Add(swtches.Get(0));
    NetDeviceContainer devs0_0 = csma0.Install(nc0_0);
    
    for (int i = 0; i < 8; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
        }
    }
    NodeContainer nc0_1;
    nc0_1.Add(swtches.Get(0));
    nc0_1.Add(gpunodes.Get(0));
    NetDeviceContainer devs0_1 = csma0.Install(nc0_1);
    
    for (int i = 0; i < 8; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_1.Get(1));
        }
    }
    NodeContainer nc0_2;
    nc0_2.Add(gpunodes.Get(1));
    nc0_2.Add(swtches.Get(0));
    NetDeviceContainer devs0_2 = csma0.Install(nc0_2);
    
    for (int i = 0; i < 8; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_2.Get(0));
        }
    }
    NodeContainer nc0_3;
    nc0_3.Add(swtches.Get(0));
    nc0_3.Add(gpunodes.Get(1));
    NetDeviceContainer devs0_3 = csma0.Install(nc0_3);
    
    for (int i = 0; i < 8; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_3.Get(1));
        }
    }
    NodeContainer nc0_4;
    nc0_4.Add(gpunodes.Get(2));
    nc0_4.Add(swtches.Get(0));
    NetDeviceContainer devs0_4 = csma0.Install(nc0_4);
    
    for (int i = 0; i < 8; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(i, devs0_4.Get(0));
        }
    }
    NodeContainer nc0_5;
    nc0_5.Add(swtches.Get(0));
    nc0_5.Add(gpunodes.Get(2));
    NetDeviceContainer devs0_5 = csma0.Install(nc0_5);
    
    for (int i = 0; i < 8; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(i, devs0_5.Get(1));
        }
    }
    NodeContainer nc0_6;
    nc0_6.Add(gpunodes.Get(3));
    nc0_6.Add(swtches.Get(0));
    NetDeviceContainer devs0_6 = csma0.Install(nc0_6);
    
    for (int i = 0; i < 8; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushSendPeerDevice(i, devs0_6.Get(0));
        }
    }
    NodeContainer nc0_7;
    nc0_7.Add(swtches.Get(0));
    nc0_7.Add(gpunodes.Get(3));
    NetDeviceContainer devs0_7 = csma0.Install(nc0_7);
    
    for (int i = 0; i < 8; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushRecvPeerDevice(i, devs0_7.Get(1));
        }
    }
    NodeContainer nc0_8;
    nc0_8.Add(gpunodes.Get(4));
    nc0_8.Add(swtches.Get(1));
    NetDeviceContainer devs0_8 = csma0.Install(nc0_8);
    
    for (int i = 0; i < 8; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushSendPeerDevice(i, devs0_8.Get(0));
        }
    }
    NodeContainer nc0_9;
    nc0_9.Add(swtches.Get(1));
    nc0_9.Add(gpunodes.Get(4));
    NetDeviceContainer devs0_9 = csma0.Install(nc0_9);
    
    for (int i = 0; i < 8; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushRecvPeerDevice(i, devs0_9.Get(1));
        }
    }
    NodeContainer nc0_10;
    nc0_10.Add(gpunodes.Get(5));
    nc0_10.Add(swtches.Get(1));
    NetDeviceContainer devs0_10 = csma0.Install(nc0_10);
    
    for (int i = 0; i < 8; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushSendPeerDevice(i, devs0_10.Get(0));
        }
    }
    NodeContainer nc0_11;
    nc0_11.Add(swtches.Get(1));
    nc0_11.Add(gpunodes.Get(5));
    NetDeviceContainer devs0_11 = csma0.Install(nc0_11);
    
    for (int i = 0; i < 8; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushRecvPeerDevice(i, devs0_11.Get(1));
        }
    }
    NodeContainer nc0_12;
    nc0_12.Add(gpunodes.Get(6));
    nc0_12.Add(swtches.Get(1));
    NetDeviceContainer devs0_12 = csma0.Install(nc0_12);
    
    for (int i = 0; i < 8; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushSendPeerDevice(i, devs0_12.Get(0));
        }
    }
    NodeContainer nc0_13;
    nc0_13.Add(swtches.Get(1));
    nc0_13.Add(gpunodes.Get(6));
    NetDeviceContainer devs0_13 = csma0.Install(nc0_13);
    
    for (int i = 0; i < 8; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushRecvPeerDevice(i, devs0_13.Get(1));
        }
    }
    NodeContainer nc0_14;
    nc0_14.Add(gpunodes.Get(7));
    nc0_14.Add(swtches.Get(1));
    NetDeviceContainer devs0_14 = csma0.Install(nc0_14);
    
    for (int i = 0; i < 8; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushSendPeerDevice(i, devs0_14.Get(0));
        }
    }
    NodeContainer nc0_15;
    nc0_15.Add(swtches.Get(1));
    nc0_15.Add(gpunodes.Get(7));
    NetDeviceContainer devs0_15 = csma0.Install(nc0_15);
    
    for (int i = 0; i < 8; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushRecvPeerDevice(i, devs0_15.Get(1));
        }
    }
    NodeContainer nc1_16;
    nc1_16.Add(swtches.Get(0));
    nc1_16.Add(swtches.Get(2));
    NetDeviceContainer devs1_16 = csma1.Install(nc1_16);
    
    NodeContainer nc1_17;
    nc1_17.Add(swtches.Get(1));
    nc1_17.Add(swtches.Get(4));
    NetDeviceContainer devs1_17 = csma1.Install(nc1_17);
    
    NodeContainer nc1_18;
    nc1_18.Add(swtches.Get(2));
    nc1_18.Add(swtches.Get(5));
    NetDeviceContainer devs1_18 = csma1.Install(nc1_18);
    
    NodeContainer nc1_19;
    nc1_19.Add(swtches.Get(2));
    nc1_19.Add(swtches.Get(3));
    NetDeviceContainer devs1_19 = csma1.Install(nc1_19);
    
    NodeContainer nc1_20;
    nc1_20.Add(swtches.Get(3));
    nc1_20.Add(swtches.Get(4));
    NetDeviceContainer devs1_20 = csma1.Install(nc1_20);
    
    NodeContainer nc1_21;
    nc1_21.Add(swtches.Get(4));
    nc1_21.Add(swtches.Get(5));
    NetDeviceContainer devs1_21 = csma1.Install(nc1_21);
    
    
    // Bridge setup for switches
    NetDeviceContainer bridgePorts_c1_sw;
    bridgePorts_c1_sw.Add(devs0_0.Get(1));
    bridgePorts_c1_sw.Add(devs0_1.Get(0));
    bridgePorts_c1_sw.Add(devs0_2.Get(1));
    bridgePorts_c1_sw.Add(devs0_3.Get(0));
    bridgePorts_c1_sw.Add(devs0_4.Get(1));
    bridgePorts_c1_sw.Add(devs0_5.Get(0));
    bridgePorts_c1_sw.Add(devs0_6.Get(1));
    bridgePorts_c1_sw.Add(devs0_7.Get(0));
    bridgePorts_c1_sw.Add(devs1_16.Get(0));
    SmartSwitchHelper bridge_c1_sw;
    bridge_c1_sw.Install(swtches.Get(0), bridgePorts_c1_sw);
    
    NetDeviceContainer bridgePorts_c2_sw;
    bridgePorts_c2_sw.Add(devs0_8.Get(1));
    bridgePorts_c2_sw.Add(devs0_9.Get(0));
    bridgePorts_c2_sw.Add(devs0_10.Get(1));
    bridgePorts_c2_sw.Add(devs0_11.Get(0));
    bridgePorts_c2_sw.Add(devs0_12.Get(1));
    bridgePorts_c2_sw.Add(devs0_13.Get(0));
    bridgePorts_c2_sw.Add(devs0_14.Get(1));
    bridgePorts_c2_sw.Add(devs0_15.Get(0));
    bridgePorts_c2_sw.Add(devs1_17.Get(0));
    SmartSwitchHelper bridge_c2_sw;
    bridge_c2_sw.Install(swtches.Get(1), bridgePorts_c2_sw);
    
    NetDeviceContainer bridgePorts_sw1;
    bridgePorts_sw1.Add(devs1_16.Get(1));
    bridgePorts_sw1.Add(devs1_18.Get(0));
    bridgePorts_sw1.Add(devs1_19.Get(0));
    SmartSwitchHelper bridge_sw1;
    bridge_sw1.Install(swtches.Get(2), bridgePorts_sw1);
    
    NetDeviceContainer bridgePorts_sw3;
    bridgePorts_sw3.Add(devs1_17.Get(1));
    bridgePorts_sw3.Add(devs1_20.Get(1));
    bridgePorts_sw3.Add(devs1_21.Get(0));
    SmartSwitchHelper bridge_sw3;
    bridge_sw3.Install(swtches.Get(4), bridgePorts_sw3);
    
    NetDeviceContainer bridgePorts_sw4;
    bridgePorts_sw4.Add(devs1_18.Get(1));
    bridgePorts_sw4.Add(devs1_21.Get(1));
    SmartSwitchHelper bridge_sw4;
    bridge_sw4.Install(swtches.Get(5), bridgePorts_sw4);
    
    NetDeviceContainer bridgePorts_sw2;
    bridgePorts_sw2.Add(devs1_19.Get(1));
    bridgePorts_sw2.Add(devs1_20.Get(0));
    SmartSwitchHelper bridge_sw2;
    bridge_sw2.Install(swtches.Get(3), bridgePorts_sw2);
    
    
    /*
        c1_n1 -> c1_sw: devs0_0
        c1_sw -> c1_n1: devs0_1
        c1_n2 -> c1_sw: devs0_2
        c1_sw -> c1_n2: devs0_3
        c1_n3 -> c1_sw: devs0_4
        c1_sw -> c1_n3: devs0_5
        c1_n4 -> c1_sw: devs0_6
        c1_sw -> c1_n4: devs0_7
        c2_n1 -> c2_sw: devs0_8
        c2_sw -> c2_n1: devs0_9
        c2_n2 -> c2_sw: devs0_10
        c2_sw -> c2_n2: devs0_11
        c2_n3 -> c2_sw: devs0_12
        c2_sw -> c2_n3: devs0_13
        c2_n4 -> c2_sw: devs0_14
        c2_sw -> c2_n4: devs0_15
        c1_sw -> sw1: devs1_16
        c2_sw -> sw3: devs1_17
        sw1 -> sw4: devs1_18
        sw1 -> sw2: devs1_19
        sw2 -> sw3: devs1_20
        sw3 -> sw4: devs1_21
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}