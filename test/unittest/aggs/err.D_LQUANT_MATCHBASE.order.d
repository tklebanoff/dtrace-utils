/*
 * Oracle Linux DTrace.
 * Copyright (c) 2006, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */
/* @@xfail: dtv2 */

BEGIN
{
	@ = lquantize(0, 10, 20, 1);
	@ = lquantize(0, 15, 30, 10);
}
