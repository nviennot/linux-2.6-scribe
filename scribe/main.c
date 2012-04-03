/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/scribe.h>

extern int scribe_init_device(void);
extern void scribe_init_pump(void);
extern void scribe_mem_init_caches(void);
extern void scribe_res_init_caches(void);

static int __init scribe_init(void)
{
	int err;

	/* TODO clean exit path */
	if ((err = scribe_init_device()))
		return err;
	scribe_init_pump();
	scribe_mem_init_caches();
	scribe_res_init_caches();
	return 0;
}

device_initcall(scribe_init);
