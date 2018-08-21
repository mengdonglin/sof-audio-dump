/*
 * Copyright © 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhenyu Wang <zhenyu.z.wang@intel.com>
 *    Wu Fengguang <fengguang.wu@intel.com>
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <pciaccess.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "audio_register.h"

/* check if SOC is Broxton/ApolloLake */
#define IS_BXT(pci) ((pci)->vendor_id == 0x8086 && (pci)->device_id == 0x5a98)

static uint32_t devid; /* pci deivce id of HDAudio controller */
static struct audio_mem_region regions[2];
static void *hda_global_mmio = NULL;

/**
 * intel_get_audio_pci_device:
 *
 * Looks up the HDA pci device using libpciaccess.
 *
 * Returns:
 * The pci_device, exits the program on any failures.
 */
struct pci_device *intel_get_audio_pci_device(void)
{
	struct pci_device *pci_dev = NULL;
	int error;

	error = pci_system_init();
	if (error != 0) {
		printf( "Couldn't initialize PCI system\n");
		return NULL;
	}

	/* Grab the hda controller. Try the canonical slot first, then
	 * walk the entire PCI bus for a matching device. */
	pci_dev = pci_device_find_by_slot(0, AUDIO_PCI_SLOT, 0, 0);
	if (pci_dev)
		printf("Found Intel HD-Audio controller at slot 0x%x\n", AUDIO_PCI_SLOT);

	if (pci_dev == NULL || pci_dev->vendor_id != 0x8086) {
		struct pci_device_iterator *iter;
		struct pci_id_match match;

		match.vendor_id = 0x8086; /* Intel */
		match.device_id = PCI_MATCH_ANY;
		match.subvendor_id = PCI_MATCH_ANY;
		match.subdevice_id = PCI_MATCH_ANY;

		/* The class is at bits [23:16], subclass is at bits [15:8]
		 * HDA controller uses multimedia class (0x4),
		 * audio sub-class (0x1).
		 */
		match.device_class = 0x4 << 16 | 0x1 << 8;
		match.device_class_mask = 0xffff << 8;

		match.match_data = 0;

		iter = pci_id_match_iterator_create(&match);
		pci_dev = pci_device_next(iter);
		pci_iterator_destroy(iter);

		if (pci_dev) {
			printf("Found Intel HD-Audio controller: id %x, class %x\n",
				pci_dev->device_id, pci_dev->device_class);
			error = pci_device_probe(pci_dev);
			if (error)
				printf("Fail to probe HDA controller, error %d\n", error);
		} else
			printf("Couldn't find Intel HD-Audio controller\n");
	}

	return pci_dev;
}

int read_pci_header(struct pci_device *pci_dev)
{
	int ret= 0;
	uint32_t hdalba, hdauba; /* HDA lower and upper base address */
	uint32_t dsplba, dspuba; /* HDA lower and upper base address */

	/* HDA host BAR (BAR 0) */
	ret = pci_device_cfg_read_u32 (pci_dev, &hdalba, 0x10);
	ret = pci_device_cfg_read_u32 (pci_dev, &hdauba, 0x14);

	/* DSP BAR (BAR 1) */
	ret = pci_device_cfg_read_u32 (pci_dev, &dsplba, 0x20);
	ret = pci_device_cfg_read_u32 (pci_dev, &dspuba, 0x24);

	hdalba &= LOW_BASE_ADDR_MASK;
	dsplba &= LOW_BASE_ADDR_MASK;

	printf("HDA BAR lower %x, upper %x\n", hdalba, hdauba);
	printf("DSP BAR lower %x, upper %x\n\n", dsplba, dspuba);

	return ret;
}

/**
 * intel_mmio_use_pci_bar:
 * @pci_dev: intel gracphis pci device
 *
 * Sets up #igt_global_mmio to point at the mmio bar.
 *
 * @pci_dev can be obtained from intel_get_pci_device().
 */
int intel_mmio_use_pci_bar(struct pci_device *pci_dev)
{
	int mmio_bar, mmio_size;
	int error;

	regions[HDA_MEM_REGION].base_addr = pci_dev->regions[HDA_MEM_REGION].base_addr;
	regions[HDA_MEM_REGION].size = pci_dev->regions[HDA_MEM_REGION].size;

	regions[DSP_MEM_REGION].base_addr = pci_dev->regions[DSP_MEM_REGION].base_addr;
	regions[DSP_MEM_REGION].size = pci_dev->regions[DSP_MEM_REGION].size;

	printf("HDA BAR 0x%lx, size 0x%lx\n",  regions[HDA_MEM_REGION].base_addr, regions[HDA_MEM_REGION].size);
	printf("DSP BAR 0x%lx, size 0x%lx\n\n",  regions[DSP_MEM_REGION].base_addr, regions[DSP_MEM_REGION].size);

	//printf("pci DSP BAR 0x%lx, size 0x%lx\n",  pci_dev->regions[1].base_addr, pci_dev->regions[1].size);

	devid = pci_dev->device_id;
	mmio_bar = HDA_MEM_REGION;
	mmio_size = HDA_MEM_REGION_SIZE;

	error = pci_device_map_range (pci_dev,
				      pci_dev->regions[mmio_bar].base_addr,
				      mmio_size,
				      PCI_DEV_MAP_FLAG_WRITABLE,
				      &hda_global_mmio);

	if(error != 0 || !hda_global_mmio) {
		printf("Couldn't map MMIO region, error %d. Need root access.\n", error);
		return -1;
	}

	printf("audio_global_mmio %p\n", hda_global_mmio);

	return 0;
}

/**
 * intel_register_read:
 * @reg: register offset
 *
 * 32-bit read of the register at @offset. This function only works when the new
 * register access helper is initialized with intel_register_access_init().
 *
 * Compared to INREG() it can do optional checking with the register access
 * white lists.
 *
 * Returns:
 * The value read from the register.
 */
uint32_t intel_register_read(uint32_t reg)
{
	uint32_t ret;

	ret = *(volatile uint32_t *)((volatile char *)hda_global_mmio + reg);
	return ret;
}


int dump_memory_to_file(const char *outfile)
{
	int out_fd;
	size_t bytes = 0;
	int ret = 0;

	if (!hda_global_mmio || !outfile)
		return -EINVAL;

	out_fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (out_fd < 0) {
		printf("error: failed to open %s err %d\n",
			outfile, -errno);
		return -errno;
	}

	bytes = write(out_fd, hda_global_mmio, HDA_MEM_REGION_SIZE);
	if (bytes != HDA_MEM_REGION_SIZE) {
		printf("error: can't write HDA memory, %lu bytes written\n",
			(long unsigned int)bytes);
		ret = -EIO;
	} else {
		printf("%lu (0x%lx) bytes written to file %s\n",
			(long unsigned int)bytes, bytes, outfile);
	}

	close(out_fd);
	return ret;
}

void self_test(void)
{
	uint32_t val32;

	printf("\nSelf test:\n");

	val32 = intel_register_read(0);
	printf("HDA Global capabilites 0x%x (should be 0x6701)\n", val32 & 0xffff);
	printf("\n");
}

static void usage(char *name)
{
	printf("Please run this as root.\n"
	"Usage: %s [OPTIONS]...\n"
	"\n"
	"-h, --help              help\n"
	"-o, --output=FILE       set output file for binary dump\n\n", name);
}

int main(int argc, char **argv)
{
	struct pci_device *pci_dev;
	int ret;
	static const char short_options[] = "ho:";
	static const struct option long_options[] = {
		{"help", 0, NULL, 'h'},
		{"output", 1, NULL, 'o'},
		{0, 0, 0, 0},
	};
	int c, option_index;
	char *output_file = NULL;

	while ((c = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
		switch (c) {
		case 'h':
			usage(argv[0]);
			return 0;

		case 'o':
			output_file = optarg;
			break;

		default:
			fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
			return 1;
		}
	}

	pci_dev = intel_get_audio_pci_device();
	if (!pci_dev)
		return -1;

	if(!IS_BXT(pci_dev)) {
		fprintf(stderr, "Only support Broxton/ApolloLake atm.\n");
		return -EINVAL;
	}
	
	devid = pci_dev->device_id;

	read_pci_header(pci_dev); /* optional, for debug purpose */

	ret = intel_mmio_use_pci_bar(pci_dev);
	if (ret < 0)
		return ret;

	self_test();

	if (output_file) {
		ret = dump_memory_to_file(output_file);
		if (ret < 0)
			return ret;
	}

	return 0;
}
