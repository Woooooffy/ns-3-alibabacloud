#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/bridge-module.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer swtches;
    
    gpunodes.Create<GPU>(32);
    swtches.Create(4);
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
    
    for (int i = 0; i < 32; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
        }
    }
    NodeContainer nc0_1;
    nc0_1.Add(swtches.Get(0));
    nc0_1.Add(gpunodes.Get(0));
    NetDeviceContainer devs0_1 = csma0.Install(nc0_1);
    
    for (int i = 0; i < 32; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_1.Get(1));
        }
    }
    NodeContainer nc0_2;
    nc0_2.Add(gpunodes.Get(1));
    nc0_2.Add(swtches.Get(0));
    NetDeviceContainer devs0_2 = csma0.Install(nc0_2);
    
    for (int i = 0; i < 32; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_2.Get(0));
        }
    }
    NodeContainer nc0_3;
    nc0_3.Add(swtches.Get(0));
    nc0_3.Add(gpunodes.Get(1));
    NetDeviceContainer devs0_3 = csma0.Install(nc0_3);
    
    for (int i = 0; i < 32; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_3.Get(1));
        }
    }
    NodeContainer nc0_4;
    nc0_4.Add(gpunodes.Get(2));
    nc0_4.Add(swtches.Get(0));
    NetDeviceContainer devs0_4 = csma0.Install(nc0_4);
    
    for (int i = 0; i < 32; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(i, devs0_4.Get(0));
        }
    }
    NodeContainer nc0_5;
    nc0_5.Add(swtches.Get(0));
    nc0_5.Add(gpunodes.Get(2));
    NetDeviceContainer devs0_5 = csma0.Install(nc0_5);
    
    for (int i = 0; i < 32; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(i, devs0_5.Get(1));
        }
    }
    NodeContainer nc0_6;
    nc0_6.Add(gpunodes.Get(3));
    nc0_6.Add(swtches.Get(0));
    NetDeviceContainer devs0_6 = csma0.Install(nc0_6);
    
    for (int i = 0; i < 32; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushSendPeerDevice(i, devs0_6.Get(0));
        }
    }
    NodeContainer nc0_7;
    nc0_7.Add(swtches.Get(0));
    nc0_7.Add(gpunodes.Get(3));
    NetDeviceContainer devs0_7 = csma0.Install(nc0_7);
    
    for (int i = 0; i < 32; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushRecvPeerDevice(i, devs0_7.Get(1));
        }
    }
    NodeContainer nc0_8;
    nc0_8.Add(gpunodes.Get(4));
    nc0_8.Add(swtches.Get(0));
    NetDeviceContainer devs0_8 = csma0.Install(nc0_8);
    
    for (int i = 0; i < 32; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushSendPeerDevice(i, devs0_8.Get(0));
        }
    }
    NodeContainer nc0_9;
    nc0_9.Add(swtches.Get(0));
    nc0_9.Add(gpunodes.Get(4));
    NetDeviceContainer devs0_9 = csma0.Install(nc0_9);
    
    for (int i = 0; i < 32; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushRecvPeerDevice(i, devs0_9.Get(1));
        }
    }
    NodeContainer nc0_10;
    nc0_10.Add(gpunodes.Get(5));
    nc0_10.Add(swtches.Get(0));
    NetDeviceContainer devs0_10 = csma0.Install(nc0_10);
    
    for (int i = 0; i < 32; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushSendPeerDevice(i, devs0_10.Get(0));
        }
    }
    NodeContainer nc0_11;
    nc0_11.Add(swtches.Get(0));
    nc0_11.Add(gpunodes.Get(5));
    NetDeviceContainer devs0_11 = csma0.Install(nc0_11);
    
    for (int i = 0; i < 32; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushRecvPeerDevice(i, devs0_11.Get(1));
        }
    }
    NodeContainer nc0_12;
    nc0_12.Add(gpunodes.Get(6));
    nc0_12.Add(swtches.Get(0));
    NetDeviceContainer devs0_12 = csma0.Install(nc0_12);
    
    for (int i = 0; i < 32; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushSendPeerDevice(i, devs0_12.Get(0));
        }
    }
    NodeContainer nc0_13;
    nc0_13.Add(swtches.Get(0));
    nc0_13.Add(gpunodes.Get(6));
    NetDeviceContainer devs0_13 = csma0.Install(nc0_13);
    
    for (int i = 0; i < 32; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushRecvPeerDevice(i, devs0_13.Get(1));
        }
    }
    NodeContainer nc0_14;
    nc0_14.Add(gpunodes.Get(7));
    nc0_14.Add(swtches.Get(0));
    NetDeviceContainer devs0_14 = csma0.Install(nc0_14);
    
    for (int i = 0; i < 32; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushSendPeerDevice(i, devs0_14.Get(0));
        }
    }
    NodeContainer nc0_15;
    nc0_15.Add(swtches.Get(0));
    nc0_15.Add(gpunodes.Get(7));
    NetDeviceContainer devs0_15 = csma0.Install(nc0_15);
    
    for (int i = 0; i < 32; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushRecvPeerDevice(i, devs0_15.Get(1));
        }
    }
    NodeContainer nc0_16;
    nc0_16.Add(gpunodes.Get(8));
    nc0_16.Add(swtches.Get(1));
    NetDeviceContainer devs0_16 = csma0.Install(nc0_16);
    
    for (int i = 0; i < 32; ++i){
        if (i != 8){
            DynamicCast<GPU>(gpunodes.Get(8))->PushSendPeerDevice(i, devs0_16.Get(0));
        }
    }
    NodeContainer nc0_17;
    nc0_17.Add(swtches.Get(1));
    nc0_17.Add(gpunodes.Get(8));
    NetDeviceContainer devs0_17 = csma0.Install(nc0_17);
    
    for (int i = 0; i < 32; ++i){
        if (i != 8){
            DynamicCast<GPU>(gpunodes.Get(8))->PushRecvPeerDevice(i, devs0_17.Get(1));
        }
    }
    NodeContainer nc0_18;
    nc0_18.Add(gpunodes.Get(9));
    nc0_18.Add(swtches.Get(1));
    NetDeviceContainer devs0_18 = csma0.Install(nc0_18);
    
    for (int i = 0; i < 32; ++i){
        if (i != 9){
            DynamicCast<GPU>(gpunodes.Get(9))->PushSendPeerDevice(i, devs0_18.Get(0));
        }
    }
    NodeContainer nc0_19;
    nc0_19.Add(swtches.Get(1));
    nc0_19.Add(gpunodes.Get(9));
    NetDeviceContainer devs0_19 = csma0.Install(nc0_19);
    
    for (int i = 0; i < 32; ++i){
        if (i != 9){
            DynamicCast<GPU>(gpunodes.Get(9))->PushRecvPeerDevice(i, devs0_19.Get(1));
        }
    }
    NodeContainer nc0_20;
    nc0_20.Add(gpunodes.Get(10));
    nc0_20.Add(swtches.Get(1));
    NetDeviceContainer devs0_20 = csma0.Install(nc0_20);
    
    for (int i = 0; i < 32; ++i){
        if (i != 10){
            DynamicCast<GPU>(gpunodes.Get(10))->PushSendPeerDevice(i, devs0_20.Get(0));
        }
    }
    NodeContainer nc0_21;
    nc0_21.Add(swtches.Get(1));
    nc0_21.Add(gpunodes.Get(10));
    NetDeviceContainer devs0_21 = csma0.Install(nc0_21);
    
    for (int i = 0; i < 32; ++i){
        if (i != 10){
            DynamicCast<GPU>(gpunodes.Get(10))->PushRecvPeerDevice(i, devs0_21.Get(1));
        }
    }
    NodeContainer nc0_22;
    nc0_22.Add(gpunodes.Get(11));
    nc0_22.Add(swtches.Get(1));
    NetDeviceContainer devs0_22 = csma0.Install(nc0_22);
    
    for (int i = 0; i < 32; ++i){
        if (i != 11){
            DynamicCast<GPU>(gpunodes.Get(11))->PushSendPeerDevice(i, devs0_22.Get(0));
        }
    }
    NodeContainer nc0_23;
    nc0_23.Add(swtches.Get(1));
    nc0_23.Add(gpunodes.Get(11));
    NetDeviceContainer devs0_23 = csma0.Install(nc0_23);
    
    for (int i = 0; i < 32; ++i){
        if (i != 11){
            DynamicCast<GPU>(gpunodes.Get(11))->PushRecvPeerDevice(i, devs0_23.Get(1));
        }
    }
    NodeContainer nc0_24;
    nc0_24.Add(gpunodes.Get(12));
    nc0_24.Add(swtches.Get(1));
    NetDeviceContainer devs0_24 = csma0.Install(nc0_24);
    
    for (int i = 0; i < 32; ++i){
        if (i != 12){
            DynamicCast<GPU>(gpunodes.Get(12))->PushSendPeerDevice(i, devs0_24.Get(0));
        }
    }
    NodeContainer nc0_25;
    nc0_25.Add(swtches.Get(1));
    nc0_25.Add(gpunodes.Get(12));
    NetDeviceContainer devs0_25 = csma0.Install(nc0_25);
    
    for (int i = 0; i < 32; ++i){
        if (i != 12){
            DynamicCast<GPU>(gpunodes.Get(12))->PushRecvPeerDevice(i, devs0_25.Get(1));
        }
    }
    NodeContainer nc0_26;
    nc0_26.Add(gpunodes.Get(13));
    nc0_26.Add(swtches.Get(1));
    NetDeviceContainer devs0_26 = csma0.Install(nc0_26);
    
    for (int i = 0; i < 32; ++i){
        if (i != 13){
            DynamicCast<GPU>(gpunodes.Get(13))->PushSendPeerDevice(i, devs0_26.Get(0));
        }
    }
    NodeContainer nc0_27;
    nc0_27.Add(swtches.Get(1));
    nc0_27.Add(gpunodes.Get(13));
    NetDeviceContainer devs0_27 = csma0.Install(nc0_27);
    
    for (int i = 0; i < 32; ++i){
        if (i != 13){
            DynamicCast<GPU>(gpunodes.Get(13))->PushRecvPeerDevice(i, devs0_27.Get(1));
        }
    }
    NodeContainer nc0_28;
    nc0_28.Add(gpunodes.Get(14));
    nc0_28.Add(swtches.Get(1));
    NetDeviceContainer devs0_28 = csma0.Install(nc0_28);
    
    for (int i = 0; i < 32; ++i){
        if (i != 14){
            DynamicCast<GPU>(gpunodes.Get(14))->PushSendPeerDevice(i, devs0_28.Get(0));
        }
    }
    NodeContainer nc0_29;
    nc0_29.Add(swtches.Get(1));
    nc0_29.Add(gpunodes.Get(14));
    NetDeviceContainer devs0_29 = csma0.Install(nc0_29);
    
    for (int i = 0; i < 32; ++i){
        if (i != 14){
            DynamicCast<GPU>(gpunodes.Get(14))->PushRecvPeerDevice(i, devs0_29.Get(1));
        }
    }
    NodeContainer nc0_30;
    nc0_30.Add(gpunodes.Get(15));
    nc0_30.Add(swtches.Get(1));
    NetDeviceContainer devs0_30 = csma0.Install(nc0_30);
    
    for (int i = 0; i < 32; ++i){
        if (i != 15){
            DynamicCast<GPU>(gpunodes.Get(15))->PushSendPeerDevice(i, devs0_30.Get(0));
        }
    }
    NodeContainer nc0_31;
    nc0_31.Add(swtches.Get(1));
    nc0_31.Add(gpunodes.Get(15));
    NetDeviceContainer devs0_31 = csma0.Install(nc0_31);
    
    for (int i = 0; i < 32; ++i){
        if (i != 15){
            DynamicCast<GPU>(gpunodes.Get(15))->PushRecvPeerDevice(i, devs0_31.Get(1));
        }
    }
    NodeContainer nc0_32;
    nc0_32.Add(gpunodes.Get(16));
    nc0_32.Add(swtches.Get(2));
    NetDeviceContainer devs0_32 = csma0.Install(nc0_32);
    
    for (int i = 0; i < 32; ++i){
        if (i != 16){
            DynamicCast<GPU>(gpunodes.Get(16))->PushSendPeerDevice(i, devs0_32.Get(0));
        }
    }
    NodeContainer nc0_33;
    nc0_33.Add(swtches.Get(2));
    nc0_33.Add(gpunodes.Get(16));
    NetDeviceContainer devs0_33 = csma0.Install(nc0_33);
    
    for (int i = 0; i < 32; ++i){
        if (i != 16){
            DynamicCast<GPU>(gpunodes.Get(16))->PushRecvPeerDevice(i, devs0_33.Get(1));
        }
    }
    NodeContainer nc0_34;
    nc0_34.Add(gpunodes.Get(17));
    nc0_34.Add(swtches.Get(2));
    NetDeviceContainer devs0_34 = csma0.Install(nc0_34);
    
    for (int i = 0; i < 32; ++i){
        if (i != 17){
            DynamicCast<GPU>(gpunodes.Get(17))->PushSendPeerDevice(i, devs0_34.Get(0));
        }
    }
    NodeContainer nc0_35;
    nc0_35.Add(swtches.Get(2));
    nc0_35.Add(gpunodes.Get(17));
    NetDeviceContainer devs0_35 = csma0.Install(nc0_35);
    
    for (int i = 0; i < 32; ++i){
        if (i != 17){
            DynamicCast<GPU>(gpunodes.Get(17))->PushRecvPeerDevice(i, devs0_35.Get(1));
        }
    }
    NodeContainer nc0_36;
    nc0_36.Add(gpunodes.Get(18));
    nc0_36.Add(swtches.Get(2));
    NetDeviceContainer devs0_36 = csma0.Install(nc0_36);
    
    for (int i = 0; i < 32; ++i){
        if (i != 18){
            DynamicCast<GPU>(gpunodes.Get(18))->PushSendPeerDevice(i, devs0_36.Get(0));
        }
    }
    NodeContainer nc0_37;
    nc0_37.Add(swtches.Get(2));
    nc0_37.Add(gpunodes.Get(18));
    NetDeviceContainer devs0_37 = csma0.Install(nc0_37);
    
    for (int i = 0; i < 32; ++i){
        if (i != 18){
            DynamicCast<GPU>(gpunodes.Get(18))->PushRecvPeerDevice(i, devs0_37.Get(1));
        }
    }
    NodeContainer nc0_38;
    nc0_38.Add(gpunodes.Get(19));
    nc0_38.Add(swtches.Get(2));
    NetDeviceContainer devs0_38 = csma0.Install(nc0_38);
    
    for (int i = 0; i < 32; ++i){
        if (i != 19){
            DynamicCast<GPU>(gpunodes.Get(19))->PushSendPeerDevice(i, devs0_38.Get(0));
        }
    }
    NodeContainer nc0_39;
    nc0_39.Add(swtches.Get(2));
    nc0_39.Add(gpunodes.Get(19));
    NetDeviceContainer devs0_39 = csma0.Install(nc0_39);
    
    for (int i = 0; i < 32; ++i){
        if (i != 19){
            DynamicCast<GPU>(gpunodes.Get(19))->PushRecvPeerDevice(i, devs0_39.Get(1));
        }
    }
    NodeContainer nc0_40;
    nc0_40.Add(gpunodes.Get(20));
    nc0_40.Add(swtches.Get(2));
    NetDeviceContainer devs0_40 = csma0.Install(nc0_40);
    
    for (int i = 0; i < 32; ++i){
        if (i != 20){
            DynamicCast<GPU>(gpunodes.Get(20))->PushSendPeerDevice(i, devs0_40.Get(0));
        }
    }
    NodeContainer nc0_41;
    nc0_41.Add(swtches.Get(2));
    nc0_41.Add(gpunodes.Get(20));
    NetDeviceContainer devs0_41 = csma0.Install(nc0_41);
    
    for (int i = 0; i < 32; ++i){
        if (i != 20){
            DynamicCast<GPU>(gpunodes.Get(20))->PushRecvPeerDevice(i, devs0_41.Get(1));
        }
    }
    NodeContainer nc0_42;
    nc0_42.Add(gpunodes.Get(21));
    nc0_42.Add(swtches.Get(2));
    NetDeviceContainer devs0_42 = csma0.Install(nc0_42);
    
    for (int i = 0; i < 32; ++i){
        if (i != 21){
            DynamicCast<GPU>(gpunodes.Get(21))->PushSendPeerDevice(i, devs0_42.Get(0));
        }
    }
    NodeContainer nc0_43;
    nc0_43.Add(swtches.Get(2));
    nc0_43.Add(gpunodes.Get(21));
    NetDeviceContainer devs0_43 = csma0.Install(nc0_43);
    
    for (int i = 0; i < 32; ++i){
        if (i != 21){
            DynamicCast<GPU>(gpunodes.Get(21))->PushRecvPeerDevice(i, devs0_43.Get(1));
        }
    }
    NodeContainer nc0_44;
    nc0_44.Add(gpunodes.Get(22));
    nc0_44.Add(swtches.Get(2));
    NetDeviceContainer devs0_44 = csma0.Install(nc0_44);
    
    for (int i = 0; i < 32; ++i){
        if (i != 22){
            DynamicCast<GPU>(gpunodes.Get(22))->PushSendPeerDevice(i, devs0_44.Get(0));
        }
    }
    NodeContainer nc0_45;
    nc0_45.Add(swtches.Get(2));
    nc0_45.Add(gpunodes.Get(22));
    NetDeviceContainer devs0_45 = csma0.Install(nc0_45);
    
    for (int i = 0; i < 32; ++i){
        if (i != 22){
            DynamicCast<GPU>(gpunodes.Get(22))->PushRecvPeerDevice(i, devs0_45.Get(1));
        }
    }
    NodeContainer nc0_46;
    nc0_46.Add(gpunodes.Get(23));
    nc0_46.Add(swtches.Get(2));
    NetDeviceContainer devs0_46 = csma0.Install(nc0_46);
    
    for (int i = 0; i < 32; ++i){
        if (i != 23){
            DynamicCast<GPU>(gpunodes.Get(23))->PushSendPeerDevice(i, devs0_46.Get(0));
        }
    }
    NodeContainer nc0_47;
    nc0_47.Add(swtches.Get(2));
    nc0_47.Add(gpunodes.Get(23));
    NetDeviceContainer devs0_47 = csma0.Install(nc0_47);
    
    for (int i = 0; i < 32; ++i){
        if (i != 23){
            DynamicCast<GPU>(gpunodes.Get(23))->PushRecvPeerDevice(i, devs0_47.Get(1));
        }
    }
    NodeContainer nc0_48;
    nc0_48.Add(gpunodes.Get(24));
    nc0_48.Add(swtches.Get(3));
    NetDeviceContainer devs0_48 = csma0.Install(nc0_48);
    
    for (int i = 0; i < 32; ++i){
        if (i != 24){
            DynamicCast<GPU>(gpunodes.Get(24))->PushSendPeerDevice(i, devs0_48.Get(0));
        }
    }
    NodeContainer nc0_49;
    nc0_49.Add(swtches.Get(3));
    nc0_49.Add(gpunodes.Get(24));
    NetDeviceContainer devs0_49 = csma0.Install(nc0_49);
    
    for (int i = 0; i < 32; ++i){
        if (i != 24){
            DynamicCast<GPU>(gpunodes.Get(24))->PushRecvPeerDevice(i, devs0_49.Get(1));
        }
    }
    NodeContainer nc0_50;
    nc0_50.Add(gpunodes.Get(25));
    nc0_50.Add(swtches.Get(3));
    NetDeviceContainer devs0_50 = csma0.Install(nc0_50);
    
    for (int i = 0; i < 32; ++i){
        if (i != 25){
            DynamicCast<GPU>(gpunodes.Get(25))->PushSendPeerDevice(i, devs0_50.Get(0));
        }
    }
    NodeContainer nc0_51;
    nc0_51.Add(swtches.Get(3));
    nc0_51.Add(gpunodes.Get(25));
    NetDeviceContainer devs0_51 = csma0.Install(nc0_51);
    
    for (int i = 0; i < 32; ++i){
        if (i != 25){
            DynamicCast<GPU>(gpunodes.Get(25))->PushRecvPeerDevice(i, devs0_51.Get(1));
        }
    }
    NodeContainer nc0_52;
    nc0_52.Add(gpunodes.Get(26));
    nc0_52.Add(swtches.Get(3));
    NetDeviceContainer devs0_52 = csma0.Install(nc0_52);
    
    for (int i = 0; i < 32; ++i){
        if (i != 26){
            DynamicCast<GPU>(gpunodes.Get(26))->PushSendPeerDevice(i, devs0_52.Get(0));
        }
    }
    NodeContainer nc0_53;
    nc0_53.Add(swtches.Get(3));
    nc0_53.Add(gpunodes.Get(26));
    NetDeviceContainer devs0_53 = csma0.Install(nc0_53);
    
    for (int i = 0; i < 32; ++i){
        if (i != 26){
            DynamicCast<GPU>(gpunodes.Get(26))->PushRecvPeerDevice(i, devs0_53.Get(1));
        }
    }
    NodeContainer nc0_54;
    nc0_54.Add(gpunodes.Get(27));
    nc0_54.Add(swtches.Get(3));
    NetDeviceContainer devs0_54 = csma0.Install(nc0_54);
    
    for (int i = 0; i < 32; ++i){
        if (i != 27){
            DynamicCast<GPU>(gpunodes.Get(27))->PushSendPeerDevice(i, devs0_54.Get(0));
        }
    }
    NodeContainer nc0_55;
    nc0_55.Add(swtches.Get(3));
    nc0_55.Add(gpunodes.Get(27));
    NetDeviceContainer devs0_55 = csma0.Install(nc0_55);
    
    for (int i = 0; i < 32; ++i){
        if (i != 27){
            DynamicCast<GPU>(gpunodes.Get(27))->PushRecvPeerDevice(i, devs0_55.Get(1));
        }
    }
    NodeContainer nc0_56;
    nc0_56.Add(gpunodes.Get(28));
    nc0_56.Add(swtches.Get(3));
    NetDeviceContainer devs0_56 = csma0.Install(nc0_56);
    
    for (int i = 0; i < 32; ++i){
        if (i != 28){
            DynamicCast<GPU>(gpunodes.Get(28))->PushSendPeerDevice(i, devs0_56.Get(0));
        }
    }
    NodeContainer nc0_57;
    nc0_57.Add(swtches.Get(3));
    nc0_57.Add(gpunodes.Get(28));
    NetDeviceContainer devs0_57 = csma0.Install(nc0_57);
    
    for (int i = 0; i < 32; ++i){
        if (i != 28){
            DynamicCast<GPU>(gpunodes.Get(28))->PushRecvPeerDevice(i, devs0_57.Get(1));
        }
    }
    NodeContainer nc0_58;
    nc0_58.Add(gpunodes.Get(29));
    nc0_58.Add(swtches.Get(3));
    NetDeviceContainer devs0_58 = csma0.Install(nc0_58);
    
    for (int i = 0; i < 32; ++i){
        if (i != 29){
            DynamicCast<GPU>(gpunodes.Get(29))->PushSendPeerDevice(i, devs0_58.Get(0));
        }
    }
    NodeContainer nc0_59;
    nc0_59.Add(swtches.Get(3));
    nc0_59.Add(gpunodes.Get(29));
    NetDeviceContainer devs0_59 = csma0.Install(nc0_59);
    
    for (int i = 0; i < 32; ++i){
        if (i != 29){
            DynamicCast<GPU>(gpunodes.Get(29))->PushRecvPeerDevice(i, devs0_59.Get(1));
        }
    }
    NodeContainer nc0_60;
    nc0_60.Add(gpunodes.Get(30));
    nc0_60.Add(swtches.Get(3));
    NetDeviceContainer devs0_60 = csma0.Install(nc0_60);
    
    for (int i = 0; i < 32; ++i){
        if (i != 30){
            DynamicCast<GPU>(gpunodes.Get(30))->PushSendPeerDevice(i, devs0_60.Get(0));
        }
    }
    NodeContainer nc0_61;
    nc0_61.Add(swtches.Get(3));
    nc0_61.Add(gpunodes.Get(30));
    NetDeviceContainer devs0_61 = csma0.Install(nc0_61);
    
    for (int i = 0; i < 32; ++i){
        if (i != 30){
            DynamicCast<GPU>(gpunodes.Get(30))->PushRecvPeerDevice(i, devs0_61.Get(1));
        }
    }
    NodeContainer nc0_62;
    nc0_62.Add(gpunodes.Get(31));
    nc0_62.Add(swtches.Get(3));
    NetDeviceContainer devs0_62 = csma0.Install(nc0_62);
    
    for (int i = 0; i < 32; ++i){
        if (i != 31){
            DynamicCast<GPU>(gpunodes.Get(31))->PushSendPeerDevice(i, devs0_62.Get(0));
        }
    }
    NodeContainer nc0_63;
    nc0_63.Add(swtches.Get(3));
    nc0_63.Add(gpunodes.Get(31));
    NetDeviceContainer devs0_63 = csma0.Install(nc0_63);
    
    for (int i = 0; i < 32; ++i){
        if (i != 31){
            DynamicCast<GPU>(gpunodes.Get(31))->PushRecvPeerDevice(i, devs0_63.Get(1));
        }
    }
    NodeContainer nc1_64;
    nc1_64.Add(swtches.Get(0));
    nc1_64.Add(swtches.Get(1));
    NetDeviceContainer devs1_64 = csma1.Install(nc1_64);
    
    NodeContainer nc1_65;
    nc1_65.Add(swtches.Get(0));
    nc1_65.Add(swtches.Get(2));
    NetDeviceContainer devs1_65 = csma1.Install(nc1_65);
    
    NodeContainer nc1_66;
    nc1_66.Add(swtches.Get(0));
    nc1_66.Add(swtches.Get(3));
    NetDeviceContainer devs1_66 = csma1.Install(nc1_66);
    
    NodeContainer nc1_67;
    nc1_67.Add(swtches.Get(1));
    nc1_67.Add(swtches.Get(0));
    NetDeviceContainer devs1_67 = csma1.Install(nc1_67);
    
    NodeContainer nc1_68;
    nc1_68.Add(swtches.Get(1));
    nc1_68.Add(swtches.Get(2));
    NetDeviceContainer devs1_68 = csma1.Install(nc1_68);
    
    NodeContainer nc1_69;
    nc1_69.Add(swtches.Get(1));
    nc1_69.Add(swtches.Get(3));
    NetDeviceContainer devs1_69 = csma1.Install(nc1_69);
    
    NodeContainer nc1_70;
    nc1_70.Add(swtches.Get(2));
    nc1_70.Add(swtches.Get(0));
    NetDeviceContainer devs1_70 = csma1.Install(nc1_70);
    
    NodeContainer nc1_71;
    nc1_71.Add(swtches.Get(2));
    nc1_71.Add(swtches.Get(1));
    NetDeviceContainer devs1_71 = csma1.Install(nc1_71);
    
    NodeContainer nc1_72;
    nc1_72.Add(swtches.Get(2));
    nc1_72.Add(swtches.Get(3));
    NetDeviceContainer devs1_72 = csma1.Install(nc1_72);
    
    NodeContainer nc1_73;
    nc1_73.Add(swtches.Get(3));
    nc1_73.Add(swtches.Get(0));
    NetDeviceContainer devs1_73 = csma1.Install(nc1_73);
    
    NodeContainer nc1_74;
    nc1_74.Add(swtches.Get(3));
    nc1_74.Add(swtches.Get(1));
    NetDeviceContainer devs1_74 = csma1.Install(nc1_74);
    
    NodeContainer nc1_75;
    nc1_75.Add(swtches.Get(3));
    nc1_75.Add(swtches.Get(2));
    NetDeviceContainer devs1_75 = csma1.Install(nc1_75);
    
    
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
    bridgePorts_c1_sw.Add(devs0_8.Get(1));
    bridgePorts_c1_sw.Add(devs0_9.Get(0));
    bridgePorts_c1_sw.Add(devs0_10.Get(1));
    bridgePorts_c1_sw.Add(devs0_11.Get(0));
    bridgePorts_c1_sw.Add(devs0_12.Get(1));
    bridgePorts_c1_sw.Add(devs0_13.Get(0));
    bridgePorts_c1_sw.Add(devs0_14.Get(1));
    bridgePorts_c1_sw.Add(devs0_15.Get(0));
    bridgePorts_c1_sw.Add(devs1_64.Get(0));
    bridgePorts_c1_sw.Add(devs1_65.Get(0));
    bridgePorts_c1_sw.Add(devs1_66.Get(0));
    bridgePorts_c1_sw.Add(devs1_67.Get(1));
    bridgePorts_c1_sw.Add(devs1_70.Get(1));
    bridgePorts_c1_sw.Add(devs1_73.Get(1));
    BridgeHelper bridge_c1_sw;
    bridge_c1_sw.Install(swtches.Get(0), bridgePorts_c1_sw);
    
    NetDeviceContainer bridgePorts_c2_sw;
    bridgePorts_c2_sw.Add(devs0_16.Get(1));
    bridgePorts_c2_sw.Add(devs0_17.Get(0));
    bridgePorts_c2_sw.Add(devs0_18.Get(1));
    bridgePorts_c2_sw.Add(devs0_19.Get(0));
    bridgePorts_c2_sw.Add(devs0_20.Get(1));
    bridgePorts_c2_sw.Add(devs0_21.Get(0));
    bridgePorts_c2_sw.Add(devs0_22.Get(1));
    bridgePorts_c2_sw.Add(devs0_23.Get(0));
    bridgePorts_c2_sw.Add(devs0_24.Get(1));
    bridgePorts_c2_sw.Add(devs0_25.Get(0));
    bridgePorts_c2_sw.Add(devs0_26.Get(1));
    bridgePorts_c2_sw.Add(devs0_27.Get(0));
    bridgePorts_c2_sw.Add(devs0_28.Get(1));
    bridgePorts_c2_sw.Add(devs0_29.Get(0));
    bridgePorts_c2_sw.Add(devs0_30.Get(1));
    bridgePorts_c2_sw.Add(devs0_31.Get(0));
    bridgePorts_c2_sw.Add(devs1_64.Get(1));
    bridgePorts_c2_sw.Add(devs1_67.Get(0));
    bridgePorts_c2_sw.Add(devs1_68.Get(0));
    bridgePorts_c2_sw.Add(devs1_69.Get(0));
    bridgePorts_c2_sw.Add(devs1_71.Get(1));
    bridgePorts_c2_sw.Add(devs1_74.Get(1));
    BridgeHelper bridge_c2_sw;
    bridge_c2_sw.Install(swtches.Get(1), bridgePorts_c2_sw);
    
    NetDeviceContainer bridgePorts_c3_sw;
    bridgePorts_c3_sw.Add(devs0_32.Get(1));
    bridgePorts_c3_sw.Add(devs0_33.Get(0));
    bridgePorts_c3_sw.Add(devs0_34.Get(1));
    bridgePorts_c3_sw.Add(devs0_35.Get(0));
    bridgePorts_c3_sw.Add(devs0_36.Get(1));
    bridgePorts_c3_sw.Add(devs0_37.Get(0));
    bridgePorts_c3_sw.Add(devs0_38.Get(1));
    bridgePorts_c3_sw.Add(devs0_39.Get(0));
    bridgePorts_c3_sw.Add(devs0_40.Get(1));
    bridgePorts_c3_sw.Add(devs0_41.Get(0));
    bridgePorts_c3_sw.Add(devs0_42.Get(1));
    bridgePorts_c3_sw.Add(devs0_43.Get(0));
    bridgePorts_c3_sw.Add(devs0_44.Get(1));
    bridgePorts_c3_sw.Add(devs0_45.Get(0));
    bridgePorts_c3_sw.Add(devs0_46.Get(1));
    bridgePorts_c3_sw.Add(devs0_47.Get(0));
    bridgePorts_c3_sw.Add(devs1_65.Get(1));
    bridgePorts_c3_sw.Add(devs1_68.Get(1));
    bridgePorts_c3_sw.Add(devs1_70.Get(0));
    bridgePorts_c3_sw.Add(devs1_71.Get(0));
    bridgePorts_c3_sw.Add(devs1_72.Get(0));
    bridgePorts_c3_sw.Add(devs1_75.Get(1));
    BridgeHelper bridge_c3_sw;
    bridge_c3_sw.Install(swtches.Get(2), bridgePorts_c3_sw);
    
    NetDeviceContainer bridgePorts_c4_sw;
    bridgePorts_c4_sw.Add(devs0_48.Get(1));
    bridgePorts_c4_sw.Add(devs0_49.Get(0));
    bridgePorts_c4_sw.Add(devs0_50.Get(1));
    bridgePorts_c4_sw.Add(devs0_51.Get(0));
    bridgePorts_c4_sw.Add(devs0_52.Get(1));
    bridgePorts_c4_sw.Add(devs0_53.Get(0));
    bridgePorts_c4_sw.Add(devs0_54.Get(1));
    bridgePorts_c4_sw.Add(devs0_55.Get(0));
    bridgePorts_c4_sw.Add(devs0_56.Get(1));
    bridgePorts_c4_sw.Add(devs0_57.Get(0));
    bridgePorts_c4_sw.Add(devs0_58.Get(1));
    bridgePorts_c4_sw.Add(devs0_59.Get(0));
    bridgePorts_c4_sw.Add(devs0_60.Get(1));
    bridgePorts_c4_sw.Add(devs0_61.Get(0));
    bridgePorts_c4_sw.Add(devs0_62.Get(1));
    bridgePorts_c4_sw.Add(devs0_63.Get(0));
    bridgePorts_c4_sw.Add(devs1_66.Get(1));
    bridgePorts_c4_sw.Add(devs1_69.Get(1));
    bridgePorts_c4_sw.Add(devs1_72.Get(1));
    bridgePorts_c4_sw.Add(devs1_73.Get(0));
    bridgePorts_c4_sw.Add(devs1_74.Get(0));
    bridgePorts_c4_sw.Add(devs1_75.Get(0));
    BridgeHelper bridge_c4_sw;
    bridge_c4_sw.Install(swtches.Get(3), bridgePorts_c4_sw);
    
    
    /*
        c1_n1 -> c1_sw: devs0_0
        c1_sw -> c1_n1: devs0_1
        c1_n2 -> c1_sw: devs0_2
        c1_sw -> c1_n2: devs0_3
        c1_n3 -> c1_sw: devs0_4
        c1_sw -> c1_n3: devs0_5
        c1_n4 -> c1_sw: devs0_6
        c1_sw -> c1_n4: devs0_7
        c1_n5 -> c1_sw: devs0_8
        c1_sw -> c1_n5: devs0_9
        c1_n6 -> c1_sw: devs0_10
        c1_sw -> c1_n6: devs0_11
        c1_n7 -> c1_sw: devs0_12
        c1_sw -> c1_n7: devs0_13
        c1_n8 -> c1_sw: devs0_14
        c1_sw -> c1_n8: devs0_15
        c2_n1 -> c2_sw: devs0_16
        c2_sw -> c2_n1: devs0_17
        c2_n2 -> c2_sw: devs0_18
        c2_sw -> c2_n2: devs0_19
        c2_n3 -> c2_sw: devs0_20
        c2_sw -> c2_n3: devs0_21
        c2_n4 -> c2_sw: devs0_22
        c2_sw -> c2_n4: devs0_23
        c2_n5 -> c2_sw: devs0_24
        c2_sw -> c2_n5: devs0_25
        c2_n6 -> c2_sw: devs0_26
        c2_sw -> c2_n6: devs0_27
        c2_n7 -> c2_sw: devs0_28
        c2_sw -> c2_n7: devs0_29
        c2_n8 -> c2_sw: devs0_30
        c2_sw -> c2_n8: devs0_31
        c3_n1 -> c3_sw: devs0_32
        c3_sw -> c3_n1: devs0_33
        c3_n2 -> c3_sw: devs0_34
        c3_sw -> c3_n2: devs0_35
        c3_n3 -> c3_sw: devs0_36
        c3_sw -> c3_n3: devs0_37
        c3_n4 -> c3_sw: devs0_38
        c3_sw -> c3_n4: devs0_39
        c3_n5 -> c3_sw: devs0_40
        c3_sw -> c3_n5: devs0_41
        c3_n6 -> c3_sw: devs0_42
        c3_sw -> c3_n6: devs0_43
        c3_n7 -> c3_sw: devs0_44
        c3_sw -> c3_n7: devs0_45
        c3_n8 -> c3_sw: devs0_46
        c3_sw -> c3_n8: devs0_47
        c4_n1 -> c4_sw: devs0_48
        c4_sw -> c4_n1: devs0_49
        c4_n2 -> c4_sw: devs0_50
        c4_sw -> c4_n2: devs0_51
        c4_n3 -> c4_sw: devs0_52
        c4_sw -> c4_n3: devs0_53
        c4_n4 -> c4_sw: devs0_54
        c4_sw -> c4_n4: devs0_55
        c4_n5 -> c4_sw: devs0_56
        c4_sw -> c4_n5: devs0_57
        c4_n6 -> c4_sw: devs0_58
        c4_sw -> c4_n6: devs0_59
        c4_n7 -> c4_sw: devs0_60
        c4_sw -> c4_n7: devs0_61
        c4_n8 -> c4_sw: devs0_62
        c4_sw -> c4_n8: devs0_63
        c1_sw -> c2_sw: devs1_64
        c1_sw -> c3_sw: devs1_65
        c1_sw -> c4_sw: devs1_66
        c2_sw -> c1_sw: devs1_67
        c2_sw -> c3_sw: devs1_68
        c2_sw -> c4_sw: devs1_69
        c3_sw -> c1_sw: devs1_70
        c3_sw -> c2_sw: devs1_71
        c3_sw -> c4_sw: devs1_72
        c4_sw -> c1_sw: devs1_73
        c4_sw -> c2_sw: devs1_74
        c4_sw -> c3_sw: devs1_75
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}