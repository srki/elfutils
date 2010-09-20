/* Routines related to .debug_loc and .debug_range.

   Copyright (C) 2009, 2010 Red Hat, Inc.
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

// xxx drop as soon as not necessary
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <cassert>
#include <sstream>
#include <algorithm>
#include "../libdw/dwarf.h"

#include "low.h"
#include "check_debug_loc_range.hh"
#include "check_debug_info.hh"
#include "sections.hh"
#include "../src/dwarf-opcodes.h"
#include "pri.hh"

bool do_range_coverage = false; // currently no option

checkdescriptor const *
check_debug_ranges::descriptor ()
{
  static checkdescriptor cd
    (checkdescriptor::create ("check_debug_ranges")
     .groups ("@low")
     .prereq<typeof (*_m_sec_ranges)> ()
     .prereq<typeof (*_m_info)> ()
     .description (
"Checks for low-level structure of .debug_ranges.  In addition it\n"
"checks:\n"
" - for overlapping and dangling references from .debug_info\n"
" - that base address is set and that it actually changes the address\n"
" - that ranges have a positive size\n"
" - that there are no unreferenced holes in the section\n"
" - that relocations are valid.  In ET_REL files that certain fields\n"
"   are relocated\n"
" - neither or both of range start and range end are expected to be\n"
"   relocated.  It's expected that they are both relocated against the\n"
"   same section.\n"));
  return &cd;
}

checkdescriptor const *
check_debug_loc::descriptor ()
{
  static checkdescriptor cd
    (checkdescriptor::create ("check_debug_loc")
     .groups ("@low")
     .prereq<typeof (*_m_sec_loc)> ()
     .prereq<typeof (*_m_info)> ()
     .description (
"Checks for low-level structure of .debug_loc.  In addition it\n"
"makes the same checks as .debug_ranges.  For location expressions\n"
"it further checks:\n"
" - that DW_OP_bra and DW_OP_skip argument is non-zero and doesn't\n"
"   escape the expression.  In addition it is required that the jump\n"
"   ends on another instruction, not arbitrarily in the middle of the\n"
"   byte stream, even if that position happened to be interpretable as\n"
"   another well-defined instruction stream.\n"
" - on 32-bit machines it rejects DW_OP_const8u and DW_OP_const8s\n"
" - on 32-bit machines it checks that ULEB128-encoded arguments aren't\n"
"   quantities that don't fit into 32 bits\n"));
  return &cd;
}

namespace
{
  void
  section_coverage_init (struct section_coverage *sco,
			 struct sec *sec, bool warn)
  {
    assert (sco != NULL);
    assert (sec != NULL);

    sco->sec = sec;
    WIPE (sco->cov);
    sco->hit = false;
    sco->warn = warn;
  }

  bool
  coverage_map_init (struct coverage_map *coverage_map,
		     struct elf_file *elf,
		     Elf64_Xword mask,
		     Elf64_Xword warn_mask,
		     bool allow_overlap)
  {
    assert (coverage_map != NULL);
    assert (elf != NULL);

    WIPE (*coverage_map);
    coverage_map->elf = elf;
    coverage_map->allow_overlap = allow_overlap;

    for (size_t i = 1; i < elf->size; ++i)
      {
	struct sec *sec = elf->sec + i;

	bool normal = (sec->shdr.sh_flags & mask) == mask;
	bool warn = (sec->shdr.sh_flags & warn_mask) == warn_mask;
	if (normal || warn)
	  {
	    REALLOC (coverage_map, scos);
	    section_coverage_init
	      (coverage_map->scos + coverage_map->size++, sec, !normal);
	  }
      }

    return true;
  }

  struct coverage_map *
  coverage_map_alloc_XA (struct elf_file *elf, bool allow_overlap)
  {
    coverage_map *ret = (coverage_map *)xmalloc (sizeof (*ret));
    if (!coverage_map_init (ret, elf,
			    SHF_EXECINSTR | SHF_ALLOC,
			    SHF_ALLOC,
			    allow_overlap))
      {
	free (ret);
	return NULL;
      }
    return ret;
  }

  struct hole_env
  {
    struct where *where;
    uint64_t address;
    uint64_t end;
  };

  bool
  range_hole (uint64_t h_start, uint64_t h_length, void *xenv)
  {
    hole_env *env = (hole_env *)xenv;
    char buf[128], buf2[128];
    assert (h_length != 0);
    wr_error (env->where,
	      ": portion %s of the range %s "
	      "doesn't fall into any ALLOC section.\n",
	      range_fmt (buf, sizeof buf,
			 h_start + env->address, h_start + env->address + h_length),
	      range_fmt (buf2, sizeof buf2, env->address, env->end));
    return true;
  }

  struct coverage_map_hole_info
  {
    struct elf_file *elf;
    struct hole_info info;
  };

  /* begin is inclusive, end is exclusive. */
  bool
  coverage_map_found_hole (uint64_t begin, uint64_t end,
			   struct section_coverage *sco, void *user)
  {
    struct coverage_map_hole_info *info = (struct coverage_map_hole_info *)user;

    struct where where = WHERE (info->info.section, NULL);
    const char *scnname = sco->sec->name;

    struct sec *sec = sco->sec;
    GElf_Xword align = sec->shdr.sh_addralign;

    /* We don't expect some sections to be covered.  But if they
       are at least partially covered, we expect the same
       coverage criteria as for .text.  */
    if (!sco->hit
	&& ((sco->sec->shdr.sh_flags & SHF_EXECINSTR) == 0
	    || strcmp (scnname, ".init") == 0
	    || strcmp (scnname, ".fini") == 0
	    || strcmp (scnname, ".plt") == 0))
      return true;

    /* For REL files, don't print addresses mangled by our layout.  */
    uint64_t base = info->elf->ehdr.e_type == ET_REL ? 0 : sco->sec->shdr.sh_addr;

    /* If the hole is filled with NUL bytes, don't report it.  But if we
       get stripped debuginfo file, the data may not be available.  In
       that case don't report the hole, if it seems to be alignment
       padding.  */
    if (sco->sec->data->d_buf != NULL)
      {
	bool zeroes = true;
	for (uint64_t j = begin; j < end; ++j)
	  if (((char *)sco->sec->data->d_buf)[j] != 0)
	    {
	      zeroes = false;
	      break;
	    }
	if (zeroes)
	  return true;
      }
    else if (necessary_alignment (base + begin, end - begin, align))
      return true;

    char buf[128];
    wr_message (info->info.category | mc_acc_suboptimal | mc_impact_4, &where,
		": addresses %s of section %s are not covered.\n",
		range_fmt (buf, sizeof buf, begin + base, end + base), scnname);
    return true;
  }

  struct wrap_cb_arg
  {
    bool (*cb) (uint64_t begin, uint64_t end,
		struct section_coverage *, void *);
    section_coverage *sco;
    void *user;
  };

  bool
  unwrap_cb (uint64_t h_start, uint64_t h_length, void *user)
  {
    wrap_cb_arg *arg = (wrap_cb_arg *)user;
    return (arg->cb) (h_start, h_start + h_length, arg->sco, arg->user);
  }

  bool
  coverage_map_find_holes (struct coverage_map *coverage_map,
			   bool (*cb) (uint64_t begin, uint64_t end,
				       struct section_coverage *, void *),
			   void *user)
  {
    for (size_t i = 0; i < coverage_map->size; ++i)
      {
	section_coverage *sco = coverage_map->scos + i;
	wrap_cb_arg arg = {cb, sco, user};
	if (!coverage_find_holes (&sco->cov, 0, sco->sec->shdr.sh_size,
				  unwrap_cb, &arg))
	  return false;
      }

    return true;
  }

  void
  coverage_map_add (struct coverage_map *coverage_map,
		    uint64_t address,
		    uint64_t length,
		    struct where *where,
		    enum message_category cat)
  {
    bool found = false;
    bool crosses_boundary = false;
    bool overlap = false;
    uint64_t end = address + length;
    char buf[128]; // for messages

    /* This is for analyzing how much of the current range falls into
       sections in coverage map.  Whatever is left uncovered doesn't
       fall anywhere and is reported.  */
    struct coverage range_cov;
    WIPE (range_cov);

    for (size_t i = 0; i < coverage_map->size; ++i)
      {
	struct section_coverage *sco = coverage_map->scos + i;
	GElf_Shdr *shdr = &sco->sec->shdr;
	struct coverage *cov = &sco->cov;

	Elf64_Addr s_end = shdr->sh_addr + shdr->sh_size;
	if (end <= shdr->sh_addr || address >= s_end)
	  /* no overlap */
	  continue;

	if (found && !crosses_boundary)
	  {
	    /* While probably not an error, it's very suspicious.  */
	    wr_message (cat | mc_impact_2, where,
			": the range %s crosses section boundaries.\n",
			range_fmt (buf, sizeof buf, address, end));
	    crosses_boundary = true;
	  }

	found = true;

	if (length == 0)
	  /* Empty range.  That means no actual coverage, and we can
	     also be sure that there are no more sections that this one
	     falls into.  */
	  break;

	uint64_t cov_begin
	  = address < shdr->sh_addr ? 0 : address - shdr->sh_addr;
	uint64_t cov_end
	  = end < s_end ? end - shdr->sh_addr : shdr->sh_size;
	assert (cov_begin < cov_end);

	uint64_t r_delta = shdr->sh_addr - address;
	uint64_t r_cov_begin = cov_begin + r_delta;
	uint64_t r_cov_end = cov_end + r_delta;

	if (!overlap && !coverage_map->allow_overlap
	    && coverage_is_overlap (cov, cov_begin, cov_end - cov_begin))
	  {
	    /* Not a show stopper, this shouldn't derail high-level.  */
	    wr_message (cat | mc_impact_2 | mc_error, where,
			": the range %s overlaps with another one.\n",
			range_fmt (buf, sizeof buf, address, end));
	    overlap = true;
	  }

	if (sco->warn)
	  wr_message (cat | mc_impact_2, where,
		      ": the range %s covers section %s.\n",
		      range_fmt (buf, sizeof buf, address, end), sco->sec->name);

	/* Section coverage... */
	coverage_add (cov, cov_begin, cov_end - cov_begin);
	sco->hit = true;

	/* And range coverage... */
	coverage_add (&range_cov, r_cov_begin, r_cov_end - r_cov_begin);
      }

    if (!found)
      /* Not a show stopper.  */
      wr_error (where,
		": couldn't find a section that the range %s covers.\n",
		range_fmt (buf, sizeof buf, address, end));
    else if (length > 0)
      {
	hole_env env = {where, address, end};
	coverage_find_holes (&range_cov, 0, length, range_hole, &env);
      }

    coverage_free (&range_cov);
  }

  void
  coverage_map_free (struct coverage_map *coverage_map)
  {
    for (size_t i = 0; i < coverage_map->size; ++i)
      coverage_free (&coverage_map->scos[i].cov);
    free (coverage_map->scos);
  }

  void
  coverage_map_free_XA (coverage_map *coverage_map)
  {
    if (coverage_map != NULL)
      {
	coverage_map_free (coverage_map);
	free (coverage_map);
      }
  }

  bool
  check_loc_or_range_ref (struct elf_file *file,
			  const struct read_ctx *parent_ctx,
			  struct cu *cu,
			  struct sec *sec,
			  struct coverage *coverage,
			  struct coverage_map *coverage_map,
			  struct coverage *pc_coverage,
			  uint64_t addr,
			  struct where const *wh,
			  enum message_category cat)
  {
    char buf[128]; // messages

    assert (sec->id == sec_loc || sec->id == sec_ranges);
    assert (cat == mc_loc || cat == mc_ranges);
    assert ((sec->id == sec_loc) == (cat == mc_loc));
    assert (coverage != NULL);

    struct read_ctx ctx;
    read_ctx_init (&ctx, parent_ctx->data, file->other_byte_order);
    if (!read_ctx_skip (&ctx, addr))
      {
	wr_error (wh, ": invalid reference outside the section "
		  "%#" PRIx64 ", size only %#tx.\n",
		  addr, ctx.end - ctx.begin);
	return false;
      }

    bool retval = true;
    bool contains_locations = sec->id == sec_loc;

    if (coverage_is_covered (coverage, addr, 1))
      {
	wr_error (wh, ": reference to %#" PRIx64
		  " points into another location or range list.\n", addr);
	retval = false;
      }

    uint64_t escape = cu->head->address_size == 8
      ? (uint64_t)-1 : (uint64_t)(uint32_t)-1;

    bool overlap = false;
    uint64_t base = cu->low_pc;
    while (!read_ctx_eof (&ctx))
      {
	struct where where = WHERE (sec->id, wh);
	where_reset_1 (&where, read_ctx_get_offset (&ctx));

#define HAVE_OVERLAP						\
	do {							\
	  wr_error (&where, ": range definitions overlap.\n");	\
	  retval = false;					\
	  overlap = true;					\
	} while (0)

	/* begin address */
	uint64_t begin_addr;
	uint64_t begin_off = read_ctx_get_offset (&ctx);
	GElf_Sym begin_symbol_mem, *begin_symbol = &begin_symbol_mem;
	bool begin_relocated = false;
	if (!overlap
	    && coverage_is_overlap (coverage, begin_off, cu->head->address_size))
	  HAVE_OVERLAP;

	if (!read_ctx_read_offset (&ctx, cu->head->address_size == 8, &begin_addr))
	  {
	    wr_error (&where, ": can't read address range beginning.\n");
	    return false;
	  }

	struct relocation *rel;
	if ((rel = relocation_next (&sec->rel, begin_off,
				    &where, skip_mismatched)))
	  {
	    begin_relocated = true;
	    relocate_one (file, &sec->rel, rel, cu->head->address_size,
			  &begin_addr, &where, rel_value,	&begin_symbol);
	  }

	/* end address */
	uint64_t end_addr;
	uint64_t end_off = read_ctx_get_offset (&ctx);
	GElf_Sym end_symbol_mem, *end_symbol = &end_symbol_mem;
	bool end_relocated = false;
	if (!overlap
	    && coverage_is_overlap (coverage, end_off, cu->head->address_size))
	  HAVE_OVERLAP;

	if (!read_ctx_read_offset (&ctx, cu->head->address_size == 8,
				   &end_addr))
	  {
	    wr_error (&where, ": can't read address range ending.\n");
	    return false;
	  }

	if ((rel = relocation_next (&sec->rel, end_off,
				    &where, skip_mismatched)))
	  {
	    end_relocated = true;
	    relocate_one (file, &sec->rel, rel, cu->head->address_size,
			  &end_addr, &where, rel_value, &end_symbol);
	    if (begin_addr != escape)
	      {
		if (!begin_relocated)
		  wr_message (cat | mc_impact_2 | mc_reloc, &where,
			      ": end of address range is relocated, but the beginning wasn't.\n");
		else
		  check_range_relocations (cat, &where, file,
					   begin_symbol, end_symbol,
					   "begin and end address");
	      }
	  }
	else if (begin_relocated)
	  wr_message (cat | mc_impact_2 | mc_reloc, &where,
		      ": end of address range is not relocated, but the beginning was.\n");

	bool done = false;
	if (begin_addr == 0 && end_addr == 0 && !begin_relocated && !end_relocated)
	  done = true;
	else if (begin_addr != escape)
	  {
	    if (base == (uint64_t)-1)
	      {
		wr_error (&where,
			  ": address range with no base address set: %s.\n",
			  range_fmt (buf, sizeof buf, begin_addr, end_addr));
		/* This is not something that would derail high-level,
		   so carry on.  */
	      }

	    if (end_addr < begin_addr)
	      wr_message (cat | mc_error, &where, ": has negative range %s.\n",
			  range_fmt (buf, sizeof buf, begin_addr, end_addr));
	    else if (begin_addr == end_addr)
	      /* 2.6.6: A location list entry [...] whose beginning
		 and ending addresses are equal has no effect.  */
	      wr_message (cat | mc_acc_bloat | mc_impact_3, &where,
			  ": entry covers no range.\n");
	    /* Skip coverage analysis if we have errors or have no base
	       (or just don't do coverage analysis at all).  */
	    else if (base < (uint64_t)-2 && retval
		     && (coverage_map != NULL || pc_coverage != NULL))
	      {
		uint64_t address = begin_addr + base;
		uint64_t length = end_addr - begin_addr;
		if (coverage_map != NULL)
		  coverage_map_add (coverage_map, address, length, &where, cat);
		if (pc_coverage != NULL)
		  coverage_add (pc_coverage, address, length);
	      }

	    if (contains_locations)
	      {
		/* location expression length */
		uint16_t len;
		if (!overlap
		    && coverage_is_overlap (coverage,
					    read_ctx_get_offset (&ctx), 2))
		  HAVE_OVERLAP;

		if (!read_ctx_read_2ubyte (&ctx, &len))
		  {
		    wr_error (where)
		      << "can't read length of location expression."
		      << std::endl;
		    return false;
		  }

		/* location expression itself */
		uint64_t expr_start = read_ctx_get_offset (&ctx);
		if (!check_location_expression (*file, &ctx, cu, expr_start,
						&sec->rel, len, &where))
		  return false;
		uint64_t expr_end = read_ctx_get_offset (&ctx);
		if (!overlap
		    && coverage_is_overlap (coverage,
					    expr_start, expr_end - expr_start))
		  HAVE_OVERLAP;

		if (!read_ctx_skip (&ctx, len))
		  {
		    /* "can't happen" */
		    wr_error (&where, PRI_NOT_ENOUGH, "location expression");
		    return false;
		  }
	      }
	  }
	else
	  {
	    if (end_addr == base)
	      wr_message (cat | mc_acc_bloat | mc_impact_3, &where,
			  ": base address selection doesn't change base address"
			  " (%#" PRIx64 ").\n", base);
	    else
	      base = end_addr;
	  }
#undef HAVE_OVERLAP

	coverage_add (coverage, where.addr1, read_ctx_get_offset (&ctx) - where.addr1);
	if (done)
	  break;
      }

    return retval;
  }

  struct ref_cu
  {
    struct ref ref;
    struct cu *cu;
    bool operator < (ref_cu const& other) const {
      return ref.addr < other.ref.addr;
    }
  };

  bool
  check_loc_or_range_structural (struct elf_file *file,
				 struct sec *sec,
				 struct cu *cu_chain,
				 struct coverage *pc_coverage)
  {
    assert (sec->id == sec_loc || sec->id == sec_ranges);
    assert (cu_chain != NULL);

    struct read_ctx ctx;
    read_ctx_init (&ctx, sec->data, file->other_byte_order);

    bool retval = true;

    /* For .debug_ranges, we optionally do ranges vs. ELF sections
       coverage analysis.  */
    // xxx this is a candidate for a separate check
    struct coverage_map *coverage_map = NULL;
    if (do_range_coverage && sec->id == sec_ranges
	&& (coverage_map
	    = coverage_map_alloc_XA (file, sec->id == sec_loc)) == NULL)
      {
	wr_error (WHERE (sec->id, NULL))
	  << "couldn't read ELF, skipping coverage analysis." << std::endl;
	retval = false;
      }

    /* Overlap discovery.  */
    struct coverage coverage;
    WIPE (coverage);

    enum message_category cat = sec->id == sec_loc ? mc_loc : mc_ranges;

    /* Relocation checking in the followings assumes that all the
       references are organized in monotonously increasing order.  That
       doesn't have to be the case.  So merge all the references into
       one sorted array.  */
    {
    typedef std::vector<ref_cu> ref_cu_vect;
    ref_cu_vect refs;
    for (struct cu *cu = cu_chain; cu != NULL; cu = cu->next)
      {
	struct ref_record *rec
	  = sec->id == sec_loc ? &cu->loc_refs : &cu->range_refs;
	for (size_t i = 0; i < rec->size; ++i)
	  {
	    ref_cu ref = {rec->refs[i], cu};
	    refs.push_back (ref);
	  }
      }
    std::sort (refs.begin (), refs.end ());

    uint64_t last_off = 0;
    for (ref_cu_vect::const_iterator it = refs.begin ();
	 it != refs.end (); ++it)
      {
	uint64_t off = it->ref.addr;
	if (it != refs.begin ())
	  {
	    if (off == last_off)
	      continue;
	    struct where wh = WHERE (sec->id, NULL);
	    relocation_skip (&sec->rel, off, &wh, skip_unref);
	  }

	/* XXX We pass cu_coverage down for all ranges.  That means all
	   ranges get recorded, not only those belonging to CUs.
	   Perhaps that's undesirable.  */
	if (!check_loc_or_range_ref (file, &ctx, it->cu, sec,
				     &coverage, coverage_map, pc_coverage,
				     off, &it->ref.who, cat))
	  retval = false;
	last_off = off;
      }
    }

    if (retval)
      {
	relocation_skip_rest (&sec->rel, sec->id);

	/* We check that all CUs have the same address size when building
	   the CU chain.  So just take the address size of the first CU in
	   chain.  */
	struct hole_info hi = {
	  sec->id, cat, ctx.data->d_buf, cu_chain->head->address_size
	};
	coverage_find_holes (&coverage, 0, ctx.data->d_size, found_hole, &hi);

	if (coverage_map)
	  {
	    struct coverage_map_hole_info cmhi = {
	      coverage_map->elf, {sec->id, cat, NULL, 0}
	    };
	    coverage_map_find_holes (coverage_map, &coverage_map_found_hole,
				     &cmhi);
	  }
      }

    coverage_free (&coverage);
    coverage_map_free_XA (coverage_map);

    return retval;
  }
}

check_debug_ranges::check_debug_ranges (checkstack &stack, dwarflint &lint)
  : _m_sec_ranges (lint.check (stack, _m_sec_ranges))
  , _m_info (lint.check (stack, _m_info))
{
  memset (&_m_cov, 0, sizeof (_m_cov));
  if (!::check_loc_or_range_structural (&_m_sec_ranges->file,
					&_m_sec_ranges->sect,
					&_m_info->cus.front (),
					&_m_cov))
    throw check_base::failed ();
}

check_debug_ranges::~check_debug_ranges ()
{
  coverage_free (&_m_cov);
}

check_debug_loc::check_debug_loc (checkstack &stack, dwarflint &lint)
  : _m_sec_loc (lint.check (stack, _m_sec_loc))
  , _m_info (lint.check (stack, _m_info))
{
  if (!::check_loc_or_range_structural (&_m_sec_loc->file,
					&_m_sec_loc->sect,
					&_m_info->cus.front (),
					NULL))
    throw check_base::failed ();
}

namespace
{
  /* Operands are passed back as attribute forms.  In particular,
     DW_FORM_dataX for X-byte operands, DW_FORM_[us]data for
     ULEB128/SLEB128 operands, and DW_FORM_addr/DW_FORM_ref_addr
     for 32b/64b operands.
     If the opcode takes no operands, 0 is passed.

     Return value is false if we couldn't determine (i.e. invalid
     opcode).
  */

  bool
  get_location_opcode_operands (uint8_t opcode, uint8_t *op1, uint8_t *op2)
  {
    switch (opcode)
      {
#define DW_OP_2(OPCODE, OP1, OP2)				\
	case OPCODE: *op1 = OP1; *op2 = OP2; return true;
#define DW_OP_1(OPCODE, OP1) DW_OP_2(OPCODE, OP1, 0)
#define DW_OP_0(OPCODE) DW_OP_2(OPCODE, 0, 0)

	DW_OP_OPERANDS

#undef DEF_DW_OP_2
#undef DEF_DW_OP_1
#undef DEF_DW_OP_0
      default:
	return false;
      };
  }

  /* The value passed back in uint64_t VALUEP may actually be
     type-casted signed quantity.  WHAT and WHERE describe error
     message and context for LEB128 loading.

     If IS_BLOCKP is non-NULL, block values are accepted, and
     *IS_BLOCKP is initialized depending on whether FORM is a block
     form.  For block forms, the value passed back in VALUEP is block
     length.  */
  bool
  read_ctx_read_form (struct read_ctx *ctx, struct cu *cu,
		      uint8_t form, uint64_t *valuep, struct where *where,
		      const char *what, bool *is_blockp)
  {
    if (is_blockp != NULL)
      *is_blockp = false;
    switch (form)
      {
      case DW_FORM_addr:
	return read_ctx_read_offset (ctx, cu->head->address_size == 8, valuep);
      case DW_FORM_ref_addr:
	return read_ctx_read_offset (ctx, (cu->head->version >= 3
					   ? cu->head->offset_size
					   : cu->head->address_size) == 8,
				     valuep);
      case DW_FORM_udata:
	return checked_read_uleb128 (ctx, valuep, where, what);
      case DW_FORM_sdata:
	return checked_read_sleb128 (ctx, (int64_t *)valuep, where, what);
      case DW_FORM_data1:
	{
	  uint8_t v;
	  if (!read_ctx_read_ubyte (ctx, &v))
	    return false;
	  if (valuep != NULL)
	    *valuep = v;
	  return true;
	}
      case DW_FORM_data2:
	{
	  uint16_t v;
	  if (!read_ctx_read_2ubyte (ctx, &v))
	    return false;
	  if (valuep != NULL)
	    *valuep = v;
	  return true;
	}
      case DW_FORM_data4:
	{
	  uint32_t v;
	  if (!read_ctx_read_4ubyte (ctx, &v))
	    return false;
	  if (valuep != NULL)
	    *valuep = v;
	  return true;
	}
      case DW_FORM_data8:
	return read_ctx_read_8ubyte (ctx, valuep);
      };

    if (is_blockp != NULL)
      {
	int dform;
	switch (form)
	  {
#define HANDLE(BFORM, DFORM)			\
	    case BFORM:				\
	      dform = DFORM;			\
	      break
	    HANDLE (DW_FORM_block, DW_FORM_udata);
	    HANDLE (DW_FORM_block1, DW_FORM_data1);
	    HANDLE (DW_FORM_block2, DW_FORM_data2);
	    HANDLE (DW_FORM_block4, DW_FORM_data4);
#undef HANDLE
	  default:
	    return false;
	  }

	*is_blockp = true;
	return read_ctx_read_form (ctx, cu, dform,
				   valuep, where, what, NULL)
	  && read_ctx_skip (ctx, *valuep);
      }

    return false;
  }

  static enum section_id
  reloc_target_loc (uint8_t opcode)
  {
    switch (opcode)
      {
      case DW_OP_call2:
      case DW_OP_call4:
	return sec_info;

      case DW_OP_addr:
	return rel_address;

      case DW_OP_call_ref:
	assert (!"Can't handle call_ref!");
      };

    std::cout << "XXX don't know how to handle opcode="
	      << pri::locexpr_opcode (opcode) << std::endl;

    return rel_value;
  }

  bool
  op_read_form (struct elf_file const &file,
		struct read_ctx *ctx,
		struct cu *cu,
		uint64_t init_off,
		struct relocation_data *reloc,
		int opcode,
		int form,
		uint64_t *valuep,
		char const *str,
		struct where *where)
  {
    if (form == 0)
      return true;

    bool isblock;
    uint64_t off = read_ctx_get_offset (ctx) + init_off;

    if (!read_ctx_read_form (ctx, cu, form,
			     valuep, where, str, &isblock))
      {
	wr_error (*where)
	  << "opcode \"" << pri::locexpr_opcode (opcode)
	  << "\": can't read " << str << " (form \""
	  << pri::form (form) << "\")." << std::endl;
	return false;
      }

    /* For non-block forms, allow relocation of the datum.  For block
       form, allow relocation of block contents, but not the
       block length).  */

    struct relocation *rel;
    if ((rel = relocation_next (reloc, off,
				where, skip_mismatched)))
      {
	if (!isblock)
	  relocate_one (&file, reloc, rel,
			cu->head->address_size, valuep, where,
			reloc_target_loc (opcode), NULL);
	else
	  wr_error (where, ": relocation relocates a length field.\n");
      }
    if (isblock)
      {
	uint64_t off_block_end = read_ctx_get_offset (ctx) + init_off - 1;
	relocation_next (reloc, off_block_end, where, skip_ok);
      }

    return true;
  }
}

bool
check_location_expression (elf_file const &file,
			   struct read_ctx *parent_ctx,
			   struct cu *cu,
			   uint64_t init_off,
			   struct relocation_data *reloc,
			   size_t length,
			   struct where *wh)
{
  struct read_ctx ctx;
  if (!read_ctx_init_sub (&ctx, parent_ctx, parent_ctx->ptr,
			  parent_ctx->ptr + length))
    {
      wr_error (wh, PRI_NOT_ENOUGH, "location expression");
      return false;
    }

  struct ref_record oprefs;
  WIPE (oprefs);

  struct addr_record opaddrs;
  WIPE (opaddrs);

  while (!read_ctx_eof (&ctx))
    {
      struct where where = WHERE (sec_locexpr, wh);
      uint64_t opcode_off = read_ctx_get_offset (&ctx) + init_off;
      where_reset_1 (&where, opcode_off);
      addr_record_add (&opaddrs, opcode_off);

      uint8_t opcode;
      if (!read_ctx_read_ubyte (&ctx, &opcode))
	{
	  wr_error (&where, ": can't read opcode.\n");
	  break;
	}

      uint8_t op1, op2;
      if (!get_location_opcode_operands (opcode, &op1, &op2))
	{
	  wr_error (where)
	    << "can't decode opcode \""
	    << pri::locexpr_opcode (opcode) << "\"." << std::endl;
	  break;
	}

      uint64_t value1, value2;
      if (!op_read_form (file, &ctx, cu, init_off, reloc,
			 opcode, op1, &value1, "1st operand", &where)
	  || !op_read_form (file, &ctx, cu, init_off, reloc,
			    opcode, op2, &value2, "2st operand", &where))
	goto out;

      switch (opcode)
	{
	case DW_OP_bra:
	case DW_OP_skip:
	  {
	    int16_t skip = (uint16_t)value1;

	    if (skip == 0)
	      wr_message (where, cat (mc_loc, mc_acc_bloat, mc_impact_3))
		<< pri::locexpr_opcode (opcode)
		<< " with skip 0." << std::endl;
	    else if (skip > 0 && !read_ctx_need_data (&ctx, (size_t)skip))
	      wr_error (where)
		<< pri::locexpr_opcode (opcode)
		<< " branches out of location expression." << std::endl;
	    /* Compare with the offset after the two-byte skip value.  */
	    else if (skip < 0 && ((uint64_t)-skip) > read_ctx_get_offset (&ctx))
	      wr_error (where)
		<< pri::locexpr_opcode (opcode)
		<< " branches before the beginning of location expression."
		<< std::endl;
	    else
	      {
		uint64_t off_after = read_ctx_get_offset (&ctx) + init_off;
		ref_record_add (&oprefs, off_after + skip, &where);
	      }

	    break;
	  }

	case DW_OP_const8u:
	case DW_OP_const8s:
	  if (cu->head->address_size == 4)
	    wr_error (where)
	      << pri::locexpr_opcode (opcode) << " on 32-bit machine."
	      << std::endl;
	  break;

	default:
	  if (cu->head->address_size == 4
	      && (opcode == DW_OP_constu
		  || opcode == DW_OP_consts
		  || opcode == DW_OP_deref_size
		  || opcode == DW_OP_plus_uconst)
	      && (value1 > (uint64_t)(uint32_t)-1))
	    wr_message (where, cat (mc_loc, mc_acc_bloat, mc_impact_3))
	      << pri::locexpr_opcode (opcode)
	      << " with operand " << pri::hex (value1)
	      << " on a 32-bit machine." << std::endl;
	}
    }

 out:
  for (size_t i = 0; i < oprefs.size; ++i)
    {
      struct ref *ref = oprefs.refs + i;
      if (!addr_record_has_addr (&opaddrs, ref->addr))
	wr_error (&ref->who,
		  ": unresolved reference to opcode at %#" PRIx64 ".\n",
		  ref->addr);
    }

  addr_record_free (&opaddrs);
  ref_record_free (&oprefs);

  return true;
}

bool
found_hole (uint64_t start, uint64_t length, void *data)
{
  struct hole_info *info = (struct hole_info *)data;
  bool all_zeroes = true;
  for (uint64_t i = start; i < start + length; ++i)
    if (((char*)info->data)[i] != 0)
      {
	all_zeroes = false;
	break;
      }

  uint64_t end = start + length;
  if (all_zeroes)
    {
      /* Zero padding is valid, if it aligns on the bounds of
	 info->align bytes, and is not excessive.  */
      if (!(info->align != 0 && info->align != 1
	    && (end % info->align == 0) && (start % 4 != 0)
	    && (length < info->align)))
	{
	  struct where wh = WHERE (info->section, NULL);
	  wr_message_padding_0 (info->category, &wh, start, end);
	}
    }
  else
    {
      /* XXX: This actually lies when the unreferenced portion is
	 composed of sequences of zeroes and non-zeroes.  */
      struct where wh = WHERE (info->section, NULL);
      wr_message_padding_n0 (info->category, &wh, start, end);
    }

  return true;
}