/* $NetBSD: acpi_power.h,v 1.6 2010/06/07 17:28:17 jruoho Exp $ */

/*-
 * Copyright (c) 2009, 2010 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jukka Ruohonen.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _SYS_DEV_ACPI_ACPI_POWER_H
#define _SYS_DEV_ACPI_ACPI_POWER_H

/* Explicit resource-off request; ACPICA D3 (3) means D3hot. */
#define ACPI_STATE_D3_COLD 4

#define ACPI_STATE_ERROR	-1

/*
 * Diagnostic D3cold entry after the caller has quiesced the device and
 * prepared its native bus power state. Skip _PS3, retaining resource and
 * wake ownership checks. Restore through ordinary acpi_power_set(D0).
 */
#define ACPI_POWER_NOPS3 0x01U

bool		acpi_power_register(ACPI_HANDLE);
void		acpi_power_deregister(ACPI_HANDLE);
bool		acpi_power_get(ACPI_HANDLE, int *);
bool		acpi_power_set(ACPI_HANDLE, int);
bool		acpi_power_set_flags(ACPI_HANDLE, int, unsigned int);
ACPI_STATUS	acpi_power_res(ACPI_HANDLE, ACPI_HANDLE, bool);
void		acpi_power_add(struct acpi_devnode *);

#endif	/* !_SYS_DEV_ACPI_ACPI_POWER_H */
