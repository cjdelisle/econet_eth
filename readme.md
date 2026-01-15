# EcoNet EN751221 Ethernet Driver

This is an ethernet driver for EcoNet EN751221 devices, it is likely to be
easily ported to EN751627 and EN7528, but it is significantly different from
the ethernet used in EN7523, EN7580, EN7581, and EN7583. For those you should
use airoha_eth.

The EcoNet Ethernet system has 32 channels, each of which has 8 queues. WRR
prioritization can be done between queues within a channel and (to some extent)
between channels. This driver does no QoS at the moment, but does expose all
256 channel/queue combinations as queues to the kernel, ensuring that separate
flows will generally get fair treatment.

## Current status
- Loads and unloads correctly as long as reset controller is provided
- Sending and receiving is fast and seems to be correct
- NAPI based
- QDMA and GDM (port) subsystems are in separate sub-modules
- Stats collected
- Debugfs introspection
- "Kernel Quality" - not much needed to be considered for upstreaming
- 

## TODO
- Short term:
  - Implement DSA for the integrated MT7530 switch
  - Send upstream to Linux for reviews
- Medium term:
  - Implement FlowTable offloading
  - Implement QoS flow grouping by sender/recipient to protect users from
    each other
  - Integrate FlowTable offloading with QoS channel selection
  - Implement jumbo frames over 2KB (requires shutdown/restart of QDMA on
    change of MTU, to increase buffer sizes)
  - EN751627 + EN7528 support

## File structure
* `econet_eth.c` - The main driver and entrypoint.
* `econet_qdma.c` - Controls only the QDMA engine, is oblivious to the rest
                    of the ethernet system. Receives and send packets.
* `econet_port.c` - Controls the GDM (LAN or WAN) port, registers the
                    net_device with the kernel. Has a reference to the QDMA
		    in order to send packets.
* `econet_eth_debug.c` - Provides debugfs introspection.
* `econet_eth.h` - Main internal header file.
* `qdma_desc.h` - Header file with packet descriptor for communicating with
                  the QDMA engine.
* `qdma_regs.h` - Struct representation of MMIO registers of QDMA engine
* `gdm_regs.h` - Struct representation of MMIO registers of GDM port control

## How to use

First, you must edit your `dts` / `dtsi` file and add an entry for the
ethernet device.

Then you need to build the kernel module using `./build.sh`. The `build.sh`
script expects that next to your `econet_eth` directory, there is `../openwrt`,
an OpenWrt tree that has been compiled for the EcoNet device. If your OpenWrt
is in a different location then you'll need to edit it.

## DeviceTree Entry

First, you should have `econet,en751221-scu` for being able to reset the
ethernet when loading and unloading the driver, otherwise it will only load
once per reboot. See: https://github.com/openwrt/openwrt/pull/21545

```c
	chip_scu: syscon@1fa20000 {
		compatible = "econet,en751221-chip-scu", "syscon";
		reg = <0x1fa20000 0x388>;
	};
	scuclk: clock-controller@1fb00000 {
		compatible = "econet,en751221-scu", "syscon";
		reg = <0x1fb00000 0x970>;
		#clock-cells = <1>;
		#reset-cells = <1>;
	};
```

Then add this entry to your DT:

```c
	ethernet: ethernet@1fb50000 {
		compatible = "econet,en751221-eth"; 
		reg = <0x1fb50000 0x10000>;

		// If you didn't include the system controller, remove
		// these resets. You will get a warning on load and you
		// won't be able to load the driver more than once per reboot.
		resets = <&scuclk EN751221_FE_RST>,
			 <&scuclk EN751221_FE_QDMA1_RST>,
			 <&scuclk EN751221_FE_QDMA2_RST>,
			 <&scuclk EN751221_GSW_RST>,
			 <&scuclk EN751221_XPON_MAC_RST>,
			 <&scuclk EN751221_XPON_PHY_RST>;
		reset-names = "fe", "qdma0", "qdma1", "gsw",
			      "xpon-mac", "xpon-phy";

		#address-cells = <1>;
		#size-cells = <0>;

		interrupt-parent = <&intc>;
		interrupts = <21>, <22>;

		gmac0: mac@0 {
			compatible = "econet,eth-mac";
			reg = <0>;
			phy-mode = "trgmii";

			fixed-link {
				speed = <1000>;
				full-duplex;
				pause;
			};
		};

		gmac1: mac@1 {
			compatible = "econet,eth-mac";
			reg = <1>;
			status = "disable";
			phy-mode = "rgmii-rxid";
		};

		 mdio: mdio-bus {
			#address-cells = <1>;
			#size-cells = <0>;
				
			switch0: switch@1f {
				compatible = "mediatek,mt7530";
				#address-cells = <1>;
				#size-cells = <0>;
				reg = <0x1f>;
				mediatek,mcm;
				//resets = <&rstctrl 2>;
				reset-names = "mcm";

				ports {
					#address-cells = <1>;
					#size-cells = <0>;
					reg = <0>;

					port@0 {
						status = "disabled";
						reg = <0>;
						label = "lan0";
					};

					port@1 {
						status = "disabled";
						reg = <1>;
						label = "lan1";
					};

					port@2 {
						status = "disabled"; 
						reg = <2>;
						label = "lan2";
					};

					port@3 {
						status = "disabled";
						reg = <3>;
						label = "lan3";
					};

					port@4 {
						/* status = "disabled"; */
						reg = <4>;
						label = "lan4";
					};

					port@6 {
						reg = <6>;
						label = "cpu";
						//ethernet = <&gmac0>;
						phy-mode = "trgmii";

						fixed-link {
							speed = <1000>;
							full-duplex;
						};
					};
				};
			};
		};
	};
```

## License and Credits
All of this code is licensed GPL-2.0-only.

It is based on the work here https://github.com/gchmiel/en7512_kernel5
which is in turn based on the the Mediatek MT7621 driver on upstream.