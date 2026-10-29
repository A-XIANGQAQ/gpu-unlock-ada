/* SPDX-License-Identifier: GPL-2.0-only
 * Experimental opt-in handoff repair: restore the VGA/ISA attribute bits
 * (0x18) that this firmware's display-driver Stop clears, and verify the
 * parent bridge's VGA-enable bit, after the core transaction/FLR and while the
 * exclusive claim is still held. No BME, driver/GOP reconnect, MMIO write,
 * DMA or reset is performed here.
 */
#pragma once
#include "port.h"

EFI_STATUS GuRestoreVga(VOID);
