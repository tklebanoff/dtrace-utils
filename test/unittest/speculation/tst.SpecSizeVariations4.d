/*
 * Oracle Linux DTrace.
 * Copyright (c) 2006, 2020, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */
/* @@xfail: dtv2 */

/*
 * ASSERTION:
 * Verify the behavior of speculations with changes in specsize.
 *
 * SECTION: Speculative Tracing/Options and Tuning;
 *	Options and Tunables/specsize
 *
 */

#pragma D option quiet
#pragma D option specsize=39

BEGIN
{
	self->speculateFlag = 0;
	self->commitFlag = 0;
	self->spec = speculation();
	printf("Speculative buffer ID: %d\n", self->spec);
}

BEGIN
{
	speculate(self->spec);
	printf("Lots of data\n");
	printf("Has to be crammed into this buffer\n");
	printf("Until it overflows\n");
	printf("And causes flops\n");
	self->speculateFlag++;

}

BEGIN
/1 <= self->speculateFlag/
{
	commit(self->spec);
	self->commitFlag++;
}

BEGIN
/1 <= self->commitFlag/
{
	printf("Statement was executed\n");
	exit(1);
}

BEGIN
/1 > self->commitFlag/
{
	printf("Statement wasn't executed\n");
	exit(0);
}
