/*
The MIT License (MIT)

Copyright (c) 2014-2015 CSAIL, MIT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#if defined (KERNEL_MODE)
#include <linux/module.h>
#include <linux/slab.h>

#elif defined (USER_MODE)
#include <stdio.h>
#include <stdint.h>

#else
#error Invalid Platform (KERNEL_MODE or USER_MODE)
#endif

#include "debug.h"
#include "platform.h"
#include "params.h"
#include "bdbm_drv.h"
#include "llm_noq.h"


/* llm interface */
bdbm_llm_inf_t _llm_noq_inf = {
	.ptr_private = NULL,
	.create = llm_noq_create,
	.destroy = llm_noq_destroy,
	.make_req = llm_noq_make_req,
	.flush = llm_noq_flush,
	.end_req = llm_noq_end_req,
};

struct bdbm_llm_noq_private {
	uint64_t nr_punits;
	bdbm_mutex_t* punit_locks;
};

uint32_t llm_noq_create (bdbm_drv_info_t* bdi)
{
	struct bdbm_llm_noq_private* p;
	uint64_t loop;

	/* create a private info for llm_nt */
	if ((p = (struct bdbm_llm_noq_private*)bdbm_malloc_atomic
			(sizeof (struct bdbm_llm_noq_private))) == NULL) {
		bdbm_error ("bdbm_malloc_atomic failed");
		return -1;
	}

	/* get the total number of parallel units */
	p->nr_punits = BDBM_GET_NR_PUNITS (bdi->parm_dev);

	/* create completion locks for parallel units */
	if ((p->punit_locks = (bdbm_mutex_t*)bdbm_malloc_atomic
			(sizeof (bdbm_mutex_t) * p->nr_punits)) == NULL) {
		bdbm_error ("bdbm_malloc_atomic failed");
		bdbm_free_atomic (p);
		return -1;
	}

	/* initialize completion locks */
	for (loop = 0; loop < p->nr_punits; loop++) {
		bdbm_mutex_init (&p->punit_locks[loop]);
	}

	/* keep the private structures for llm_nt */
	bdi->ptr_llm_inf->ptr_private = (void*)p;

	return 0;
}

/* NOTE: we assume that all of the host requests are completely served.
 * the host adapter must be first closed before this function is called.
 * if not, it would work improperly. */
void llm_noq_destroy (bdbm_drv_info_t* bdi)
{
	struct bdbm_llm_noq_private* p;
	uint64_t loop;

	p = (struct bdbm_llm_noq_private*)BDBM_LLM_PRIV(bdi);

	/* complete all the completion locks */
	for (loop = 0; loop < p->nr_punits; loop++) {
		bdbm_mutex_lock (&p->punit_locks[loop]);
	}

	/* release all the relevant data structures */
	bdbm_free_atomic (p->punit_locks);
	bdbm_free_atomic (p);
}

uint32_t llm_noq_make_req (bdbm_drv_info_t* bdi, bdbm_llm_req_t* llm_req)
{
	uint32_t ret;
	uint64_t punit_id;
	struct bdbm_llm_noq_private* p;

	p = (struct bdbm_llm_noq_private*)BDBM_LLM_PRIV(bdi);

	/* get a parallel unit ID */
	punit_id = llm_req->phyaddr->channel_no * 
		BDBM_GET_DEVICE_PARAMS (bdi)->nr_chips_per_channel +
		llm_req->phyaddr->chip_no;

	/* wait until a parallel unit becomes idle */
	bdbm_mutex_lock (&p->punit_locks[punit_id]);

	/* send a request to a device manager */
	ret = bdi->ptr_dm_inf->make_req (bdi, llm_req);

	/* handle error cases */
	if (ret != 0) {
		/* complete a lock */
		bdbm_mutex_unlock (&p->punit_locks[punit_id]);
		bdbm_error ("llm_make_req failed");
	}

	return ret;
}

void llm_noq_flush (bdbm_drv_info_t* bdi)
{
	struct bdbm_llm_noq_private* p = (struct bdbm_llm_noq_private*)BDBM_LLM_PRIV(bdi);
	uint64_t loop;

	for (loop = 0; loop < p->nr_punits; loop++) {
		/* FIXME: it is wired.. */
		bdbm_mutex_lock (&p->punit_locks[loop]);
		bdbm_mutex_unlock (&p->punit_locks[loop]);
	}
}

void llm_noq_end_req (bdbm_drv_info_t* bdi, bdbm_llm_req_t* llm_req)
{
	struct bdbm_llm_noq_private* p;
	uint64_t punit_id;

	p = (struct bdbm_llm_noq_private*)BDBM_LLM_PRIV(bdi);

	/* get a parallel unit ID */
	punit_id = llm_req->phyaddr->channel_no * 
		BDBM_GET_DEVICE_PARAMS (bdi)->nr_chips_per_channel +
		llm_req->phyaddr->chip_no;

	/* complete a lock */
	bdbm_mutex_unlock (&p->punit_locks[punit_id]);

	/* finish a request */
	bdi->ptr_hlm_inf->end_req (bdi, llm_req);
}

