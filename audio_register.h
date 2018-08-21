#ifndef AUDIO_REGISTER_H
#define AUDIO_REGISTER_H

#define AUDIO_PCI_SLOT 0x0e

#define HDA_MEM_REGION	0	/* HD AUudio region index, BAR 0 */
#define DSP_MEM_REGION	1	/* Audio DSP region index, BAR 1 */

#define HDA_MEM_REGION_SIZE	0x4000  /* HDA alias registers end at 0x3FFFh */
#define DSP_MEM_REGION_SIZE	0x10000	/* Digital MIC regsiters end at 0xFFFFh */

#define LOW_BASE_ADDR_MASK 0xfffffc000

struct audio_mem_region {
	/* Base physical address of the region from the CPU's point of view */
	pciaddr_t base_addr;

	/* * Size, in bytes, of the region */
	pciaddr_t size;
};

#endif /* AUDIO_REGISTER_H */