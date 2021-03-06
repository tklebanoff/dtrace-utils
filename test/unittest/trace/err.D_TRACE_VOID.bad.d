/*
 * Oracle Linux DTrace.
 * Copyright (c) 2006, 2020, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */
/* @@xfail: dtv2 */

/*
 * ASSERTION:
 *   Test an invalid specification of trace() with a void argument.
 *
 * SECTION: Actions and Subroutines/trace()
 */

BEGIN
{
	trace((void)`max_pfn);
}
