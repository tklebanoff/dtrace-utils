#!/bin/bash
#
# Oracle Linux DTrace.
# Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
# Licensed under the Universal Permissive License v 1.0 as shown at
# http://oss.oracle.com/licenses/upl.
#

#
# This script tests that libproc can still track the state of the link map when
# it is continuously changing.  We turn LD_AUDIT off first to make sure that
# only one lmid is in use.
#

# @@timeout: 50
# @@tags: unstable

unset LD_AUDIT

exec test/triggers/libproc-consistency test/triggers/libproc-dlmadopen
