/*
 * Oracle Linux DTrace.
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */
/* @@xfail: dtv2 */

/*
 * ASSERTION: The 'ppid' variable can be accessed and is not -1.
 *
 * SECTION: Variables/Built-in Variables/ppid
 */

#pragma D option quiet

BEGIN {
	trace(ppid);
	exit(ppid != -1 ? 0 : 1);
}
