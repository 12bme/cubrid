/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */


/*
 * slotted_page.h - SLOTTED PAGE MANAGEMENT MODULE (AT SERVER)
 */

#ifndef _SLOTTED_PAGE_H_
#define _SLOTTED_PAGE_H_

#ident "$Id$"

#include "config.h"

#include "error_manager.h"
#include "storage_common.h"
#include "log_manager.h"
#include "vacuum.h"

#define PEEK          true	/* Peek for a slotted record */
#define COPY          false	/* Don't peek, but copy a slotted record */
#define DONT_PEEK     COPY	/* Same as copy */

enum
{
  ANCHORED = 1,
  ANCHORED_DONT_REUSE_SLOTS = 2,
  UNANCHORED_ANY_SEQUENCE = 3,
  UNANCHORED_KEEP_SEQUENCE = 4
};

enum
{
/* Unknown record type */
  REC_UNKNOWN = 0,

/* Record without content, just the address */
  REC_ASSIGN_ADDRESS = 1,

/* Home of record */
  REC_HOME = 2,

/* No the original home of record.  part of relocation process */
  REC_NEWHOME = 3,

/* Record describe new home of record */
  REC_RELOCATION = 4,

/* Record describe location of big record */
  REC_BIGONE = 5,

/* Slot does not describe any record.
 * A record was stored in this slot.  Slot cannot be reused. 
 */
  REC_MARKDELETED = 6,

/* Slot does not describe any record.
 * A record was stored in this slot.  Slot will be reused. 
 */
  REC_DELETED_WILL_REUSE = 7,

/* Slot's record has been deleted from heap file but has to be deleted from
 * index entries too (MVCC context).
 */
  REC_DEAD = 8,

/* unused reserved record type */
  REC_RESERVED_TYPE_9 = 9,
  REC_RESERVED_TYPE_10 = 10,
  REC_RESERVED_TYPE_11 = 11,
  REC_RESERVED_TYPE_12 = 12,
  REC_RESERVED_TYPE_13 = 13,
  REC_RESERVED_TYPE_14 = 14,
  REC_RESERVED_TYPE_15 = 15,
/* 4bit record type max */
  REC_4BIT_USED_TYPE_MAX = REC_DEAD,
  REC_4BIT_TYPE_MAX = REC_RESERVED_TYPE_15
};

/* Some platform like windows used their own SP_ERROR. */
#ifdef SP_ERROR
#undef SP_ERROR
#endif

#define SP_ERROR      (-1)
#define SP_SUCCESS     (1)
#define SP_DOESNT_FIT  (3)

#define SAFEGUARD_RVSPACE      true
#define DONT_SAFEGUARD_RVSPACE false

/* Slotted page header flags */
#define SPAGE_HEADER_FLAG_NONE		0x0	/* No flags */
#define SPAGE_HEADER_FLAG_ALL_VISIBLE	0x1	/* All records are visible */

typedef struct spage_header SPAGE_HEADER;
struct spage_header
{
  PGNSLOTS num_slots;		/* Number of allocated slots for the page */
  PGNSLOTS num_records;		/* Number of records on page */
  INT16 anchor_type;		/* Valid ANCHORED, ANCHORED_DONT_REUSE_SLOTS
				   UNANCHORED_ANY_SEQUENCE,
				   UNANCHORED_KEEP_SEQUENCE */
  unsigned short alignment;	/* Alignment for records: Valid values sizeof
				   char, short, int, double */
  int total_free;		/* Total free space on page */
  int cont_free;		/* Contiguous free space on page */
  int offset_to_free_area;	/* Byte offset from the beginning of the page
				   to the first free byte area on the page. */
  int last_mvcc_id;		/* The ID of last modifying mvcc_id.
				 * Only next scenarios are considered:
				 * - an inserted record belonging to an aborted transaction
				 * - a deleted record
				 * - an updated record
				 */
  int flags;			/* Page flags */
  unsigned int is_saving:1;	/* True if saving is need for recovery (undo) */
  unsigned int need_update_best_hint:1;	/* True if we should update best pages hint
					 * for this page. See heap_stats_update. */

  /* The followings are reserved for future use. */
  /* SPAGE_HEADER should be 8 bytes aligned. Packing of bit fields depends on
   * compiler's behavior. It's better to use 4-bytes type in order not to be
   * affected by the compiler.
   */
  unsigned int reserved_bits:30;
  int reserved1;
  int reserved2;
};

/* 4-byte disk storage slot design */
typedef struct spage_slot SPAGE_SLOT;
struct spage_slot
{
  unsigned int offset_to_record:14;	/* Byte Offset from the beginning of the page
					 * to the beginning of the record */
  unsigned int record_length:14;	/* Length of record */
  unsigned int record_type:4;	/* Record type (REC_HOME, REC_NEWHOME, ...)
				 * described by slot. */
};

extern int spage_boot (THREAD_ENTRY * thread_p);
extern void spage_finalize (THREAD_ENTRY * thread_p);
extern void spage_free_saved_spaces (THREAD_ENTRY * thread_p,
				     void *first_save_entry);
extern int spage_slot_size (void);
extern int spage_header_size (void);
extern int spage_get_free_space (THREAD_ENTRY * thread_p, PAGE_PTR pgptr);
extern int spage_get_free_space_without_saving (THREAD_ENTRY * thread_p,
						PAGE_PTR page_p,
						bool * need_update);
extern void spage_set_need_update_best_hint (THREAD_ENTRY * thread_p,
					     PAGE_PTR page_p,
					     bool need_update);
extern PGNSLOTS spage_number_of_records (PAGE_PTR pgptr);
extern PGNSLOTS spage_number_of_slots (PAGE_PTR pgptr);
extern void spage_initialize (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			      INT16 slots_type, unsigned short alignment,
			      bool safeguard_rvspace);
extern int spage_insert (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			 RECDES * recdes, PGSLOTID * slotid);
extern int spage_find_slot_for_insert (THREAD_ENTRY * thread_p,
				       PAGE_PTR pgptr, RECDES * recdes,
				       PGSLOTID * slotid, void **slotptr,
				       int *used_space);
extern int spage_insert_at (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			    PGSLOTID slotid, RECDES * recdes);
extern int spage_insert_for_recovery (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
				      PGSLOTID slotid, RECDES * recdes);
extern PGSLOTID spage_delete (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			      PGSLOTID slotid);
extern PGSLOTID spage_delete_for_recovery (THREAD_ENTRY * thread_p,
					   PAGE_PTR pgptr, PGSLOTID slotid);
extern int spage_update (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			 PGSLOTID slotid, const RECDES * recdes);
extern void spage_update_record_type (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
				      PGSLOTID slotid, INT16 type);
extern bool spage_is_updatable (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
				PGSLOTID slotid, const RECDES * recdes);
extern bool spage_reclaim (THREAD_ENTRY * thread_p, PAGE_PTR pgptr);
extern int spage_split (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			PGSLOTID slotid, int offset, PGSLOTID * new_slotid);
extern int spage_append (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			 PGSLOTID slotid, const RECDES * recdes);
extern int spage_take_out (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			   PGSLOTID slotid, int takeout_offset,
			   int takeout_length);
extern int spage_put (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
		      PGSLOTID slotid, int offset, const RECDES * recdes);
extern int spage_overwrite (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			    PGSLOTID slotid, int overwrite_offset,
			    const RECDES * recdes);
extern int spage_merge (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
			PGSLOTID slotid1, PGSLOTID slotid2);
extern SCAN_CODE spage_next_record (PAGE_PTR pgptr, PGSLOTID * slotid,
				    RECDES * recdes, int ispeeking);
extern SCAN_CODE spage_next_record_dont_skip_empty (PAGE_PTR pgptr,
						    PGSLOTID * slotid,
						    RECDES * recdes,
						    int ispeeking);
extern SCAN_CODE spage_previous_record (PAGE_PTR pgptr, PGSLOTID * slotid,
					RECDES * recdes, int ispeeking);
extern SCAN_CODE spage_previous_record_dont_skip_empty (PAGE_PTR pgptr,
							PGSLOTID * slotid,
							RECDES * recdes,
							int ispeeking);
extern SCAN_CODE spage_get_page_header_info (PAGE_PTR page_p,
					     DB_VALUE ** page_header_info);
extern SCAN_CODE spage_get_record (PAGE_PTR pgptr, PGSLOTID slotid,
				   RECDES * recdes, int ispeeking);
extern SCAN_CODE spage_get_record_mvcc (PAGE_PTR page_p, PGSLOTID slot_id,
					RECDES * record_descriptor_p,
					int is_peeking);
extern bool spage_is_slot_exist (PAGE_PTR pgptr, PGSLOTID slotid);
extern void spage_dump (THREAD_ENTRY * thread_p, FILE * fp, PAGE_PTR pgptr,
			int isrecord_printed);
extern SPAGE_SLOT *spage_get_slot (PAGE_PTR page_p, PGSLOTID slot_id);
extern int spage_get_record_length (PAGE_PTR pgptr, PGSLOTID slotid);
extern int spage_get_record_offset (PAGE_PTR page_p, PGSLOTID slot_id);
extern int spage_get_space_for_record (PAGE_PTR page_p, PGSLOTID slot_id);
extern INT16 spage_get_record_type (PAGE_PTR pgptr, PGSLOTID slotid);
extern int spage_max_space_for_new_record (THREAD_ENTRY * thread_p,
					   PAGE_PTR pgptr);
extern void spage_collect_statistics (PAGE_PTR pgptr,
				      int *npages, int *nrecords,
				      int *rec_length);
extern int spage_max_record_size (void);
extern int spage_check_slot_owner (THREAD_ENTRY * thread_p, PAGE_PTR pgptr,
				   PGSLOTID slotid);
extern int spage_compact (PAGE_PTR pgptr);
extern bool spage_is_valid_anchor_type (const INT16 anchor_type);
extern const char *spage_anchor_flag_string (const INT16 anchor_type);
extern const char *spage_alignment_string (unsigned short alignment);
extern int spage_mark_deleted_slot_as_reusable (THREAD_ENTRY * thread_p,
						PAGE_PTR page_p,
						PGSLOTID slot_id);

extern PGSLOTID spage_find_free_slot (PAGE_PTR page_p,
				      SPAGE_SLOT ** out_slot_p,
				      PGSLOTID start_id);
extern int spage_vacuum_page (THREAD_ENTRY * thread_p, PAGE_PTR * page_p,
			      VPID page_vpid,
			      VACUUM_PAGE_DATA * page_clean_p,
			      MVCCID lowest_active_mvccid,
			      bool vacuum_page_only);
extern int spage_execute_vacuum_page (THREAD_ENTRY * thread_p,
				      PAGE_PTR page_p,
				      VACUUM_PAGE_DATA vacuum_data);
extern bool spage_should_vacuum_page (PAGE_PTR page_ptr,
				      MVCCID oldest_active);
extern void spage_finalize_vacuum_data (THREAD_ENTRY * thread_p,
					VACUUM_PAGE_DATA * vacuum_data_p);
extern void spage_mark_page_for_vacuum (THREAD_ENTRY * thread_p,
					PAGE_PTR page_ptr, MVCCID mvcc_id);
extern void spage_mark_page_as_vacuumed (THREAD_ENTRY * thread_p,
					 PAGE_PTR page_ptr);

#endif /* _SLOTTED_PAGE_H_ */
