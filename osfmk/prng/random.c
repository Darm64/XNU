/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <mach/machine.h>
#include <mach/processor.h>
#include <kern/processor.h>
#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/kalloc.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <machine/cpu_data.h>
#include <machine/simple_lock.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>
#include <sys/kdebug.h>
#include <sys/random.h>

#include <prng/random.h>
#include <corecrypto/ccdrbg.h>
#include <corecrypto/ccsha1.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>

#include <pexpert/pexpert.h>
#include <console/serial_protos.h>
#include <IOKit/IOPlatformExpert.h>

#if defined(__x86_64__)
#include <i386/cpuid.h>

static int rdseed_step(uint64_t *seed)
{
	uint8_t ok;
	
	asm volatile ("rdseed %0; setc %1" : "=r" (*seed), "=qm" (ok));
	
	return (int) ok;
}

static int rdseed_retry(uint64_t *seed, size_t nretries)
{
	size_t i;
	
	for (i = 0; i < nretries; i += 1) {
		if (rdseed_step(seed)) {
			return 1;
		} else {
			asm volatile ("pause");
		}
	}
	
	return 0;
}

static size_t rdseed_seed(void *buf, size_t nwords)
{
	uint64_t *buf_words;
	size_t i;
	
	if (nwords > 8) {
		nwords = 8;
	}
	
	buf_words = buf;
	for (i = 0; i < nwords; i += 1) {
		if (!rdseed_retry(buf_words + i, 10)) {
			return i;
		}
	}
	
	return nwords;
}

static int rdrand_step(uint64_t *rand)
{
	uint8_t ok;
	
	asm volatile ("rdrand %0; setc %1" : "=r" (*rand), "=qm" (ok));
	
	return (int) ok;
}

static int rdrand_retry(uint64_t *rand, size_t nretries)
{
	size_t i;
	
	for (i = 0; i < nretries; i += 1) {
		if (rdrand_step(rand)) {
			return 1;
		}
	}
	
	return 0;
}

static size_t rdrand_seed(void *buf, size_t nwords)
{
	size_t i;
	uint64_t w;
	uint8_t hash[CCSHA256_OUTPUT_SIZE];
	const struct ccdigest_info *di = &ccsha256_ltc_di;
	
	ccdigest_di_decl(di, ctx);
	ccdigest_init(di, ctx);
	
	for (i = 0; i < 1023; i += 1) {
		if (!rdrand_retry(&w, 10)) {
			nwords = 0;
			goto out;
		}
		ccdigest_update(di, ctx, sizeof w, &w);
	}
	
	ccdigest_final(di, ctx, hash);
	
	if (nwords > 2) {
		nwords = 2;
	}
	
	memcpy(buf, hash, nwords * sizeof (uint64_t));
	
out:
	ccdigest_di_clear(di, ctx);
	bzero(hash, sizeof hash);
	bzero(&w, sizeof w);
	
	return nwords;
}

static void intel_entropysource(void *buf, size_t *nbytes)
{
	size_t nwords;
	
	/* only handle complete words */
	assert(*nbytes % sizeof (uint64_t) == 0);
	
	nwords = (*nbytes) / sizeof (uint64_t);
	if (cpuid_leaf7_features() & CPUID_LEAF7_FEATURE_RDSEED) {
		nwords = rdseed_seed(buf, nwords);
		*nbytes = nwords * sizeof (uint64_t);
	} else if (cpuid_features() & CPUID_FEATURE_RDRAND) {
		nwords = rdrand_seed(buf, nwords);
		*nbytes = nwords * sizeof (uint64_t);
	} else {
		*nbytes = 0;
	}
}

#endif

typedef void (*entropysource)(void *buf, size_t *nbytes);

static const entropysource entropysources[] = {
	entropy_buffer_read,
#if defined(__x86_64__)
	intel_entropysource,
#endif
};

static const size_t nsources = sizeof entropysources / sizeof entropysources[0];

static size_t entropy_readall(void *buf, size_t nbytes_persource)
{
	uint8_t *buf_bytes = buf;
	size_t i;
	size_t nbytes_total = 0;
	
	for (i = 0; i < nsources; i += 1) {
		size_t nbytes = nbytes_persource;
		entropysources[i](buf_bytes, &nbytes);
		bzero(buf_bytes + nbytes, nbytes_persource - nbytes);
		nbytes_total += nbytes;
		buf_bytes += nbytes_persource;
	}
	
	return nbytes_total;
}

static struct {
	lck_grp_t *group;
	lck_attr_t *attrs;
	lck_grp_attr_t *group_attrs;
	lck_mtx_t *mutex;
} lock;

typedef struct prngContext {
	struct ccdrbg_info *infop;
	struct ccdrbg_state *statep;
	uint64_t bytes_generated;
	uint64_t bytes_reseeded;
} *prngContextp;

ccdrbg_factory_t prng_ccdrbg_factory = NULL;

entropy_data_t EntropyData = {
	.index_ptr = EntropyData.buffer
};

static struct {
	uint8_t seed[nsources][EARLY_RANDOM_SEED_SIZE];
	size_t seedset;
	uint8_t master_drbg_state[EARLY_RANDOM_STATE_STATIC_SIZE];
	struct ccdrbg_state *drbg_states[MAX_CPUS];
	struct ccdrbg_info drbg_info;
	const struct ccdrbg_nisthmac_custom drbg_custom;
} erandom = {
	.drbg_custom = {
		.di = &ccsha1_eay_di,
		.strictFIPS = 0,
	}
};

static void read_erandom(void *buf, uint32_t nbytes);

void 
entropy_buffer_read(void *buffer, size_t *count)
{
	boolean_t current_state;
	unsigned int i, j;

	if (!erandom.seedset) {
		panic("early_random was never invoked");
	}

	if (*count > ENTROPY_BUFFER_BYTE_SIZE) {
		*count = ENTROPY_BUFFER_BYTE_SIZE;
	}

	current_state = ml_set_interrupts_enabled(FALSE);

	memcpy(buffer, EntropyData.buffer, *count);

	/* Consider removing this mixing step rdar://problem/31668239 */
	for (i = 0, j = (ENTROPY_BUFFER_SIZE - 1); i < ENTROPY_BUFFER_SIZE; j = i, i++)
		EntropyData.buffer[i] = EntropyData.buffer[i] ^ EntropyData.buffer[j];

	(void) ml_set_interrupts_enabled(current_state);

#if DEVELOPMENT || DEBUG
	uint32_t *word = buffer;
	/* Good for both 32-bit and 64-bit kernels. */
	for (i = 0; i < ENTROPY_BUFFER_SIZE; i += 4)
		/*
		 * We use "EARLY" here so that we can grab early entropy on
		 * ARM, where tracing is not started until after PRNG is
		 * initialized.
		*/
		KERNEL_DEBUG_EARLY(ENTROPY_READ(i/4),
			word[i+0], word[i+1], word[i+2], word[i+3]);
#endif
}

/*
 * Return a uniformly distributed 64-bit random number.
 *
 * This interface should have minimal dependencies on kernel
 * services, and thus be available very early in the life
 * of the kernel.
 * This provides cryptographically secure randomness.
 * Each processor has its own generator instance.
 * It is seeded (lazily) with entropy provided by the Booter.
 *
 * For <rdar://problem/17292592> the algorithm switched from LCG to
 * NIST HMAC DBRG as follows:
 *  - When first called (on OSX this is very early while page tables are being
 *    built) early_random() calls ccdrbg_factory_hmac() to set-up a ccdbrg info
 *    structure.
 *  - The boot processor's ccdrbg state structure is a statically allocated area
 *    which is then initialized by calling the ccdbrg_init method.
 *    The initial entropy is 16 bytes of boot entropy.
 *    The nonce is the first 8 bytes of entropy xor'ed with a timestamp
 *    from ml_get_timebase().
 *    The personalization data provided is null.
 *  - The first 64-bit random value is returned on the boot processor from
 *    an invocation of the ccdbrg_generate method.
 *  - Non-boot processor's DRBG state structures are allocated dynamically
 *    from prng_init(). Each is initialized with the same 16 bytes of entropy
 *    but with a different timestamped nonce and cpu number as personalization.
 *  - Subsequent calls to early_random() pass to read_erandom() to generate
 *    an 8-byte random value.  read_erandom() ensures that pre-emption is
 *    disabled and selects the DBRG state from the current processor.
 *    The ccdbrg_generate method is called for the required random output.
 *    If this method returns CCDRBG_STATUS_NEED_RESEED, the erandom.seed buffer
 *    is re-filled with kernel-harvested entropy and the ccdbrg_reseed method is
 *    called with this new entropy. The kernel panics if a reseed fails.
 */
uint64_t
early_random(void)
{
	uint32_t	cnt = 0;
	uint64_t	result;
	uint64_t	nonce;
	int		rc;
	int		ps;
	struct ccdrbg_state *state;

	if (!erandom.seedset) {
		erandom.seedset = 1;
		cnt = PE_get_random_seed((unsigned char *) EntropyData.buffer,
					 sizeof(EntropyData.buffer));

		if (cnt < sizeof(EntropyData.buffer)) {
			/*
			 * Insufficient entropy is fatal.  We must fill the
			 * entire entropy buffer during initializaton.
			 */
			panic("EntropyData needed %lu bytes, but got %u.\n",
				sizeof(EntropyData.buffer), cnt);
		}		

		entropy_readall(&erandom.seed, EARLY_RANDOM_SEED_SIZE);

		/* Init DRBG for NIST HMAC */
		ccdrbg_factory_nisthmac(&erandom.drbg_info, &erandom.drbg_custom);
		assert(erandom.drbg_info.size <= sizeof(erandom.master_drbg_state));
		state = (struct ccdrbg_state *) erandom.master_drbg_state;
		erandom.drbg_states[master_cpu] = state;

		/*
		 * Init our DBRG from the boot entropy and a timestamp as nonce
		 * and the cpu number as personalization.
		 */
		assert(sizeof(erandom.seed) > sizeof(nonce));
		nonce = ml_get_timebase();
		ps = 0;				/* boot cpu */
		rc = ccdrbg_init(&erandom.drbg_info, state,
				 sizeof(erandom.seed), erandom.seed,
				 sizeof(nonce), &nonce,
				 sizeof(ps), &ps);
		cc_clear(sizeof(nonce), &nonce);
		if (rc != CCDRBG_STATUS_OK)
			panic("ccdrbg_init() returned %d", rc);

		/* Generate output */
		rc = ccdrbg_generate(&erandom.drbg_info, state,
					 sizeof(result), &result,
					 0, NULL);
		if (rc != CCDRBG_STATUS_OK)
			panic("ccdrbg_generate() returned %d", rc);
	
		return result;
	};

	read_erandom(&result, sizeof(result));

	return result;
}

static void
read_erandom(void *buffer, u_int numBytes)
{
	int		cpu;
	int		rc;
	size_t nbytes;
	struct ccdrbg_state *state;

	mp_disable_preemption();
	cpu = cpu_number();
	state = erandom.drbg_states[cpu];
	assert(state);
	for (;;) {
		/* Generate output */
		rc = ccdrbg_generate(&erandom.drbg_info, state,
					 numBytes, buffer,
					 0, NULL);
		if (rc == CCDRBG_STATUS_OK)
			break;
		if (rc == CCDRBG_STATUS_NEED_RESEED) {
			/* It's time to reseed. Get more entropy */
			nbytes = entropy_readall(erandom.seed, EARLY_RANDOM_SEED_SIZE);
			assert(nbytes >= EARLY_RANDOM_SEED_SIZE);
			rc = ccdrbg_reseed(&erandom.drbg_info, state,
					   sizeof(erandom.seed), erandom.seed,
					   0, NULL);
			cc_clear(sizeof(erandom.seed), erandom.seed);
			if (rc == CCDRBG_STATUS_OK)
				continue;
			panic("read_erandom reseed error %d\n", rc);
		}
		panic("read_erandom ccdrbg error %d\n", rc);
	}
	mp_enable_preemption();
}

void
read_frandom(void *buffer, u_int numBytes)
{
	uint8_t *buffer_bytes = buffer;
	int nbytes;

	/*
	 * Split up into requests for blocks smaller than
	 * than the DBRG request limit. iThis limit is private but
	 * for NISTHMAC it's known to be greater then 4096.
	 */
	while (numBytes) {
		nbytes = MIN(numBytes, PAGE_SIZE);
		read_erandom(buffer_bytes, nbytes);
		buffer_bytes += nbytes;
		numBytes -= nbytes;
	}
}

/*
 * Register a DRBG factory routine to e used in constructing the kernel PRNG.
 * XXX to be called from the corecrypto kext.
 */
void
prng_factory_register(ccdrbg_factory_t factory)
{
	prng_ccdrbg_factory = factory;
	thread_wakeup((event_t) &prng_ccdrbg_factory);
}

void
prng_cpu_init(int cpu)
{	
	uint64_t	nonce;
	int		rc;
	struct ccdrbg_state *state;
	prngContextp	pp;

	/*
	 * Allocate state and initialize DBRG state for early_random()
	 * for this processor, if necessary.
	 */
	if (erandom.drbg_states[cpu] == NULL) {
		
		state = kalloc(erandom.drbg_info.size);
		if (state == NULL) {
			panic("prng_init kalloc failed\n");
		}
		erandom.drbg_states[cpu] = state;

		/*
		 * Init our DBRG from boot entropy, nonce as timestamp
		 * and use the cpu number as the personalization parameter.
		 */
		nonce = ml_get_timebase();
		rc = ccdrbg_init(&erandom.drbg_info, state,
				 sizeof(erandom.seed), erandom.seed,
				 sizeof(nonce), &nonce,
				 sizeof(cpu), &cpu);
		cc_clear(sizeof(nonce), &nonce);
		if (rc != CCDRBG_STATUS_OK)
			panic("ccdrbg_init() returned %d", rc);
	}

	/* Non-boot cpus use the master cpu's global context */
	if (cpu != master_cpu) {
		cpu_datap(cpu)->cpu_prng = master_prng_context();
		return;
	}

	assert(lock.mutex == NULL);		/* Once only, please */

	/* make a mutex to control access */
	lock.group_attrs = lck_grp_attr_alloc_init();
	lock.group = lck_grp_alloc_init("random", lock.group_attrs);
	lock.attrs = lck_attr_alloc_init();
	lock.mutex = lck_mtx_alloc_init(lock.group, lock.attrs);

	pp = kalloc(sizeof(*pp));
	if (pp == NULL)
		panic("Unable to allocate prng context");
	pp->bytes_generated = 0;
	pp->bytes_reseeded = 0;
	pp->infop = NULL;

	/* XXX Temporary registration */
	prng_factory_register(ccdrbg_factory_yarrow);

	master_prng_context() = pp;
}

static struct ccdrbg_info *
prng_infop(prngContextp pp)
{
	uint8_t buf[nsources][ENTROPY_BUFFER_BYTE_SIZE];
	size_t nbytes;
	
	lck_mtx_assert(lock.mutex, LCK_MTX_ASSERT_OWNED);

	/* Usual case: the info is all set */
	if (pp->infop)
		return pp->infop;

	/*
	 * Possibly wait for the CCDRBG factory routune to be registered
	 * by corecypto. But panic after waiting for more than 10 seconds.
	 */
	while (prng_ccdrbg_factory == NULL ) {
		wait_result_t	wait_result;
		assert_wait_timeout((event_t) &prng_ccdrbg_factory, TRUE,
					10, NSEC_PER_USEC);
		lck_mtx_unlock(lock.mutex);
		wait_result = thread_block(THREAD_CONTINUE_NULL);
		if (wait_result == THREAD_TIMED_OUT)
			panic("prng_ccdrbg_factory registration timeout");
		lck_mtx_lock(lock.mutex);
	}
	/* Check we didn't lose the set-up race */
	if (pp->infop)
		return pp->infop;

	pp->infop = (struct ccdrbg_info *) kalloc(sizeof(struct ccdrbg_info));
	if (pp->infop == NULL)
		panic("Unable to allocate prng info");

	prng_ccdrbg_factory(pp->infop, NULL);

	pp->statep = kalloc(pp->infop->size);
	if (pp->statep == NULL)
		panic("Unable to allocate prng state");

	nbytes = entropy_readall(buf, ENTROPY_BUFFER_BYTE_SIZE);

	(void) ccdrbg_init(pp->infop, pp->statep,
			   nbytes, buf,
			   0, NULL,
			   0, NULL);
	cc_clear(sizeof (buf), buf);
	return pp->infop;
}

static void
Reseed(prngContextp pp)
{
	uint8_t buf[nsources][ENTROPY_BUFFER_BYTE_SIZE];
	size_t nbytes;
	
	nbytes = entropy_readall(buf, ENTROPY_BUFFER_BYTE_SIZE);

	PRNG_CCDRBG((void) ccdrbg_reseed(pp->infop, pp->statep,
					 nbytes, buf,
					 0, NULL)); 

	cc_clear(sizeof (buf), buf);
	pp->bytes_reseeded = pp->bytes_generated;
}


/* export good random numbers to the rest of the kernel */
void
read_random(void* buffer, u_int numbytes)
{
	prngContextp pp;
	struct ccdrbg_info *infop;
	int ccdrbg_err;

	lck_mtx_lock(lock.mutex);

	pp = current_prng_context();
	infop = prng_infop(pp);

	/*
	 * Call DRBG, reseeding and retrying if requested.
	 */
	for (;;) {
		PRNG_CCDRBG(
			ccdrbg_err = ccdrbg_generate(infop, pp->statep,
							 numbytes, buffer,
							 0, NULL));
		if (ccdrbg_err == CCDRBG_STATUS_OK)
			break;
		if (ccdrbg_err == CCDRBG_STATUS_NEED_RESEED) {
			Reseed(pp);
			continue;
		}
		panic("read_random ccdrbg error %d\n", ccdrbg_err);
	}

	pp->bytes_generated += numbytes;
	lck_mtx_unlock(lock.mutex);
}

int
write_random(void* buffer, u_int numbytes)
{
#if 0
	int		retval = 0;
	prngContextp	pp;

	lck_mtx_lock(lock.mutex);

	pp = current_prng_context();

	if (ccdrbg_reseed(prng_infop(pp), pp->statep,
			  bytesToInput, rdBuffer, 0, NULL) != 0)
		retval = EIO;

	lck_mtx_unlock(lock.mutex);
	return retval;
#else
#pragma unused(buffer, numbytes)
	return 0;
#endif
}


/*
 * Boolean PRNG for generating booleans to randomize order of elements
 * in certain kernel data structures. The algorithm is a
 * modified version of the KISS RNG proposed in the paper:
 * http://stat.fsu.edu/techreports/M802.pdf
 * The modifications have been documented in the technical paper
 * paper from UCL:
 * http://www0.cs.ucl.ac.uk/staff/d.jones/GoodPracticeRNG.pdf
 */

/* Initialize the PRNG structures. */
void random_bool_init(struct bool_gen *bg)
{
	/* Seed the random boolean generator */
	for (int i = 0; i < RANDOM_BOOL_GEN_SEED_COUNT; i++) {
		bg->seed[i] = (unsigned int)early_random();
	}
	bg->state = 0;
	simple_lock_init(&bg->lock, 0);
}

/* Generate random bits and add them to an entropy pool. */
void random_bool_gen_entropy(
		struct bool_gen	*bg,
		unsigned int 	*buffer,
		int 		count)
{

	simple_lock(&bg->lock);
	int i, t;
	for (i = 0; i < count; i++) {
		bg->seed[1] ^= (bg->seed[1] << 5);
		bg->seed[1] ^= (bg->seed[1] >> 7);
		bg->seed[1] ^= (bg->seed[1] << 22);
		t = bg->seed[2] + bg->seed[3] + bg->state;
		bg->seed[2] = bg->seed[3];
		bg->state = t < 0;
		bg->seed[3] = t & 2147483647;
		bg->seed[0] += 1411392427;
		buffer[i] = (bg->seed[0] + bg->seed[1] + bg->seed[3]);
	}
	simple_unlock(&bg->lock);
}

/* Get some number of bits from the entropy pool, refilling if necessary. */
unsigned int random_bool_gen_bits(
		struct bool_gen	*bg,
		unsigned int	*buffer,
		unsigned int	count,
		unsigned int	numbits)
{
	unsigned int index = 0;
	unsigned int rbits = 0;
	for (unsigned int bitct = 0; bitct < numbits; bitct++) {
		/*
		 * Find a portion of the buffer that hasn't been emptied.
		 * We might have emptied our last index in the previous iteration.
		 */
		while (index < count && buffer[index] == 0)
			index++;

		/* If we've exhausted the pool, refill it. */
		if (index == count) {
			random_bool_gen_entropy(bg, buffer, count);
			index = 0;
		}

		/* Collect-a-bit */
		unsigned int bit = buffer[index] & 1;
		buffer[index] = buffer[index] >> 1;
		rbits = bit | (rbits << 1);
	}
	return rbits;
}
