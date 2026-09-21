/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PCI_H
#define _ASM_FLUX_PCI_H

#define pcibios_assign_all_busses() 0
#define PCIBIOS_MIN_IO 0x1000
#define PCIBIOS_MIN_MEM 0x10000000

#if defined(CONFIG_PCI) && defined(CONFIG_NUMA)
static inline int pcibus_to_node(struct pci_bus *bus)
{
	return dev_to_node(&bus->dev);
}
#ifndef cpumask_of_pcibus
#define cpumask_of_pcibus(bus)\
	(pcibus_to_node(bus) == NUMA_NO_NODE ? cpu_all_mask :\
	 cpumask_of_node(pcibus_to_node(bus)))
#endif
#endif

#include <asm-generic/pci.h>

#endif /* _ASM_FLUX_PCI_H */
