#!/bin/bash
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2006 Oracle, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"%Z%%M%	%I%	%E% SMI"

# Rebuilding an object file containing DOF changes slightly when the object
# files containing the probes have already been modified. This tests that
# case by generating the DOF object, removing it, and building it again.

if [ $# != 1 ]; then
	echo expected one argument: '<'dtrace-path'>'
	exit 2
fi

dtrace=$1
CC=/usr/bin/gcc
CFLAGS="-I${PWD}/uts/common"
DIR=${TMPDIR:-/tmp}/static2.$$

mkdir $DIR
cd $DIR

cat > test.c <<EOF
#include <unistd.h>
#include <sys/sdt.h>

static void
foo(void)
{
	DTRACE_PROBE(test_prov, probe1);
	DTRACE_PROBE(test_prov, probe2);
}

int
main(int argc, char **argv)
{
	DTRACE_PROBE(test_prov, probe1);
	DTRACE_PROBE(test_prov, probe2);
	foo();
}
EOF

cat > prov.d <<EOF
provider test_prov {
	probe probe1();
	probe probe2();
};
EOF

${CC} ${CFLAGS} -c test.c
if [ $? -ne 0 ]; then
	echo "failed to compile test.c" >& 2
	exit 1
fi
$dtrace -G -s prov.d test.o
if [ $? -ne 0 ]; then
	echo "failed to create initial DOF" >& 2
	exit 1
fi
rm -f prov.o
$dtrace -G -s prov.d test.o
if [ $? -ne 0 ]; then
	echo "failed to create final DOF" >& 2
	exit 1
fi
${CC} ${CFLAGS} -o test test.o prov.o
if [ $? -ne 0 ]; then
	echo "failed to link final executable" >& 2
	exit 1
fi

script()
{
	$dtrace -c ./test -qs /dev/stdin <<EOF
	test_prov\$target:::
	{
		printf("%s:%s:%s\n", probemod, probefunc, probename);
	}
EOF
}

script
status=$?

cd /
rm -rf $DIR

exit $status