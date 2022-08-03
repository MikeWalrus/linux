/dts-v1/;

/ {
	description = "Linux image for nscscc";
	#address-cells = <1>;

	images {
		kernel {
			description = KERNEL_NAME;
			data = /incbin/(VMLINUX_BINARY);
			type = "kernel";
			arch = "mips";
			os = "linux";
			compression = VMLINUX_COMPRESSION;
			load = /bits/ ADDR_BITS <VMLINUX_LOAD_ADDRESS>;
			entry = /bits/ ADDR_BITS <VMLINUX_ENTRY_ADDRESS>;
			hash {
				algo = "sha1";
			};
		};

		fdt {
			description = "PPMIPS flattened device tree";
			data = /incbin/("boot/dts/ppmips/ppmips_nscscc.dtb");
			type = "flat_dt";
			arch = "mips";
			compression = "none";
			hash {
				algo = "sha1";
			};
		};
	};

	configurations {
		default = "config";

		config {
			description = "Default and only configuration";
			kernel = "kernel";
			ramdisk = "ramdisk";
			fdt = "fdt";
		};
	};
};

