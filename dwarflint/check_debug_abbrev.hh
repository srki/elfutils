/* Low-level checking of .debug_abbrev.
   Copyright (C) 2009, 2010, 2011 Red Hat, Inc.
   This file is part of Red Hat elfutils.

   Red Hat elfutils is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 2 of the License.

   Red Hat elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with Red Hat elfutils; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301 USA.

   Red Hat elfutils is an included package of the Open Invention Network.
   An included package of the Open Invention Network is a package for which
   Open Invention Network licensees cross-license their patents.  No patent
   license is granted, either expressly or impliedly, by designation as an
   included package.  Should you wish to participate in the Open Invention
   Network licensing program, please visit www.openinventionnetwork.com
   <http://www.openinventionnetwork.com>.  */

#ifndef DWARFLINT_CHECK_DEBUG_ABBREV_HH
#define DWARFLINT_CHECK_DEBUG_ABBREV_HH

#include "checks.hh"
#include "sections_i.hh"
#include "check_debug_info_i.hh"
#include "dwarf_version_i.hh"

class abbrev_locus
  : public clonable_locus<abbrev_locus>
{
  uint64_t _m_abbr_offset;

public:
  explicit abbrev_locus (uint64_t abbr_offset = -1);

  abbrev_locus (abbrev_locus const &copy);

  std::string format (bool brief = false) const;
};

class abbrev_attrib_locus
  : public clonable_locus<abbrev_attrib_locus>
{
  uint64_t _m_abbr_offset;
  uint64_t _m_attr_offset;
  int _m_name;

public:
  explicit abbrev_attrib_locus (uint64_t abbr_offset = -1,
				uint64_t attr_offset = -1,
				int name = -1);

  abbrev_attrib_locus (abbrev_attrib_locus const &copy);

  abbrev_attrib_locus non_symbolic ();

  void set_name (int name);
  std::string format (bool brief = false) const;
  std::string name () const;
};

struct abbrev_attrib
{
  abbrev_attrib_locus where;
  uint16_t name;
  uint8_t form;

  abbrev_attrib ()
    : where ()
    , name (0)
    , form (0)
  {}
};

struct abbrev
{
  abbrev_locus where;
  uint64_t code;

  /* Attributes.  */
  abbrev_attrib *attribs;
  size_t size;
  size_t alloc;

  /* While ULEB128 can hold numbers > 32bit, these are not legal
     values of many enum types.  So just use as large type as
     necessary to cover valid values.  */
  uint16_t tag;
  bool has_children;

  /* Whether some DIE uses this abbrev.  */
  bool used;

  explicit abbrev (abbrev_locus const &loc)
    : where (loc)
    , code (0)
    , attribs (0)
    , size (0)
    , alloc (0)
    , tag (0)
    , has_children (false)
    , used (false)
  {}
};

struct abbrev_table
{
  struct abbrev_table *next;
  struct abbrev *abbr;
  uint64_t offset;
  size_t size;
  size_t alloc;
  bool used;		/* There are CUs using this table.  */

  abbrev *find_abbrev (uint64_t abbrev_code) const;

  abbrev_table ()
    : next (NULL)
    , abbr (NULL)
    , offset (0)
    , size (0)
    , alloc (0)
    , used (false)
  {}
};

class check_debug_abbrev
  : public check<check_debug_abbrev>
{
  section<sec_abbrev> *_m_sec_abbr;
  read_cu_headers *_m_cu_headers;

public:
  static checkdescriptor const *descriptor ();

  // offset -> abbreviations
  typedef std::map< ::Dwarf_Off, abbrev_table> abbrev_map;
  abbrev_map const abbrevs;

  check_debug_abbrev (checkstack &stack, dwarflint &lint);
  static form const *check_form (dwarf_version const *ver,
				 attribute const *attr,
				 int form_name,
				 locus const &loc,
				 bool indirect);

  ~check_debug_abbrev ();
};

#endif//DWARFLINT_CHECK_DEBUG_ABBREV_HH
