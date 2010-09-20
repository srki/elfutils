/* Pedantic checking of DWARF files
   Copyright (C) 2009,2010 Red Hat, Inc.
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

#include "messages.h"
#include "misc.h"
#include "coverage.h"

#include <vector>
#include <sstream>
#include <cassert>
#include <cstdarg>
#include <libintl.h>

unsigned error_count = 0;

bool
message_accept (struct message_criteria const *cri,
		unsigned long cat)
{
  for (size_t i = 0; i < cri->size; ++i)
    {
      struct message_term *t = cri->terms + i;
      if ((t->positive & cat) == t->positive
	  && (t->negative & cat) == 0)
	return true;
    }
  return false;
}

namespace
{
  struct cat_to_str
    : public std::vector<std::string>
  {
    cat_to_str ()
    {
      int count = 0;
#define MC(CAT, ID) if (ID > count) count = ID;
      MESSAGE_CATEGORIES
#undef MC

      resize (count + 1);
#define MC(CAT, ID) (*this)[ID] = #CAT;
      MESSAGE_CATEGORIES
#undef MC
    }
  };
}

std::string
message_term::str () const
{
  static cat_to_str names;
  static size_t max = names.size ();

  std::ostringstream os;
  os << '(';

  bool got = false;
  for (size_t i = 0; i <= max; ++i)
    {
      size_t mask = 1u << i;
      if ((positive & mask) != 0
	  || (negative & mask) != 0)
	{
	  if (got)
	    os << " & ";
	  if ((negative & (1u << i)) != 0)
	    os << '~';
	  os << names[i];
	  got = true;
	}
    }

  if (!got)
    os << '1';

  os << ')';
  return os.str ();
}

std::string
message_criteria::str () const
{
  std::ostringstream os;

  for (size_t i = 0; i < size; ++i)
    {
      message_term const &t = terms[i];
      if (i > 0)
	os << " | ";
      os << t.str ();
    }

  return os.str ();
}

void
message_criteria::operator &= (message_term const &term)
{
  assert ((term.positive & term.negative) == 0);
  for (size_t i = 0; i < size; )
    {
      message_term &t = terms[i];
      t.positive |= term.positive;
      t.negative |= term.negative;
      if ((t.positive & t.negative) != 0)
	/* A ^ ~A -> drop the term.  */
	terms[i] = terms[--size];
      else
	++i;
    }
}

void
message_criteria::operator |= (message_term const &term)
{
  assert ((term.positive & term.negative) == 0);
  REALLOC (this, terms);
  terms[size++] = term;
}

// xxx this one is inaccessible from the outside.  Make it like &=, |=
// above
/* NEG(a&b&~c) -> (~a + ~b + c) */
message_criteria
operator ! (message_term const &term)
{
  assert ((term.positive & term.negative) == 0);

  unsigned max = 0;
#define MC(CAT, ID) max = ID;
  MESSAGE_CATEGORIES
#undef MC

  message_criteria ret;
  for (size_t i = 0; i < max; ++i)
    {
      unsigned mask = 1u << i;
      if ((term.positive & mask) != 0)
	ret |= message_term (1u << i, mc_none);
      else if ((term.negative & mask) != 0)
	ret |= message_term (mc_none, 1u << i);
    }

  return ret;
}

// xxx this one is inaccessible from the outside.  Make it like &=, |=
// above
/* MUL((a&b + c&d), (e&f + g&h)) -> (a&b&e&f + a&b&g&h + c&d&e&f + c&d&g&h) */
void
message_cri_mul (struct message_criteria *cri, struct message_criteria *rhs)
{
  struct message_criteria ret;
  WIPE (ret);

  for (size_t i = 0; i < cri->size; ++i)
    for (size_t j = 0; j < rhs->size; ++j)
      {
	struct message_term t1 = cri->terms[i];
	struct message_term *t2 = rhs->terms + j;
	t1.positive |= t2->positive;
	t1.negative |= t2->negative;
	if (t1.positive & t1.negative)
	  /* A ^ ~A -> drop the term.  */
	  continue;
	ret |= t1;
      }

  free (cri->terms);
  *cri = ret;
}

// xxx this one is inaccessible from the outside.  Bind it properly
/* Reject message if TERM passes.  */
void
message_cri_and_not (message_criteria &cri, message_term const &term)
{
  message_criteria tmp = !message_term (term.negative, term.positive);
  message_cri_mul (&cri, &tmp);
}

static void
wr_verror (const struct where *wh, const char *format, va_list ap)
{
  printf ("error: %s", where_fmt (wh, NULL));
  vprintf (format, ap);
  where_fmt_chain (wh, "error");
  ++error_count;
}

static void
wr_vwarning (const struct where *wh, const char *format, va_list ap)
{
  printf ("warning: %s", where_fmt (wh, NULL));
  vprintf (format, ap);
  where_fmt_chain (wh, "warning");
  ++error_count;
}

void
wr_error (const struct where *wh, const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  wr_verror (wh, format, ap);
  va_end (ap);
}

void
wr_warning (const struct where *wh, const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  wr_vwarning (wh, format, ap);
  va_end (ap);
}

void
wr_message (unsigned long category, const struct where *wh,
	    const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  if (message_accept (&warning_criteria, category))
    {
      if (message_accept (&error_criteria, category))
	wr_verror (wh, format, ap);
      else
	wr_vwarning (wh, format, ap);
    }
  va_end (ap);
}

namespace
{
  class nostream: public std::ostream {};
  nostream nostr;

  std::ostream &get_stream ()
  {
    return std::cout;
  }
}

std::ostream &
wr_warning ()
{
  ++error_count;
  return get_stream () << gettext ("warning: ");
}

std::ostream &
wr_error ()
{
  ++error_count;
  return get_stream () << gettext ("error: ");
}

std::ostream &
wr_message (message_category category)
{
  if (!message_accept (&warning_criteria, category))
    return nostr;
  else if (message_accept (&error_criteria, category))
    return wr_error ();
  else
    return wr_warning ();
}

std::ostream &
wr_warning (where const &wh)
{
  return wr_warning () << wh << ": ";
}

std::ostream &
wr_error (where const &wh)
{
  return wr_error () << wh << ": ";
}

std::ostream &
wr_message (where const &wh, message_category category)
{
  return wr_message (category) << wh << ": ";
}

void
wr_format_padding_message (unsigned long category,
			   struct where const *wh,
			   uint64_t start, uint64_t end, char const *kind)
{
  char msg[128];
  wr_message (category, wh, ": %s: %s.\n",
	      range_fmt (msg, sizeof msg, start, end), kind);
}

void
wr_format_leb128_message (struct where const *where,
			  const char *what,
			  const char *purpose,
			  const unsigned char *begin, const unsigned char *end)
{
  unsigned long category = mc_leb128 | mc_acc_bloat | mc_impact_3;
  char buf[(end - begin) * 3 + 1]; // 2 hexa digits+" " per byte, and term. 0
  char *ptr = buf;
  for (; begin < end; ++begin)
    ptr += sprintf (ptr, " %02x", *begin);
  wr_message (category, where,
	      ": %s: value %s encoded as `%s'.\n",
	      what, purpose, buf + 1);
}

void
wr_message_padding_0 (unsigned long category,
		      struct where const *wh,
		      uint64_t start, uint64_t end)
{
  wr_format_padding_message (category | mc_acc_bloat | mc_impact_1,
			     wh, start, end,
			     "unnecessary padding with zero bytes");
}

void
wr_message_padding_n0 (unsigned long category,
		       struct where const *wh,
		       uint64_t start, uint64_t end)
{
  wr_format_padding_message (category | mc_acc_bloat | mc_impact_1,
			     wh, start, end,
			     "unreferenced non-zero bytes");
}