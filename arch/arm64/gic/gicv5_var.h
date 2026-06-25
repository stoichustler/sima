/*
 * Copyright (c) 2025 Arm Ltd
 * Copyright (c) 2026 Hustler Lo
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _ARM64_GICV5VAR_H_
#define	_ARM64_GICV5VAR_H_

#include <types.h>
#include <asm/irq.h>

#define	GICV5_LPI_ID_BASE	8192U

enum gicv5_irq_space {
	GICv5_INVALID,
	/* GICv5 interrupt types so they are the same values as in the spec */
	GICv5_PPI,
	GICv5_LPI,
	GICv5_SPI,
};

struct gicv5_base_irqsrc {
	enum gicv5_irq_space	gbi_space;
	bool			gbi_ipi;
	uint32_t		gbi_irq;
};

void beau_gicv5_its_init(uint64_t base, uint64_t size);
bool beau_gicv5_its_present(void);

void beau_gicv5_iwb_init(uint64_t base, uint64_t size);
bool beau_gicv5_iwb_present(void);
uint32_t beau_gicv5_iwb_irq_count(void);
int32_t beau_gicv5_iwb_enable(uint32_t irq);
int32_t beau_gicv5_iwb_disable(uint32_t irq);
int32_t beau_gicv5_iwb_set_level(uint32_t irq, bool level);

#endif /* _ARM64_GICV5VAR_H_ */
