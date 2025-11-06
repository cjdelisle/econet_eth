# EcoNet EN751221 Ethernet Driver

This is a work in progress, it's published now because it works well enough
to provide ethernet on EcoNet devices but it is far from complete.

The EN751221 ethernet device has 2 ports, one goes to an onboard switch
that goes to the 4 LAN ports on the device, the other one goes to either
the fiber subsystem or else another ethernet port (in the case of a DSL
application).

Currently this driver only brings up port 1 (LAN) and puts the switch into
open forwarding mode.

## TODO
- Verify that module-unloading is correct to allow rapid development by
downloading and reloading new versions of the module
- Rewrite the DMA ring because the way it works now will overwrite packets
   if it loops around.
- Fix MDIO and try to the switch working as a DSA so we can have one port
   as a WAN and the others for LAN
- Implement NAPI
- Separate out the QDMA engine so that we can run two, also each QDMA has
   2 chains (chain = 1 RX ring and 1 TX ring)
- Add stats because they are collected
- Enable jumbo frames because they are supported in hardware
- Decide what QoS / NAT / ... features to make available

## File structure
* Core driver which is mostly old code that needs to be updated
  * `econet_eth1.c`
* Old `mtk_soc_eth` based stuff which should be rewritten:
  * `econet_eth1.h`
* New code
  * `econet_eth.h`
  * `econet_eth_regs.h`
  * `qdma_desc.h`
  * `econet_eth_debug.c`

## How to use

First, you must edit your `dts` / `dtsi` file and add an entry for the
ethernet device.

Then you need to build the kernel module using `./build.sh`. The `build.sh`
script expects that next to your `econet_eth` directory, there is `../openwrt`,
an OpenWrt tree that has been compiled for the EcoNet device. If your OpenWrt
is in a different location then you'll need to edit it.

## DeviceTree Entry

```c
	ethernet: ethernet@1fb50000 {
		compatible = "econet,en751221-eth"; 
		reg = <0x1fb50000 0x10000>;

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