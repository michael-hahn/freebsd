/*-
 * Copyright (c) 2018 Domagoj Stolfa
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/dtrace.h>
#include <machine/vmm.h>

#include "dtvirt.h"

static MALLOC_DEFINE(M_DTVIRT, "dtvirt", "");
static lwpid_t dtvirt_priv_gettid(void *);
static uint16_t dtvirt_priv_getns(void *);

lwpid_t (*vmm_gettid)(void *biscuit);
uint16_t (*vmm_getid)(void *biscuit);

void
dtvirt_probe(void *biscuit, int probeid, struct dtvirt_args *dtv_args)
{

	dtrace_ns_probe(biscuit, probeid, dtv_args);
}

static int
dtvirt_handler(module_t mod __unused, int what, void *arg __unused)
{
	switch (what) {
	case MOD_LOAD:
		dtvirt_gettid = dtvirt_priv_gettid;
		dtvirt_getns = dtvirt_priv_getns;
		break;
	case MOD_UNLOAD:
		dtvirt_gettid = NULL;
		dtvirt_getns = NULL;
		break;
	default:
		break;
	}
	return (0);
}

static lwpid_t
dtvirt_priv_gettid(void *biscuit)
{

	if (vmm_gettid != NULL)
		return (vmm_gettid(biscuit));
	return (0);
}

static uint16_t
dtvirt_priv_getns(void *biscuit)
{

	if (vmm_getid != NULL)
		return (vmm_getid(biscuit));
	return (0);
}

static moduledata_t dtvirt_kmod = {
	"dtvirt",
	dtvirt_handler,
	NULL
};

DECLARE_MODULE(dtvirt, dtvirt_kmod, SI_SUB_DTRACE + 1, SI_ORDER_ANY);
MODULE_VERSION(dtvirt, 1);
MODULE_DEPEND(dtvirt, dtrace, 1, 1, 1);
