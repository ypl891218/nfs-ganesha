// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2005, 2006
 * Matthew D. Fuller <fullermd@over-yonder.net>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Various comparison functions
 */
#include "config.h"

#include <errno.h>
#include <stdio.h> /* For NULL */

#include "../include/cidr.h"

/* Is one block entirely contained in another? */
int cidr_contains(const CIDR *big, const CIDR *little)
{
	int i, oct, bit;
	int pflen;

	/* First off, they better be the same type */
	if (big->proto != little->proto) {
		errno = EPROTO;
		return (-1);
	}

	/* We better understand the protocol, too */
	if (big->proto != CIDR_IPV4 && big->proto != CIDR_IPV6) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * little better be SMALL enough to fit in big.  Note: The prefix
	 * lengths CAN be the same, and little could still 'fit' in big if
	 * the network bits are all the same.  No need to special-case it, as
	 * the normal tests below will DTRT.  Save big's pflen for the test
	 * loop.
	 */
	if (cidr_get_pflen(little) < (pflen = cidr_get_pflen(big))) {
		errno = 0;
		return (-1);
	}

	/*
	 * Now let's compare.  Note that for IPv4 addresses, the first 12
	 * octets are irrelevant.  We take care throughout to keep them
	 * zero'd out, so we don't _need_ to explicitly ignore them.  But,
	 * it's wasteful; it quadrules the amount of work needed to be done
	 * to compare to v4 blocks, and this function may be useful in fairly
	 * performance-sensitive parts of the application.  Sure, an extra 12
	 * uint8_t compares better not be the make-or-break performance point
	 * for anything real, but why make it harder unnecessarily?
	 */
	if (big->proto == CIDR_IPV4) {
		i = 96;
		pflen += 96;
	} else if (big->proto == CIDR_IPV6)
		i = 0;
	else {
		/* Shouldn't happen */
		errno = ENOENT; /* This is a really bad choice of errno */
		return (-1);
	}

	/* Start comparing */
	for (/* i */; i < pflen; i++) {
		/* For convenience, set temp. vars to the octet/bit */
		oct = i / 8;
		bit = 7 - (i % 8);

		if ((big->addr[oct] & (1 << bit)) !=
		    (little->addr[oct] & (1 << bit))) {
			errno = 0;
			return (-1);
		}
	}

	/* If we get here, all their network bits are the same */
	return (0);
}

/* Are two CIDR's the same? */
int cidr_equals(const CIDR *one, const CIDR *two)
{
	int i;

	/* Check protocols */
	if (one->proto != two->proto)
		return (-1);

	/* Check addresses/masks */
	if (one->proto == CIDR_IPV4)
		i = 12;
	else
		i = 0;
	for (/* i */; i <= 15; i++) {
		if (one->addr[i] != two->addr[i])
			return (-1);
		if (one->mask[i] != two->mask[i])
			return (-1);
	}

	/* If we make it here, they're the same */
	return (0);
}
