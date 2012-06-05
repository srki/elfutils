/* i386 specific symbolic name handling.
   Copyright (C) 2000-2010 Red Hat, Inc.
   This file is part of elfutils.
   Written by Ulrich Drepper <drepper@redhat.com>, 2000.

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <elf.h>
#include <stddef.h>
#include <string.h>

#define BACKEND i386_
#include "libebl_CPU.h"


/* Return true if the symbol type is that referencing the GOT.  */
bool
i386_gotpc_reloc_check (Elf *elf __attribute__ ((unused)), int type)
{
  return type == R_386_GOTPC;
}

/* Check for the simple reloc types.  */
int
i386_reloc_simple_types (Ebl *ebl __attribute__ ((unused)),
			 const int **rel8_types, const int **rel4_types)
{
  static const int rel8[] = { 0 };
  static const int rel4[] = { R_386_32, 0 };
  *rel8_types = rel8;
  *rel4_types = rel4;
  return 0;
}

/* Check section name for being that of a debug information section.  */
bool (*generic_debugscn_p) (const char *);
bool
i386_debugscn_p (const char *name)
{
  return (generic_debugscn_p (name)
	  || strcmp (name, ".stab") == 0
	  || strcmp (name, ".stabstr") == 0);
}
