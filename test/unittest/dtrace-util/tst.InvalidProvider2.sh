#!/bin/bash
#
# Oracle Linux DTrace.
# Copyright (c) 2006, Oracle and/or its affiliates. All rights reserved.
# Licensed under the Universal Permissive License v 1.0 as shown at
# http://oss.oracle.com/licenses/upl.
#

##
#
# ASSERTION:
# The -lP option can be used to list the probes from their provider names.
# Invalid module names result in error.
#
# SECTION: dtrace Utility/-l Option;
# 	dtrace Utility/-P Option
#
##

if [ $# != 1 ]; then
	echo expected one argument: '<'dtrace-path'>'
	exit 2
fi

dtrace=$1

$dtrace $dt_flags -lP profile:::

status=$?

echo $status

if [ "$status" -ne 0 ]; then
	exit 0
fi

echo $tst: dtrace failed
exit $status
