#! /bin/sh
# Copyright (C) 2009 Red Hat, Inc.
# This file is part of Red Hat elfutils.
#
# Red Hat elfutils is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by the
# Free Software Foundation; version 2 of the License.
#
# Red Hat elfutils is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with Red Hat elfutils; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301 USA.
#
# Red Hat elfutils is an included package of the Open Invention Network.
# An included package of the Open Invention Network is a package for which
# Open Invention Network licensees cross-license their patents.  No patent
# license is granted, either expressly or impliedly, by designation as an
# included package.  Should you wish to participate in the Open Invention
# Network licensing program, please visit www.openinventionnetwork.com
# <http://www.openinventionnetwork.com>.

. $srcdir/test-subr.sh

testfiles testfile

testrun_compare ./dwarf-print --offsets --depth=1 testfile <<\EOF
testfile:
 <compile_unit offset=[0xb] stmt_list=[{1 dirs}, {5 line entries}] high_pc=0x804845a low_pc=0x804842c name="m.c" comp_dir="/home/drepper/gnu/new-bu/build/ttt" producer="GNU C 2.96 20000731 (Red Hat Linux 7.0)" language=C89>...
 <compile_unit offset=[0xca] stmt_list=[{1 dirs}, {4 line entries}] high_pc=0x8048466 low_pc=0x804845c name="b.c" comp_dir="/home/drepper/gnu/new-bu/build/ttt" producer="GNU C 2.96 20000731 (Red Hat Linux 7.0)" language=C89>...
 <compile_unit offset=[0x15fc] stmt_list=[{1 dirs}, {4 line entries}] high_pc=0x8048472 low_pc=0x8048468 name="f.c" comp_dir="/home/drepper/gnu/new-bu/build/ttt" producer="GNU C 2.96 20000731 (Red Hat Linux 7.0)" language=C89>...
EOF

exit 0