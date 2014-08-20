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
 * btree.c - B+-Tree mananger
 */

#ident "$Id$"

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "btree_load.h"
#include "storage_common.h"
#include "error_manager.h"
#include "page_buffer.h"
#include "file_io.h"
#include "file_manager.h"
#include "slotted_page.h"
#include "oid.h"
#include "log_manager.h"
#include "memory_alloc.h"
#include "overflow_file.h"
#include "xserver_interface.h"
#include "btree.h"
#include "scan_manager.h"
#if defined(SERVER_MODE)
#include "thread.h"
#endif /* SERVER_MODE */
#include "heap_file.h"
#include "object_primitive.h"
#include "list_file.h"
#include "fetch.h"
#include "connection_defs.h"
#include "locator_sr.h"
#include "network_interface_sr.h"
#include "utility.h"
#include "mvcc.h"
#include "transform.h"

/* this must be the last header file included!!! */
#include "dbval.h"

#define BTREE_HEALTH_CHECK

#define BTREE_DEBUG_DUMP_SIMPLE		0x0001	/* simple messege in SMO */
#define BTREE_DEBUG_DUMP_FULL		0x0002	/* full dump in insert or delete */

#define BTREE_DEBUG_HEALTH_SIMPLE	0x0010	/* simple health check in SMO */
#define BTREE_DEBUG_HEALTH_FULL		0x0020	/* full health check (traverse all slot in page) */

#define BTREE_DEBUG_TEST_SPLIT		0x0100	/* full split test */

#define BTREE_SPLIT_LOWER_BOUND 0.20f
#define BTREE_SPLIT_UPPER_BOUND (1.0f - BTREE_SPLIT_LOWER_BOUND)

#define BTREE_SPLIT_MIN_PIVOT 0.05f
#define BTREE_SPLIT_MAX_PIVOT (1.0f - BTREE_SPLIT_MIN_PIVOT)

#define BTREE_SPLIT_DEFAULT_PIVOT 0.5f
#define DISK_PAGE_BITS  (DB_PAGESIZE * CHAR_BIT)	/* Num of bits per page   */
#define RESERVED_SIZE_IN_PAGE   sizeof(FILEIO_PAGE_RESERVED)

#define BTREE_NODE_MAX_SPLIT_SIZE(page_ptr) \
  (db_page_size() - spage_header_size() - spage_get_space_for_record((page_ptr), HEADER))

#define OID_MSG_BUF_SIZE 64

#define MIN_KEY_SIZE DB_ALIGN (1, BTREE_MAX_ALIGN)
#define MIN_LEAF_REC_SIZE (OR_OID_SIZE + MIN_KEY_SIZE)
#define MAX_LEAF_REC_NUM (IO_MAX_PAGE_SIZE / MIN_LEAF_REC_SIZE)

#define MAX_MERGE_ALIGN_WASTE ((DB_PAGESIZE/MIN_LEAF_REC_SIZE) * (BTREE_MAX_ALIGN - 1))
#define FIXED_EMPTY (MAX (DB_PAGESIZE * 0.33, MAX_MERGE_ALIGN_WASTE * 1.3))

/* Clear MVCC flags from object OID */
#define BTREE_CLEAR_MVCC_FLAGS_FROM_OID(oid_ptr) \
  ((oid_ptr)->volid &= ~BTREE_LEAF_OID_MVCC_MASK)
/* Clear record flags from object OID */
#define BTREE_CLEAR_RECORD_FLAGS_FROM_OID(oid_ptr) \
  ((oid_ptr)->slotid &= ~BTREE_LEAF_RECORD_MASK)

/* Get an object OID from a b-tree record. If MVCC is enabled, mvcc flags are
 * cleared.
 */
#define BTREE_GET_OID(buf, oid_ptr) \
  do \
    { \
      OR_GET_OID (buf, oid_ptr); \
      if (mvcc_Enabled)	\
	{ \
	  BTREE_CLEAR_MVCC_FLAGS_FROM_OID (oid_ptr); \
	} \
    } while (0)
/* Get an object OID from b-tree record. MVCC flags are not removed */
#define BTREE_GET_OID_WITH_MVCC_FLAGS(buf, oid_ptr) \
  OR_GET_OID (buf, oid_ptr)

/* Get a class OID from b-tree record. */
#define BTREE_GET_CLASS_OID(buf, class_oid_ptr) \
  OR_GET_OID (buf, class_oid_ptr)

/* check whether insert is in fact a logical delete */
#define BTREE_INSERT_IS_LOGICAL_DELETE(p_mvcc_rec_header)  \
  ((p_mvcc_rec_header != NULL)	\
   && (!(p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID))  \
   && (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID))

/* move the data inside the record */
#define BTREE_LEAF_MOVE_INSIDE_RECORD(rec, dest_offset, src_offset)  \
  do {	\
    assert ((rec) != NULL && (dest_offset) >= 0 && (src_offset) >= 0); \
    assert (((rec)->length - (src_offset)) >= 0);  \
    assert (((rec)->area_size <= 0) || ((rec)->area_size >= (rec)->length));  \
    assert (((rec)->area_size <= 0) \
	    || (((rec)->length + ((dest_offset) - (src_offset)))  \
		<= (rec)->area_size));  \
    if ((dest_offset) != (src_offset))  \
    { \
      memmove ((rec)->data + (dest_offset), (rec)->data + (src_offset),	\
	       (rec)->length - (src_offset)); \
      (rec)->length = (rec)->length + ((dest_offset) - (src_offset)); \
    } \
  }while (0)

/* get MVCC size from leaf record flags */
#define BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG(leaf_oid_mvcc_flags) \
  (leaf_oid_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID ? \
    leaf_oid_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID ? \
    2 * OR_MVCCID_SIZE : OR_MVCCID_SIZE : \
  leaf_oid_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID ? \
    OR_MVCCID_SIZE : 0)

/* get MVCC size from record header */
#define BTREE_GET_MVCC_SIZE_FROM_REC_HEADER_FLAG(rec_header_flag) \
  (rec_header_flag & OR_MVCC_FLAG_VALID_INSID ?  \
    rec_header_flag & OR_MVCC_FLAG_VALID_DELID ? \
      2 * OR_MVCCID_SIZE : OR_MVCCID_SIZE : \
   rec_header_flag & OR_MVCC_FLAG_VALID_DELID ?  \
    OR_MVCCID_SIZE : 0)

/*
 * Page header information related defines
 */
#define NOT_FOUND -1

/* B'0001 0000 0000 0000' */
#define BTREE_LEAF_RECORD_FENCE 0x1000
/* B'0010 0000 0000 0000' */
#define BTREE_LEAF_RECORD_OVERFLOW_OIDS 0x2000
/* B'0100 0000 0000 0000' */
#define BTREE_LEAF_RECORD_OVERFLOW_KEY 0x4000
/* B'1000 0000 0000 0000' */
#define BTREE_LEAF_RECORD_SUBCLASS 0x8000
/* B'1111 0000 0000 0000' */
#define BTREE_LEAF_RECORD_MASK 0xF000

/* B'0100 0000 0000 0000' */
#define BTREE_LEAF_OID_HAS_MVCC_INSID 0x4000
/* B'1000 0000 0000 0000' */
#define BTREE_LEAF_OID_HAS_MVCC_DELID 0x8000
/* B'1100 0000 0000 0000' */
#define BTREE_LEAF_OID_MVCC_MASK 0xC000

/* The maximum number of OID's in a page */
#define BTREE_MAX_OID_COUNT IO_MAX_PAGE_SIZE / OR_OID_SIZE

#define MVCC_BTREE_DELETE_REMOVE_DELID(mvcc_args) \
  ((mvcc_args) != NULL && ((mvcc_args)->purpose == MVCC_BTREE_DELETE_DELID))
#define MVCC_BTREE_DELETE_VACUUM_INSID(mvcc_args) \
  ((mvcc_args) != NULL && ((mvcc_args)->purpose == MVCC_BTREE_VACUUM_INSID))
#define MVCC_BTREE_DELETE_VACUUM_OBJECT(mvcc_args) \
  ((mvcc_args) != NULL && ((mvcc_args)->purpose == MVCC_BTREE_VACUUM_OBJECT))
/* btree_delete doesn't need locks in MVCC context */
#define MVCC_BTREE_DELETE_NO_LOCK_REQUIRED(mvcc_args) \
  ((mvcc_args) != NULL)

/*
 * Recovery structures
 */
typedef struct pageid_struct PAGEID_STRUCT;
struct pageid_struct
{				/* Recovery pageid structure */
  VFID vfid;			/* Volume id in which page resides */
  VPID vpid;			/* Virtual page identifier */
};

typedef struct recset_header RECSET_HEADER;
struct recset_header
{				/* Recovery set of recdes structure */
  INT16 rec_cnt;		/* number of RECDESs stored */
  INT16 first_slotid;		/* first slot id */
};

typedef enum
{
  LEAF_RECORD_REGULAR = 1,
  LEAF_RECORD_OVERFLOW
} LEAF_RECORD_TYPE;

typedef enum
{
  BTREE_BOUNDARY_FIRST = 1,
  BTREE_BOUNDARY_LAST,
} BTREE_BOUNDARY;

typedef enum
{
  BTREE_MERGE_NO = 0,
  BTREE_MERGE_L_EMPTY,
  BTREE_MERGE_R_EMPTY,
  BTREE_MERGE_SIZE,
} BTREE_MERGE_STATUS;

/* Redo recovery of insert structure and flags */
#define BTREE_INSERT_RCV_FLAG_OID_INSERTED	0x8000
#define BTREE_INSERT_RCV_FLAG_OVFL_CHANGED	0x4000
#define BTREE_INSERT_RCV_FLAG_NEW_OVFLPG	0x2000
#define BTREE_INSERT_RCV_FLAG_REC_TYPE		0x1000
#define BTREE_INSERT_RCV_FLAG_INSOID_MODE	0x0C00
#define BTREE_INSERT_RCV_FLAG_UNIQUE		0x0200
#define BTREE_INSERT_RCV_FLAG_KEY_DOMAIN	0x0100

/* Check flag bits are set */
#define BTREE_INSERT_RCV_IS_OID_INSERTED(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_OID_INSERTED) != 0)
#define BTREE_INSERT_RCV_IS_OVFL_CHANGED(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_OVFL_CHANGED) != 0)
#define BTREE_INSERT_RCV_IS_NEW_OVFLPG(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_NEW_OVFLPG) != 0)
#define BTREE_INSERT_RCV_IS_UNIQUE(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_UNIQUE) != 0)
#define BTREE_INSERT_RCV_HAS_KEY_DOMAIN(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_KEY_DOMAIN) != 0)

#define BTREE_INSERT_RCV_SET_FLAGS(recins, flags_) \
  ((recins)->flags |= (flags_))

/* Check record type */
#define BTREE_INSERT_RCV_IS_RECORD_REGULAR(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_REC_TYPE) != 0)
#define BTREE_INSERT_RCV_IS_RECORD_OVERFLOW(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_REC_TYPE) == 0)
/* Set record type */
#define BTREE_INSERT_RCV_SET_RECORD_REGULAR(recins) \
  ((recins)->flags |= BTREE_INSERT_RCV_FLAG_REC_TYPE)
#define BTREE_INSERT_RCV_SET_RECORD_OVERFLOW(recins) \
  ((recins)->flags &= ~BTREE_INSERT_RCV_FLAG_REC_TYPE)

/* Check insert OID mode */
#define BTREE_INSERT_OID_MODE_AT_BEGINNING	0x0800
#define BTREE_INSERT_OID_MODE_REPLACE_FIRST	0x0400
#define BTREE_INSERT_OID_MODE_DEFAULT		0x0C00

#define BTREE_INSERT_RCV_IS_INSMODE_AT_BEGINNING(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_INSOID_MODE) \
   == BTREE_INSERT_OID_MODE_AT_BEGINNING)

#define BTREE_INSERT_RCV_IS_INSMODE_REPLACE_FIRST(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_INSOID_MODE) \
   == BTREE_INSERT_OID_MODE_REPLACE_FIRST)

#define BTREE_INSERT_RCV_IS_INSMODE_DEFAULT(recins) \
  (((recins)->flags & BTREE_INSERT_RCV_FLAG_INSOID_MODE) \
   == BTREE_INSERT_OID_MODE_DEFAULT)

/* Set insert OID mode */
#define BTREE_INSERT_RCV_SET_INSMODE_AT_BEGINNING(recins) \
  ((recins)->flags = \
   (((recins)->flags & (~BTREE_INSERT_RCV_FLAG_INSOID_MODE)) \
    | BTREE_INSERT_OID_MODE_AT_BEGINNING))

#define BTREE_INSERT_RCV_SET_INSMODE_REPLACE_FIRST(recins) \
  ((recins)->flags = \
   (((recins)->flags & (~BTREE_INSERT_RCV_FLAG_INSOID_MODE)) \
    | BTREE_INSERT_OID_MODE_REPLACE_FIRST))

#define BTREE_INSERT_RCV_SET_INSMODE_DEFAULT(recins) \
  ((recins)->flags = \
   (((recins)->flags & (~BTREE_INSERT_RCV_FLAG_INSOID_MODE)) \
    | BTREE_INSERT_OID_MODE_DEFAULT))

typedef struct recins_struct RECINS_STRUCT;
struct recins_struct
{				/* Recovery leaf record oid insertion structure */
  OID class_oid;		/* class oid only in case of unique index */
  OID oid;			/* oid to be inserted to the record    */
  VPID ovfl_vpid;		/* Next Overflow pageid  */
  INT16 flags;			/* Flags to describe different context of
				 * recovered insert object:
				 * - oid inserted
				 * - is overflow changed
				 * - is new overflow
				 * - record type (regular or overflow)
				 * - insert OID mode
				 */
};

/* Redo recovery of insert delete MVCCID */
#define BTREE_INSERT_DELID_RCV_FLAG_UNIQUE	0x80000000
#define BTREE_INSERT_DELID_RCV_FLAG_OVERFLOW	0x40000000
#define BTREE_INSERT_DELID_RCV_FLAG_KEY_DOMAIN	0x20000000

#define BTREE_INSERT_DELID_RCV_FLAG_MASK	0xE0000000

#define BTREE_INSERT_DELID_RCV_IS_UNIQUE(offset) \
  (((offset) & BTREE_INSERT_DELID_RCV_FLAG_UNIQUE) != 0)
#define BTREE_INSERT_DELID_RCV_IS_OVERFLOW(offset) \
  (((offset) & BTREE_INSERT_DELID_RCV_FLAG_OVERFLOW) != 0)
#define BTREE_INSERT_DELID_RCV_HAS_KEY_DOMAIN(offset) \
  (((offset) & BTREE_INSERT_DELID_RCV_FLAG_KEY_DOMAIN) != 0)

#define BTREE_INSERT_DELID_RCV_CLEAR_FLAGS(offset) \
  ((offset) = (offset) & (~BTREE_INSERT_DELID_RCV_FLAG_MASK))

#define BTID_DOMAIN_BUFFER_SIZE 64

/* Offset of the fields in the Leaf/NonLeaf Record Recovery Log Data */
#define OFFS1  0		/* Node Type Offset: Leaf/NonLeaf Information */
#define OFFS2  2		/* RECDES Type Offset */
#define OFFS3  4		/* RECDES Data Offset */

/* for Leaf Page Key Insertions */
#define LOFFS1  0		/* Key Len Offset */
#define LOFFS2  2		/* Node Type Offset: Leaf/NonLeaf Information */
#define LOFFS3  4		/* RECDES Type Offset */
#define LOFFS4  6		/* RECDES Data Offset */

/* B+tree statistical information environment */
typedef struct btree_stats_env BTREE_STATS_ENV;
struct btree_stats_env
{
  BTREE_SCAN btree_scan;	/* BTS */
  BTREE_STATS *stat_info;
  int pkeys_val_num;
  DB_VALUE pkeys_val[BTREE_STATS_PKEYS_NUM];	/* partial key-value */
};

/* Structure used by btree_range_search to initialize and handle variables
 * needed throughout the process.
 */
typedef struct btree_range_search_helper BTREE_RANGE_SEARCH_HELPER;
struct btree_range_search_helper
{
  OID *mem_oid_ptr;		/* Pointer to OID memory storage */
  int pg_oid_cnt;		/* The capacity of OID memory storage */
  int oids_cnt;			/* Current count of stored OID's */
  int oid_size;			/* Size of one OID */
  int cp_oid_cnt;		/* The OID count that can be stored in
				 * the current step
				 */
  int rec_oid_cnt;		/* The OID count in current record */
  char *rec_oid_ptr;		/* Pointer in record to current OID */
  bool swap_key_range;		/* Swaps key range if true */
  bool is_key_range_satisfied;	/* Does current key satisfy range */
  bool is_key_filter_satisfied;	/* Does current key satisfy filter */
  bool is_condition_satisfied;	/* Does current key satisfy range and
				 * filter
				 */
  RECDES rec;			/* Current record */
  LEAF_REC leaf_pnt;		/* Leaf record pointer OID overflows */
  int offset;			/* Offset in record to the first OID */
  OID class_oid;		/* Class identifier for current object */
  OID inst_oid;			/* Current object identifier */
  BTREE_NODE_TYPE node_type;	/* Current node type: leaf or overflow */
  bool iss_get_first_result_only;	/* Index skip scan special case */
  bool restart_on_first;	/* restart after first OID */
  int CLS_satisfied;		/* All conditions are satisfied */
  OID saved_class_oid;		/* Saved class identifier */
  OID saved_inst_oid;		/* Saved object identifier */
  char oid_space[2 * OR_OID_SIZE];	/* OID buffer to store "last" index key */
  DB_VALUE prev_key;		/* Previous key */
  bool clear_prev_key;		/* Previous key needs clear if true */
  LOG_LSA prev_leaf_lsa;	/* LSA of previous page */
  LOG_LSA ovfl_page_lsa;	/* LSA of overflow page */
  bool keep_on_copying;		/* True when OID storage exceeds it's
				 * default maximum size and need to
				 * stop current iteration of range
				 * search after this key
				 */
  OID ck_pseudo_oid;		/* Current key pseudo identifier */
  OID saved_ck_pseudo_oid;	/* Saved current key pseudo identifier */
  OID nk_pseudo_oid;		/* Next key pseudo identifier */
  OID saved_nk_pseudo_oid;	/* Saved next key pseudo identifier */
  OID saved_nk_class_oid;	/* Saved class oid for next key */

  bool end_of_leaf_level;	/* True if end of leaf level was reached */
  bool curr_key_locked;		/* Is current key locked */
  bool next_key_locked;		/* Is next key locked */
  bool current_lock_request;	/* Current key needs locking */
  bool read_prev_key;		/* Previous key is read */
};

typedef struct show_index_scan_ctx SHOW_INDEX_SCAN_CTX;
struct show_index_scan_ctx
{
  int indexes_count;		/* The total of indexes */
  bool is_all;			/* Get all indexes or get a specified index */
  char *index_name;		/* Index name which user specified  */
  OID *class_oids;		/* Class oids array */
  int class_oid_count;		/* The count of above oids array */
  int show_type;		/* Show type */
};

static int btree_store_overflow_key (THREAD_ENTRY * thread_p,
				     BTID_INT * btid, DB_VALUE * key,
				     int size, BTREE_NODE_TYPE node_type,
				     VPID * firstpg_vpid);
static int btree_load_overflow_key (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    VPID * firstpg_vpid, DB_VALUE * key,
				    BTREE_NODE_TYPE node_type);
static int btree_delete_overflow_key (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, PAGE_PTR page_ptr,
				      INT16 slot_id,
				      BTREE_NODE_TYPE node_type);
static void btree_write_fixed_portion_of_non_leaf_record (RECDES * rec,
							  NON_LEAF_REC *
							  nlf_rec);
static void btree_read_fixed_portion_of_non_leaf_record (RECDES * rec,
							 NON_LEAF_REC *
							 nlf_rec);
static void btree_write_fixed_portion_of_non_leaf_record_to_orbuf (OR_BUF *
								   buf,
								   NON_LEAF_REC
								   * nlf_rec);
static int btree_read_fixed_portion_of_non_leaf_record_from_orbuf (OR_BUF *
								   buf,
								   NON_LEAF_REC
								   * nlf_rec);
static void btree_append_oid (RECDES * rec, OID * oid);
static void btree_add_mvcc_delid (RECDES * rec, int oid_offset,
				  int mvcc_delid_offset,
				  MVCCID * p_mvcc_delid);
static void btree_delete_mvcc_delid (RECDES * rec, int oid_offset,
				     int mvcc_delid_offset);
static void btree_delete_mvcc_insid (RECDES * rec, int oid_offset,
				     int mvcc_insid_offset);
static void btree_delete_mvcc_object (RECDES * rec, int oid_offset,
				      int oid_size, bool has_fixed_size);
static void btree_update_mvcc_info (RECDES * recp, int offset,
				    int oid_size,
				    MVCC_REC_HEADER * p_mvcc_rec_header);
static void btree_append_mvcc_info (RECDES * rec,
				    MVCC_REC_HEADER * p_mvcc_rec_header,
				    int oid_size);
static void btree_insert_oid_in_front_of_ovfl_vpid (RECDES * rec, OID * oid,
						    OID * class_oid,
						    bool is_unique,
						    VPID * ovfl_vpid,
						    MVCC_REC_HEADER *
						    p_mvcc_rec_header);
static int btree_insert_oid_with_order (RECDES * rec, OID * oid,
					OID * class_oid, bool is_unique,
					MVCC_REC_HEADER * p_mvcc_rec_header);
static int btree_start_overflow_page (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, VPID * new_vpid,
				      PAGE_PTR * new_page_ptr,
				      VPID * near_vpid, OID * oid,
				      OID * class_oid, VPID * next_ovfl_vpid,
				      MVCC_REC_HEADER * p_mvcc_rec_header);
static void btree_read_record_helper (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, RECDES * Rec,
				      DB_VALUE * key, void *rec_header,
				      int node_type, bool * clear_key,
				      int *offset, int copy);
static PAGE_PTR btree_get_new_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    VPID * vpid, VPID * near_vpid);
static int btree_dealloc_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
			       VPID * vpid);
static bool btree_initialize_new_page (THREAD_ENTRY * thread_p,
				       const VFID * vfid,
				       const FILE_TYPE file_type,
				       const VPID * vpid, INT32 ignore_npages,
				       void *args);
static int btree_search_nonleaf_page (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, PAGE_PTR page_ptr,
				      DB_VALUE * key, INT16 * slot_id,
				      VPID * child_vpid);
static bool btree_search_leaf_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    PAGE_PTR page_ptr, DB_VALUE * key,
				    INT16 * slot_id);
static int xbtree_test_unique (THREAD_ENTRY * thread_p, BTID * btid);
#if defined(ENABLE_UNUSED_FUNCTION)
static int btree_get_subtree_stats (THREAD_ENTRY * thread_p,
				    BTID_INT * btid, PAGE_PTR pg_ptr,
				    BTREE_STATS_ENV * env);
#endif
static int btree_get_stats_midxkey (THREAD_ENTRY * thread_p,
				    BTREE_STATS_ENV * env,
				    DB_MIDXKEY * midxkey);
static int btree_get_stats_key (THREAD_ENTRY * thread_p,
				BTREE_STATS_ENV * env,
				MVCC_SNAPSHOT * mvcc_snapshot);
static int btree_get_stats_with_AR_sampling (THREAD_ENTRY * thread_p,
					     BTREE_STATS_ENV * env);
static int btree_get_stats_with_fullscan (THREAD_ENTRY * thread_p,
					  BTREE_STATS_ENV * env);
static DISK_ISVALID btree_check_page_key (THREAD_ENTRY * thread_p,
					  const OID * class_oid_p,
					  BTID_INT * btid, const char *btname,
					  PAGE_PTR page_ptr,
					  VPID * page_vpid);
static DISK_ISVALID btree_check_pages (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR pg_ptr,
				       VPID * pg_vpid);
static DISK_ISVALID btree_verify_subtree (THREAD_ENTRY * thread_p,
					  const OID * class_oid_p,
					  BTID_INT * btid, const char *btname,
					  PAGE_PTR pg_ptr, VPID * pg_vpid,
					  BTREE_NODE_INFO * INFO);
static int btree_get_subtree_capacity (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR pg_ptr,
				       BTREE_CAPACITY * cpc);
static void btree_print_space (FILE * fp, int n);
static int btree_delete_from_leaf (THREAD_ENTRY * thread_p,
				   bool * key_deleted, BTID_INT * btid,
				   VPID * leaf_vpid, DB_VALUE * key,
				   OID * class_oid, OID * oid,
				   INT16 leaf_slot_id,
				   MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_insert_into_leaf (THREAD_ENTRY * thread_p,
				   int *key_added_deleted, BTID_INT * btid,
				   PAGE_PTR page_ptr, DB_VALUE * key,
				   OID * cls_oid, OID * oid,
				   VPID * nearp_vpid, int op_type,
				   bool key_found, INT16 slot_id,
				   MVCC_REC_HEADER * p_mvcc_rec_header);
static int btree_delete_meta_record (THREAD_ENTRY * thread_p, BTID_INT * btid,
				     PAGE_PTR page_ptr, int slot_id);
static int btree_merge_root (THREAD_ENTRY * thread_p, BTID_INT * btid,
			     PAGE_PTR P, PAGE_PTR Q, PAGE_PTR R);
static int btree_merge_node (THREAD_ENTRY * thread_p, BTID_INT * btid,
			     PAGE_PTR P, PAGE_PTR Q, PAGE_PTR R,
			     INT16 p_slot_id, VPID * child_vpid,
			     BTREE_MERGE_STATUS status);
static int btree_node_size_uncompressed (THREAD_ENTRY * thread_p,
					 BTID_INT * btid, PAGE_PTR page_ptr);
static BTREE_MERGE_STATUS btree_node_mergeable (THREAD_ENTRY * thread_p,
						BTID_INT * btid, PAGE_PTR L,
						PAGE_PTR R);
static DB_VALUE *btree_find_split_point (THREAD_ENTRY * thread_p,
					 BTID_INT * btid, PAGE_PTR page_ptr,
					 int *mid_slot, DB_VALUE * key,
					 bool * clear_midkey);
static int btree_split_next_pivot (BTREE_NODE_SPLIT_INFO * split_info,
				   float new_value, int max_index);
static int btree_split_find_pivot (int total,
				   BTREE_NODE_SPLIT_INFO * split_info);
static int btree_split_node (THREAD_ENTRY * thread_p, BTID_INT * btid,
			     PAGE_PTR P, PAGE_PTR Q, PAGE_PTR R,
			     VPID * P_vpid, VPID * Q_vpid, VPID * R_vpid,
			     INT16 p_slot_id, BTREE_NODE_TYPE node_type,
			     DB_VALUE * key, VPID * child_vpid);
static int btree_split_root (THREAD_ENTRY * thread_p, BTID_INT * btid,
			     PAGE_PTR P, PAGE_PTR Q, PAGE_PTR R,
			     VPID * P_vpid, VPID * Q_vpid, VPID * R_vpid,
			     BTREE_NODE_TYPE node_type, DB_VALUE * key,
			     VPID * child_vpid);
static PAGE_PTR btree_locate_key (THREAD_ENTRY * thread_p,
				  BTID_INT * btid_int, DB_VALUE * key,
				  VPID * pg_vpid, INT16 * slot_id,
				  bool * found_p);
static int btree_find_lower_bound_leaf (THREAD_ENTRY * thread_p,
					BTREE_SCAN * BTS,
					BTREE_STATS * stat_info_p);
static PAGE_PTR btree_find_leftmost_leaf (THREAD_ENTRY * thread_p,
					  BTID * btid, VPID * pg_vpid,
					  BTREE_STATS * stat_info_p);
static PAGE_PTR btree_find_rightmost_leaf (THREAD_ENTRY * thread_p,
					   BTID * btid, VPID * pg_vpid,
					   BTREE_STATS * stat_info_p);
static PAGE_PTR btree_find_AR_sampling_leaf (THREAD_ENTRY * thread_p,
					     BTID * btid, VPID * pg_vpid,
					     BTREE_STATS * stat_info_p,
					     bool * found_p);
static PAGE_PTR btree_find_boundary_leaf (THREAD_ENTRY * thread_p,
					  BTID * btid, VPID * pg_vpid,
					  BTREE_STATS * stat_info,
					  BTREE_BOUNDARY where);
static int btree_initialize_bts (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
				 BTID * btid, int readonly_purpose,
				 OID * class_oid,
				 KEY_VAL_RANGE * key_val_range,
				 FILTER_INFO * filter,
				 bool need_construct_btid_int, char *copy_buf,
				 int copy_buf_len, bool for_update,
				 bool mvcc_need_locks);
static int btree_find_next_index_record (THREAD_ENTRY * thread_p,
					 BTREE_SCAN * bts);
static int btree_find_next_index_record_holding_current (THREAD_ENTRY *
							 thread_p,
							 BTREE_SCAN * bts,
							 RECDES * peek_rec);
static int btree_find_next_index_record_holding_current_helper (THREAD_ENTRY *
								thread_p,
								BTREE_SCAN *
								bts,
								PAGE_PTR
								first_page);
static int btree_get_next_oidset_pos (THREAD_ENTRY * thread_p,
				      BTREE_SCAN * bts,
				      VPID * first_ovfl_vpid);
static int btree_prepare_first_search (THREAD_ENTRY * thread_p,
				       BTREE_SCAN * bts);
static int btree_prepare_next_search (THREAD_ENTRY * thread_p,
				      BTREE_SCAN * bts);
static int btree_apply_key_range_and_filter (THREAD_ENTRY * thread_p,
					     BTREE_SCAN * bts, bool is_iss,
					     bool * key_range_satisfied,
					     bool * key_filter_satisfied,
					     bool need_to_check_null);
static int btree_dump_curr_key (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
				FILTER_INFO * filter, OID * oid,
				INDX_SCAN_ID * iscan_id);
#if 0				/* TODO: currently, unused */
static int btree_get_prev_keyvalue (BTREE_SCAN * bts,
				    DB_VALUE * prev_key, int *prev_clr_key);
#endif
static int btree_handle_prev_leaf_after_locking (THREAD_ENTRY * thread_p,
						 BTREE_SCAN * bts,
						 int oid_idx,
						 LOG_LSA * prev_leaf_lsa,
						 DB_VALUE * prev_key,
						 int *which_action);
static int btree_handle_curr_leaf_after_locking (THREAD_ENTRY * thread_p,
						 BTREE_SCAN * bts,
						 int oid_idx,
						 LOG_LSA * ovfl_page_lsa,
						 DB_VALUE * prev_key,
						 OID * prev_oid_ptr,
						 int *which_action);
static int btree_lock_current_key (THREAD_ENTRY * thread_p,
				   BTREE_SCAN * bts,
				   OID * prev_oid_locked_ptr,
				   OID * first_key_oid, OID * class_oid,
				   int scanid_bit, DB_VALUE * prev_key,
				   OID * ck_pseudo_oid, int *which_action);
static int btree_lock_next_key (THREAD_ENTRY * thread_p,
				BTREE_SCAN * bts,
				OID * prev_oid_locked_ptr,
				int scanid_bit,
				DB_VALUE * prev_key,
				OID * nk_pseudo_oid,
				OID * saved_nk_class_oid, int *which_action);
static void btree_make_pseudo_oid (int p, short s, short v, BTID * btid,
				   OID * oid);
static DISK_ISVALID btree_find_key_from_leaf (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      PAGE_PTR pg_ptr, int key_cnt,
					      OID * oid, DB_VALUE * key,
					      bool * clear_key);
static DISK_ISVALID btree_find_key_from_nleaf (THREAD_ENTRY * thread_p,
					       BTID_INT * btid,
					       PAGE_PTR pg_ptr, int key_cnt,
					       OID * oid, DB_VALUE * key,
					       bool * clear_key);
static DISK_ISVALID btree_find_key_from_page (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      PAGE_PTR pg_ptr, OID * oid,
					      DB_VALUE * key,
					      bool * clear_key);

/* Dump & verify routines */
static void btree_dump_root_header (FILE * fp, PAGE_PTR page_ptr);
static void btree_dump_leaf_record (THREAD_ENTRY * thread_p, FILE * fp,
				    BTID_INT * btid, RECDES * rec, int n);
static void btree_dump_non_leaf_record (THREAD_ENTRY * thread_p,
					FILE * fp, BTID_INT * btid,
					RECDES * rec, int n, int print_key);
static void btree_dump_page (THREAD_ENTRY * thread_p, FILE * fp,
			     const OID * class_oid_p, BTID_INT * btid,
			     const char *btname, PAGE_PTR page_ptr,
			     VPID * pg_vpid, int depth, int level);

static void btree_dump_page_with_subtree (THREAD_ENTRY * thread_p,
					  FILE * fp, BTID_INT * btid,
					  PAGE_PTR pg_ptr,
					  VPID * pg_vpid, int depth,
					  int level);

#if !defined(NDEBUG)
static DB_VALUE *btree_set_split_point (THREAD_ENTRY * thread_p,
					BTID_INT * btid, PAGE_PTR page_ptr,
					INT16 mid_slot, DB_VALUE * key,
					bool * clear_midkey);
static void btree_split_test (THREAD_ENTRY * thread_p, BTID_INT * btid,
			      DB_VALUE * key, VPID * S_vpid, PAGE_PTR S_page,
			      BTREE_NODE_TYPE node_type);
static int btree_verify_node (THREAD_ENTRY * thread_p,
			      BTID_INT * btid_int, PAGE_PTR page_ptr);
static int btree_verify_nonleaf_node (THREAD_ENTRY * thread_p,
				      BTID_INT * btid_int, PAGE_PTR page_ptr);
static int btree_verify_leaf_node (THREAD_ENTRY * thread_p,
				   BTID_INT * btid_int, PAGE_PTR page_ptr);
#endif

static void btree_set_unknown_key_error (THREAD_ENTRY * thread_p,
					 BTID * btid, DB_VALUE * key,
					 const char *debug_msg);
static int btree_rv_write_log_record_for_key_insert (char *log_rec,
						     int *log_length,
						     INT16 key_len,
						     RECDES * recp);

static int btree_rv_write_log_record (char *log_rec, int *log_length,
				      RECDES * recp,
				      BTREE_NODE_TYPE node_type);
static int btree_check_duplicate_oid (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, PAGE_PTR leaf_page,
				      INT16 slot_id, RECDES * leaf_rec_p,
				      int oid_list_offset, OID * oid,
				      VPID * ovfl_vpid);
static int btree_find_oid_from_leaf (BTID_INT * btid, RECDES * rec_p,
				     int oid_list_offset, OID * oid,
				     MVCC_BTREE_OP_ARGUMENTS * mvcc_args,
				     MVCC_REC_HEADER * mvcc_local_rec_header);
static int btree_find_oid_from_ovfl (RECDES * rec_p, OID * oid, int oid_size,
				     MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_leaf_get_vpid_for_overflow_oids (RECDES * rec, VPID * vpid);
#if defined(ENABLE_UNUSED_FUNCTION)
static int btree_leaf_put_first_oid (RECDES * recp, OID * oidp,
				     short record_flag);
#endif
static int btree_leaf_get_last_oid (BTID_INT * btid, RECDES * recp,
				    BTREE_NODE_TYPE node_type,
				    int mvcc_oid_list_offset,
				    OID * oidp, OID * class_oid,
				    MVCC_REC_HEADER * p_mvcc_header,
				    int *last_oid_mvcc_offset);
static int btree_leaf_remove_last_oid (BTID_INT * btid, RECDES * recp,
				       BTREE_NODE_TYPE node_type,
				       int oid_size,
				       int last_oid_mvcc_offset);
static char *btree_leaf_get_nth_oid_ptr (BTID_INT * btid, RECDES * recp,
					 BTREE_NODE_TYPE node_type,
					 int oid_list_offset, int n);
static void btree_leaf_set_flag (RECDES * recp, short record_flag);
static void btree_leaf_clear_flag (RECDES * recp, short record_flag);
static short btree_leaf_get_flag (RECDES * recp);
static bool btree_leaf_is_flaged (RECDES * recp, short record_flag);
static void btree_leaf_key_oid_set_mvcc_flag (char *data, short record_flag);
static void btree_leaf_key_oid_clear_mvcc_flag (char *rec_data,
						short record_flag);
static short btree_leaf_key_oid_get_mvcc_flag (char *data);
static short btree_leaf_key_oid_is_mvcc_flaged (char *data,
						short record_flag);
static void btree_leaf_rebuild_mvccids_in_record (RECDES * recp, int offset,
						  int mvcc_old_oid_mvcc_flags,
						  int oid_size,
						  MVCC_REC_HEADER *
						  p_mvcc_rec_header);
static void btree_leaf_set_fixed_mvcc_size_for_first_record_oid (RECDES *
								 recp,
								 bool
								 is_unique,
								 OID *
								 class_oid);
static void btree_leaf_get_oid_from_oidptr (BTREE_SCAN * bts,
					    char *rec_oid_ptr,
					    BTREE_NODE_TYPE node_type,
					    OID * oid, OID * class_oid,
					    MVCC_REC_HEADER * p_mvcc_header);
static char *btree_leaf_advance_oidptr (BTREE_SCAN * bts, char *rec_oid_ptr,
					int offset,
					BTREE_NODE_TYPE node_type);
static int btree_leaf_get_num_oids (RECDES * rec, int offset,
				    BTREE_NODE_TYPE node_type, int oid_size);
static int btree_leaf_mvcc_get_num_visible_oids (THREAD_ENTRY * thread_p,
						 BTID_INT * btid,
						 RECDES * rec, int oid_offset,
						 BTREE_NODE_TYPE node_type,
						 int *max_visible_oids,
						 MVCC_SNAPSHOT *
						 mvcc_snapshot);
static int btree_mvcc_get_num_visible_oids_from_all_ovf (THREAD_ENTRY *
							 thread_p,
							 BTID_INT * btid,
							 VPID *
							 first_ovfl_vpid,
							 int
							 *num_visible_oids,
							 int
							 *max_visible_oids,
							 MVCC_SNAPSHOT *
							 mvcc_snapshot);
static void btree_write_default_split_info (BTREE_NODE_SPLIT_INFO * info);
static int btree_set_vpid_previous_vpid (THREAD_ENTRY * thread_p,
					 BTID_INT * btid, PAGE_PTR page_p,
					 VPID * prev);
static PAGE_PTR btree_get_next_page (THREAD_ENTRY * thread_p,
				     PAGE_PTR page_p);
static int btree_range_opt_check_add_index_key (THREAD_ENTRY * thread_p,
						BTREE_SCAN * bts,
						MULTI_RANGE_OPT *
						multi_range_opt,
						OID * p_new_oid,
						OID * ck_pseudo_oid,
						OID * nk_pseudo_oid,
						OID * class_oid,
						bool * key_added);
static int btree_top_n_items_binary_search (RANGE_OPT_ITEM ** top_n_items,
					    int *att_idxs,
					    TP_DOMAIN ** domains,
					    bool * desc_order,
					    DB_VALUE * new_key_values,
					    int no_keys, int first, int last,
					    int *new_pos);
static int btree_iss_set_key (BTREE_SCAN * bts, INDEX_SKIP_SCAN * iss);
static int btree_insert_lock_curr_key_remaining_pseudo_oid (THREAD_ENTRY *
							    thread_p,
							    BTID_INT *
							    btid_int,
							    RECDES * rec,
							    int offset,
							    VPID *
							    first_ovfl_vpid,
							    OID * oid,
							    OID * class_oid);
static int btree_insert_unlock_curr_key_remaining_pseudo_oid (THREAD_ENTRY *
							      thread_p,
							      BTID_INT *
							      btid_int,
							      RECDES * rec,
							      int offset,
							      VPID *
							      first_ovfl_vpid,
							      OID *
							      class_oid);
static int btree_delete_lock_curr_key_next_pseudo_oid (THREAD_ENTRY *
						       thread_p,
						       BTID_INT * btid_int,
						       RECDES * rec,
						       int offset,
						       VPID * P_vpid,
						       PAGE_PTR * P_page,
						       VPID * first_ovfl_vpid,
						       OID * class_oid,
						       bool *
						       search_without_locking);
static bool btree_is_new_file (BTID_INT * btid_int);
static int btree_insert_init_locks (THREAD_ENTRY * thread_p, bool is_active,
				    OID * class_oid, BTID_INT * btid_int,
				    LOCK * class_lock,
				    int *next_key_lock_request,
				    int *mvcc_lock_request);
static int btree_insert_oid_with_new_key (THREAD_ENTRY * thread_p,
					  BTID_INT * btid, PAGE_PTR leaf_page,
					  DB_VALUE * key, OID * cls_oid,
					  OID * oid, INT16 slot_id,
					  MVCC_REC_HEADER *
					  p_mvcc_rec_header);
static int btree_insert_oid_into_leaf_rec (THREAD_ENTRY * thread_p,
					   BTID_INT * btid,
					   PAGE_PTR leaf_page, DB_VALUE * key,
					   OID * cls_oid, OID * oid,
					   INT16 slot_id, RECDES * rec,
					   VPID * first_ovfl_vpid,
					   MVCC_REC_HEADER *
					   p_mvcc_rec_header,
					   INT16 insoid_mode);
static int btree_insert_mvcc_delid_into_page (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      PAGE_PTR page_ptr,
					      BTREE_NODE_TYPE node_type,
					      DB_VALUE * key, OID * cls_oid,
					      OID * oid, INT16 slot_id,
					      RECDES * rec, int oid_offset,
					      MVCC_REC_HEADER *
					      p_mvcc_rec_header);
static int btree_delete_mvcc_delid_from_page (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      PAGE_PTR page_ptr,
					      BTREE_NODE_TYPE node_type,
					      INT16 slot_id, DB_VALUE * key,
					      OID * oid, OID * class_oid,
					      RECDES * rec,
					      int del_oid_offset,
					      MVCC_BTREE_OP_ARGUMENTS *
					      mvcc_args);
static int btree_delete_mvcc_insid_from_page (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      PAGE_PTR page_ptr,
					      BTREE_NODE_TYPE node_type,
					      INT16 slot_id, DB_VALUE * key,
					      OID * oid, OID * class_oid,
					      RECDES * rec, int oid_offset,
					      MVCC_BTREE_OP_ARGUMENTS *
					      mvcc_args);
static int btree_append_overflow_oids_page (THREAD_ENTRY * thread_p,
					    BTID_INT * btid,
					    PAGE_PTR leaf_page,
					    DB_VALUE * key, OID * cls_oid,
					    OID * oid, INT16 slot_id,
					    RECDES * leaf_rec,
					    VPID * near_vpid,
					    VPID * first_ovfl_vpid,
					    MVCC_REC_HEADER *
					    p_mvcc_rec_header,
					    bool skip_overflow_undo);
static int btree_insert_oid_overflow_page (THREAD_ENTRY * thread_p,
					   BTID_INT * btid,
					   PAGE_PTR ovfl_page, DB_VALUE * key,
					   OID * cls_oid, OID * oid,
					   MVCC_REC_HEADER *
					   p_mvcc_rec_header,
					   bool skip_overflow_undo);
static PAGE_PTR btree_find_free_overflow_oids_page (THREAD_ENTRY * thread_p,
						    BTID_INT * btid,
						    VPID * ovfl_vpid);

static int btree_delete_key_from_leaf (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR leaf_pg,
				       INT16 slot_id, DB_VALUE * key,
				       OID * oid, OID * class_oid,
				       RECDES * leaf_rec,
				       LEAF_REC * leafrec_pnt,
				       MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_swap_first_oid_with_ovfl_rec (THREAD_ENTRY * thread_p,
					       BTID_INT * btid,
					       PAGE_PTR leaf_page,
					       INT16 slot_id, DB_VALUE * key,
					       OID * oid, OID * class_oid,
					       RECDES * leaf_rec,
					       VPID * ovfl_vpid,
					       MVCC_BTREE_OP_ARGUMENTS *
					       mvcc_args);
static int btree_delete_oid_from_leaf (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR leaf_page,
				       INT16 slot_id, DB_VALUE * key,
				       OID * oid, OID * class_oid,
				       RECDES * leaf_rec, int del_oid_offset,
				       int *oid_list_offset,
				       MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_modify_leaf_ovfl_vpid (THREAD_ENTRY * thread_p,
					BTID_INT * btid, PAGE_PTR leaf_page,
					INT16 slot_id, DB_VALUE * key,
					OID * oid, OID * class_oid,
					RECDES * leaf_rec,
					VPID * next_ovfl_vpid,
					MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_modify_overflow_link (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR ovfl_page,
				       DB_VALUE * key, OID * oid,
				       OID * class_oid, RECDES * ovfl_rec,
				       VPID * next_ovfl_vpid,
				       MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_delete_oid_from_ovfl (THREAD_ENTRY * thread_p,
				       BTID_INT * btid, PAGE_PTR ovfl_page,
				       DB_VALUE * key, OID * oid,
				       OID * class_oid, RECDES * ovfl_rec,
				       int del_oid_offset,
				       MVCC_BTREE_OP_ARGUMENTS * mvcc_args);
static int btree_leaf_update_overflow_oids_vpid (RECDES * rec,
						 VPID * ovfl_vpid);

static void btree_range_search_init_helper (THREAD_ENTRY * thread_p,
					    BTREE_RANGE_SEARCH_HELPER *
					    btrs_helper,
					    BTREE_SCAN * bts,
					    INDX_SCAN_ID * index_scan_id_p,
					    int oids_size, OID * oids_ptr,
					    int ils_prefix_len);
static int btree_prepare_range_search (THREAD_ENTRY * thread_p,
				       BTREE_SCAN * bts);
static int btree_get_oid_count_and_pointer (THREAD_ENTRY * thread_p,
					    BTREE_SCAN * bts,
					    BTREE_RANGE_SEARCH_HELPER *
					    btrs_helper,
					    DB_BIGINT * key_limit_upper,
					    INDX_SCAN_ID * index_scan_id_p,
					    bool need_to_check_null,
					    int *which_action);
static int btree_handle_current_oid (THREAD_ENTRY * thread_p,
				     BTREE_SCAN * bts,
				     BTREE_RANGE_SEARCH_HELPER * btrs_helper,
				     DB_BIGINT * key_limit_lower,
				     DB_BIGINT * key_limit_upper,
				     INDX_SCAN_ID * index_scan_id_p,
				     bool need_count_only, OID * inst_oid,
				     MVCC_REC_HEADER * mvcc_rec_header,
				     int *which_action);
static DISK_ISVALID btree_repair_prev_link_by_btid (THREAD_ENTRY * thread_p,
						    BTID * btid, bool repair,
						    char *index_name);
static DISK_ISVALID btree_repair_prev_link_by_class_oid (THREAD_ENTRY *
							 thread_p, OID * oid,
							 BTID * idx_btid,
							 bool repair);
static bool btree_node_is_compressed (THREAD_ENTRY * thread_p,
				      BTID_INT * btid, PAGE_PTR page_ptr);
static int btree_node_common_prefix (THREAD_ENTRY * thread_p, BTID_INT * btid,
				     PAGE_PTR page_ptr);
static int btree_compress_records (THREAD_ENTRY * thread_p, BTID_INT * btid,
				   RECDES * rec, int key_cnt);
static int btree_compress_node (THREAD_ENTRY * thread_p, BTID_INT * btid,
				PAGE_PTR page_ptr);
static const char *node_type_to_string (short node_type);
static SCAN_CODE btree_scan_for_show_index_header (THREAD_ENTRY * thread_p,
						   DB_VALUE ** out_values,
						   int out_cnt,
						   const char *class_name,
						   const char *index_name,
						   BTID * btid_p);
static SCAN_CODE btree_scan_for_show_index_capacity (THREAD_ENTRY * thread_p,
						     DB_VALUE ** out_values,
						     int out_cnt,
						     const char *class_name,
						     const char *index_name,
						     BTID * btid_p);
static bool btree_leaf_lsa_eq (THREAD_ENTRY * thread_p, LOG_LSA * a,
			       LOG_LSA * b);

#if !defined(NDEBUG)
static int btree_get_node_level (PAGE_PTR page_ptr);
static void random_exit (THREAD_ENTRY * thread_p);

#define RANDOM_EXIT(a)        random_exit(a)
#else
#define RANDOM_EXIT(a)
#endif

static int btree_range_search_handle_previous_locks (THREAD_ENTRY * thread_p,
						     BTREE_SCAN * bts,
						     BTREE_RANGE_SEARCH_HELPER
						     * btrs_helper,
						     DB_BIGINT *
						     key_limit_lower,
						     DB_BIGINT *
						     key_limit_upper,
						     INDX_SCAN_ID *
						     index_scan_id_p,
						     bool need_count_only,
						     int *which_action);
static int btree_handle_current_oid_and_locks (THREAD_ENTRY * thread_p,
					       BTREE_SCAN * bts,
					       BTREE_RANGE_SEARCH_HELPER *
					       btrs_helper, BTID * btid,
					       DB_BIGINT * key_limit_lower,
					       DB_BIGINT * key_limit_upper,
					       INDX_SCAN_ID * index_scan_id_p,
					       bool need_count_only,
					       int num_classes,
					       OID * class_oids_ptr,
					       int scan_op_type,
					       int oid_index,
					       int *which_action);

static BTREE_SEARCH
btree_key_find_first_visible_row (THREAD_ENTRY * thread_p,
				  BTID_INT * btid_int, RECDES * rec,
				  int offset, BTREE_NODE_TYPE node_type,
				  OID * oid, OID * class_oid, int max_oids);
static BTREE_SEARCH
btree_key_find_first_visible_row_from_all_ovf (THREAD_ENTRY * thread_p,
					       BTID_INT * btid_int,
					       VPID * first_ovfl_vpid,
					       OID * oid, OID * class_oid);
static DB_VALUE *btree_insert (THREAD_ENTRY * thread_p, BTID * btid,
			       DB_VALUE * key, OID * cls_oid, OID * oid,
			       int op_type,
			       BTREE_UNIQUE_STATS * unique_stat_info,
			       int *pkyn,
			       MVCC_REC_HEADER * p_mvcc_rec_header);

			      /* ejin: for replication,
			       *    Replication log is created only when
			       *    the target index is a primary key..
			       *    btree_insert knows that the index is pk
			       *    or not, but it doesn't return the result.
			       *    So, we add the return value pkyn in order
			       *    to decide to make a repl. log or not.
			       */

static BTREE_SEARCH xbtree_non_mvcc_find_unique (THREAD_ENTRY * thread_p,
						 BTID * btid,
						 SCAN_OPERATION_TYPE
						 scan_op_type,
						 DB_VALUE * key,
						 OID * class_oid, OID * oid,
						 bool is_all_class_srch);
static BTREE_SEARCH xbtree_mvcc_find_unique (THREAD_ENTRY * thread_p,
					     BTID * btid,
					     SCAN_OPERATION_TYPE scan_op_type,
					     DB_VALUE * key, OID * class_oid,
					     OID * oid,
					     bool is_all_class_srch);

static int btree_vacuum_key (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
			     int oid_size, OID * remove_oids,
			     int n_remove_oids, bool * removed_key);
static int btree_vacuum_overflow_pages (THREAD_ENTRY * thread_p,
					BTREE_SCAN * bts, PAGE_PTR leaf_page,
					int oid_size, OID * remove_oids,
					int n_remove_oids,
					VPID * first_ovf_vpid);

static int btree_root_merge_and_advance (THREAD_ENTRY * thread_p,
					 BTID_INT * btid_int,
					 DB_VALUE * key,
					 PAGE_PTR * p_page_ptr, VPID * p_vpid,
					 BTREE_NODE_TYPE * node_type);
static int btree_node_merge_and_advance (THREAD_ENTRY * thread_p,
					 BTID_INT * btid_int, DB_VALUE * key,
					 PAGE_PTR * p_page_ptr, VPID * p_vpid,
					 BTREE_NODE_TYPE * node_type);
static int btree_merge_with_key (THREAD_ENTRY * thread_p, BTID_INT * btid_int,
				 DB_VALUE * key);
static int btree_find_overflow_page (THREAD_ENTRY * thread_p,
				     OID * oid, VPID * first_ovfl_vpid,
				     PAGE_PTR * ovfl_page,
				     RECDES * ovfl_copy_rec,
				     int *ovfl_rec_offset, int oid_size);

static int btree_rv_keyval_undo_insert_internal (THREAD_ENTRY * thread_p,
						 LOG_RCV * recv,
						 bool is_mvcc_operation);

static int btree_or_put_mvccinfo (OR_BUF * buf,
				  MVCC_REC_HEADER * p_mvcc_header);
static int btree_or_get_mvccinfo (OR_BUF * buf,
				  MVCC_REC_HEADER * p_mvcc_header,
				  short btree_mvcc_flags);


#if !defined(NDEBUG)
/*
 * btree_get_node_level () -
 *
 *   return:
 *   page_ptr(in):
 *
 */
static int
btree_get_node_level (PAGE_PTR page_ptr)
{
  BTREE_NODE_HEADER *header = NULL;

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      return -1;
    }

  assert (header->node_level > 0);

  return header->node_level;
}

static void
random_exit (THREAD_ENTRY * thread_p)
{
  static bool init = false;
  int r;
#define MOD_FACTOR	20000

  if (prm_get_bool_value (PRM_ID_QA_BTREE_RANDOM_EXIT) == false)
    {
      return;
    }

  if (init == false)
    {
      srand (time (NULL));
      init = true;
    }

  r = rand ();

  if ((r % 10) == 0)
    {
      LOG_CS_ENTER (thread_p);
      logpb_flush_pages_direct (thread_p);
      LOG_CS_EXIT (thread_p);
    }

  if ((r % MOD_FACTOR) == 0)
    {
      _exit (0);
    }
}
#endif


/*
 * btree_clear_key_value () -
 *   return: cleared flag
 *   clear_flag (in/out):
 *   key_value (in/out):
 */
bool
btree_clear_key_value (bool * clear_flag, DB_VALUE * key_value)
{
  if (*clear_flag == true)
    {
      pr_clear_value (key_value);
      *clear_flag = false;
    }

  return *clear_flag;
}

/*
 * btree_create_overflow_key_file () -
 *   return: NO_ERROR
 *   btid(in):
 *
 * Note: An overflow key file is created (permanently) and the VFID
 * is written to the root header for the btree.
 */
int
btree_create_overflow_key_file (THREAD_ENTRY * thread_p, BTID_INT * btid)
{
  FILE_OVF_BTREE_DES btdes_ovf;

  /*
   * Create the overflow file. Try to create the overflow file in the
   * same volume where the btree was defined
   */

  btid->ovfid.volid = btid->sys_btid->vfid.volid;

  /* Initialize description of overflow heap file */
  btdes_ovf.btid = *btid->sys_btid;	/* structure copy */

  if (file_create (thread_p, &btid->ovfid, 3, FILE_BTREE_OVERFLOW_KEY,
		   &btdes_ovf, NULL, 0) == NULL)
    {
      VFID_SET_NULL (&btid->ovfid);
      return ER_FAILED;
    }

  return NO_ERROR;
}

/*
 * btree_store_overflow_key () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   key(in): Pointer to the overflow key memory area
 *   size(in): Overflow key memory area size
 *   node_type(in): Type of node
 *   first_overflow_page_vpid(out): Set to the first overflow key page identifier
 *
 * Note: The overflow key given is stored in a chain of pages.
 */
static int
btree_store_overflow_key (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  DB_VALUE * key, int size, BTREE_NODE_TYPE node_type,
			  VPID * first_overflow_page_vpid)
{
  RECDES rec;
  OR_BUF buf;
  VFID overflow_file_vfid;
  int ret = NO_ERROR;
  TP_DOMAIN *tp_domain;
  PR_TYPE *pr_type;
  DB_TYPE src_type, dst_type;
  DB_VALUE new_key;
  DB_VALUE *key_ptr = key;

  assert (!VFID_ISNULL (&btid->ovfid));

  if (node_type == BTREE_LEAF_NODE)
    {
      tp_domain = btid->key_type;
    }
  else
    {
      tp_domain = btid->nonleaf_key_type;
    }

  pr_type = tp_domain->type;

  src_type = DB_VALUE_DOMAIN_TYPE (key);
  dst_type = pr_type->id;

  if (src_type != dst_type)
    {
      TP_DOMAIN_STATUS status;

      assert (pr_is_string_type (src_type));
      assert (pr_is_string_type (dst_type));

      key_ptr = &new_key;
      status = tp_value_cast (key, key_ptr, tp_domain, false);
      if (status != DOMAIN_COMPATIBLE)
	{
	  assert (false);
	  goto exit_on_error;
	}

      size = btree_get_key_length (key_ptr);
    }

  overflow_file_vfid = btid->ovfid;	/* structure copy */

  rec.area_size = size;
  rec.data = (char *) db_private_alloc (thread_p, size);
  if (rec.data == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, size);
      goto exit_on_error;
    }

  or_init (&buf, rec.data, rec.area_size);

  if ((*(pr_type->index_writeval)) (&buf, key_ptr) != NO_ERROR)
    {
      goto exit_on_error;
    }

  rec.length = (int) (buf.ptr - buf.buffer);

  /* don't need undo log because undo log of btree insert/delete is logical log */
  if (overflow_insert_without_undo_logging (thread_p, &overflow_file_vfid,
					    first_overflow_page_vpid,
					    &rec, NULL) == NULL)
    {
      goto exit_on_error;
    }

  if (rec.data)
    {
      db_private_free_and_init (thread_p, rec.data);
    }

  if (key_ptr != key)
    {
      db_value_clear (key_ptr);
    }

  return ret;

exit_on_error:

  if (rec.data)
    {
      db_private_free_and_init (thread_p, rec.data);
    }

  if (key_ptr != key)
    {
      db_value_clear (key_ptr);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_load_overflow_key () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   first_overflow_page_vpid(in): Overflow key first page identifier
 *   key(out): Set to the overflow key memory area
 *
 * Note: The overflow key is loaded from the pages.
 */
static int
btree_load_overflow_key (THREAD_ENTRY * thread_p, BTID_INT * btid,
			 VPID * first_overflow_page_vpid, DB_VALUE * key,
			 BTREE_NODE_TYPE node_type)
{
  RECDES rec;
  OR_BUF buf;
  PR_TYPE *pr_type;
  int ret = NO_ERROR;

  if (node_type == BTREE_LEAF_NODE)
    {
      pr_type = btid->key_type->type;
    }
  else
    {
      pr_type = btid->nonleaf_key_type->type;
    }

  rec.area_size = overflow_get_length (thread_p, first_overflow_page_vpid);
  if (rec.area_size == -1)
    {
      return ER_FAILED;
    }

  rec.data = (char *) db_private_alloc (thread_p, rec.area_size);
  if (rec.data == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, rec.area_size);
      goto exit_on_error;
    }

  if (overflow_get (thread_p, first_overflow_page_vpid, &rec, NULL) !=
      S_SUCCESS)
    {
      goto exit_on_error;
    }

  or_init (&buf, rec.data, rec.length);

  /* we always copy overflow keys */
  if ((*(pr_type->index_readval)) (&buf, key, btid->key_type, -1, true,
				   NULL, 0) != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (rec.data)
    {
      db_private_free_and_init (thread_p, rec.data);
    }

  return NO_ERROR;

exit_on_error:

  if (rec.data)
    {
      db_private_free_and_init (thread_p, rec.data);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_delete_overflow_key () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   page_ptr(in): Page that contains the overflow key
 *   slot_id(in): Slot that contains the overflow key
 *   node_type(in): Leaf or NonLeaf page
 *
 * Note: The overflow key is deleted. This routine will not delete the
 * btree slot containing the key.
 */
static int
btree_delete_overflow_key (THREAD_ENTRY * thread_p, BTID_INT * btid,
			   PAGE_PTR page_ptr, INT16 slot_id,
			   BTREE_NODE_TYPE node_type)
{
  RECDES rec;
  VPID page_vpid;
  char *start_ptr;
  OR_BUF buf;
  int rc = NO_ERROR;

  assert (slot_id > 0);

  rec.area_size = -1;

  /* first read the record to get first page identifier */
  if (spage_get_record (page_ptr, slot_id, &rec, PEEK) != S_SUCCESS)
    {
      goto exit_on_error;
    }

  /* get first page identifier */
  if (node_type == BTREE_LEAF_NODE)
    {
      int mvccids_size = 0;
      assert (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));

      if (btree_leaf_key_oid_is_mvcc_flaged (rec.data,
					     BTREE_LEAF_OID_HAS_MVCC_INSID))
	{
	  mvccids_size += OR_MVCCID_SIZE;
	}

      if (btree_leaf_key_oid_is_mvcc_flaged (rec.data,
					     BTREE_LEAF_OID_HAS_MVCC_DELID))
	{
	  mvccids_size += OR_MVCCID_SIZE;
	}

      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  start_ptr = rec.data + (2 * OR_OID_SIZE) + mvccids_size;
	}
      else
	{
	  start_ptr = rec.data + OR_OID_SIZE + mvccids_size;
	}
    }
  else
    {
      start_ptr = rec.data + NON_LEAF_RECORD_SIZE;
    }

  or_init (&buf, start_ptr, DISK_VPID_SIZE);

  page_vpid.pageid = or_get_int (&buf, &rc);
  if (rc == NO_ERROR)
    {
      page_vpid.volid = or_get_short (&buf, &rc);
    }
  if (rc != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (overflow_delete (thread_p, &(btid->ovfid), &page_vpid) == NULL)
    {
      goto exit_on_error;
    }

  return NO_ERROR;

exit_on_error:

  return (rc == NO_ERROR && (rc = er_errid ()) == NO_ERROR) ? ER_FAILED : rc;
}

/*
 * Common utility routines
 */



/*
 * btree_leaf_get_vpid_for_overflow_oids () -
 *   return: error code or NO_ERROR
 *   rec(in):
 *   ovfl_vpid(out):
 */
static int
btree_leaf_get_vpid_for_overflow_oids (RECDES * rec, VPID * ovfl_vpid)
{
  OR_BUF buf;
  int rc = NO_ERROR;

  assert (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS));

  or_init (&buf, rec->data + rec->length - OR_OID_SIZE, DISK_VPID_SIZE);

  ovfl_vpid->pageid = or_get_int (&buf, &rc);
  if (rc == NO_ERROR)
    {
      ovfl_vpid->volid = or_get_short (&buf, &rc);
    }

  return rc;
}

/*
 * btree_leaf_update_overflow_oids_vpid () -
 *   return: error code or NO_ERROR
 *   rec(in/out):
 *   ovfl_vpid(in):
 */
static int
btree_leaf_update_overflow_oids_vpid (RECDES * rec, VPID * ovfl_vpid)
{
  OR_BUF buf;
  int rc = NO_ERROR;

  assert (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS));

  or_init (&buf, rec->data + rec->length - OR_OID_SIZE, DISK_VPID_SIZE);

  rc = or_put_int (&buf, ovfl_vpid->pageid);
  assert (rc == NO_ERROR);

  rc = or_put_short (&buf, ovfl_vpid->volid);
  assert (rc == NO_ERROR);

  return rc;
}

/*
 * btree_leaf_append_vpid_for_overflow_oids () -
 *   return: error code or NO_ERROR
 *   rec(in/out):
 *   ovfl_vpid(in):
 *   bool is_unique(in): true id is unique index
 *   class_oid(in): class oid used in case of unique index
 */
int
btree_leaf_new_overflow_oids_vpid (RECDES * rec, VPID * ovfl_vpid,
				   bool is_unique, OID * class_oid)
{
  OR_BUF buf;

  int rc = NO_ERROR;

  or_init (&buf, rec->data + rec->length, OR_OID_SIZE);

  rc = or_put_int (&buf, ovfl_vpid->pageid);
  assert (rc == NO_ERROR);

  rc = or_put_short (&buf, ovfl_vpid->volid);
  assert (rc == NO_ERROR);

  rc = or_put_align32 (&buf);
  assert (rc == NO_ERROR);

  assert (CAST_BUFLEN (buf.ptr - buf.buffer) == OR_OID_SIZE);

  rec->length += CAST_BUFLEN (buf.ptr - buf.buffer);

  if (mvcc_Enabled
      && !btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      /* If MVCC is enabled, we must allocate the maximum required space for
       * the first entry in order to easily swap objects.
       * Do this only the flag is set for the first time.
       */
      btree_leaf_set_fixed_mvcc_size_for_first_record_oid (rec, is_unique,
							   class_oid);
      assert (!is_unique
	      || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS));
    }

  btree_leaf_set_flag (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS);

  return rc;
}

/*
 * btree_leaf_get_first_oid () -
 *   return: NO_ERROR
 *   btid(in):
 *   recp(in):
 *   oidp(out):
 *   class_oid(out):
 */
int
btree_leaf_get_first_oid (BTID_INT * btid, RECDES * recp, OID * oidp,
			  OID * class_oid, MVCC_REC_HEADER * p_mvcc_header)
{
  short mvcc_flags = 0;
  char *ptr = NULL;
  MVCCID mvccid = MVCCID_NULL;

  assert (btid != NULL);

  if (p_mvcc_header != NULL)
    {
      /* Must read MVCC info too */
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (recp->data);
    }

  /* instance OID */
  BTREE_GET_OID (recp->data, oidp);
  oidp->slotid = oidp->slotid & (~BTREE_LEAF_RECORD_MASK);
  ptr = recp->data + OR_OID_SIZE;

  /* class OID */
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      if (btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  BTREE_GET_CLASS_OID (ptr, class_oid);
	  ptr += OR_OID_SIZE;
	}
      else
	{
	  assert (!OID_ISNULL (&btid->topclass_oid));
	  COPY_OID (class_oid, &btid->topclass_oid);
	}
    }
  else
    {
      OID_SET_NULL (class_oid);
    }

  if (p_mvcc_header != NULL)
    {
      BTREE_INIT_MVCC_HEADER (p_mvcc_header);
      if (mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID)
	{
	  OR_GET_MVCCID (ptr, &mvccid);
	  ptr += OR_MVCCID_SIZE;
	  MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID);
	  MVCC_SET_INSID (p_mvcc_header, mvccid);
	}
      if (mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID)
	{
	  OR_GET_MVCCID (ptr, &mvccid);
	  ptr += OR_MVCCID_SIZE;
	  MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID);
	  MVCC_SET_DELID (p_mvcc_header, mvccid);
	}
    }

  return NO_ERROR;
}

/*
 * btree_leaf_mvcc_get_num_visible_oids () - get number of visible OIDS from
 *					    all overflow pages
 *   return: error code
 *   thread_p(in): thread entry
 *   btid(in): B+tree index identifier
 *   first_ovfl_vpid(in): first overflow vpid of leaf record
 *   num_visible_oids(in/out): number of visible OIDs from all overflow pages
 *   max_visible_oids(in): max visible oids to search for
 *   mvcc_snapshot(in): MVCC snapshot
 */
static int
btree_mvcc_get_num_visible_oids_from_all_ovf (THREAD_ENTRY * thread_p,
					      BTID_INT * btid,
					      VPID * first_ovfl_vpid,
					      int *num_visible_oids,
					      int *max_visible_oids,
					      MVCC_SNAPSHOT * mvcc_snapshot)
{
  RECDES ovfl_copy_rec;
  char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  VPID next_ovfl_vpid;
  PAGE_PTR ovfl_page = NULL;
  int ret;
  int num_node_visible_oids = 0;
  int max_page_visible_oids = 0, *p_max_page_visible_oids = NULL;

  if (max_visible_oids != NULL)
    {
      max_page_visible_oids = *max_visible_oids;
      p_max_page_visible_oids = &max_page_visible_oids;
    }
  /* not found in leaf page - search in overflow page */
  ovfl_page = NULL;

  ovfl_copy_rec.area_size = DB_PAGESIZE;
  ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);

  assert (mvcc_Enabled == true && btid != NULL && num_visible_oids != NULL);

  *num_visible_oids = 0;
  ovfl_page = NULL;
  next_ovfl_vpid = *first_ovfl_vpid;
  /* search for OID into overflow page */
  while (!VPID_ISNULL (&next_ovfl_vpid))
    {
      ovfl_page = pgbuf_fix (thread_p, &next_ovfl_vpid, OLD_PAGE,
			     PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (ovfl_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

      if (spage_get_record (ovfl_page, 1, &ovfl_copy_rec, COPY) != S_SUCCESS)
	{
	  goto error;
	}
      assert (ovfl_copy_rec.length % 4 == 0);

      num_node_visible_oids =
	btree_leaf_mvcc_get_num_visible_oids (thread_p, btid, &ovfl_copy_rec,
					      0, BTREE_OVERFLOW_NODE,
					      p_max_page_visible_oids,
					      mvcc_snapshot);
      if (num_node_visible_oids < 0)
	{
	  goto error;
	}
      (*num_visible_oids) += num_node_visible_oids;

      if (max_visible_oids)
	{
	  if ((*num_visible_oids) >= (*max_visible_oids))
	    {
	      pgbuf_unfix_and_init (thread_p, ovfl_page);
	      return NO_ERROR;
	    }

	  /* update remaining visible oids to search for */
	  max_page_visible_oids -= num_node_visible_oids;
	}

      btree_get_next_overflow_vpid (ovfl_page, &next_ovfl_vpid);
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return NO_ERROR;

error:

  if (ovfl_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  ret = er_errid ();
  if (ret == NO_ERROR)
    {
      ret = ER_FAILED;
    }

  return ret;
}


/*
 * btree_leaf_mvcc_get_num_visible_oids () - get number of visible OIDS
 *   return: number of visible OIDs
 *   thread_p(in): thread entry
 *   btid(in): B+tree index identifier
 *   rec(in): record descriptor
 *   oid_offset(in):  OID offset
 *   node_type(in): node type
 *   max_visible_oids(in): max visible oids to search for
 *   mvcc_snapshot(in): MVCC snapshot
 */
static int
btree_leaf_mvcc_get_num_visible_oids (THREAD_ENTRY * thread_p,
				      BTID_INT * btid,
				      RECDES * rec, int oid_offset,
				      BTREE_NODE_TYPE node_type,
				      int *max_visible_oids,
				      MVCC_SNAPSHOT * mvcc_snapshot)
{
  int mvcc_flags = 0, rec_oid_cnt = 0, length = 0;
  bool have_mvcc_fixed_size = false;
  MVCC_REC_HEADER mvcc_rec_header;
  OR_BUF buf;
  bool is_first = true;

  assert (mvcc_Enabled == true && btid != NULL && rec != NULL &&
	  oid_offset >= 0 && node_type != BTREE_NON_LEAF_NODE);

  if (mvcc_snapshot == NULL)
    {
      return -1;
    }
  length = rec->length;
  if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      length -= DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
    }

  or_init (&buf, rec->data, length);
  while (buf.ptr < buf.endptr)
    {
      /* Get MVCC flags */
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);

      /* Skip object OID */
      if (or_advance (&buf, OR_OID_SIZE) != NO_ERROR)
	{
	  return ER_FAILED;
	}

      if (BTREE_IS_UNIQUE (btid->unique_pk)
	  && (node_type == BTREE_OVERFLOW_NODE || !is_first
	      || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS)))
	{
	  /* Skip class OID */
	  if (or_advance (&buf, OR_OID_SIZE) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}

      /* Get MVCC information */
      if (btree_or_get_mvccinfo (&buf, &mvcc_rec_header, mvcc_flags)
	  != NO_ERROR)
	{
	  return ER_FAILED;
	}

      /* Check snapshot */
      if ((mvcc_snapshot)->
	  snapshot_fnc (thread_p, &mvcc_rec_header, mvcc_snapshot))
	{
	  /* Satisfies snapshot so counter must be incremented */
	  rec_oid_cnt++;
	}

      if (max_visible_oids != NULL)
	{
	  if (rec_oid_cnt >= *max_visible_oids)
	    {
	      return rec_oid_cnt;
	    }
	}

      if (node_type == BTREE_LEAF_NODE && is_first)
	{
	  /* Must skip over the key value to the next object */
	  or_seek (&buf, oid_offset);
	}
      is_first = false;
    }

  return rec_oid_cnt;
}

/*
 * btree_leaf_get_num_oids () -
 *   return: number of OIDs
 *   rec(in): record descriptor
 *   oid_offset(in): OID offset
 *   node_type(in): node type
 *   oid_size(in): OID size
 */
static int
btree_leaf_get_num_oids (RECDES * rec, int oid_offset,
			 BTREE_NODE_TYPE node_type, int oid_size)
{
  int rec_oid_cnt, vpid_size = 0;
  short mvcc_flag;

  assert (rec != NULL && oid_offset >= 0 && node_type != BTREE_NON_LEAF_NODE);

  if (mvcc_Enabled)
    {
      char *rec_ptr, *end_ptr;
      if (node_type == BTREE_LEAF_NODE)
	{
	  rec_oid_cnt = 1;

	  /* non-unique page - MVCCID has variable size */
	  if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	    {
	      vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
	    }

	  if (oid_size == 2 * OR_OID_SIZE)
	    {
	      /* unique page - MVCCID has fixed size */
	      rec_oid_cnt +=
		CEIL_PTVDIV (rec->length - oid_offset - vpid_size,
			     oid_size + 2 * OR_MVCCID_SIZE);
	      return rec_oid_cnt;
	    }

	  rec_ptr = rec->data + oid_offset;
	  end_ptr = rec->data + rec->length - vpid_size;
	  /* count oids */
	  while (rec_ptr < end_ptr)
	    {
	      mvcc_flag = btree_leaf_key_oid_get_mvcc_flag (rec_ptr);

	      rec_ptr +=
		oid_size +
		BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG (mvcc_flag);
	      rec_oid_cnt++;
	    }
	  assert (rec_ptr == end_ptr);
	}
      else
	{
	  /* BTREE_OVERFLOW_NODE - MVCCID fixed size */
	  assert (oid_offset == 0);
	  rec_oid_cnt = CEIL_PTVDIV (rec->length,
				     oid_size + 2 * OR_MVCCID_SIZE);
	}

      return rec_oid_cnt;
    }

  if (node_type == BTREE_LEAF_NODE)
    {
      rec_oid_cnt = 1;
      if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	{
	  vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
	  rec_oid_cnt += CEIL_PTVDIV (rec->length - oid_offset - vpid_size,
				      oid_size);
	}
      else
	{
	  rec_oid_cnt += CEIL_PTVDIV (rec->length - oid_offset, oid_size);
	}
    }
  else
    {				/* BTREE_OVERFLOW_NODE */
      assert (oid_offset == 0);
      assert (node_type == BTREE_OVERFLOW_NODE);
      assert (mvcc_Enabled || oid_size == OR_OID_SIZE);

      rec_oid_cnt = CEIL_PTVDIV (rec->length, oid_size);
    }

  return rec_oid_cnt;
}

/*
 * btree_leaf_get_oid_from_oidptr () - Get OID and class OID from b-tree
 *				       record. If MVCC is enabled, also
 *				       MVCC information may be obtained.
 *   return		 : Pointer after current object.
 *   bts (in)		 : B-tree scan data.
 *   rec_oid_ptr (in)	 : Pointer to current object.
 *   node_type (in)	 :
 *   oid (out)		 :
 *   class_oid (out)	 :
 *   p_mvcc_header (out) : MVCC header
 */
static void
btree_leaf_get_oid_from_oidptr (BTREE_SCAN * bts, char *rec_oid_ptr,
				BTREE_NODE_TYPE node_type,
				OID * oid, OID * class_oid,
				MVCC_REC_HEADER * p_mvcc_header)
{
  short mvcc_flags = 0;
  char *ptr = NULL;

  assert (bts != NULL);
  assert (rec_oid_ptr != NULL);
  assert (oid != NULL);
  assert (class_oid != NULL);
  assert (node_type != BTREE_NON_LEAF_NODE);

  ptr = rec_oid_ptr;

  /* Read OID without clearing the MVCC flags */
  ptr = or_unpack_oid (ptr, oid);
  /* Get MVCC flags from object */
  mvcc_flags = oid->volid & BTREE_LEAF_OID_MVCC_MASK;

  if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      if (node_type == BTREE_LEAF_NODE && bts->oid_pos == 0
	  && ((oid->slotid & BTREE_LEAF_RECORD_SUBCLASS) == 0))
	{
	  /* Use top class OID */
	  COPY_OID (class_oid, &bts->btid_int.topclass_oid);
	}
      else
	{
	  /* Read class OID */
	  ptr = or_unpack_oid (ptr, class_oid);
	}
    }
  else
    {
      /* No class OID */
      OID_SET_NULL (class_oid);
    }

  if (mvcc_Enabled)
    {
      /* OID's may be followed by MVCC information */
      if (p_mvcc_header == NULL)
	{
	  /* Skip MVCC info */
	  ptr += BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG (mvcc_flags);
	}
      else
	{
	  /* Get MVCC info */
	  ptr = btree_unpack_mvccinfo (ptr, p_mvcc_header, mvcc_flags);
	}
    }

  /* Clear flags from object */
  BTREE_CLEAR_MVCC_FLAGS_FROM_OID (oid);
  BTREE_CLEAR_RECORD_FLAGS_FROM_OID (oid);
}

/*
 * btree_leaf_advance_oidptr () -
 *   return: next pointer
 *   bts(in/out):
 *   rec_oid_ptr(in):
 *   offset(in):
 *   node_type(in):
 */
static char *
btree_leaf_advance_oidptr (BTREE_SCAN * bts, char *rec_oid_ptr, int offset,
			   BTREE_NODE_TYPE node_type)
{
  assert (bts != NULL && rec_oid_ptr != NULL && offset >= 0
	  && node_type != BTREE_NON_LEAF_NODE);

  if (node_type == BTREE_LEAF_NODE && bts->oid_pos == 0)
    {
      rec_oid_ptr += offset;
    }
  else
    {
      int mvcc_offset = 0;

      if (mvcc_Enabled)
	{
	  if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk)
	      || node_type == BTREE_OVERFLOW_NODE)
	    {
	      mvcc_offset += 2 * OR_MVCCID_SIZE;
	    }
	  else
	    {
	      if (btree_leaf_key_oid_is_mvcc_flaged
		  (rec_oid_ptr, BTREE_LEAF_OID_HAS_MVCC_INSID))
		{
		  mvcc_offset += OR_MVCCID_SIZE;
		}

	      if (btree_leaf_key_oid_is_mvcc_flaged
		  (rec_oid_ptr, BTREE_LEAF_OID_HAS_MVCC_DELID))
		{
		  mvcc_offset += OR_MVCCID_SIZE;
		}
	    }
	}

      if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
	{
	  rec_oid_ptr += (2 * OR_OID_SIZE + mvcc_offset);
	}
      else
	{
	  rec_oid_ptr += OR_OID_SIZE + mvcc_offset;
	}
    }

  bts->oid_pos++;

  return rec_oid_ptr;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * btree_leaf_put_first_oid () -
 *   return: NO_ERROR
 *   recp(in/out):
 *   oidp(in):
 *   record_flag(in):
 */
static int
btree_leaf_put_first_oid (RECDES * recp, OID * oidp, short record_flag)
{
  OR_BUF buf;
  int rc;

  assert ((short) (record_flag & ~BTREE_LEAF_RECORD_MASK) == 0);

  or_init (&buf, recp->data, OR_OID_SIZE);

  rc = or_put_int (&buf, oidp->pageid);
  assert (rc == NO_ERROR);
  rc = or_put_short (&buf, oidp->slotid | record_flag);
  assert (rc == NO_ERROR);
  rc = or_put_short (&buf, oidp->volid);
  assert (rc == NO_ERROR);

  return NO_ERROR;
}
#endif

/*
 * btree_leaf_change_first_oid () - rebuild leaf record
 *   return: NO_ERROR
 *   recp(in/out):  leaf record
 *   btid(in):	B+tree index identifier
 *   oidp(in): oid to insert into record
 *   class_oidp(in): class oid to insert into record
 *   offset(in): oid offset
 *   p_mvcc_rec_header(in/out): MVCC record header containing MVCC info
 *			      to be inserted into record
 *   key_offset(out): offset of the key after moving data
 *
 * Note: This function replace OID, CLASS OID and MVCCID from recp->data + offset
 *  with values specified by oidp, class_oidp, p_mvcc_rec_header.
 *	 This function is called only for leaf page if offset == 0 or non unique
 */
void
btree_leaf_change_first_oid (RECDES * recp, BTID_INT * btid, OID * oidp,
			     OID * class_oidp,
			     MVCC_REC_HEADER * p_mvcc_rec_header,
			     int *key_offset)
{
  short old_rec_flag = 0, new_rec_flag = 0, mvcc_flags = 0;
  char *src = NULL, *desc = NULL;
  int old_object_size, new_object_size;
  bool new_has_insid = false, new_has_delid = false;
  bool new_has_class_oid = false;
  OR_BUF buffer;
  MVCC_REC_HEADER local_mvcc_header;

  /* Get old record flags */
  old_rec_flag = btree_leaf_get_flag (recp);
  /* Initialize new record flags same as old record flags */
  new_rec_flag = old_rec_flag;

  /* Get the size of old object */
  if (BTREE_IS_UNIQUE (btid->unique_pk)
      && btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS))
    {
      /* Also class OID is saved */
      old_object_size = 2 * OR_OID_SIZE;
    }
  else
    {
      old_object_size = OR_OID_SIZE;
    }

  /* Compute the required size for the new object */
  if (BTREE_IS_UNIQUE (btid->unique_pk)
      && !OID_EQ (&btid->topclass_oid, class_oidp))
    {
      /* Also class OID is saved */
      new_object_size = 2 * OR_OID_SIZE;
      /* Add BTREE_LEAF_RECORD_SUBCLASS flag */
      new_rec_flag |= BTREE_LEAF_RECORD_SUBCLASS;
      new_has_class_oid = true;
    }
  else
    {
      new_object_size = OR_OID_SIZE;
      /* Clear BTREE_LEAF_RECORD_SUBCLASS flag */
      new_rec_flag &= ~BTREE_LEAF_RECORD_SUBCLASS;
    }

  if (mvcc_Enabled)
    {
      /* insert/delete MVCCID's may be present or may be added */
      if (p_mvcc_rec_header == NULL)
	{
	  /* use empty MVCC rec header */
	  p_mvcc_rec_header = &local_mvcc_header;
	  BTREE_INIT_MVCC_HEADER (p_mvcc_rec_header);
	}

      if (old_rec_flag & BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	{
	  /* First object must have fixed size */
	  old_object_size += 2 * OR_MVCCID_SIZE;
	  new_object_size += 2 * OR_MVCCID_SIZE;
	  new_has_insid = true;
	  new_has_delid = true;
	  BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_rec_header);
	  if (BTREE_IS_UNIQUE (btid->unique_pk) && !new_has_class_oid)
	    {
	      /* Fixed size is required, so force adding class OID also */
	      new_rec_flag |= BTREE_LEAF_RECORD_SUBCLASS;
	      new_object_size += OR_OID_SIZE;
	      new_has_class_oid = true;
	    }
	}
      else
	{
	  /* Check new MVCCID's */
	  if (MVCC_IS_FLAG_SET (p_mvcc_rec_header, OR_MVCC_FLAG_VALID_INSID))
	    {
	      new_object_size += OR_MVCCID_SIZE;
	      new_has_insid = true;
	    }
	  if (MVCC_IS_FLAG_SET (p_mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID))
	    {
	      new_object_size += OR_MVCCID_SIZE;
	      new_has_delid = true;
	    }

	  /* Check old MVCCID's */
	  if (btree_leaf_key_oid_is_mvcc_flaged
	      (recp->data, BTREE_LEAF_OID_HAS_MVCC_INSID))
	    {
	      old_object_size += OR_MVCCID_SIZE;
	    }
	  if (btree_leaf_key_oid_is_mvcc_flaged
	      (recp->data, BTREE_LEAF_OID_HAS_MVCC_DELID))
	    {
	      old_object_size += OR_MVCCID_SIZE;
	    }
	}
    }

  /* Key and any other OID's may need to be moved */
  BTREE_LEAF_MOVE_INSIDE_RECORD (recp, new_object_size, old_object_size);
  if (key_offset)
    {
      *key_offset = (new_object_size - old_object_size);
    }

  /* Add new data */
  or_init (&buffer, recp->data, new_object_size);
  /* Object OID first */
  if (or_put_oid (&buffer, oidp) != NO_ERROR)
    {
      assert_release (false);
      return;
    }
  /* Set record flags */
  btree_leaf_set_flag (recp, new_rec_flag);

  if (new_has_class_oid)
    {
      /* Add class OID */
      if (or_put_oid (&buffer, class_oidp) != NO_ERROR)
	{
	  assert_release (false);
	  return;
	}
    }

  if (mvcc_Enabled)
    {
      /* Add MVCC info */
      if (new_has_insid)
	{
	  assert (MVCCID_IS_VALID (MVCC_GET_INSID (p_mvcc_rec_header))
		  && mvcc_id_precedes (MVCC_GET_INSID (p_mvcc_rec_header),
				       log_Gl.hdr.mvcc_next_id));
	  /* Add insert MVCCID */
	  if (or_put_mvccid (&buffer, MVCC_GET_INSID (p_mvcc_rec_header))
	      != NO_ERROR)
	    {
	      assert_release (false);
	      return;
	    }
	  mvcc_flags |= BTREE_LEAF_OID_HAS_MVCC_INSID;
	}
      if (new_has_delid)
	{
	  assert (MVCC_GET_DELID (p_mvcc_rec_header) == MVCCID_NULL
		  || mvcc_id_precedes (MVCC_GET_DELID (p_mvcc_rec_header),
				       log_Gl.hdr.mvcc_next_id));
	  /* Add delete MVCCID */
	  if (or_put_mvccid (&buffer, MVCC_GET_DELID (p_mvcc_rec_header))
	      != NO_ERROR)
	    {
	      assert_release (false);
	    }
	  mvcc_flags |= BTREE_LEAF_OID_HAS_MVCC_DELID;
	}
      if (mvcc_flags != 0)
	{
	  /* Set MVCC flags */
	  btree_leaf_key_oid_set_mvcc_flag (recp->data, mvcc_flags);
	}
    }
  /* Make sure everything was packed correctly */
  assert_release (buffer.ptr == buffer.endptr);

  /* If MVCC is enabled and if b-tree is unique and if the record has overflow
   * OID's, the first oid must have fixed size and should also contain class
   * OID (marked as BTREE_LEAF_RECORD_SUBCLASS).
   */
  assert (!mvcc_Enabled || !BTREE_IS_UNIQUE (btid->unique_pk)
	  || !btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	  || btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS));

#if !defined (NDEBUG)
  btree_check_valid_record (NULL, btid, recp, BTREE_LEAF_NODE, NULL);
#endif
}

/*
 * btree_leaf_set_fixed_mvcc_size_for_first_record_oid () - set fixed MVCC size
 *						      for first OID leaf record
 *   return: nothing
 *   recp(in/out):  leaf record
 *   bool is_unique(in): true id is unique index
 *   class_oid(in): class oid used in case of unique index
 */
static void
btree_leaf_set_fixed_mvcc_size_for_first_record_oid (RECDES * recp,
						     bool is_unique,
						     OID * class_oid)
{
  int old_mvcc_size = 0;
  int mvcc_old_oid_mvcc_flags;
  int src_offset, dest_offset;
  int old_oid_size = OR_OID_SIZE;
  int new_oid_size = OR_OID_SIZE;
  MVCCID delid, insid;

  if (is_unique)
    {
      if (btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  old_oid_size += OR_OID_SIZE;
	}
      new_oid_size += OR_OID_SIZE;
    }

  mvcc_old_oid_mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (recp->data);
  if (mvcc_old_oid_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID)
    {
      OR_GET_MVCCID (recp->data + old_oid_size, &insid);
      old_mvcc_size += OR_MVCCID_SIZE;
    }
  else
    {
      insid = MVCCID_ALL_VISIBLE;
    }

  if (mvcc_old_oid_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID)
    {
      OR_GET_MVCCID (recp->data + old_oid_size + old_mvcc_size, &delid);
      old_mvcc_size += OR_MVCCID_SIZE;
    }
  else
    {
      delid = MVCCID_NULL;
    }

  if (old_mvcc_size == 2 * OR_MVCCID_SIZE)
    {
      if (old_oid_size == new_oid_size)
	{
	  /* All information is already here */
	  return;
	}
    }

  /* Must free space to add extra info */
  src_offset = old_oid_size + old_mvcc_size;
  dest_offset = new_oid_size + 2 * OR_MVCCID_SIZE;
  BTREE_LEAF_MOVE_INSIDE_RECORD (recp, dest_offset, src_offset);

  dest_offset = OR_OID_SIZE;

  /* add class oid in case of unique - it may be possible to add topclass_oid */
  if (is_unique)
    {
      if (!btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  /* Add class OID */
	  OR_PUT_OID (recp->data + OR_OID_SIZE, class_oid);
	  btree_leaf_set_flag (recp, BTREE_LEAF_RECORD_SUBCLASS);
	  dest_offset += OR_OID_SIZE;
	}
    }

  /* Add both insert MVCCID and delete MVCCID */
  OR_PUT_MVCCID (recp->data + dest_offset, &insid);
  dest_offset += OR_MVCCID_SIZE;
  OR_PUT_MVCCID (recp->data + dest_offset, &delid);
  btree_leaf_key_oid_set_mvcc_flag (recp->data,
				    BTREE_LEAF_OID_HAS_MVCC_INSID
				    | BTREE_LEAF_OID_HAS_MVCC_DELID);
}

/*
 * btree_leaf_get_nth_oid_ptr () - Advance to the nth object in b-tree key
 *				   record data and return pointer.
 *
 * return		: Pointer to nth object in record data.
 * btid (in)		: B-tree data.
 * recp (in)		: Record data.
 * node_type (in)	: Node type (leaf or overflow).
 * oid_list_offset (in) : Offset to list of objects (for leaf it must skip the
 *			  packed key).
 * n (in)		: Required object index.
 */
static char *
btree_leaf_get_nth_oid_ptr (BTID_INT * btid, RECDES * recp,
			    BTREE_NODE_TYPE node_type, int oid_list_offset,
			    int n)
{
  OR_BUF buf;
  int fixed_size;
  int oids_size;
  int vpid_size;
  int mvcc_info_size;
  short mvcc_flags;
  bool is_fixed_size;

  assert (node_type == BTREE_LEAF_NODE || node_type == BTREE_OVERFLOW_NODE);

  if (n == 0)
    {
      /* First object is always first in record data */
      return recp->data;
    }

  vpid_size = (btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	       ? DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT) : 0);

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oids_size = 2 * OR_OID_SIZE;
    }
  else
    {
      oids_size = OR_OID_SIZE;
    }

  is_fixed_size = (!mvcc_Enabled || (node_type == BTREE_OVERFLOW_NODE)
		   || BTREE_IS_UNIQUE (btid->unique_pk));
  if (is_fixed_size)
    {
      /* Each object has fixed size */
      fixed_size = oids_size;
      if (mvcc_Enabled)
	{
	  /* MVCCID's are also saved */
	  fixed_size += 2 * OR_MVCCID_SIZE;
	}

      if (node_type == BTREE_OVERFLOW_NODE)
	{
	  assert (oid_list_offset == 0);
	  assert (n * fixed_size + vpid_size < recp->length);
	  return recp->data + n * fixed_size;
	}
      else			/* node_type == BTREE_LEAF_NODE */
	{
	  assert ((oid_list_offset + (n - 1) * fixed_size + vpid_size)
		  < recp->length);
	  return recp->data + oid_list_offset + (n - 1) * fixed_size;
	}
    }

  assert (mvcc_Enabled);

  or_init (&buf, recp->data + oid_list_offset,
	   recp->length - vpid_size - oid_list_offset);

  if (node_type == BTREE_LEAF_NODE)
    {
      /* In case of leaf node, oid_list_offset is after the first object */
      n = n - 1;
    }

  while (n > 0)
    {
      /* Skip object */
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);
      mvcc_info_size = BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG (mvcc_flags);

      if (or_advance (&buf, oids_size + mvcc_info_size) != NO_ERROR)
	{
	  assert_release (false);
	  return NULL;
	}

      n--;
    }

  /* buf.ptr points to nth object */
  assert (buf.ptr < buf.endptr);

  return buf.ptr;
}

/*
 * btree_leaf_get_last_oid () -
 *   return: NO_ERROR
 *   btid(in/out):
 *   recp(in):
 *   node_type(in):
 *   mvcc_oid_list_offset(in): oid list offset, null in non-MVCC
 *   oidp(out):
 *   class_oidp(out):
 *   p_mvcc_header(out): MVCC rec header - null in non-MVCC
 *   last_oid_mvcc_offset(out): last oid MVCC offset - null in non-MVCC
 *
 * Note: This function must be called when the buffer contains at least
 *  2 OIDs
 */
static int
btree_leaf_get_last_oid (BTID_INT * btid, RECDES * recp,
			 BTREE_NODE_TYPE node_type,
			 int mvcc_oid_list_offset,
			 OID * oidp, OID * class_oid,
			 MVCC_REC_HEADER * p_mvcc_header,
			 int *last_oid_mvcc_offset)
{
  int vpid_size = 0, fixed_object_size;
  char *offset;
  short mvcc_flags = 0;
  bool is_object_size_fixed =
    !mvcc_Enabled || BTREE_IS_UNIQUE (btid->unique_pk)
    || node_type == BTREE_OVERFLOW_NODE;

  assert (btid != NULL && recp != NULL && node_type != BTREE_NON_LEAF_NODE
	  && oidp != NULL && class_oid != NULL);

  if (node_type == BTREE_LEAF_NODE
      && btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
    }

  if (is_object_size_fixed)
    {
      fixed_object_size = OR_OID_SIZE;
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  fixed_object_size += OR_OID_SIZE;
	}
      if (mvcc_Enabled)
	{
	  fixed_object_size += 2 * OR_MVCCID_SIZE;
	}
      offset = recp->data + recp->length - vpid_size - fixed_object_size;

      if (last_oid_mvcc_offset != NULL)
	{
	  assert (mvcc_Enabled);
	  *last_oid_mvcc_offset = offset - recp->data;
	}

      assert (!mvcc_Enabled
	      || btree_leaf_key_oid_is_mvcc_flaged (offset,
						    BTREE_LEAF_OID_HAS_MVCC_DELID
						    |
						    BTREE_LEAF_OID_HAS_MVCC_INSID));

      /* Read OID */
      BTREE_GET_OID (offset, oidp);
      if (mvcc_Enabled && p_mvcc_header != NULL)
	{
	  /* Read mvcc flags now, they will be needed to unpack MVCC info */
	  mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (offset);
	}
      offset += OR_OID_SIZE;

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  /* Read class OID */
	  BTREE_GET_CLASS_OID (offset, class_oid);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (class_oid);
	}

      if (mvcc_Enabled && p_mvcc_header != NULL)
	{
	  /* Get MVCC info */
	  (void) btree_unpack_mvccinfo (offset, p_mvcc_header, mvcc_flags);
	}
    }
  else
    {
      int oids_size = OR_OID_SIZE, mvccid_info_size;
      OR_BUF buf;
      assert (mvcc_Enabled);
      assert (mvcc_oid_list_offset >= 0);

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  /* Object OID is followed by class OID */
	  oids_size += OR_OID_SIZE;
	}

      /* Must advance in record until the end */
      or_init (&buf, recp->data + mvcc_oid_list_offset,
	       recp->length - vpid_size - mvcc_oid_list_offset);
      while (buf.ptr < buf.endptr)
	{
	  /* Save pointer before advancing */
	  offset = buf.ptr;
	  mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);
	  mvccid_info_size =
	    BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG (mvcc_flags);
	  if (or_advance (&buf, oids_size + mvccid_info_size) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}
      assert (buf.ptr == buf.endptr && offset != NULL);

      /* Offset is positioned to last OID in record */
      if (last_oid_mvcc_offset != NULL)
	{
	  *last_oid_mvcc_offset = offset - recp->data;
	}

      /* Get OID */
      BTREE_GET_OID (offset, oidp);
      offset += OR_OID_SIZE;

      /* Get class OID */
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  BTREE_GET_CLASS_OID (offset, class_oid);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (class_oid);
	}

      if (p_mvcc_header)
	{
	  /* Get MVCC info */
	  /* MVCC flags have been set while looking for the last record entry.
	   */
	  (void) btree_unpack_mvccinfo (offset, p_mvcc_header, mvcc_flags);
	}
    }

  return NO_ERROR;
}

/*
 * btree_leaf_remove_last_oid () -
 *   return: NO_ERROR
 *   btid(in/out):
 *   recp(in):
 *   node_type(in):
 *   oid_size(in):
 *   last_oid_mvcc_offset(in): last oid MVCC offset
 */
static int
btree_leaf_remove_last_oid (BTID_INT * btid, RECDES * recp,
			    BTREE_NODE_TYPE node_type, int oid_size,
			    int last_oid_mvcc_offset)
{
  int vpid_size = 0;
  char *offset;

  if (node_type == BTREE_LEAF_NODE)
    {
      if (btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	{
	  vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
	  offset = recp->data + recp->length - vpid_size;
	  if (mvcc_Enabled)
	    {
	      assert (last_oid_mvcc_offset >= 0);
	      memmove (recp->data + last_oid_mvcc_offset, offset, vpid_size);
	    }
	  else
	    {
	      memmove (offset - oid_size, offset, vpid_size);
	    }
	}
    }

  if (mvcc_Enabled)
    {
      recp->length = last_oid_mvcc_offset + vpid_size;
    }
  else
    {
      recp->length -= oid_size;
    }

  return NO_ERROR;
}

/*
 * btree_leaf_get_flag () -
 *   return: flag of leaf record
 *   recp(in):
 */
static short
btree_leaf_get_flag (RECDES * recp)
{
  short slot_id;

  slot_id = OR_GET_SHORT (recp->data + OR_OID_SLOTID);

  return slot_id & BTREE_LEAF_RECORD_MASK;
}

/*
 * btree_leaf_key_oid_get_mvcc_flag () - get MVCC flag for key oid
 *   return: MVCC flag for key oid
 *   data(in): pointer to OID into key buffer
 */
static short
btree_leaf_key_oid_get_mvcc_flag (char *data)
{
  short vol_id;

  assert (data != NULL);
  vol_id = OR_GET_SHORT (data + OR_OID_VOLID);

  return vol_id & BTREE_LEAF_OID_MVCC_MASK;
}

/*
 * btree_leaf_is_flaged () -
 *   return:
 *   recp(in):
 *   record_flag(in):
 */
static bool
btree_leaf_is_flaged (RECDES * recp, short record_flag)
{
  short ret;
  assert ((short) (record_flag & ~BTREE_LEAF_RECORD_MASK) == 0);

  ret = OR_GET_SHORT (recp->data + OR_OID_SLOTID) & record_flag;

  return ret ? true : false;
}

/*
 * btree_leaf_key_oid_is_mvcc_flaged () - check whether the leaf key oid is
 *					MVCC flagged
 *   return: whether the OID is flagged
 *   rec_data(in): pointer to OID into key buffer
 *   record_flag(in): the flag to test
 */
static short
btree_leaf_key_oid_is_mvcc_flaged (char *rec_data, short record_flag)
{
  assert ((short) (record_flag & ~BTREE_LEAF_OID_MVCC_MASK) == 0);

  return (OR_GET_SHORT (rec_data + OR_OID_VOLID) & record_flag);
}

/*
 * btree_leaf_set_flag () -
 *   return:
 *   recp(in/out):
 *   record_flag(in):
 */
static void
btree_leaf_set_flag (RECDES * recp, short record_flag)
{
  short slot_id;

  assert ((short) (record_flag & ~BTREE_LEAF_RECORD_MASK) == 0);

  slot_id = OR_GET_SHORT (recp->data + OR_OID_SLOTID);

  OR_PUT_SHORT (recp->data + OR_OID_SLOTID, slot_id | record_flag);
}

/*
 * btree_leaf_key_oid_set_mvcc_flag () - set MVCC flag for key oid
 *   return: nothing
 *   rec_data(in/out): pointer to OID into key buffer
 *   record_flag(in): the flag to set
 */
static void
btree_leaf_key_oid_set_mvcc_flag (char *rec_data, short record_flag)
{
  short vol_id;

  assert ((short) (record_flag & ~BTREE_LEAF_OID_MVCC_MASK) == 0);

  vol_id = OR_GET_SHORT (rec_data + OR_OID_VOLID);

  OR_PUT_SHORT (rec_data + OR_OID_VOLID, vol_id | record_flag);
}

/*
 * btree_leaf_clear_flag () - clear leaf key oid flag
 *   return:  nothing
 *   recp(in/out):
 *   record_flag(in):
 */
static void
btree_leaf_clear_flag (RECDES * recp, short record_flag)
{
  short slot_id;

  assert ((short) (record_flag & ~BTREE_LEAF_RECORD_MASK) == 0);

  slot_id = OR_GET_SHORT (recp->data + OR_OID_SLOTID);

  OR_PUT_SHORT (recp->data + OR_OID_SLOTID, slot_id & ~record_flag);
}

/*
 * btree_leaf_key_oid_clear_mvcc_flag () - clear MVCC flag for key oid
 *   return: nothing
 *   rec_data(in/out): pointer to OID into key buffer
 *   record_flag(in): the flag to set
 */
static void
btree_leaf_key_oid_clear_mvcc_flag (char *rec_data, short record_flag)
{
  short vol_id;

  assert ((short) (record_flag & ~BTREE_LEAF_OID_MVCC_MASK) == 0);

  vol_id = OR_GET_SHORT (rec_data + OR_OID_VOLID);

  OR_PUT_SHORT (rec_data + OR_OID_VOLID, vol_id & ~record_flag);
}

/*
 * btree_write_fixed_portion_of_non_leaf_record () -
 *   return:
 *   rec(in):
 *   non_leaf_rec(in):
 *
 * Note: Writes the fixed portion (preamble) of a non leaf record.
 * rec must be long enough to hold the header info.
 */
static void
btree_write_fixed_portion_of_non_leaf_record (RECDES * rec,
					      NON_LEAF_REC * non_leaf_rec)
{
  char *ptr = rec->data;

  assert (!VPID_ISNULL (&(non_leaf_rec->pnt)));

  OR_PUT_INT (ptr, non_leaf_rec->pnt.pageid);
  ptr += OR_INT_SIZE;

  OR_PUT_SHORT (ptr, non_leaf_rec->pnt.volid);
  ptr += OR_SHORT_SIZE;

  OR_PUT_SHORT (ptr, non_leaf_rec->key_len);
}

/*
 * btree_read_fixed_portion_of_non_leaf_record () -
 *   return:
 *   rec(in):
 *   non_leaf_rec(in):
 *
 * Note: Reads the fixed portion (preamble) of a non leaf record.
 */
static void
btree_read_fixed_portion_of_non_leaf_record (RECDES * rec,
					     NON_LEAF_REC * non_leaf_rec)
{
  char *ptr = rec->data;

  non_leaf_rec->pnt.pageid = OR_GET_INT (ptr);
  ptr += OR_INT_SIZE;

  non_leaf_rec->pnt.volid = OR_GET_SHORT (ptr);
  ptr += OR_SHORT_SIZE;

  assert (!VPID_ISNULL (&(non_leaf_rec->pnt)));

  non_leaf_rec->key_len = OR_GET_SHORT (ptr);
}

/*
 * btree_write_fixed_portion_of_non_leaf_record_to_orbuf () -
 *   return:
 *   buf(in):
 *   nlf_rec(in):
 *
 * Note: Writes the fixed portion (preamble) of a non leaf record using
 * the OR_BUF stuff.
 */
static void
btree_write_fixed_portion_of_non_leaf_record_to_orbuf (OR_BUF * buf,
						       NON_LEAF_REC *
						       non_leaf_rec)
{
  assert (!VPID_ISNULL (&(non_leaf_rec->pnt)));

  or_put_int (buf, non_leaf_rec->pnt.pageid);
  or_put_short (buf, non_leaf_rec->pnt.volid);
  or_put_short (buf, non_leaf_rec->key_len);
}

/*
 * btree_read_fixed_portion_of_non_leaf_record_from_orbuf () -
 *   return: NO_ERROR
 *   buf(in):
 *   non_leaf_rec(in):
 *
 * Note: Reads the fixed portion (preamble) of a non leaf record using
 * the OR_BUF stuff.
 */
static int
btree_read_fixed_portion_of_non_leaf_record_from_orbuf (OR_BUF * buf,
							NON_LEAF_REC *
							non_leaf_rec)
{
  int rc = NO_ERROR;

  non_leaf_rec->pnt.pageid = or_get_int (buf, &rc);

  if (rc == NO_ERROR)
    {
      non_leaf_rec->pnt.volid = or_get_short (buf, &rc);
    }

  assert (!VPID_ISNULL (&(non_leaf_rec->pnt)));

  if (rc == NO_ERROR)
    {
      non_leaf_rec->key_len = or_get_short (buf, &rc);
    }

  return rc;
}

/*
 * btree_append_oid () -
 *   return:
 *   rec(in):
 *   oid(in):
 *
 * Note: Appends an OID onto the record.  rec is assumed to have room
 * for the new OID and rec.length points to the end of the record
 * where the new OID will go and is word aligned.
 */
static void
btree_append_oid (RECDES * rec, OID * oid)
{
  char *ptr;

  ptr = rec->data + rec->length;
  OR_PUT_OID (ptr, oid);
  rec->length += OR_OID_SIZE;
}

/*
 * btree_add_mvcc_delid () - add MVCC delete id for an OID
 *   return: nothing
 *   rec(in/out): record descriptor
 *   oid_offset(in): OID offset for which MVCC delid is added
 *   mvcc_delid_offset(in): MVCC delid offset
 *   p_mvcc_delid(in): MVCC delete id to insert
 *
 *  Note: This function must be called if the record does not contain
 *    MVCC delid for specified OID.
 *	  rec must have enough free allocated area in order to fit MVCCID
 */
static void
btree_add_mvcc_delid (RECDES * rec, int oid_offset, int mvcc_delid_offset,
		      MVCCID * p_mvcc_delid)
{
  int dest_offset;
  assert (rec != NULL && p_mvcc_delid != NULL && oid_offset >= 0
	  && mvcc_delid_offset > 0 && oid_offset < mvcc_delid_offset);
  assert (!btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					      BTREE_LEAF_OID_HAS_MVCC_DELID));
  assert (rec->length + OR_MVCCID_SIZE < rec->area_size);

  dest_offset = mvcc_delid_offset + OR_MVCCID_SIZE;
  BTREE_LEAF_MOVE_INSIDE_RECORD (rec, dest_offset, mvcc_delid_offset);
  btree_leaf_key_oid_set_mvcc_flag (rec->data + oid_offset,
				    BTREE_LEAF_OID_HAS_MVCC_DELID);
  OR_PUT_MVCCID (rec->data + mvcc_delid_offset, p_mvcc_delid);
}

/*
 * btree_set_mvcc_delid () - set MVCC delete id for an OID
 *   return: nothing
 *   rec(in/out): record descriptor
 *   oid_offset(in): OID offset for which MVCC delid is set
 *   mvcc_delid_offset(in): MVCC delid offset
 *   p_mvcc_delid(in): MVCC delete id
 *
 * Note: This function must be called if the record already contain MVCC delid
 *  for specified OID. Thus, the old MVCCC delid is replaced by the new one.
 *	The length of the record is not affected by this function.
 */
static void
btree_set_mvcc_delid (RECDES * rec, int oid_offset, int mvcc_delid_offset,
		      MVCCID * p_mvcc_delid)
{
  char *data;
  assert (rec != NULL && oid_offset >= 0 && mvcc_delid_offset > 0
	  && p_mvcc_delid != NULL);

  data = rec->data + oid_offset;
  OR_PUT_MVCCID (rec->data + mvcc_delid_offset, p_mvcc_delid);
}

/*
 * btree_delete_mvcc_delid () - delete MVCC delete id for an OID
 *   return: nothing
 *   rec(in/out): record descriptor
 *   oid_offset(in): OID offset for which MVCC delid is deleted
 *   mvcc_delid_offset(in): MVCC delid offset inside rec
 *
 * Note: This function must be called only if the record contains MVCC delid
 *  for specified OID.
 */
static void
btree_delete_mvcc_delid (RECDES * rec, int oid_offset, int mvcc_delid_offset)
{
  int src_offset;
  assert (rec != NULL && oid_offset < mvcc_delid_offset);

  src_offset = mvcc_delid_offset + OR_MVCCID_SIZE;
  BTREE_LEAF_MOVE_INSIDE_RECORD (rec, mvcc_delid_offset, src_offset);

  btree_leaf_key_oid_clear_mvcc_flag (rec->data + oid_offset,
				      BTREE_LEAF_OID_HAS_MVCC_DELID);
}

/*
 * btree_delete_mvcc_delid () - delete MVCC insert id for an OID
 *   return: nothing
 *   rec(in/out): record descriptor
 *   oid_offset(in): OID offset for which MVCC insid is deleted
 *   mvcc_delid_offset(in): MVCC insid offset inside rec
 *
 * Note: This function must be called only if the record contains MVCC insid
 *  for specified OID.
 */
static void
btree_delete_mvcc_insid (RECDES * rec, int oid_offset, int mvcc_insid_offset)
{
  int src_offset;

  assert (rec != NULL && oid_offset < mvcc_insid_offset);

  src_offset = mvcc_insid_offset + OR_MVCCID_SIZE;
  BTREE_LEAF_MOVE_INSIDE_RECORD (rec, mvcc_insid_offset, src_offset);

  btree_leaf_key_oid_clear_mvcc_flag (rec->data + oid_offset,
				      BTREE_LEAF_OID_HAS_MVCC_INSID);
}

/*
 * btree_delete_mvcc_object () - Remove object and following MVCCID's.
 *
 * return	       : Void.
 * rec (in/out)	       : B-tree key record.
 * oid_offset (in)     : Offset in key record to object that is being removed.
 * oid_size (in)       : Size of OID's (object OID and eventually the class
 *			 OID).
 * has_fixed_size (in) : True if fixed size is forced for current object
 *			 (as a consequence both insert and delete MVCCID's
 *			 are present).
 */
static void
btree_delete_mvcc_object (RECDES * rec, int oid_offset, int oid_size,
			  bool has_fixed_size)
{
  assert (rec != NULL && oid_offset > 0);

  if (has_fixed_size)
    {
      assert (btree_leaf_key_oid_is_mvcc_flaged
	      (rec->data + oid_offset, BTREE_LEAF_OID_HAS_MVCC_INSID)
	      && btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
						    BTREE_LEAF_OID_HAS_MVCC_DELID));
      oid_size += 2 * OR_MVCCID_SIZE;
    }
  else
    {
      if (btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					     BTREE_LEAF_OID_HAS_MVCC_INSID))
	{
	  oid_size += OR_MVCCID_SIZE;
	}
      if (btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					     BTREE_LEAF_OID_HAS_MVCC_DELID))
	{
	  oid_size += OR_MVCCID_SIZE;
	}
    }
  BTREE_LEAF_MOVE_INSIDE_RECORD (rec, oid_offset, oid_offset + oid_size);
}

/*
 * btree_update_record_mvccid () - update record MVCCID info
 *   return: nothing
 *   recp(in/out): record descriptor
 *   offset(in): the offset of OID where MVCC must be updated
 *   oid_size(in): oid size
 *   p_mvcc_rec_header(in): MVCC rec header
 *
 * Note : This function assumes that the record contain free space for MVCCIDs
 *  that will be filled. Thus, the length of the record is not updated by this
 *  function. This function may be called if offset = 0 or non unique (MVCC
 *  flags must be set/reset)
 */
static void
btree_update_mvcc_info (RECDES * recp, int offset, int oid_size,
			MVCC_REC_HEADER * p_mvcc_rec_header)
{
  int mvccid_size = 0;
  if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)
    {
      /* has insert id */
      btree_leaf_key_oid_set_mvcc_flag (recp->data + offset,
					BTREE_LEAF_OID_HAS_MVCC_INSID);
      OR_PUT_MVCCID (recp->data + offset + oid_size,
		     &MVCC_GET_INSID (p_mvcc_rec_header));
      mvccid_size += OR_MVCCID_SIZE;
    }
  else
    {

      btree_leaf_key_oid_clear_mvcc_flag (recp->data + offset,
					  BTREE_LEAF_OID_HAS_MVCC_INSID);

    }

  if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID)
    {
      /* has delete id */
      btree_leaf_key_oid_set_mvcc_flag (recp->data + offset,
					BTREE_LEAF_OID_HAS_MVCC_DELID);
      OR_PUT_MVCCID (recp->data + offset + oid_size + mvccid_size,
		     &MVCC_GET_DELID (p_mvcc_rec_header));
    }
  else
    {
      btree_leaf_key_oid_clear_mvcc_flag (recp->data + offset,
					  BTREE_LEAF_OID_HAS_MVCC_DELID);
    }
}

/*
 * btree_append_mvcc_info () -
 *   return: nothing
 *   rec(in/out): record descriptor
 *   p_mvcc_rec_header(in): MVCC record header
 *   flags_needed(in): true if need to set MVCC dlags
 *
 * Note: Appends MVCC insert id for last record OID. rec is assumed to have room
 * for MVCCID and rec.length points to the end of the record
 * where the MVCCID will go and is word aligned.
 */
static void
btree_append_mvcc_info (RECDES * rec, MVCC_REC_HEADER * p_mvcc_rec_header,
			int oid_size)
{
  assert (mvcc_Enabled == true && rec != NULL && p_mvcc_rec_header != NULL
	  && oid_size > 0);

  if (p_mvcc_rec_header != NULL)
    {
      char *ptr = rec->data + rec->length - oid_size;
      assert (mvcc_Enabled == true);

      if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)
	{
	  btree_leaf_key_oid_set_mvcc_flag (ptr,
					    BTREE_LEAF_OID_HAS_MVCC_INSID);
	  OR_PUT_MVCCID (rec->data + rec->length,
			 &p_mvcc_rec_header->mvcc_ins_id);
	  rec->length += OR_MVCCID_SIZE;
	}

      if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID)
	{
	  btree_leaf_key_oid_set_mvcc_flag (ptr,
					    BTREE_LEAF_OID_HAS_MVCC_DELID);
	  OR_PUT_MVCCID (rec->data + rec->length,
			 &p_mvcc_rec_header->delid_chn.mvcc_del_id);
	  rec->length += OR_MVCCID_SIZE;
	}
    }
}

/*
 * btree_insert_oid_front_ovfl_vpid () -
 *   return:
 *   rec(in): leaf record
 *   oid(in): OID to insert into record
 *   class_oid: class OID to insert into buffer - only if unique and MVCC
 *   is_unique: true if unique tree
 *   ovfl_vpid(in): overflow VPID
 *   p_mvcc_rec_header(in): MVCC rec header
 */
static void
btree_insert_oid_in_front_of_ovfl_vpid (RECDES * rec, OID * oid,
					OID * class_oid,
					bool is_unique,
					VPID * ovfl_vpid,
					MVCC_REC_HEADER * p_mvcc_rec_header)
{
  char *ptr;
  int oid_size = OR_OID_SIZE;
  OID mvcc_flagged_oid;
#if !defined(NDEBUG)
  VPID vpid;

  btree_leaf_get_vpid_for_overflow_oids (rec, &vpid);
  assert (ovfl_vpid->volid == vpid.volid && ovfl_vpid->pageid == vpid.pageid);
#endif

  /* Add object before first overflow page VPID */
  rec->length -= DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
  ptr = rec->data + rec->length;
  if (mvcc_Enabled)
    {
      /* Set MVCC flags to OID before adding it to record */
      COPY_OID (&mvcc_flagged_oid, oid);
      btree_set_mvcc_flags_into_oid (p_mvcc_rec_header, &mvcc_flagged_oid);
      /* Add flagged OID to record */
      ptr = or_pack_oid (ptr, &mvcc_flagged_oid);
      rec->length += OR_OID_SIZE;

      if (is_unique)
	{
	  /* Add class OID */
	  ptr = or_pack_oid (ptr, class_oid);
	  rec->length += OR_OID_SIZE;
	}

      /* Add MVCC info */
      ptr = btree_pack_mvccinfo (ptr, p_mvcc_rec_header);
      rec->length += btree_packed_mvccinfo_size (p_mvcc_rec_header);
    }
  else
    {
      /* Add object */
      ptr = or_pack_oid (ptr, oid);
      rec->length += OR_OID_SIZE;
    }

  btree_leaf_new_overflow_oids_vpid (rec, ovfl_vpid, is_unique, class_oid);
}

/*
 * btree_insert_oid_with_order () -
 *   return:
 *   rec(in):
 *   oid(in):
 *   class_oid: class OID to insert into buffer - only if unique and MVCC
 *   is_unique: true if unique tree
 *   p_mvcc_rec_header(in): MVCC record header
 */
static int
btree_insert_oid_with_order (RECDES * rec, OID * oid, OID * class_oid,
			     bool is_unique,
			     MVCC_REC_HEADER * p_mvcc_rec_header)
{
  char *ptr = NULL, *oid_ptr = NULL;
  int min, mid, max, len, num;
  OID mid_oid;
  int oid_size = OR_OID_SIZE;
  int size = OR_OID_SIZE;
  bool duplicate_oid = false;

  assert (rec->length >= OR_OID_SIZE);
  assert (rec->length % 4 == 0);

  if (mvcc_Enabled == true)
    {
      assert (p_mvcc_rec_header != NULL
	      && p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID
	      && p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID
	      && p_mvcc_rec_header->mvcc_ins_id != MVCCID_NULL);
      if (is_unique)
	{
	  oid_size += OR_OID_SIZE;
	  size += OR_OID_SIZE;
	}
      size += (2 * OR_MVCCID_SIZE);
    }

  num = CEIL_PTVDIV (rec->length, size);
  assert (num >= 0);

  if (num == 0)
    {
      /* Add at the beginning of the record */
      oid_ptr = rec->data;
    }
  else
    {
      /* Binary search for the right position to keep the order */
      min = 0;
      max = num - 1;
      mid = 0;

      while (min <= max)
	{
	  mid = (min + max) / 2;
	  oid_ptr = rec->data + (size * mid);
	  BTREE_GET_OID (oid_ptr, &mid_oid);
	  /* if remove flags from overflow, remove next line */
	  BTREE_CLEAR_RECORD_FLAGS_FROM_OID (&mid_oid);

	  if (OID_EQ (oid, &mid_oid))
	    {
	      if (!mvcc_Enabled)
		{
		  assert (mvcc_Enabled);
		  return ER_FAILED;
		}
	      /* If MVCC is enabled, this case is possible if some conditions are
	       * met:
	       * 1. OID is reusable.
	       * 2. Vacuum cleaned heap entry but didn't clean b-tree entry.
	       * 3. A new record is inserted in the same slot.
	       * 4. The key for old record and new record is the same.
	       * Just replace the old OID's insert/delete information.
	       */
	      duplicate_oid = true;
	      oid_ptr = rec->data + (size * mid);
	      break;
	    }
	  else if (OID_GT (oid, &mid_oid))
	    {
	      min = mid + 1;
	      mid++;
	    }
	  else
	    {
	      max = mid - 1;
	    }
	}

      oid_ptr = rec->data + (size * mid);
    }

  /* oid_ptr points to the address where the new object should be saved */
  if (!duplicate_oid)
    {
      /* Make room for a new OID */
      len = (rec->data + rec->length) - (oid_ptr);
      if (len > 0)
	{
	  /* Move all following data to the right */
	  memmove (oid_ptr + size, oid_ptr, len);
	}

      /* Add new object */
      ptr = oid_ptr;

      OR_PUT_OID (ptr, oid);
      ptr += OR_OID_SIZE;

      if (mvcc_Enabled && is_unique)
	{
	  /* Add class OID */
	  OR_PUT_OID (ptr, class_oid);
	  ptr += OR_OID_SIZE;
	}

      /* A new record is added, increment record length by object total size.
       */
      rec->length += size;
      /* If MVCC is enabled, MVCC information is also included. Fall through.
       */
    }
  else
    {
      /* Duplicates are possible only if MVCC is enabled. */
      assert (mvcc_Enabled);
      ptr = oid_ptr + OR_OID_SIZE;
      /* Need to update MVCC information only... skip OID's */
      if (is_unique)
	{
	  ptr += OR_OID_SIZE;
	}
      /* Record length doesn't change */
      /* Fall through to add MVCC information */
    }

  if (mvcc_Enabled)
    {
      /* Add MVCC information */
      assert (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID
	      && p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID);

      OR_PUT_MVCCID (ptr, &p_mvcc_rec_header->mvcc_ins_id);
      ptr += OR_MVCCID_SIZE;
      OR_PUT_MVCCID (ptr, &p_mvcc_rec_header->delid_chn.mvcc_del_id);
      assert ((is_unique
	       && (rec->length % (2 * OR_OID_SIZE + 2 * OR_MVCCID_SIZE) == 0))
	      || (rec->length % (OR_OID_SIZE + 2 * OR_MVCCID_SIZE) == 0));

      /* Set flags where OID is saved */
      btree_leaf_key_oid_set_mvcc_flag (oid_ptr,
					BTREE_LEAF_OID_HAS_MVCC_INSID |
					BTREE_LEAF_OID_HAS_MVCC_DELID);
    }

  return NO_ERROR;
}

/*
 * btree_start_overflow_page () -
 *   return: NO_ERROR
 *   rec(in):
 *   btid(in):
 *   new_vpid(in):
 *   new_page_ptr(out):
 *   near_vpid(in):
 *   oid(in):
 *   class_oid(in): class oid - used in MVCC for unique index only
 *   next_ovfl_vpid(in): next overflow vpid
 *   p_mvcc_rec_header(in): MVCC record header
 *
 * Note: Gets a new overflow page and puts the first OID onto it.  The
 * VPID is returned.  rec is assumed to be large enough to write
 * the overflow records.
 */
static int
btree_start_overflow_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
			   VPID * new_vpid, PAGE_PTR * new_page_ptr,
			   VPID * near_vpid, OID * oid, OID * class_oid,
			   VPID * next_ovfl_vpid,
			   MVCC_REC_HEADER * p_mvcc_rec_header)
{
#define BTREE_START_OVERFLOW_PAGE_REDO_CRUMBS_MAX 3
  RECINS_STRUCT recins;		/* for recovery purposes */
  int ret = NO_ERROR;
  RECDES rec;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  BTREE_OVERFLOW_HEADER ovf_header_info, *ovf_header = NULL;
  int oid_size = OR_OID_SIZE;
  LOG_RCVINDEX rcvindex;
  LOG_DATA_ADDR addr;
  LOG_CRUMB redo_crumbs[BTREE_START_OVERFLOW_PAGE_REDO_CRUMBS_MAX];
  int n_redo_crumbs = 0;
#if !defined (NDEBUG)
  char domain_buf[BTID_DOMAIN_BUFFER_SIZE], *domain_ptr = NULL;
  int domain_size = or_packed_domain_size (btid->key_type, 0);
#endif

  ovf_header = &ovf_header_info;

  rec.type = REC_HOME;
  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  /* get a new overflow page */
  *new_page_ptr = btree_get_new_page (thread_p, btid, new_vpid, near_vpid);
  if (*new_page_ptr == NULL)
    {
      goto exit_on_error;
    }

  ovf_header->next_vpid = *next_ovfl_vpid;

  if (btree_init_overflow_header (thread_p, *new_page_ptr, ovf_header) !=
      NO_ERROR)
    {
      goto exit_on_error;
    }

  /* insert the value in the new overflow page */
  rec.length = 0;
  btree_append_oid (&rec, oid);

  if (mvcc_Enabled == true)
    {
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  btree_append_oid (&rec, class_oid);
	  oid_size += OR_OID_SIZE;
	}

      assert (p_mvcc_rec_header != NULL
	      && p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID
	      && p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID);
      btree_append_mvcc_info (&rec, p_mvcc_rec_header, oid_size);

      rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
    }
  else
    {
      rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
    }

  if (spage_insert_at (thread_p, *new_page_ptr, 1, &rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

#if !defined(NDEBUG)
  btree_check_valid_record (thread_p, btid, &rec, BTREE_OVERFLOW_NODE, NULL);
#endif

  /* log new overflow page changes for redo purposes */
  recins.flags = 0;

  /* Set record type */
  BTREE_INSERT_RCV_SET_RECORD_OVERFLOW (&recins);

  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_OID_INSERTED);
  recins.oid = *oid;
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_UNIQUE);
      COPY_OID (&recins.class_oid, class_oid);
    }
  else
    {
      OID_SET_NULL (&recins.class_oid);
    }
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_OVFL_CHANGED);
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_NEW_OVFLPG);
  recins.ovfl_vpid = *next_ovfl_vpid;

  BTREE_INSERT_RCV_SET_INSMODE_DEFAULT (&recins);

#if !defined (NDEBUG)
  if (domain_size > BTID_DOMAIN_BUFFER_SIZE)
    {
      domain_ptr = (char *) db_private_alloc (thread_p, domain_size);
      if (domain_ptr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, domain_size);
	  goto exit_on_error;
	}
    }
  else
    {
      domain_ptr = domain_buf;
    }
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_KEY_DOMAIN);
#endif

  /* Create redo crumbs */
  redo_crumbs[n_redo_crumbs].length = sizeof (recins);
  redo_crumbs[n_redo_crumbs++].data = &recins;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      /* Add top class OID to crumbs */
      redo_crumbs[n_redo_crumbs].length = sizeof (btid->topclass_oid);
      redo_crumbs[n_redo_crumbs++].data = &btid->topclass_oid;
    }

#if !defined (NDEBUG)
  /* Pack and add domain to crumbs */
  (void) or_pack_domain (domain_ptr, btid->key_type, 0, 0);
  redo_crumbs[n_redo_crumbs].length = domain_size;
  redo_crumbs[n_redo_crumbs++].data = domain_ptr;
#endif

  assert (n_redo_crumbs <= BTREE_START_OVERFLOW_PAGE_REDO_CRUMBS_MAX);

  /* Initialized log data address */
  addr.offset = -1;
  addr.pgptr = *new_page_ptr;
  addr.vfid = &btid->sys_btid->vfid;

  log_append_redo_crumbs (thread_p, rcvindex, &addr, n_redo_crumbs,
			  redo_crumbs);

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif

  return ret;

exit_on_error:

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_get_key_length () -
 *   return:
 *   key(in):
 */
int
btree_get_key_length (DB_VALUE * key)
{
  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      return 0;
    }

  return pr_index_writeval_disk_size (key);
}

/*
 * btree_write_record () -
 *   return: NO_ERROR
 *   btid(in):
 *   node_rec(in):
 *   key(in):
 *   node_type(in):
 *   key_type(in):
 *   key_len(in):
 *   during_loading(in):
 *   class_oid(in):
 *   oid(in):
 *   p_mvcc_rec_header(in): MVCC record header
 *   rec(out):
 *
 * Note: This routine forms a btree record for both leaf and non leaf pages.
 *
 * node_rec is a NON_LEAF_REC * if we are writing a non leaf page,
 * otherwise it is a LEAF_REC *. ovfl_key indicates whether the key will
 * be written to the page or stored by the overflow manager. If we are
 * writing a non leaf record, oid should be NULL and will be ignored in
 * any case.
 */
int
btree_write_record (THREAD_ENTRY * thread_p, BTID_INT * btid,
		    void *node_rec, DB_VALUE * key, int node_type,
		    int key_type, int key_len, bool during_loading,
		    OID * class_oid, OID * oid,
		    MVCC_REC_HEADER * p_mvcc_rec_header, RECDES * rec)
{
  VPID key_vpid;
  OR_BUF buf;
  int rc = NO_ERROR;
  short rec_type = 0;

  assert (node_type == BTREE_LEAF_NODE || node_type == BTREE_NON_LEAF_NODE);
  assert (key_type == BTREE_NORMAL_KEY || key_type == BTREE_OVERFLOW_KEY);
  assert (rec != NULL);

  or_init (&buf, rec->data, rec->area_size);

  if (node_type == BTREE_LEAF_NODE)
    {
      /* first instance oid */
      or_put_oid (&buf, oid);
      if (rc == NO_ERROR
	  && BTREE_IS_UNIQUE (btid->unique_pk)
	  && !OID_EQ (&btid->topclass_oid, class_oid))
	{
	  /* write the subclass OID */
	  rc = or_put_oid (&buf, class_oid);
	  btree_leaf_set_flag (rec, BTREE_LEAF_RECORD_SUBCLASS);
	}

      if (p_mvcc_rec_header != NULL)
	{
	  assert (mvcc_Enabled == true);
	  if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)
	    {
	      btree_leaf_key_oid_set_mvcc_flag (rec->data,
						BTREE_LEAF_OID_HAS_MVCC_INSID);
	      or_put_mvccid (&buf, p_mvcc_rec_header->mvcc_ins_id);
	    }
	  if (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID)
	    {
	      btree_leaf_key_oid_set_mvcc_flag (rec->data,
						BTREE_LEAF_OID_HAS_MVCC_DELID);
	      or_put_mvccid (&buf, p_mvcc_rec_header->delid_chn.mvcc_del_id);
	    }
	}
    }
  else
    {
      NON_LEAF_REC *non_leaf_rec = (NON_LEAF_REC *) node_rec;

      btree_write_fixed_portion_of_non_leaf_record_to_orbuf (&buf,
							     non_leaf_rec);
    }
  if (rc != NO_ERROR)
    {
      goto end;
    }

  /* write the key */
  if (key_type == BTREE_NORMAL_KEY)
    {				/* key fits in page */
      PR_TYPE *pr_type;

      if (node_type == BTREE_LEAF_NODE)
	{
	  pr_type = btid->key_type->type;
	}
      else
	{
	  pr_type = btid->nonleaf_key_type->type;
	}

      rc = (*(pr_type->index_writeval)) (&buf, key);
    }
  else
    {
      /* overflow key */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_leaf_set_flag (rec, BTREE_LEAF_RECORD_OVERFLOW_KEY);
	}

      rc =
	btree_store_overflow_key (thread_p, btid, key, key_len, node_type,
				  &key_vpid);
      if (rc != NO_ERROR)
	{
	  goto end;
	}

      /* write the overflow VPID as the key */
      rc = or_put_int (&buf, key_vpid.pageid);
      if (rc == NO_ERROR)
	{
	  rc = or_put_short (&buf, key_vpid.volid);
	}
    }

  if (rc == NO_ERROR)
    {
      or_get_align32 (&buf);
    }

end:

  rec->length = (int) (buf.ptr - buf.buffer);
  rec->type = REC_HOME;

  return rc;
}

/*
 * btree_read_record () -
 *   return:
 *   btid(in):
 *   pgptr(in):
 *   rec(in):
 *   key(in):
 *   rec_header(in):
 *   node_type(in):
 *   clear_key(in):
 *   offset(in):
 *   copy_key(in):
 *
 * Note: This routine reads a btree record for both leaf and non leaf pages.
 *
 * copy_key indicates whether strings should be malloced and copied
 * or just returned via pointer.  offset will point to the oid(s) following
 * the key for leaf pages.  clear_key will indicate whether the key needs
 * to be cleared via pr_clear_value by the caller.  If this record is
 * a leaf record, rec_header will be filled in with the LEAF_REC,
 * otherwise, rec_header will be filled in with the NON_LEAF_REC for this
 * record.
 *
 * If you don't want to actually read the key (possibly incurring a
 * malloc for the string), you can send a NULL pointer for the key.
 * index_readval() will do the right thing and simply skip the key in this case.
 */
void
btree_read_record (THREAD_ENTRY * thread_p, BTID_INT * btid,
		   PAGE_PTR pgptr, RECDES * rec, DB_VALUE * key,
		   void *rec_header, int node_type,
		   bool * clear_key, int *offset, int copy_key,
		   BTREE_SCAN * bts)
{
  int n_prefix = COMMON_PREFIX_UNKNOWN;

  assert (pgptr != NULL);
  assert (rec != NULL);
  assert (rec->type == REC_HOME);
  assert (bts == NULL || bts->common_prefix == -1
	  || bts->common_prefix == btree_node_common_prefix (thread_p, btid,
							     pgptr));

  if (bts != NULL)
    {
      n_prefix = bts->common_prefix;
    }

  btree_read_record_helper (thread_p, btid, rec, key, rec_header, node_type,
			    clear_key, offset, copy_key);

  if (key != NULL &&
      node_type == BTREE_LEAF_NODE &&
      !btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_KEY) &&
      !btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_FENCE))
    {
      if (n_prefix == COMMON_PREFIX_UNKNOWN)
	{
	  /* recalculate n_prefix */
	  n_prefix = btree_node_common_prefix (thread_p, btid, pgptr);
	}

      assert (n_prefix >= 0);

      if (n_prefix > 0)
	{
	  RECDES peek_rec;
	  DB_VALUE lf_key, result;
	  bool lf_clear_key;
	  LEAF_REC leaf_pnt;
	  int dummy_offset;

	  (void) spage_get_record (pgptr, 1, &peek_rec, PEEK);
	  btree_read_record_helper (thread_p, btid, &peek_rec, &lf_key,
				    &leaf_pnt, BTREE_LEAF_NODE,
				    &lf_clear_key, &dummy_offset,
				    PEEK_KEY_VALUE);

	  assert (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE));
	  assert (DB_VALUE_TYPE (&lf_key) == DB_TYPE_MIDXKEY);

	  pr_midxkey_add_prefix (&result, &lf_key, key, n_prefix);

	  btree_clear_key_value (clear_key, key);
	  btree_clear_key_value (&lf_clear_key, &lf_key);

	  *key = result;
	  *clear_key = true;
	}
    }
}

/*
 * btree_read_record_helper () -
 *   return:
 *   btid(in):
 *   rec(in):
 *   key(in):
 *   rec_header(in):
 *   node_type(in):
 *   clear_key(in):
 *   offset(in):
 *   copy_key(in):
 *
 */
static void
btree_read_record_helper (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  RECDES * rec, DB_VALUE * key, void *rec_header,
			  int node_type, bool * clear_key, int *offset,
			  int copy_key)
{
  OR_BUF buf;
  VPID overflow_vpid;
  char *copy_key_buf;
  int copy_key_buf_len;
  int rc = NO_ERROR;
  int key_type = BTREE_NORMAL_KEY;
  LEAF_REC *leaf_rec = NULL;
  NON_LEAF_REC *non_leaf_rec = NULL;

  assert (rec != NULL);
  assert (rec->type == REC_HOME);

  if (key != NULL)
    {
      db_make_null (key);
    }

  *clear_key = false;

#if !defined(NDEBUG)
  if (!rec || !rec->data)
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_read_record_helper: null node header pointer."
		    " Operation Ignored.");
      return;
    }
#endif

  assert (rec_header != NULL);

  or_init (&buf, rec->data, rec->length);

  /*
   * Find the beginning position of the key within the record and read
   * the key length.
   */
  if (node_type == BTREE_LEAF_NODE)
    {
      leaf_rec = (LEAF_REC *) rec_header;
      leaf_rec->key_len = -1;

      or_advance (&buf, OR_OID_SIZE);	/* skip instance oid */

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS))
	    {
	      or_advance (&buf, OR_OID_SIZE);	/* skip class oid */
	    }
	}

      if (mvcc_Enabled)
	{
	  if (btree_leaf_key_oid_is_mvcc_flaged (rec->data,
						 BTREE_LEAF_OID_HAS_MVCC_INSID))
	    {
	      or_advance (&buf, OR_BIGINT_SIZE);	/* skip class oid */
	    }

	  if (btree_leaf_key_oid_is_mvcc_flaged (rec->data,
						 BTREE_LEAF_OID_HAS_MVCC_DELID))
	    {
	      or_advance (&buf, OR_BIGINT_SIZE);	/* skip class oid */
	    }
	}

      if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_KEY))
	{
	  key_type = BTREE_OVERFLOW_KEY;
	}

      if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	{
	  btree_leaf_get_vpid_for_overflow_oids (rec, &leaf_rec->ovfl);
	}
      else
	{
	  VPID_SET_NULL (&leaf_rec->ovfl);
	}

      assert (leaf_rec->key_len == -1);
    }
  else
    {
      non_leaf_rec = (NON_LEAF_REC *) rec_header;
      non_leaf_rec->key_len = -1;

      if (btree_read_fixed_portion_of_non_leaf_record_from_orbuf (&buf,
								  non_leaf_rec)
	  != NO_ERROR)
	{
	  return;
	}

      if (non_leaf_rec->key_len < 0)
	{
	  key_type = BTREE_OVERFLOW_KEY;
	}
    }

  if (key_type == BTREE_NORMAL_KEY)
    {				/* key is within page */
      TP_DOMAIN *key_domain;
      PR_TYPE *pr_type;
      char *old_ptr;

      if (node_type == BTREE_LEAF_NODE)
	{
	  key_domain = btid->key_type;
	}
      else
	{
	  key_domain = btid->nonleaf_key_type;
	}
      pr_type = key_domain->type;

      copy_key_buf = NULL;
      copy_key_buf_len = 0;

      /*
       * When we read the key, must copy in two cases:
       *   1) we are told to via the copy_key flag, or 2) it is a set.
       */
      if (key != NULL && copy_key == COPY_KEY_VALUE)
	{
	  *clear_key = true;
	}
      else
	{
	  *clear_key = false;
	}

      if (*clear_key)
	{			/* need to copy the key */
	  if (btid->copy_buf != NULL)
	    {			/* check for copy_buf */
	      if (pr_type->id == DB_TYPE_MIDXKEY
		  || QSTR_IS_ANY_CHAR_OR_BIT (pr_type->id))
		{		/* check for the key type */
		  /* read key_val image into the copy_buf */
		  copy_key_buf = btid->copy_buf;
		  copy_key_buf_len = btid->copy_buf_len;
		}
	    }
	}

      old_ptr = buf.ptr;
      (*(pr_type->index_readval)) (&buf, key, key_domain, -1, *clear_key,
				   copy_key_buf, copy_key_buf_len);
      assert (CAST_BUFLEN (buf.ptr - old_ptr) > 0);
      if (key != NULL)
	{
	  assert (!DB_IS_NULL (key));
	}

      if (node_type == BTREE_LEAF_NODE)
	{
	  leaf_rec->key_len = CAST_BUFLEN (buf.ptr - old_ptr);
	}
      else
	{
	  non_leaf_rec->key_len = CAST_BUFLEN (buf.ptr - old_ptr);
	}
    }
  else
    {
      /* follow the chain of overflow key page pointers and fetch key */
      overflow_vpid.pageid = or_get_int (&buf, &rc);
      if (rc == NO_ERROR)
	{
	  overflow_vpid.volid = or_get_short (&buf, &rc);
	}
      if (rc != NO_ERROR)
	{
	  assert (false);

	  if (key != NULL)
	    {
	      db_make_null (key);
	    }

	  return;
	}

      if (key != NULL)
	{
	  if (btree_load_overflow_key (thread_p, btid,
				       &overflow_vpid, key,
				       node_type) != NO_ERROR)
	    {
	      db_make_null (key);
	    }

	  assert (!DB_IS_NULL (key));

	  /* we always clear overflow keys */
	  *clear_key = true;
	}
      else
	{
	  /* we aren't copying the key so they don't have to clear it */
	  *clear_key = false;
	}
    }

  buf.ptr = PTR_ALIGN (buf.ptr, OR_INT_SIZE);

  *offset = CAST_STRLEN (buf.ptr - buf.buffer);
}

/*
 * btree_dump_root_header () -
 *   return:
 *   rec(in):
 */
static void
btree_dump_root_header (FILE * fp, PAGE_PTR page_ptr)
{
  OR_BUF buf;
  BTREE_ROOT_HEADER *root_header = NULL;
  TP_DOMAIN *key_type;

  root_header = btree_get_root_header (page_ptr);
  if (root_header == NULL)
    {
      fprintf (fp, "btree_dump_root_header: get root header failure\n");

      return;
    }

  or_init (&buf, root_header->packed_key_domain, -1);
  key_type = or_get_domain (&buf, NULL, NULL);

  fprintf (fp, "==============    R O O T    P A G E   ================\n\n");
  fprintf (fp, " Key_Type: %s\n", pr_type_name (TP_DOMAIN_TYPE (key_type)));
  fprintf (fp, " Num OIDs: %d, Num NULLs: %d, Num keys: %d\n",
	   root_header->num_oids, root_header->num_nulls,
	   root_header->num_keys);
  fprintf (fp, " Topclass_oid: (%d %d %d)\n",
	   root_header->topclass_oid.volid, root_header->topclass_oid.pageid,
	   root_header->topclass_oid.slotid);
  fprintf (fp, " Unique: ");
  if (BTREE_IS_UNIQUE (root_header->unique_pk))
    {
      fprintf (fp, "1");
      if (BTREE_IS_PRIMARY_KEY (root_header->unique_pk))
	{
	  fprintf (fp, " (Primary Key)");
	}
    }
  else
    {
      assert (!BTREE_IS_PRIMARY_KEY (root_header->unique_pk));
      fprintf (fp, "0");
    }
  fprintf (fp, "\n");
  fprintf (fp, " OVFID: %d|%d\n", root_header->ovfid.fileid,
	   root_header->ovfid.volid);
  fprintf (fp, " Btree Revision Level: %d\n", root_header->rev_level);
  fprintf (fp, " Reserved: %d\n", root_header->reverse_reserved);	/* unused */
  fprintf (fp, "\n");
}

/*
 * btree_dump_key () -
 *   return:
 *   key(in):
 */
void
btree_dump_key (FILE * fp, DB_VALUE * key)
{
  DB_TYPE key_type = DB_VALUE_DOMAIN_TYPE (key);
  PR_TYPE *pr_type = PR_TYPE_FROM_ID (key_type);

  assert (pr_type != NULL);

  if (pr_type)
    {
#if 1
      fprintf (fp, " ");
      (*(pr_type->fptrfunc)) (fp, key);
      fprintf (fp, " ");

#else /* debug routine - DO NOT DELETE ME */
      /* simple dump for debug */
      /* dump '      ' to ' +' */
      char buff[4096];
      int i, j, c;

      (*(pr_type->sptrfunc)) (key, buff, 4096);

      for (i = 0, j = 0; i < 4096; i++, j++)
	{
	  buff[j] = buff[i];

	  if (buff[i] == 0)
	    {
	      break;
	    }

	  c = 0;
	  while (buff[i] == ' ' && i > 1 && buff[i - 1] == ' ')
	    {
	      c++;
	      i++;
	    }

	  if (c > 1)
	    {
	      j++;
	      buff[j] = '+';
	    }
	}

      fprintf (fp, " ");
      fprintf (fp, buff);
      fprintf (fp, " ");
#endif
    }
}

/*
 * btree_dump_leaf_record () -
 *   return: nothing
 *   btid(in): B+tree index identifier
 *   rec(in): Pointer to a record in a leaf page of the tree
 *   n(in): Indentation left margin (number of preceding blanks)
 *
 * Note: Dumps the content of a leaf record, namely key and the set of
 * values for the key.
 */
static void
btree_dump_leaf_record (THREAD_ENTRY * thread_p, FILE * fp,
			BTID_INT * btid, RECDES * rec, int depth)
{
  OR_BUF buf;
  LEAF_REC leaf_record = {
    {
     NULL_PAGEID, NULL_VOLID}, 0
  };
  int i, k, oid_cnt;
  OID class_oid;
  OID oid;
  int key_len, offset;
  VPID overflow_vpid;
  DB_VALUE key;
  bool clear_key;
  int oid_size;
  MVCCID mvccid;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  /* output the leaf record structure content */
  btree_print_space (fp, depth * 4 + 1);

  btree_read_record_helper (thread_p, btid, rec, &key, &leaf_record,
			    BTREE_LEAF_NODE, &clear_key, &offset,
			    PEEK_KEY_VALUE);
  key_len = btree_get_key_length (&key);

  fprintf (fp, "Key_Len: %d Ovfl_Page: {%d , %d} ",
	   key_len, leaf_record.ovfl.volid, leaf_record.ovfl.pageid);

  if (leaf_record.key_len < 0)
    {
      key_len = DISK_VPID_SIZE;
    }

  fprintf (fp, "Key: ");
  btree_dump_key (fp, &key);

  btree_clear_key_value (&clear_key, &key);

  overflow_vpid = leaf_record.ovfl;

  /* output the values */
  fprintf (fp, "  Values: ");

  oid_cnt = btree_leaf_get_num_oids (rec, offset, BTREE_LEAF_NODE, oid_size);
  fprintf (fp, "Oid_Cnt: %d ", oid_cnt);

  /* output first oid */
  btree_leaf_get_first_oid (btid, rec, &oid, &class_oid, NULL);
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      fprintf (fp, " (%d %d %d : %d, %d, %d) ",
	       class_oid.volid, class_oid.pageid, class_oid.slotid,
	       oid.volid, oid.pageid, oid.slotid);
    }
  else
    {
      fprintf (fp, " (%d, %d, %d) ", oid.volid, oid.pageid, oid.slotid);
    }

  /* output MVCCIDs of first OID */
  if (mvcc_Enabled == true)
    {
      fprintf (fp, " (");
      if (btree_leaf_key_oid_is_mvcc_flaged (rec->data,
					     BTREE_LEAF_OID_HAS_MVCC_INSID))
	{
	  OR_GET_MVCCID (rec->data + oid_size, &mvccid);
	  fprintf (fp, "%lld", (long long) mvccid);
	  if (btree_leaf_key_oid_is_mvcc_flaged (rec->data,
						 BTREE_LEAF_OID_HAS_MVCC_DELID))
	    {
	      OR_GET_MVCCID (rec->data + oid_size + OR_MVCCID_SIZE, &mvccid);
	      fprintf (fp, ", %lld", (long long) mvccid);
	    }
	}
      else
	{
	  OR_GET_MVCCID (rec->data + oid_size, &mvccid);
	  fprintf (fp, "%lld", (long long) mvccid);
	}

      fprintf (fp, ")  ");
    }

  /* output remainder OIDs */
  or_init (&buf, rec->data + offset, rec->length - offset);
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      for (k = 1; k < oid_cnt; k++)
	{
	  /* values stored within the record */
	  if (k % 2 == 0)
	    {
	      fprintf (fp, "\n");
	    }

	  or_get_oid (&buf, &oid);
	  or_get_oid (&buf, &class_oid);

	  fprintf (fp, " (%d %d %d : %d, %d, %d) ",
		   class_oid.volid, class_oid.pageid, class_oid.slotid,
		   oid.volid, oid.pageid, oid.slotid);

	  fprintf (fp, " (");

	  (void) or_get_mvccid (&buf, &mvccid);
	  fprintf (fp, "%lld", (long long) mvccid);

	  (void) or_get_mvccid (&buf, &mvccid);
	  fprintf (fp, "%lld", (long long) mvccid);

	  fprintf (fp, ")  ");
	}
    }
  else
    {
      for (k = 1; k < oid_cnt; k++)
	{
	  /* values stored within the record */
	  if (k % 4 == 0)
	    {
	      fprintf (fp, "\n");
	    }

	  or_get_oid (&buf, &oid);
	  if (mvcc_Enabled == true)
	    {
	      oid.volid = oid.volid & (~BTREE_LEAF_OID_MVCC_MASK);
	    }

	  fprintf (fp, " (%d, %d, %d) ", oid.volid, oid.pageid, oid.slotid);

	  fprintf (fp, " (");
	  if (btree_leaf_key_oid_is_mvcc_flaged (buf.ptr - OR_OID_SIZE,
						 BTREE_LEAF_OID_HAS_MVCC_INSID))
	    {
	      (void) or_get_mvccid (&buf, &mvccid);
	      fprintf (fp, "%lld", (long long) mvccid);

	      if (btree_leaf_key_oid_is_mvcc_flaged (buf.ptr - OR_OID_SIZE -
						     OR_MVCCID_SIZE,
						     BTREE_LEAF_OID_HAS_MVCC_DELID))
		{
		  (void) or_get_mvccid (&buf, &mvccid);
		  fprintf (fp, ", %lld", (long long) mvccid);
		}
	    }
	  else
	    if (btree_leaf_key_oid_is_mvcc_flaged
		(buf.ptr - OR_OID_SIZE, BTREE_LEAF_OID_HAS_MVCC_DELID))
	    {
	      (void) or_get_mvccid (&buf, &mvccid);
	      fprintf (fp, "%lld", (long long) mvccid);
	    }
	  fprintf (fp, ")  ");
	}
    }

  if (!VPID_ISNULL (&overflow_vpid))
    {
      /* record has an overflow page continuation */
      RECDES overflow_rec;
      PAGE_PTR overflow_page_ptr = NULL;
      char overflow_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];

      overflow_rec.area_size = DB_PAGESIZE;
      overflow_rec.data = PTR_ALIGN (overflow_rec_buf, BTREE_MAX_ALIGN);

      fprintf (fp,
	       "\n\n=======    O V E R F L O W   P A G E S     =========\n");
      fflush (fp);

      /* get all the overflow pages and output their value content */
      while (!VPID_ISNULL (&overflow_vpid))
	{
	  fprintf (fp, "\n ------ Overflow Page {%d , %d} \n",
		   overflow_vpid.volid, overflow_vpid.pageid);
	  overflow_page_ptr = pgbuf_fix (thread_p, &overflow_vpid, OLD_PAGE,
					 PGBUF_LATCH_READ,
					 PGBUF_UNCONDITIONAL_LATCH);
	  if (overflow_page_ptr == NULL)
	    {
	      return;
	    }

	  btree_get_next_overflow_vpid (overflow_page_ptr, &overflow_vpid);

	  (void) spage_get_record (overflow_page_ptr, 1, &overflow_rec, COPY);

	  assert (mvcc_Enabled || oid_size == OR_OID_SIZE);
	  oid_cnt = btree_leaf_get_num_oids (&overflow_rec, 0,
					     BTREE_OVERFLOW_NODE, oid_size);
	  or_init (&buf, overflow_rec.data, overflow_rec.length);
	  fprintf (fp, "Oid_Cnt: %d ", oid_cnt);

	  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
	  assert (mvcc_Enabled == true
		  || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));
	  for (i = 0; i < oid_cnt; i++)
	    {
	      if (i % 4 == 0)
		{
		  fprintf (stdout, "\n");
		}

	      or_get_oid (&buf, &oid);

	      fprintf (fp, " (%d, %d, %d) ", oid.volid, oid.pageid,
		       oid.slotid);

	      if (BTREE_IS_UNIQUE (btid->unique_pk))
		{
		  or_get_oid (&buf, &class_oid);
		  fprintf (fp, " (%d, %d, %d) ", class_oid.volid,
			   class_oid.pageid, class_oid.slotid);
		}

	      fprintf (fp, " (");
	      if (mvcc_Enabled == true)
		{
		  (void) or_get_mvccid (&buf, &mvccid);
		  fprintf (fp, "%lld", (long long) mvccid);

		  (void) or_get_mvccid (&buf, &mvccid);
		  fprintf (fp, ", %lld", (long long) mvccid);
		}
	      fprintf (fp, ")  ");
	    }

	  pgbuf_unfix_and_init (thread_p, overflow_page_ptr);
	}
    }

  fprintf (fp, "\n");
  fflush (fp);
}

/*
 * btree_dump_non_leaf_record () -
 *   return: void
 *   btid(in):
 *   rec(in): Pointer to a record in a non_leaf page
 *   n(in): Indentation left margin (number of preceding blanks)
 *   print_key(in):
 *
 * Note: Dumps the content of a nonleaf record, namely key and child
 * page identifier.
 */
static void
btree_dump_non_leaf_record (THREAD_ENTRY * thread_p, FILE * fp,
			    BTID_INT * btid, RECDES * rec, int depth,
			    int print_key)
{
  NON_LEAF_REC non_leaf_record;
  int key_len, offset;
  DB_VALUE key;
  bool clear_key;

  VPID_SET_NULL (&(non_leaf_record.pnt));

  /* output the non_leaf record structure content */
  btree_read_record_helper (thread_p, btid, rec, &key, &non_leaf_record,
			    BTREE_NON_LEAF_NODE, &clear_key, &offset,
			    PEEK_KEY_VALUE);

  btree_print_space (fp, depth * 4);
  fprintf (fp, "Child_Page: {%d , %d} ",
	   non_leaf_record.pnt.volid, non_leaf_record.pnt.pageid);

  if (print_key)
    {
      key_len = btree_get_key_length (&key);
      fprintf (fp, "Key_Len: %d  Key: ", key_len);
      btree_dump_key (fp, &key);
    }
  else
    {
      fprintf (fp, "No Key");
    }

  btree_clear_key_value (&clear_key, &key);

  fprintf (fp, "\n");
  fflush (fp);
}

/*
 * btree_get_new_page () - GET a NEW PAGE for the B+tree index
 *   return: The pointer to a newly allocated page for the given
 *           B+tree or NULL.
 *           The parameter vpid is set to the page identifier.
 *   btid(in): B+tree index identifier
 *   vpid(out): Set to the page identifier for the newly allocated page
 *   near_vpid(in): A page identifier that may be used in a nearby page
 *                  allocation. (It may be ignored.)
 *
 * Note: Allocates a new page for the B+tree
 */
static PAGE_PTR
btree_get_new_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
		    VPID * vpid, VPID * near_vpid)
{
  PAGE_PTR page_ptr = NULL;
  unsigned short alignment;

  alignment = BTREE_MAX_ALIGN;
  if (file_alloc_pages
      (thread_p, &(btid->sys_btid->vfid), vpid, 1, near_vpid,
       btree_initialize_new_page, (void *) (&alignment)) == NULL)
    {
      return NULL;
    }

  /*
   * Note: we fetch the page as old since it was initialized during the
   * allocation by btree_initialize_new_page, therefore, we care about
   * the current content of the page.
   */
  page_ptr = pgbuf_fix (thread_p, vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			PGBUF_UNCONDITIONAL_LATCH);
  if (page_ptr == NULL)
    {
      (void) file_dealloc_page (thread_p, &btid->sys_btid->vfid, vpid);
      return NULL;
    }

  (void) pgbuf_check_page_ptype (thread_p, page_ptr, PAGE_BTREE);

  return page_ptr;
}

/*
 * btree_dealloc_page () -
 *   return: NO_ERROR or error code
 *
 *   btid(in):
 *   vpid(in):
 */
static int
btree_dealloc_page (THREAD_ENTRY * thread_p, BTID_INT * btid, VPID * vpid)
{
  int error = NO_ERROR;

  if (log_start_system_op (thread_p) == NULL)
    {
      return ER_FAILED;
    }

  error = file_dealloc_page (thread_p, &btid->sys_btid->vfid, vpid);

  if (btree_is_new_file (btid))
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
    }
  else
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
    }

  return error;
}

/*
 * btree_initialize_new_page () -
 *   return: bool
 *   vfid(in): File where the new page belongs
 *   file_type(in):
 *   vpid(in): The new page
 *   ignore_npages(in): Number of contiguous allocated pages
 *                      (Ignored in this function. We allocate only one page)
 *   ignore_args(in): More arguments to function.
 *                    Ignored at this moment.
 *
 * Note: Initialize a newly allocated btree page.
 */
static bool
btree_initialize_new_page (THREAD_ENTRY * thread_p, const VFID * vfid,
			   const FILE_TYPE file_type, const VPID * vpid,
			   INT32 ignore_npages, void *args)
{
  PAGE_PTR pgptr;
  unsigned short alignment;

  /*
   * fetch and initialize the new page. The parameter UNANCHORED_KEEP_
   * SEQUENCE indicates that the order of records will be preserved
   * during insertions and deletions.
   */

  pgptr = pgbuf_fix (thread_p, vpid, NEW_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
  if (pgptr == NULL)
    {
      return false;
    }

  (void) pgbuf_set_page_ptype (thread_p, pgptr, PAGE_BTREE);

  alignment = *((unsigned short *) args);
  spage_initialize (thread_p, pgptr, UNANCHORED_KEEP_SEQUENCE,
		    alignment, DONT_SAFEGUARD_RVSPACE);
  log_append_redo_data2 (thread_p, RVBT_GET_NEWPAGE, vfid, pgptr, -1,
			 sizeof (alignment), &alignment);
  pgbuf_set_dirty (thread_p, pgptr, FREE);

  return true;
}

/*
 * btree_search_nonleaf_page () -
 *   return: NO_ERROR
 *   btid(in):
 *   page_ptr(in): Pointer to the non_leaf page to be searched
 *   key(in): Key to find
 *   slot_id(out): Set to the record number that contains the key
 *   child_vpid(out): Set to the child page identifier to be followed,
 *                    or NULL_PAGEID
 *
 * Note: Binary search the page to locate the record that contains the
 * child page pointer to be followed to locate the key, and
 * return the page identifier for this child page.
 */
static int
btree_search_nonleaf_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
			   PAGE_PTR page_ptr, DB_VALUE * key,
			   INT16 * slot_id, VPID * child_vpid)
{
  int key_cnt, offset;
  int c;
  bool clear_key;
  /* the start position of non-equal-value column */
  int start_col, left_start_col, right_start_col;
  INT16 left, right;
  INT16 middle = 0;
  DB_VALUE temp_key;
  RECDES rec;
  NON_LEAF_REC non_leaf_rec;

  /* initialize child page identifier */
  VPID_SET_NULL (child_vpid);

#if !defined(NDEBUG)
  if (!page_ptr || !key || DB_IS_NULL (key))
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_search_nonleaf_page: null page/key pointer."
		    " Operation Ignored.");
      return ER_FAILED;
    }
#endif

  key_cnt = btree_node_number_of_keys (page_ptr);
  assert (key_cnt > 0);

  if (key_cnt <= 0)
    {				/* node record underflow */
      er_log_debug (ARG_FILE_LINE,
		    "btree_search_nonleaf_page: node key count underflow: %d",
		    key_cnt);
      return ER_FAILED;
    }

  if (key_cnt == 1)
    {
      /*
       * node has dummy neg-inf keys, but a child page pointer
       * So, follow this pointer
       */
      if (spage_get_record (page_ptr, 1, &rec, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}

      btree_read_fixed_portion_of_non_leaf_record (&rec, &non_leaf_rec);

      *slot_id = 1;
      *child_vpid = non_leaf_rec.pnt;

      return NO_ERROR;
    }

  /* binary search the node to find the child page pointer to be followed */
  c = 0;

  /* for non-compressed midxkey; separator is not compressed */
  left_start_col = right_start_col = 0;

  left = 2;			/* Ignore dummy key (neg-inf or 1st key) */
  right = key_cnt;

  while (left <= right)
    {
      middle = CEIL_PTVDIV ((left + right), 2);	/* get the middle record */

      assert (middle > 0);
      if (spage_get_record (page_ptr, middle, &rec, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}

      btree_read_record_helper (thread_p, btid, &rec, &temp_key,
				&non_leaf_rec, BTREE_NON_LEAF_NODE,
				&clear_key, &offset, PEEK_KEY_VALUE);

      if (DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY)
	{
	  start_col = MIN (left_start_col, right_start_col);
	}

      c = btree_compare_key (key, &temp_key, btid->key_type,
			     1, 1, &start_col);
      assert (c == DB_LT || c == DB_EQ || c == DB_GT);

      btree_clear_key_value (&clear_key, &temp_key);

      if (c == DB_UNK)
	{
	  return ER_FAILED;
	}

      if (c == 0)
	{
	  /* child page to be followed has been found */
	  *slot_id = middle;
	  *child_vpid = non_leaf_rec.pnt;

	  return NO_ERROR;
	}
      else if (c < 0)
	{
	  right = middle - 1;
	  right_start_col = start_col;
	}
      else
	{
	  left = middle + 1;
	  left_start_col = start_col;
	}
    }

  if (c < 0)
    {
      /* child page is the one pointed by the record left to the middle  */
      assert (middle - 1 > 0);
      if (spage_get_record (page_ptr, middle - 1, &rec, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}

      btree_read_fixed_portion_of_non_leaf_record (&rec, &non_leaf_rec);

      *slot_id = middle - 1;
      *child_vpid = non_leaf_rec.pnt;
    }
  else
    {
      /* child page is the one pointed by the middle record */
      *slot_id = middle;
      *child_vpid = non_leaf_rec.pnt;
    }

  return NO_ERROR;
}

/*
 * btree_search_leaf_page () -
 *   return: int false: key does not exists, true: key exists
 *           (if error, false and slot_id = NULL_SLOTID)
 *   btid(in):
 *   page_ptr(in): Pointer to the leaf page to be searched
 *   key(in): Key to search
 *   slot_id(out): Set to the record number that contains the key if key is
 *                 found, or the record number in which the key should have
 *                 been located if it doesn't exist
 *
 * Note: Binary search the page to find the location of the key.
 * If the key does not exist, it returns the location where it
 * should have been located.
 */
static bool
btree_search_leaf_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
			PAGE_PTR page_ptr, DB_VALUE * key, INT16 * slot_id)
{
  int key_cnt, offset;
  int c, n_prefix;
  bool clear_key;
  /* the start position of non-equal-value column */
  int start_col, left_start_col, right_start_col;
  INT16 left, right, middle;
  DB_VALUE temp_key;
  RECDES rec;
  LEAF_REC leaf_pnt;

  *slot_id = NULL_SLOTID;

#if !defined(NDEBUG)
  if (!key || DB_IS_NULL (key))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_NULL_KEY, 0);
      return false;
    }
#endif

  key_cnt = btree_node_number_of_keys (page_ptr);
  assert (key_cnt >= 0);

  c = 0;
  middle = 0;

  /* for compressed midxkey */
  n_prefix = btree_node_common_prefix (thread_p, btid, page_ptr);
  left_start_col = right_start_col = n_prefix;

  if (key_cnt < 0)
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_search_leaf_page: node key count underflow: %d.",
		    key_cnt);
      return false;
    }

  /*
   * binary search the node to find if the key exists and in which record it
   * exists, or if it doesn't exist , the in which record it should have been
   * located to preserve the order of keys
   */

  left = 1;
  right = key_cnt;

  while (left <= right)
    {
      middle = CEIL_PTVDIV ((left + right), 2);	/* get the middle record */

      assert (middle > 0);
      if (spage_get_record (page_ptr, middle, &rec, PEEK) != S_SUCCESS)
	{
	  er_log_debug (ARG_FILE_LINE,
			"btree_search_leaf_page: sp_getrec fails for middle record.");
	  return false;
	}

      btree_read_record_helper (thread_p, btid, &rec, &temp_key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_key,
				&offset, PEEK_KEY_VALUE);

      if (DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY)
	{
	  start_col = MIN (left_start_col, right_start_col);
	}

      c = btree_compare_key (key, &temp_key, btid->key_type,
			     1, 1, &start_col);
      assert (c == DB_LT || c == DB_EQ || c == DB_GT);

      btree_clear_key_value (&clear_key, &temp_key);

      if (c == DB_UNK)
	{
	  *slot_id = NULL_SLOTID;
	  return false;
	}

      /* skip lower fence key */
      if (c == DB_EQ && middle == 1)
	{
	  if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	    {
	      c = DB_GT;
	    }
	}

      if (c == DB_EQ)
	{
	  assert (!btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE));

	  /* key exists in the middle record */
	  *slot_id = middle;
	  return true;
	}
      else if (c < 0)
	{
	  right = middle - 1;
	  right_start_col = start_col;
	}
      else
	{
	  left = middle + 1;
	  left_start_col = start_col;
	}
    }

  if (c < 0)
    {
      /* key not exists, should be inserted in the current middle record */
      *slot_id = middle;

#if 0
      er_log_debug (ARG_FILE_LINE,
		    "btree_search_leaf_page: key not exists, "
		    "should be inserted in the current middle record.");
#endif

      return false;
    }
  else
    {
      /* key not exists, should be inserted in the record right to the middle */
      *slot_id = middle + 1;

#if 0
      er_log_debug (ARG_FILE_LINE,
		    "btree_search_leaf_page: key not exists, "
		    "should be inserted in the record right to the middle.");
#endif

      return false;
    }
}

/*
 * xbtree_add_index () - ADD (create) a new B+tree INDEX
 *   return: BTID * (btid on success and NULL on failure)
 *   btid(out): Set to the created B+tree index identifier
 *              (Note: btid->vfid.volid should be set by the caller)
 *   key_type(in): Key type of the index to be created.
 *   class_oid(in): OID of the class for which the index is created
 *   attr_id(in): Identifier of the attribute of the class for which the
 *                index is created.
 *   unique_pk(in):
 *   num_oids(in):
 *   num_nulls(in):
 *   num_keys(in):
 *
 * Note: Creates the B+tree index. A file identifier (index identifier)
 * is defined on the given volume. This identifier is used by
 * insertion, deletion and search routines, for the created
 * index. The routine allocates the root page of the tree and
 * initializes the root header information.
 */
BTID *
xbtree_add_index (THREAD_ENTRY * thread_p, BTID * btid, TP_DOMAIN * key_type,
		  OID * class_oid, int attr_id, int unique_pk,
		  int num_oids, int num_nulls, int num_keys)
{
  BTREE_ROOT_HEADER root_header_info, *root_header = NULL;
  VPID vpid;
  PAGE_PTR page_ptr = NULL;
  FILE_BTREE_DES btree_descriptor;
  bool is_file_created = false;
  unsigned short alignment;

  root_header = &root_header_info;

  if (class_oid == NULL || OID_ISNULL (class_oid))
    {
      goto error;
    }

  /* create a file descriptor */
  COPY_OID (&btree_descriptor.class_oid, class_oid);
  btree_descriptor.attr_id = attr_id;

  /* create a file descriptor, allocate and initialize the root page */
  if (file_create (thread_p, &btid->vfid, 2, FILE_BTREE, &btree_descriptor,
		   &vpid, 1) == NULL)
    {
      goto error;
    }
  is_file_created = true;

  vacuum_log_add_dropped_file (thread_p, &btid->vfid,
			       VACUUM_LOG_ADD_DROPPED_FILE_UNDO);

  alignment = BTREE_MAX_ALIGN;
  if (btree_initialize_new_page (thread_p, &btid->vfid, FILE_BTREE, &vpid, 1,
				 (void *) &alignment) == false)
    {
      goto error;
    }

  /*
   * Note: we fetch the page as old since it was initialized by
   * btree_initialize_new_page; we want the current contents of
   * the page.
   */
  page_ptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			PGBUF_UNCONDITIONAL_LATCH);
  if (page_ptr == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, page_ptr, PAGE_BTREE);

  /* form the root header information */
  root_header->node.split_info.pivot = 0.0f;
  root_header->node.split_info.index = 0;
  VPID_SET_NULL (&(root_header->node.prev_vpid));
  VPID_SET_NULL (&(root_header->node.next_vpid));
  root_header->node.node_level = 1;
  root_header->node.max_key_len = 0;

  if (unique_pk)
    {
      root_header->num_oids = num_oids;
      root_header->num_nulls = num_nulls;
      root_header->num_keys = num_keys;
      root_header->unique_pk = unique_pk;

      assert (BTREE_IS_UNIQUE (root_header->unique_pk));
      assert (BTREE_IS_PRIMARY_KEY (root_header->unique_pk)
	      || !BTREE_IS_PRIMARY_KEY (root_header->unique_pk));
    }
  else
    {
      root_header->num_oids = -1;
      root_header->num_nulls = -1;
      root_header->num_keys = -1;
      root_header->unique_pk = 0;
    }

  COPY_OID (&(root_header->topclass_oid), class_oid);

  VFID_SET_NULL (&(root_header->ovfid));
  root_header->rev_level = BTREE_CURRENT_REV_LEVEL;

  root_header->reverse_reserved = 0;	/* unused */

  if (btree_init_root_header
      (thread_p, &btid->vfid, page_ptr, root_header, key_type) != NO_ERROR)
    {
      goto error;
    }

  pgbuf_set_dirty (thread_p, page_ptr, FREE);
  page_ptr = NULL;

  /* set the B+tree index identifier */
  btid->root_pageid = vpid.pageid;

  return btid;

error:
  if (page_ptr)
    {
      pgbuf_unfix_and_init (thread_p, page_ptr);
    }

  if (is_file_created)
    {
      (void) file_destroy (thread_p, &btid->vfid);
    }

  VFID_SET_NULL (&btid->vfid);
  btid->root_pageid = NULL_PAGEID;

  return NULL;
}

/*
 * xbtree_delete_index () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *
 * Note: Removes the B+tree index. All pages associated with the index
 * are removed. After the routine is called, the index identifier
 * is not valid any more.
 */
int
xbtree_delete_index (THREAD_ENTRY * thread_p, BTID * btid)
{
  PAGE_PTR P = NULL;
  VPID P_vpid;
  BTREE_ROOT_HEADER *root_header = NULL;
  VFID ovfid;
  int ret = NO_ERROR;
  LOG_MVCC_BTID_UNIQUE_STATS *unique_stats = NULL;

  P_vpid.volid = btid->vfid.volid;	/* read the root page */
  P_vpid.pageid = btid->root_pageid;
  P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		 PGBUF_UNCONDITIONAL_LATCH);
  if (P == NULL)
    {
      return (((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret);
    }

  (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

  /* read the header record */
  root_header = btree_get_root_header (P);
  if (root_header == NULL)
    {
      pgbuf_unfix_and_init (thread_p, P);
      return (((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret);
    }

  ovfid = root_header->ovfid;

  pgbuf_unfix_and_init (thread_p, P);

  /* mark the statistics associated with deelted B-tree as deleted */
  unique_stats =
    logtb_mvcc_find_class_oid_btid_stats (thread_p,
					  &root_header->topclass_oid, btid,
					  true);
  if (unique_stats != NULL)
    {
      unique_stats->deleted = true;
    }

  btid->root_pageid = NULL_PAGEID;

  vacuum_log_add_dropped_file (thread_p, &btid->vfid,
			       VACUUM_LOG_ADD_DROPPED_FILE_POSTPONE);

  ret = file_destroy (thread_p, &btid->vfid);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (!VFID_ISNULL (&ovfid))
    {
      ret = file_destroy (thread_p, &ovfid);
    }

  return ret;
}

/*
 * btree_generate_prefix_domain () -
 *   return:
 *   btid(in):
 *
 * Note: This routine returns a varying domain of the same precision
 * for fixed domains which are one of the string types.  For all other
 * domains, it returns the same domain.
 */
TP_DOMAIN *
btree_generate_prefix_domain (BTID_INT * btid)
{
  TP_DOMAIN *domain = btid->key_type;
  TP_DOMAIN *var_domain;
  DB_TYPE dbtype;
  DB_TYPE vartype;

  dbtype = TP_DOMAIN_TYPE (domain);

  /* varying domains did not come into use until btree revision level 1 */
  if (!pr_is_variable_type (dbtype) && pr_is_string_type (dbtype))
    {
      switch (dbtype)
	{
	case DB_TYPE_CHAR:
	  vartype = DB_TYPE_VARCHAR;
	  break;
	case DB_TYPE_NCHAR:
	  vartype = DB_TYPE_VARNCHAR;
	  break;
	case DB_TYPE_BIT:
	  vartype = DB_TYPE_VARBIT;
	  break;
	default:
	  assert (false);
#if defined(CUBRID_DEBUG)
	  printf ("Corrupt domain in btree_generate_prefix_domain\n");
#endif /* CUBRID_DEBUG */
	  return NULL;
	}

      var_domain = tp_domain_resolve (vartype, domain->class_mop,
				      domain->precision, domain->scale,
				      domain->setdomain,
				      domain->collation_id);
    }
  else
    {
      var_domain = domain;
    }

  return var_domain;
}

/*
 * btree_glean_root_header_info () -
 *   return:
 *   root_header(in):
 *   btid(in):
 *
 * Note: This captures the interesting header info into the BTID_INT structure.
 */
int
btree_glean_root_header_info (THREAD_ENTRY * thread_p,
			      BTREE_ROOT_HEADER * root_header,
			      BTID_INT * btid)
{
  int rc;
  OR_BUF buf;

  rc = NO_ERROR;

  btid->unique_pk = root_header->unique_pk;

  or_init (&buf, root_header->packed_key_domain, -1);
  btid->key_type = or_get_domain (&buf, NULL, NULL);

  COPY_OID (&btid->topclass_oid, &root_header->topclass_oid);

  btid->ovfid = root_header->ovfid;	/* structure copy */

  /* check for the last element domain of partial-key is desc;
   * for btree_range_search, part_key_desc is re-set at btree_initialize_bts
   */
  btid->part_key_desc = 0;

  /* init index key copy_buf info */
  btid->copy_buf = NULL;
  btid->copy_buf_len = 0;

  /* this must be discovered after the Btree key_type */
  btid->nonleaf_key_type = btree_generate_prefix_domain (btid);

  btid->rev_level = root_header->rev_level;

  btid->new_file = (file_is_new_file (thread_p, &(btid->sys_btid->vfid))
		    == FILE_NEW_FILE) ? 1 : 0;

  return rc;
}

/*
 * xbtree_delete_with_unique_key -
 *   btid (in):
 *   class_oid (in):
 *   key_value (in):
 *   return:
 */
int
xbtree_delete_with_unique_key (THREAD_ENTRY * thread_p, BTID * btid,
			       OID * class_oid, DB_VALUE * key_value)
{
  int error = NO_ERROR;
  OID unique_oid;
  HEAP_SCANCACHE scan_cache;
  BTREE_SEARCH r;

  r = xbtree_find_unique (thread_p, btid, S_DELETE, key_value,
			  class_oid, &unique_oid, true);

  if (r == BTREE_KEY_FOUND)
    {
      HFID hfid;
      int force_count;

      error = heap_get_hfid_from_class_oid (thread_p, class_oid, &hfid);
      if (error != NO_ERROR)
	{
	  return error;
	}

      error = heap_scancache_start_modify (thread_p, &scan_cache, &hfid,
					   class_oid, SINGLE_ROW_DELETE,
					   NULL);
      if (error != NO_ERROR)
	{
	  return error;
	}

      error = locator_delete_force (thread_p, &hfid, &unique_oid, btid, false,
				    true, SINGLE_ROW_DELETE, &scan_cache,
				    &force_count, NULL);
      if (error == NO_ERROR)
	{
	  /* monitor */
	  mnt_qm_deletes (thread_p);
	}

      heap_scancache_end_modify (thread_p, &scan_cache);
    }
  else if (r == BTREE_KEY_NOTFOUND)
    {
      btree_set_unknown_key_error (thread_p, btid, key_value,
				   "xbtree_delete_with_unique_key: "
				   "current key not found.");
      error = ER_BTREE_UNKNOWN_KEY;
    }
  else
    {
      /* r == BTREE_ERROR_OCCURRED */
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      error = (error == NO_ERROR) ? ER_FAILED : error;
    }

  return error;
}

/*
 * xbtree_non_mvcc_find_unique () - non MVCC find unique key in btree
 *   return:
 *   btid(in):
 *   key(in):
 *   class_oid(in):
 *   oid(in):
 *   is_all_class_srch(in):
 *
 * Note: This returns the oid for the given key.  It assumes that the
 * btree is unique.
 */
static BTREE_SEARCH
xbtree_non_mvcc_find_unique (THREAD_ENTRY * thread_p, BTID * btid,
			     SCAN_OPERATION_TYPE scan_op_type,
			     DB_VALUE * key, OID * class_oid,
			     OID * oid, bool is_all_class_srch)
{
  BTREE_SCAN btree_scan;
  int oid_cnt = 0;
  BTREE_SEARCH status;
  INDX_SCAN_ID index_scan_id;
  /* Unique btree can have at most 1 OID for a key */
  OID temp_oid[2];
  KEY_VAL_RANGE key_val_range;

  BTREE_INIT_SCAN (&btree_scan);

  scan_init_index_scan (&index_scan_id, temp_oid, NULL);

  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      status = BTREE_KEY_NOTFOUND;
    }
  else
    {
      assert (!pr_is_set_type (DB_VALUE_DOMAIN_TYPE (key)));

      PR_SHARE_VALUE (key, &key_val_range.key1);
      PR_SHARE_VALUE (key, &key_val_range.key2);
      key_val_range.range = GE_LE;
      key_val_range.num_index_term = 0;

      /* TODO: unique with prefix length */
      oid_cnt =
	btree_keyval_search (thread_p, btid, scan_op_type,
			     &btree_scan, &key_val_range, class_oid,
			     index_scan_id.oid_list.oidp,
			     2 * sizeof (OID), NULL, &index_scan_id,
			     is_all_class_srch);

      if (DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY
	  && key->data.midxkey.domain == NULL)
	{
	  /* set the appropriate domain, as it might be needed for printing
	   * if the unique constraint is violated */
	  key->data.midxkey.domain = btree_scan.btid_int.key_type;
	}

      btree_scan_clear_key (&btree_scan);

      if (oid_cnt == 1)
	{
	  COPY_OID (oid, index_scan_id.oid_list.oidp);
	  status = BTREE_KEY_FOUND;
	}
      else if (oid_cnt == 0)
	{
	  status = BTREE_KEY_NOTFOUND;
	}
      else if (oid_cnt < 0)
	{
	  status = BTREE_ERROR_OCCURRED;
	}
      else
	{
	  /* (oid_cnt > 1) */
	  COPY_OID (oid, index_scan_id.oid_list.oidp);
	  status = BTREE_ERROR_OCCURRED;
	}
    }

  /* do not use copy_buf for key-val scan, only use for key-range scan */

  return status;
}

/*
 * xbtree_find_multi_uniques () - search a list of unique indexes for
 *				  specified values
 * return : search return code
 * thread_p (in)  : handler thread
 * class_oid (in) : class oid
 * pruning_type (in) : pruning type
 * btids (in)	  : indexes to search
 * values (in)	  : values to search for
 * count (in)	  : number of indexes
 * op_type (in)	  : operation for which this search is performed
 * oids (in/out)  : found OIDs
 * oids_count (in): number of OIDs found
 *
 * Note: This function assumes that the indexes it searches are unique
 *  indexes. If the operation is S_UPDATE, this function stops at the first
 *  oid it finds in order to comply with the behavior of ON DUPLICATE KEY
 *  UPDATE statement.
 */
BTREE_SEARCH
xbtree_find_multi_uniques (THREAD_ENTRY * thread_p, OID * class_oid,
			   int pruning_type, BTID * btids, DB_VALUE * values,
			   int count, SCAN_OPERATION_TYPE op_type,
			   OID ** oids, int *oids_count)
{
  BTREE_SEARCH result = BTREE_KEY_FOUND;
  OID *found_oids = NULL;
  int i, idx, error = NO_ERROR;
  bool is_at_least_one = false;
  BTID pruned_btid;
  OID pruned_class_oid;
  HFID pruned_hfid;
  PRUNING_CONTEXT context;
  bool is_global_index = false;

  partition_init_pruning_context (&context);

  found_oids = (OID *) db_private_alloc (thread_p, count * sizeof (OID));
  if (found_oids == NULL)
    {
      return BTREE_ERROR_OCCURRED;
    }

  if (pruning_type != DB_NOT_PARTITIONED_CLASS)
    {
      error = partition_load_pruning_context (thread_p, class_oid,
					      pruning_type, &context);
      if (error != NO_ERROR)
	{
	  result = BTREE_ERROR_OCCURRED;
	  goto error_return;
	}
    }

  idx = 0;
  for (i = 0; i < count; i++)
    {
      is_global_index = false;
      BTID_COPY (&pruned_btid, &btids[i]);
      COPY_OID (&pruned_class_oid, class_oid);
      if (pruning_type)
	{
	  /* At this point, there's no way of knowing if btids[i] refers a
	   * global unique index or a local one. Perform pruning and use the
	   * partition BTID, even if it is the same one as the BTID we
	   * received
	   */
	  error = partition_prune_unique_btid (&context, &values[i],
					       &pruned_class_oid,
					       &pruned_hfid, &pruned_btid,
					       &is_global_index);
	  if (error != NO_ERROR)
	    {
	      result = BTREE_ERROR_OCCURRED;
	      goto error_return;
	    }
	}

      result = xbtree_find_unique (thread_p, &pruned_btid, op_type,
				   &values[i], &pruned_class_oid,
				   &found_oids[idx], is_global_index);

      if (result == BTREE_KEY_FOUND)
	{
	  if (pruning_type == DB_PARTITION_CLASS)
	    {
	      if (is_global_index)
		{
		  OID found_class_oid;

		  /* find the class oid for found oid */
		  if (heap_get_class_oid (thread_p, &found_class_oid,
					  &found_oids[idx],
					  NEED_SNAPSHOT) == NULL)
		    {
		      error = ER_FAILED;
		      result = BTREE_ERROR_OCCURRED;
		      goto error_return;
		    }
		  if (!OID_EQ (&found_class_oid, class_oid))
		    {
		      /* Found a constraint violation on a different
		       * partition: throw invalid partition
		       */
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			      ER_INVALID_DATA_FOR_PARTITION, 0);
		      error = ER_INVALID_DATA_FOR_PARTITION;
		      result = BTREE_ERROR_OCCURRED;
		      goto error_return;
		    }
		}
	      else if (!OID_EQ (&pruned_class_oid, class_oid))
		{
		  /* Found a constraint violation on a different partition:
		   * throw invalid partition
		   */
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_INVALID_DATA_FOR_PARTITION, 0);
		  error = ER_INVALID_DATA_FOR_PARTITION;
		  result = BTREE_ERROR_OCCURRED;
		  goto error_return;
		}
	    }
	  is_at_least_one = true;
	  idx++;
	  if (op_type == S_UPDATE)
	    {
	      break;
	    }
	}
      else if (result == BTREE_ERROR_OCCURRED)
	{
	  goto error_return;
	}
      else
	{
	  /* result == BTREE_KEY_NOTFOUND */
	  ;			/* just go to the next one */
	}
    }

  if (is_at_least_one)
    {
      *oids_count = idx;
      *oids = found_oids;
      result = BTREE_KEY_FOUND;
    }
  else
    {
      result = BTREE_KEY_NOTFOUND;
      db_private_free_and_init (thread_p, found_oids);
      *oids = NULL;
      *oids_count = 0;
    }
  partition_clear_pruning_context (&context);
  return result;

error_return:
  if (found_oids != NULL)
    {
      db_private_free_and_init (thread_p, found_oids);
    }
  *oids_count = 0;
  *oids = NULL;
  partition_clear_pruning_context (&context);
  return BTREE_ERROR_OCCURRED;
}

/*
 * btree_find_foreign_key () -
 *   return:
 *   btid(in):
 *   key(in):
 *   class_oid(in):
 */
int
btree_find_foreign_key (THREAD_ENTRY * thread_p, BTID * btid,
			DB_VALUE * key, OID * class_oid)
{
  BTREE_SCAN btree_scan;
  int oid_cnt = 0;
  INDX_SCAN_ID index_scan_id;
  OID oid_buf[2];
  KEY_VAL_RANGE key_val_range;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;

  if (mvcc_Enabled)
    {
      mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;
    }

  BTREE_INIT_SCAN (&btree_scan);

  scan_init_index_scan (&index_scan_id, oid_buf, &mvcc_snapshot_dirty);
  index_scan_id.mvcc_need_locks = true;

  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      return 0;
    }

  assert (!pr_is_set_type (DB_VALUE_DOMAIN_TYPE (key)));

  PR_SHARE_VALUE (key, &key_val_range.key1);
  PR_SHARE_VALUE (key, &key_val_range.key2);
  key_val_range.range = GE_LE;
  key_val_range.num_index_term = 0;

  oid_cnt =
    btree_keyval_search (thread_p, btid, S_SELECT, &btree_scan,
			 &key_val_range, class_oid,
			 index_scan_id.oid_list.oidp, 2 * sizeof (OID), NULL,
			 &index_scan_id, false);

  btree_scan_clear_key (&btree_scan);

  return oid_cnt;
}

/*
 * btree_scan_clear_key () -
 *   return:
 *   btree_scan(in):
 */
void
btree_scan_clear_key (BTREE_SCAN * btree_scan)
{
  btree_clear_key_value (&btree_scan->clear_cur_key, &btree_scan->cur_key);
}

/*
 * btree_is_unique_type () -
 *   return: Whether the given BTREE_TYPE corresponds to a unique index B+tree
 *   type(in):
 */
bool
btree_is_unique_type (BTREE_TYPE type)
{
  if (type == BTREE_UNIQUE || type == BTREE_REVERSE_UNIQUE
      || type == BTREE_PRIMARY_KEY)
    {
      return true;
    }
  return false;
}

/*
 * xbtree_class_test_unique () -
 *   return: int
 *   buf(in):
 *   buf_size(in):
 *
 * Note: Return NO_ERROR if the btrees given are unique.
 * Return ER_BTREE_UNIQUE_FAILED if one of the unique tests failed.
 * This is used for interpreter and xasl batch checking of uniqueness.
 */
int
xbtree_class_test_unique (THREAD_ENTRY * thread_p, char *buf, int buf_size)
{
  int status = NO_ERROR;
  char *bufp, *buf_endptr;
  BTID btid;

  bufp = buf;
  buf_endptr = (buf + buf_size);

  while ((bufp < buf_endptr) && (status == NO_ERROR))
    {
      /* unpack the BTID */
      bufp = or_unpack_btid (bufp, &btid);

      /* check if the btree is unique */
      if ((status == NO_ERROR) && (xbtree_test_unique (thread_p, &btid) != 1))
	{
	  BTREE_SET_UNIQUE_VIOLATION_ERROR (thread_p, NULL, NULL,
					    NULL, &btid, NULL);
	  status = ER_BTREE_UNIQUE_FAILED;
	}
    }

  return status;
}

/*
 * xbtree_test_unique () -
 *   return: int
 *   btid(in): B+tree index identifier
 *
 * Note: Return 1 (true) if the index is unique, return 0 if
 * the index is not unique, return -1 if the btree isn't
 * keeping track of unique statistics (a regular, plain jane btree).
 */
static int
xbtree_test_unique (THREAD_ENTRY * thread_p, BTID * btid)
{
  INT32 num_oids, num_nulls, num_keys;

  if (btree_get_unique_statistics (thread_p, btid, &num_oids,
				   &num_nulls, &num_keys) != NO_ERROR)
    {
      return 0;
    }

  if (num_nulls == -1)
    {
      return -1;
    }
  else if ((num_nulls + num_keys) != num_oids)
    {
      assert (false);
      return 0;
    }
  else
    {
      return 1;
    }
}

/*
 * xbtree_get_unique_pk () -
 *   return:
 *   btid(in):
 */
int
xbtree_get_unique_pk (THREAD_ENTRY * thread_p, BTID * btid)
{
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  int unique_pk;

  root_vpid.pageid = btid->root_pageid;
  root_vpid.volid = btid->vfid.volid;

  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      return 0;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      return 0;
    }

  unique_pk = root_header->unique_pk;

  pgbuf_unfix_and_init (thread_p, root);

  return unique_pk;
}

/*
 * btree_get_unique_statistics_for_count () - gets unique statistics
 *   return:
 *   btid(in): B+tree index identifier
 *   oid_cnt(in/out): no. of oids
 *   null_cnt(in/out): no. of nulls
 *   key_cnt(in/out): no. of keys
 *
 * Note: In MVCC the statistics are taken from memory structures. In non-mvcc
 *	 from B-tree header
 */
int
btree_get_unique_statistics_for_count (THREAD_ENTRY * thread_p, BTID * btid,
				       int *oid_cnt, int *null_cnt,
				       int *key_cnt)
{
  LOG_MVCC_BTID_UNIQUE_STATS *unique_stats = NULL, *part_stats = NULL;

  if (mvcc_Enabled)
    {
      unique_stats =
	logtb_mvcc_search_btid_stats_all_classes (thread_p, btid, true);
      if (unique_stats == NULL)
	{
	  return ER_FAILED;
	}
      *oid_cnt =
	unique_stats->tran_stats.num_oids +
	unique_stats->global_stats.num_oids;
      *key_cnt =
	unique_stats->tran_stats.num_keys +
	unique_stats->global_stats.num_keys;
      *null_cnt =
	unique_stats->tran_stats.num_nulls +
	unique_stats->global_stats.num_nulls;
    }
  else
    {
      return btree_get_unique_statistics (thread_p, btid, oid_cnt, null_cnt,
					  key_cnt);
    }

  return NO_ERROR;
}

/*
 * btree_get_unique_statistics () -
 *   returns: NO_ERROR
 *   btid(in):
 *   oid_cnt(in):
 *   null_cnt(in):
 *   key_cnt(in):
 *
 * Note: Reads the unique btree statistics from the root header.  If
 * the btree is not a unique btree, all the stats will be -1.
 */
int
btree_get_unique_statistics (THREAD_ENTRY * thread_p, BTID * btid,
			     int *oid_cnt, int *null_cnt, int *key_cnt)
{
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  int ret;

  root_vpid.pageid = btid->root_pageid;
  root_vpid.volid = btid->vfid.volid;

  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      return (((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret);
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      return (((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret);
    }

  *oid_cnt = root_header->num_oids;
  *null_cnt = root_header->num_nulls;
  *key_cnt = root_header->num_keys;

  pgbuf_unfix_and_init (thread_p, root);

  return NO_ERROR;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * btree_get_subtree_stats () -
 *   return: NO_ERROR
 *   btid(in):
 *   pg_ptr(in):
 *   env(in):
 */
static int
btree_get_subtree_stats (THREAD_ENTRY * thread_p, BTID_INT * btid,
			 PAGE_PTR page_ptr, BTREE_STATS_ENV * stats_env)
{
  int key_cnt;
  int i, j;
  NON_LEAF_REC non_leaf_rec;
  VPID page_vpid;
  PAGE_PTR page = NULL;
  RECDES rec;
  DB_DOMAIN *key_type;
  int ret = NO_ERROR;
  BTREE_NODE_HEADER *header = NULL;

  key_type = btid->key_type;
  key_cnt = btree_node_number_of_keys (page_ptr);

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_get_subtree_stats: get node header failure: %d",
		    key_cnt);
      goto exit_on_error;
    }

  if (header->node_level > 1)	/* BTREE_NON_LEAF_NODE */
    {
      if (key_cnt < 0)
	{
	  er_log_debug (ARG_FILE_LINE,
			"btree_get_subtree_stats: node key count"
			" underflow: %d", key_cnt);
	  goto exit_on_error;
	}

      /*
       * traverse all the subtrees of this non_leaf page and accumulate
       * the statistical data in the environment structure
       */
      for (i = 1; i <= key_cnt + 1; i++)
	{
	  if (spage_get_record (page_ptr, i, &rec, PEEK) != S_SUCCESS)
	    {
	      goto exit_on_error;
	    }

	  btree_read_fixed_portion_of_non_leaf_record (&rec, &non_leaf_rec);
	  page_vpid = non_leaf_rec.pnt;

	  page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			    PGBUF_UNCONDITIONAL_LATCH);
	  if (page == NULL)
	    {
	      goto exit_on_error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, page, PAGE_BTREE);

	  ret = btree_get_subtree_stats (thread_p, btid, page, stats_env);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  pgbuf_unfix_and_init (thread_p, page);
	}

      stats_env->stat_info->height++;
    }
  else
    {
      DB_VALUE key, elem;
      LEAF_REC leaf_rec;
      bool clear_key;
      int offset;
      int k;
      DB_MIDXKEY *midxkey;
      int prev_j_index, prev_k_index;
      char *prev_j_ptr, *prev_k_ptr;

      stats_env->stat_info->leafs++;
      stats_env->stat_info->keys += key_cnt;
      stats_env->stat_info->height = 1;	/* init */

      if (stats_env->pkeys)
	{
	  if (TP_DOMAIN_TYPE (key_type) != DB_TYPE_MIDXKEY)
	    {
	      /* single column index */
	      stats_env->stat_info->pkeys[0] += key_cnt;
	    }
	  else
	    {
	      for (i = 1; i <= key_cnt; i++)
		{
		  if (spage_get_record (page_ptr, i, &rec, PEEK) != S_SUCCESS)
		    {
		      goto exit_on_error;
		    }

		  /* read key-value */
		  btree_read_record (thread_p, page_ptr, btid, &rec, &key,
				     &leaf_rec, BTREE_LEAF_NODE, &clear_key,
				     &offset, PEEK_KEY_VALUE, NULL);

		  /* extract the sequence of the key-value */
		  midxkey = DB_GET_MIDXKEY (&key);
		  if (midxkey == NULL)
		    {
		      goto exit_on_error;
		    }

		  prev_j_index = 0;
		  prev_j_ptr = NULL;

		  assert (stats_env->stat_info->pkeys_size <=
			  BTREE_STATS_PKEYS_NUM);
		  for (j = 0; j < stats_env->stat_info->pkeys_size; j++)
		    {
		      /* extract the element of the midxkey */
		      ret = pr_midxkey_get_element_nocopy (midxkey, j, &elem,
							   &prev_j_index,
							   &prev_j_ptr);
		      if (ret != NO_ERROR)
			{
			  goto exit_on_error;
			}

		      if (tp_value_compare (&(stats_env->pkeys[j]), &elem,
					    0, 1) != DB_EQ)
			{
			  /* found different value */
			  stats_env->stat_info->pkeys[j] += 1;
			  pr_clear_value (&(stats_env->pkeys[j]));	/* clear saved */
			  pr_clone_value (&elem, &(stats_env->pkeys[j]));	/* save */

			  /* propagate to the following partial key-values */
			  prev_k_index = prev_j_index;
			  prev_k_ptr = prev_j_ptr;

			  assert (stats_env->stat_info->pkeys_size <=
				  BTREE_STATS_PKEYS_NUM);
			  for (k = j + 1;
			       k < stats_env->stat_info->pkeys_size; k++)
			    {
			      ret = pr_midxkey_get_element_nocopy (midxkey,
								   k,
								   &elem,
								   &prev_k_index,
								   &prev_k_ptr);
			      if (ret != NO_ERROR)
				{
				  goto exit_on_error;
				}

			      stats_env->stat_info->pkeys[k]++;
			      pr_clear_value (&(stats_env->pkeys[k]));
			      pr_clone_value (&elem, &(stats_env->pkeys[k]));	/* save */
			    }

			  /* go to the next key */
			  break;
			}
		    }

		  btree_clear_key_value (&clear_key, &key);
		}
	    }
	}
    }

  stats_env->stat_info->pages++;

  return ret;

exit_on_error:

  if (page)
    {
      pgbuf_unfix_and_init (thread_p, page);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}
#endif

/*
 * btree_get_stats_midxkey () -
 *   return: NO_ERROR
 *   thread_p(in);
 *   env(in/out): Structure to store and return the statistical information
 *   midxkey(in);
 */
static int
btree_get_stats_midxkey (THREAD_ENTRY * thread_p, BTREE_STATS_ENV * env,
			 DB_MIDXKEY * midxkey)
{
  int i, k;
  int prev_i_index, prev_k_index;
  char *prev_i_ptr, *prev_k_ptr;
  DB_VALUE elem;
  int ret = NO_ERROR;

  if (midxkey == NULL)
    {
      assert_release (false);
      goto exit_on_error;
    }

  prev_i_index = 0;
  prev_i_ptr = NULL;
  for (i = 0; i < env->pkeys_val_num; i++)
    {
      /* extract the element of the key */
      ret = pr_midxkey_get_element_nocopy (midxkey, i, &elem,
					   &prev_i_index, &prev_i_ptr);
      if (ret != NO_ERROR)
	{
	  assert_release (false);
	  goto exit_on_error;
	}

      if (tp_value_compare (&(env->pkeys_val[i]), &elem, 0, 1) != DB_EQ)
	{
	  /* found different value */
	  env->stat_info->pkeys[i]++;
	  pr_clear_value (&(env->pkeys_val[i]));	/* clear saved */
	  pr_clone_value (&elem, &(env->pkeys_val[i]));	/* save */

	  /* propagate to the following partial key-values */
	  prev_k_index = prev_i_index;
	  prev_k_ptr = prev_i_ptr;
	  for (k = i + 1; k < env->pkeys_val_num; k++)
	    {
	      ret = pr_midxkey_get_element_nocopy (midxkey,
						   k,
						   &elem,
						   &prev_k_index,
						   &prev_k_ptr);
	      if (ret != NO_ERROR)
		{
		  assert_release (false);
		  goto exit_on_error;
		}

	      env->stat_info->pkeys[k]++;
	      pr_clear_value (&(env->pkeys_val[k]));	/* clear saved */
	      pr_clone_value (&elem, &(env->pkeys_val[k]));	/* save */
	    }

	  break;
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_get_stats_key () -
 *   return: NO_ERROR
 *   thread_p(in);
 *   env(in/out): Structure to store and return the statistical information
 */
static int
btree_get_stats_key (THREAD_ENTRY * thread_p, BTREE_STATS_ENV * env,
		     MVCC_SNAPSHOT * mvcc_snapshot)
{
  BTREE_SCAN *BTS;
  RECDES rec;
  DB_VALUE key_value;
  LEAF_REC leaf_pnt;
  bool clear_key = false;
  int offset;
  int ret = NO_ERROR;

  assert (env != NULL);

  db_make_null (&key_value);

  if (mvcc_snapshot != NULL)
    {
      BTREE_SEARCH result = BTREE_KEY_NOTFOUND;
      int max_visible_oids = 1;
      int num_visible_oids = 0;

      BTS = &(env->btree_scan);

      if (BTS->C_page == NULL)
	{
	  goto exit_on_error;
	}

      assert (BTS->slot_id > 0);
      if (spage_get_record (BTS->C_page, BTS->slot_id, &rec, PEEK) !=
	  S_SUCCESS)
	{
	  goto exit_on_error;
	}

      /* filter out fence_key */
      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{
	  goto count_keys;
	}

      /* read key-value */
      assert (clear_key == false);

      (void) btree_read_record (thread_p, &BTS->btid_int, BTS->C_page, &rec,
				&key_value, (void *) &leaf_pnt,
				BTREE_LEAF_NODE, &clear_key, &offset,
				PEEK_KEY_VALUE, NULL);
      if (DB_IS_NULL (&key_value))
	{
	  goto exit_on_error;
	}

      max_visible_oids = 1;
      num_visible_oids =
	btree_leaf_mvcc_get_num_visible_oids (thread_p, &BTS->btid_int, &rec,
					      offset, BTREE_LEAF_NODE,
					      &max_visible_oids,
					      mvcc_snapshot);
      if (num_visible_oids < 0)
	{
	  goto exit_on_error;
	}

      if (num_visible_oids == 0)
	{
	  if (VPID_ISNULL (&leaf_pnt.ovfl))
	    {
	      /* nothing to do - the key is dead */
	      goto end;
	    }

	  if (btree_mvcc_get_num_visible_oids_from_all_ovf (thread_p,
							    &BTS->btid_int,
							    &leaf_pnt.ovfl,
							    &num_visible_oids,
							    &max_visible_oids,
							    mvcc_snapshot)
	      != NO_ERROR)
	    {

	      goto exit_on_error;
	    }

	  if (num_visible_oids == 0)
	    {
	      /* nothing to do - the key is dead */
	      goto end;
	    }
	}
    }

count_keys:
  env->stat_info->keys++;

  if (env->pkeys_val_num <= 0)
    {
      ;				/* do not request pkeys info; go ahead */
    }
  else if (env->pkeys_val_num == 1)
    {
      /* single column index */
      env->stat_info->pkeys[0]++;
    }
  else
    {
      /* multi column index */

      if (mvcc_snapshot != NULL)
	{
	  /* filter out fence_key */
	  if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	    {
	      assert (ret == NO_ERROR);

	      env->stat_info->keys--;
	      assert (env->stat_info->keys >= 0);

	      goto end;
	    }

	  /* key_value already computed */
	  assert (!DB_IS_NULL (&key_value));

	  /* get pkeys info */
	  ret = btree_get_stats_midxkey (thread_p, env,
					 DB_GET_MIDXKEY (&key_value));
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  goto end;
	}

      BTS = &(env->btree_scan);

      if (BTS->C_page == NULL)
	{
	  goto exit_on_error;
	}

      assert (BTS->slot_id > 0);
      if (spage_get_record (BTS->C_page, BTS->slot_id, &rec, PEEK) !=
	  S_SUCCESS)
	{
	  goto exit_on_error;
	}

      /* filter out fence_key */
      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{
	  assert (ret == NO_ERROR);

	  env->stat_info->keys--;
	  assert (env->stat_info->keys >= 0);

	  goto end;
	}

      /* read key-value */

      assert (clear_key == false);

      (void) btree_read_record (thread_p, &BTS->btid_int, BTS->C_page, &rec,
				&key_value, (void *) &leaf_pnt,
				BTREE_LEAF_NODE, &clear_key, &offset,
				PEEK_KEY_VALUE, NULL);
      if (DB_IS_NULL (&key_value))
	{
	  goto exit_on_error;
	}

      /* get pkeys info */
      ret = btree_get_stats_midxkey (thread_p, env,
				     DB_GET_MIDXKEY (&key_value));
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

end:

  if (clear_key)
    {
      pr_clear_value (&key_value);
      clear_key = false;
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_get_stats_with_AR_sampling () - Do Acceptance/Rejection Sampling
 *   return: NO_ERROR
 *   env(in/out): Structure to store and return the statistical information
 */
static int
btree_get_stats_with_AR_sampling (THREAD_ENTRY * thread_p,
				  BTREE_STATS_ENV * env)
{
  BTREE_SCAN *BTS;
  int n, i;
  bool found;
  int key_cnt;
  int exp_ratio;
  int ret = NO_ERROR;
#if !defined(NDEBUG)
  BTREE_NODE_HEADER *header = NULL;
#endif

  assert (env != NULL);
  assert (env->stat_info != NULL);

  BTS = &(env->btree_scan);
  BTS->use_desc_index = 0;	/* init */

  for (n = 0; n < STATS_SAMPLING_THRESHOLD; n++)
    {
      if (env->stat_info->leafs >= STATS_SAMPLING_LEAFS_MAX)
	{
	  break;		/* found all samples */
	}

      BTS->C_page = btree_find_AR_sampling_leaf (thread_p,
						 BTS->btid_int.sys_btid,
						 &BTS->C_vpid,
						 env->stat_info, &found);
      if (BTS->C_page == NULL)
	{
	  goto exit_on_error;
	}

      /* found sampling leaf page */
      if (found)
	{
	  key_cnt = btree_node_number_of_keys (BTS->C_page);
	  assert_release (key_cnt >= 0);

#if !defined(NDEBUG)
	  header = btree_get_node_header (BTS->C_page);

	  assert (header != NULL);
	  assert (header->node_level == 1);	/* BTREE_LEAF_NODE */
#endif

	  if (key_cnt > 0)
	    {
	      env->stat_info->leafs++;

	      BTS->slot_id = 1;
	      BTS->oid_pos = 0;

	      assert_release (BTS->slot_id <= key_cnt);

	      for (i = 0; i < key_cnt; i++)
		{
		  ret = btree_get_stats_key (thread_p, env, NULL);
		  if (ret != NO_ERROR)
		    {
		      goto exit_on_error;
		    }

		  /* get the next index record */
		  ret = btree_find_next_index_record (thread_p, BTS);
		  if (ret != NO_ERROR)
		    {
		      goto exit_on_error;
		    }
		}
	    }
	}

      if (BTS->P_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, BTS->P_page);
	}

      if (BTS->C_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, BTS->C_page);
	}

      if (BTS->O_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, BTS->O_page);
	}
    }				/* for (n = 0; ... ) */

  /* apply distributed expension */
  if (env->stat_info->leafs > 0)
    {
      exp_ratio = env->stat_info->pages / env->stat_info->leafs;

      env->stat_info->leafs *= exp_ratio;
      if (env->stat_info->leafs < 0)
	{
	  env->stat_info->leafs = INT_MAX;
	}

      env->stat_info->keys *= exp_ratio;
      if (env->stat_info->keys < 0)
	{
	  env->stat_info->keys = INT_MAX;
	}

      for (i = 0; i < env->pkeys_val_num; i++)
	{
	  env->stat_info->pkeys[i] *= exp_ratio;
	  if (env->stat_info->pkeys[i] < 0)
	    {			/* multiply-overflow defence */
	      env->stat_info->pkeys[i] = INT_MAX;
	    }
	}
    }

end:

  if (BTS->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->P_page);
    }

  if (BTS->C_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->C_page);
    }

  if (BTS->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->O_page);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_get_stats_with_fullscan () - Do Full Scan
 *   return: NO_ERROR
 *   env(in/out): Structure to store and return the statistical information
 */
static int
btree_get_stats_with_fullscan (THREAD_ENTRY * thread_p, BTREE_STATS_ENV * env)
{
  BTREE_SCAN *BTS;
  VPID C_vpid;			/* vpid of current leaf page */
  int ret = NO_ERROR;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;

  assert (env != NULL);
  assert (env->stat_info != NULL);

  if (mvcc_Enabled)
    {
      mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
      if (mvcc_snapshot == NULL)
	{
	  goto exit_on_error;
	}
    }

  BTS = &(env->btree_scan);
  BTS->use_desc_index = 0;	/* get the left-most leaf page */

  ret = btree_find_lower_bound_leaf (thread_p, BTS, env->stat_info);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  VPID_SET_NULL (&C_vpid);	/* init */

  while (!BTREE_END_OF_SCAN (BTS))
    {
      /* move on another leaf page */
      if (!VPID_EQ (&(BTS->C_vpid), &C_vpid))
	{
	  VPID_COPY (&C_vpid, &(BTS->C_vpid));	/* keep current leaf vpid */

	  env->stat_info->leafs++;
	}

      ret = btree_get_stats_key (thread_p, env, mvcc_snapshot);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* get the next index record */
      ret = btree_find_next_index_record (thread_p, BTS);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

end:

  if (BTS->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->P_page);
    }

  if (BTS->C_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->C_page);
    }

  if (BTS->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->O_page);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_get_stats () - Get Statistical Information about the B+tree index
 *   return: NO_ERROR
 *   stat_info_p(in/out): Structure to store and
 *                        return the statistical information
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * Note: Computes and returns statistical information about B+tree
 * which consist of the number of leaf pages, total number of
 * pages, number of keys and the height of the tree.
 */
int
btree_get_stats (THREAD_ENTRY * thread_p, BTREE_STATS * stat_info_p,
		 bool with_fullscan)
{
  int npages;
  BTREE_STATS_ENV stat_env, *env;
  VPID root_vpid;
  PAGE_PTR root_page_ptr = NULL;
  DB_TYPE dom_type;
  BTREE_ROOT_HEADER *root_header = NULL;
  int i;
  int ret = NO_ERROR;

  assert_release (stat_info_p != NULL);
  assert_release (!BTID_IS_NULL (&stat_info_p->btid));

  npages = file_get_numpages (thread_p, &(stat_info_p->btid.vfid));
  if (npages < 0)
    {
      npages = INT_MAX;
    }
  assert_release (npages >= 1);

  /* For the optimization of the sampling,
   * if the btree file has currently the same pages as we gathered
   * statistics, we guess the btree file has not been modified;
   * So, we take current stats as it is
   */
  if (!with_fullscan)
    {
      /* check if the stats has been gathered */
      if (stat_info_p->keys > 0)
	{
	  /* guess the stats has not been modified */
	  if (npages == stat_info_p->pages)
	    {
	      return NO_ERROR;
	    }
	}
    }

  /* set environment variable */
  env = &stat_env;
  BTREE_INIT_SCAN (&(env->btree_scan));
  env->btree_scan.btid_int.sys_btid = &(stat_info_p->btid);
  env->stat_info = stat_info_p;
  env->pkeys_val_num = stat_info_p->pkeys_size;

  assert (env->pkeys_val_num <= BTREE_STATS_PKEYS_NUM);
  for (i = 0; i < env->pkeys_val_num; i++)
    {
      DB_MAKE_NULL (&(env->pkeys_val[i]));
    }

  root_vpid.pageid = env->stat_info->btid.root_pageid;	/* read root page */
  root_vpid.volid = env->stat_info->btid.vfid.volid;

  root_page_ptr = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			     PGBUF_UNCONDITIONAL_LATCH);
  if (root_page_ptr == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root_page_ptr, PAGE_BTREE);

  root_header = btree_get_root_header (root_page_ptr);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  ret = btree_glean_root_header_info (thread_p, root_header,
				      &(env->btree_scan.btid_int));
  if (ret != NO_ERROR)
    {
      pgbuf_unfix_and_init (thread_p, root_page_ptr);
      goto exit_on_error;
    }

  pgbuf_unfix_and_init (thread_p, root_page_ptr);

  dom_type = TP_DOMAIN_TYPE (env->btree_scan.btid_int.key_type);
  if (env->pkeys_val_num <= 0)
    {
      /* do not request pkeys info; go ahead */
      if (!tp_valid_indextype (dom_type) && dom_type != DB_TYPE_MIDXKEY)
	{
	  assert_release (false);
	  goto exit_on_error;
	}
    }
  else if (env->pkeys_val_num == 1)
    {
      /* single column index */
      if (!tp_valid_indextype (dom_type))
	{
	  assert_release (false);
	  goto exit_on_error;
	}
    }
  else
    {
      /* multi column index */
      if (dom_type != DB_TYPE_MIDXKEY)
	{
	  assert_release (false);
	  goto exit_on_error;
	}
    }

  /* initialize environment stat_info structure */
  env->stat_info->pages = npages;
  env->stat_info->leafs = 0;
  env->stat_info->height = 0;
  env->stat_info->keys = 0;

  for (i = 0; i < env->pkeys_val_num; i++)
    {
      env->stat_info->pkeys[i] = 0;	/* clear old stats */
    }

  if (with_fullscan || npages <= STATS_SAMPLING_THRESHOLD)
    {
      /* do fullscan at small table */
      ret = btree_get_stats_with_fullscan (thread_p, env);
    }
  else
    {
      ret = btree_get_stats_with_AR_sampling (thread_p, env);
    }

  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* check for emptiness */
  for (i = 0; i < env->pkeys_val_num; i++)
    {
      assert_release (env->stat_info->keys >= env->stat_info->pkeys[i]);

      if (env->stat_info->keys <= 0)
	{
	  /* is empty */
	  assert_release (env->stat_info->pkeys[i] == 0);
	  env->stat_info->pkeys[i] = 0;
	}
      else
	{
	  env->stat_info->pkeys[i] = MAX (env->stat_info->pkeys[i], 1);
	}
    }

  /* check for leaf pages */
  env->stat_info->leafs = MAX (1, env->stat_info->leafs);
  env->stat_info->leafs = MIN (env->stat_info->leafs,
			       npages - (env->stat_info->height - 1));

  assert_release (env->stat_info->pages >= 1);
  assert_release (env->stat_info->leafs >= 1);
  assert_release (env->stat_info->height >= 1);
  assert_release (env->stat_info->keys >= 0);

end:

  if (root_page_ptr)
    {
      pgbuf_unfix_and_init (thread_p, root_page_ptr);
    }

  /* clear partial key-values */
  for (i = 0; i < env->pkeys_val_num; i++)
    {
      pr_clear_value (&(env->pkeys_val[i]));
    }

  mnt_bt_get_stats (thread_p);

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * xbtree_get_key_type () - Obtains index key type.
 *
 * return	  : Error code.
 * thread_p (in)  : Thread entry
 * btid (in)	  : B-tree identifier.
 * key_type (out) : Index key type.
 */
int
xbtree_get_key_type (THREAD_ENTRY * thread_p, BTID btid,
		     TP_DOMAIN ** key_type)
{
  VPID root_vpid;
  PAGE_PTR root_page;
  BTREE_ROOT_HEADER *root_header = NULL;
  OR_BUF buf;

  root_vpid.pageid = btid.root_pageid;
  root_vpid.volid = btid.vfid.volid;

  assert (key_type != NULL);
  *key_type = NULL;

  root_page = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			 PGBUF_UNCONDITIONAL_LATCH);
  if (root_page == NULL)
    {
      return ER_FAILED;
    }

  root_header = btree_get_root_header (root_page);
  or_init (&buf, root_header->packed_key_domain, -1);
  *key_type = or_get_domain (&buf, NULL, NULL);

  pgbuf_unfix (thread_p, root_page);

  if (*key_type == NULL)
    {
      return ER_FAILED;
    }

  return NO_ERROR;
}

/*
 * btree_check_page_key () - Check (verify) page
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *   btid(in):
 *   page_ptr(in): Page pointer
 *   page_vpid(in): Page identifier
 *
 * Note: Verifies the correctness of the specified page of the B+tree.
 * Tests include checking the order of the keys in the page,
 * checking the key count and maximum key length values stored page header.
 */
static DISK_ISVALID
btree_check_page_key (THREAD_ENTRY * thread_p, const OID * class_oid_p,
		      BTID_INT * btid, const char *btname,
		      PAGE_PTR page_ptr, VPID * page_vpid)
{
  int key_cnt, offset;
  RECDES peek_rec1, peek_rec2;
  DB_VALUE key1, key2;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  int k, overflow_key1 = 0, overflow_key2 = 0;
  bool clear_key1, clear_key2;
  LEAF_REC leaf_pnt;
  NON_LEAF_REC nleaf_pnt;
  DISK_ISVALID valid = DISK_ERROR;
  int c;
  char err_buf[LINE_MAX];

  /* initialize */
  leaf_pnt.key_len = 0;
  VPID_SET_NULL (&leaf_pnt.ovfl);
  nleaf_pnt.key_len = 0;
  VPID_SET_NULL (&nleaf_pnt.pnt);

  DB_MAKE_NULL (&key1);
  DB_MAKE_NULL (&key2);

  key_cnt = btree_node_number_of_keys (page_ptr);

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      snprintf (err_buf, LINE_MAX, "btree_check_page_key: "
		"get node header failure: %d\n", key_cnt);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
	      err_buf);
      valid = DISK_INVALID;
      goto error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if ((node_type == BTREE_NON_LEAF_NODE && key_cnt <= 0)
      || (node_type == BTREE_LEAF_NODE && key_cnt < 0))
    {
      snprintf (err_buf, LINE_MAX, "btree_check_page_key: "
		"node key count underflow: %d\n", key_cnt);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
	      err_buf);
      valid = DISK_INVALID;
      goto error;
    }

  if (key_cnt == 0)
    {
      return DISK_VALID;
    }

  if (key_cnt == 1)
    {
      /* there is only one key, so no order check */
      if (spage_get_record (page_ptr, 1, &peek_rec1, PEEK) != S_SUCCESS)
	{
	  valid = DISK_ERROR;
	  goto error;
	}

      return DISK_VALID;
    }

  for (k = 1; k < key_cnt; k++)
    {
      if (spage_get_record (page_ptr, k, &peek_rec1, PEEK) != S_SUCCESS)
	{
	  valid = DISK_ERROR;
	  goto error;
	}

      if (btree_leaf_is_flaged (&peek_rec1, BTREE_LEAF_RECORD_FENCE))
	{
	  continue;
	}

      /* read the current record key */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_read_record (thread_p, btid, page_ptr, &peek_rec1, &key1,
			     (void *) &leaf_pnt, BTREE_LEAF_NODE, &clear_key1,
			     &offset, PEEK_KEY_VALUE, NULL);
	  overflow_key1 = (leaf_pnt.key_len < 0);
	}
      else
	{
	  btree_read_record (thread_p, btid, page_ptr, &peek_rec1, &key1,
			     (void *) &nleaf_pnt, BTREE_NON_LEAF_NODE,
			     &clear_key1, &offset, PEEK_KEY_VALUE, NULL);
	  overflow_key1 = (nleaf_pnt.key_len < 0);
	}

      if ((!overflow_key1
	   && (btree_get_key_length (&key1) > header->max_key_len))
	  || (overflow_key1 && (DISK_VPID_SIZE > header->max_key_len)))
	{
	  btree_dump_page (thread_p, stdout, class_oid_p, btid, btname,
			   page_ptr, page_vpid, 2, 2);

	  snprintf (err_buf, LINE_MAX, "btree_check_page_key: "
		    "--- max key length test failed for page "
		    "{%d , %d}. Check key_rec = %d\n",
		    page_vpid->volid, page_vpid->pageid, k);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
		  err_buf);
	  valid = DISK_INVALID;
	  goto error;
	}

      if (spage_get_record (page_ptr, k + 1, &peek_rec2, PEEK) != S_SUCCESS)
	{
	  valid = DISK_ERROR;
	  goto error;
	}

      if (btree_leaf_is_flaged (&peek_rec2, BTREE_LEAF_RECORD_FENCE))
	{
	  btree_clear_key_value (&clear_key1, &key1);
	  continue;
	}

      /* read the next record key */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_read_record (thread_p, btid, page_ptr, &peek_rec2, &key2,
			     (void *) &leaf_pnt, BTREE_LEAF_NODE, &clear_key2,
			     &offset, PEEK_KEY_VALUE, NULL);
	  overflow_key2 = (leaf_pnt.key_len < 0);
	}
      else
	{
	  btree_read_record (thread_p, btid, page_ptr, &peek_rec2, &key2,
			     (void *) &nleaf_pnt, BTREE_NON_LEAF_NODE,
			     &clear_key2, &offset, PEEK_KEY_VALUE, NULL);
	  overflow_key2 = (nleaf_pnt.key_len < 0);
	}

      if ((!overflow_key2
	   && (btree_get_key_length (&key2) > header->max_key_len))
	  || (overflow_key2 && (DISK_VPID_SIZE > header->max_key_len)))
	{
	  btree_dump_page (thread_p, stdout, class_oid_p, btid, btname,
			   page_ptr, page_vpid, 2, 2);

	  snprintf (err_buf, LINE_MAX, "btree_check_page_key: "
		    "--- max key length test failed for page "
		    "{%d , %d}. Check key_rec = %d\n",
		    page_vpid->volid, page_vpid->pageid, k + 1);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
		  err_buf);
	  valid = DISK_INVALID;
	  goto error;
	}

      if (k == 1 && node_type == BTREE_NON_LEAF_NODE)
	{
	  c = DB_LT;		/* TODO - may compare with neg-inf sep */
	}
      else
	{
	  /* compare the keys for the order */
	  c = btree_compare_key (&key1, &key2, btid->key_type, 1, 1, NULL);
	}

      if (c != DB_LT)
	{
	  btree_dump_page (thread_p, stdout, class_oid_p, btid, btname,
			   page_ptr, page_vpid, 2, 2);

	  snprintf (err_buf, LINE_MAX, "btree_check_page_key:"
		    "--- key order test failed for page"
		    " {%d , %d}. Check key_recs = %d and %d\n",
		    page_vpid->volid, page_vpid->pageid, k, k + 1);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
		  err_buf);
	  valid = DISK_INVALID;
	  goto error;
	}

      btree_clear_key_value (&clear_key1, &key1);
      btree_clear_key_value (&clear_key2, &key2);
    }

  /* page check passed */
  return DISK_VALID;

error:

  btree_clear_key_value (&clear_key1, &key1);
  btree_clear_key_value (&clear_key2, &key2);

  return valid;
}

/*
 * btree_verify_subtree () - Check (verify) a page and its subtrees
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *   btid(in): B+tree index identifier
 *   pg_ptr(in): Page pointer for the subtree root page
 *   pg_vpid(in): Page identifier for the subtree root page
 *   INFO(in):
 *
 * Note: Verifies the correctness of the content of the given page
 * together with its subtree
 */
static DISK_ISVALID
btree_verify_subtree (THREAD_ENTRY * thread_p, const OID * class_oid_p,
		      BTID_INT * btid, const char *btname, PAGE_PTR pg_ptr,
		      VPID * pg_vpid, BTREE_NODE_INFO * INFO)
{
  int key_cnt;
  NON_LEAF_REC nleaf_ptr;
  VPID page_vpid;
  PAGE_PTR page = NULL;
  RECDES rec;
  DB_VALUE curr_key;
  int offset;
  bool clear_key = false;
  int i;
  DISK_ISVALID valid = DISK_ERROR;
  BTREE_NODE_INFO INFO2;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  char err_buf[LINE_MAX];

  db_make_null (&INFO2.max_key);

  /* test the page for the order of the keys within the page and get the
   * biggest key of this page
   */
  valid = btree_check_page_key (thread_p, class_oid_p, btid, btname, pg_ptr,
				pg_vpid);
  if (valid != DISK_VALID)
    {
      goto error;
    }

  key_cnt = btree_node_number_of_keys (pg_ptr);

  header = btree_get_node_header (pg_ptr);
  if (header == NULL)
    {
      valid = DISK_INVALID;
      goto error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if ((node_type == BTREE_NON_LEAF_NODE && key_cnt <= 0)
      || (node_type == BTREE_LEAF_NODE && key_cnt < 0))
    {
      btree_dump_page (thread_p, stdout, class_oid_p, btid, btname,
		       pg_ptr, pg_vpid, 2, 2);
      snprintf (err_buf, LINE_MAX, "btree_verify_subtree: "
		"node key count underflow: %d\n", key_cnt);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
	      err_buf);
      valid = DISK_INVALID;
      goto error;
    }

  /* initialize INFO structure */
  INFO->max_key_len = header->max_key_len;
  INFO->height = 0;
  INFO->tot_key_cnt = 0;
  INFO->page_cnt = 0;
  INFO->leafpg_cnt = 0;
  INFO->nleafpg_cnt = 0;
  db_make_null (&INFO->max_key);

  if (node_type == BTREE_NON_LEAF_NODE)
    {				/* a non-leaf page */
      if (key_cnt < 0)
	{
	  btree_dump_page (thread_p, stdout, class_oid_p, btid, btname,
			   pg_ptr, pg_vpid, 2, 2);

	  snprintf (err_buf, LINE_MAX, "btree_verify_subtree: "
		    "node key count underflow: %d\n", key_cnt);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMERGENCY_ERROR, 1,
		  err_buf);
	  valid = DISK_INVALID;
	  goto error;
	}

      INFO2.key_area_len = 0;
      db_make_null (&INFO2.max_key);

      /* traverse all the subtrees of this non_leaf page and accumulate
       * the statistical data in the INFO structure
       */
      for (i = 1; i <= key_cnt; i++)
	{
	  if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	    {
	      valid = DISK_ERROR;
	      goto error;
	    }

	  btree_read_record (thread_p, btid, pg_ptr, &rec, &curr_key,
			     &nleaf_ptr, BTREE_NON_LEAF_NODE, &clear_key,
			     &offset, PEEK_KEY_VALUE, NULL);

	  page_vpid = nleaf_ptr.pnt;

	  page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			    PGBUF_UNCONDITIONAL_LATCH);
	  if (page == NULL)
	    {
	      valid = DISK_ERROR;
	      goto error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, page, PAGE_BTREE);

	  valid = btree_verify_subtree (thread_p, class_oid_p, btid, btname,
					page, &page_vpid, &INFO2);
	  if (valid != DISK_VALID)
	    {
	      goto error;
	    }

	  /* accumulate results */
	  INFO->height = INFO2.height + 1;
	  INFO->tot_key_cnt += INFO2.tot_key_cnt;
	  INFO->page_cnt += INFO2.page_cnt;
	  INFO->leafpg_cnt += INFO2.leafpg_cnt;
	  INFO->nleafpg_cnt += INFO2.nleafpg_cnt;

	  pgbuf_unfix_and_init (thread_p, page);
	  btree_clear_key_value (&clear_key, &curr_key);
	}
      INFO->page_cnt += 1;
      INFO->nleafpg_cnt += 1;
    }
  else
    {				/* a leaf page */
      /* form the INFO structure from the header information */
      INFO->height = 1;
      INFO->tot_key_cnt = key_cnt;
      INFO->page_cnt = 1;
      INFO->leafpg_cnt = 1;
      INFO->nleafpg_cnt = 0;
    }

  return DISK_VALID;

error:
  btree_clear_key_value (&clear_key, &curr_key);

  if (page)
    {
      pgbuf_unfix_and_init (thread_p, page);
    }

  return valid;
}

/*
 * btree_verify_tree () - Check (verify) tree
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *   btid_int(in): B+tree index identifier
 *
 * Note: Verifies the correctness of the B+tree index . During tree
 * traversal,  several tests are  conducted, such as checking
 * the order of keys on a page or among pages that are in a
 * father-child relationship.
 */
DISK_ISVALID
btree_verify_tree (THREAD_ENTRY * thread_p, const OID * class_oid_p,
		   BTID_INT * btid_int, const char *btname)
{
  VPID p_vpid;
  PAGE_PTR root = NULL;
  BTREE_NODE_INFO INFO;
  DISK_ISVALID valid = DISK_ERROR;

  p_vpid.pageid = btid_int->sys_btid->root_pageid;	/* read root page */
  p_vpid.volid = btid_int->sys_btid->vfid.volid;
  root = pgbuf_fix (thread_p, &p_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      valid = DISK_ERROR;
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  /* traverse the tree and store the statistical data in the INFO structure */
  valid =
    btree_verify_subtree (thread_p, class_oid_p, btid_int, btname, root,
			  &p_vpid, &INFO);
  if (valid != DISK_VALID)
    {
      goto error;
    }

  pgbuf_unfix_and_init (thread_p, root);

  return DISK_VALID;

error:

  if (root)
    {
      pgbuf_unfix_and_init (thread_p, root);
    }

  return valid;
}

/*
 *       		 db_check consistency routines
 */

/*
 * btree_check_pages () -
 *   return: DISK_VALID, DISK_VALID or DISK_ERROR
 *   btid(in): B+tree index identifier
 *   pg_ptr(in): Page pointer
 *   pg_vpid(in): Page identifier
 *
 * Note: Verify that given page and all its subpages are valid.
 */
static DISK_ISVALID
btree_check_pages (THREAD_ENTRY * thread_p, BTID_INT * btid,
		   PAGE_PTR pg_ptr, VPID * pg_vpid)
{
  VPID page_vpid;		/* Child page identifier */
  PAGE_PTR page = NULL;		/* Child page pointer */
  RECDES rec;			/* Record descriptor for page node records */
  DISK_ISVALID vld = DISK_ERROR;	/* Validity return code from subtree */
  int key_cnt;			/* Number of keys in the page */
  int i;			/* Loop counter */
  NON_LEAF_REC nleaf;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  /* Verify the given page */
  vld = file_isvalid_page_partof (thread_p, pg_vpid, &btid->sys_btid->vfid);
  if (vld != DISK_VALID)
    {
      goto error;
    }

#ifdef SPAGE_DEBUG
  if (spage_check (thread_p, pg_ptr) != NO_ERROR)
    {
      vld = DISK_ERROR;
      goto error;
    }
#endif /* SPAGE_DEBUG */

  /* Verify subtree child pages */

  key_cnt = btree_node_number_of_keys (pg_ptr);

  header = btree_get_node_header (pg_ptr);
  if (header == NULL)
    {
      vld = DISK_ERROR;
      goto error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {				/* non-leaf page */
      for (i = 1; i <= key_cnt; i++)
	{
	  if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	    {
	      vld = DISK_ERROR;
	      goto error;
	    }
	  btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf);
	  page_vpid = nleaf.pnt;

	  page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			    PGBUF_UNCONDITIONAL_LATCH);
	  if (page == NULL)
	    {
	      vld = DISK_ERROR;
	      goto error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, page, PAGE_BTREE);

	  vld = btree_check_pages (thread_p, btid, page, &page_vpid);
	  if (vld != DISK_VALID)
	    {
	      goto error;
	    }
	  pgbuf_unfix_and_init (thread_p, page);
	}
    }

  return DISK_VALID;

error:

  if (page)
    {
      pgbuf_unfix_and_init (thread_p, page);
    }
  return vld;

}

/*
 * btree_check_tree () -
 *   return: DISK_VALID, DISK_INVALID or DISK_ERROR
 *   btid(in): B+tree index identifier
 *
 * Note: Verify that all the pages of the specified index are valid.
 */
DISK_ISVALID
btree_check_tree (THREAD_ENTRY * thread_p, const OID * class_oid_p,
		  BTID * btid, const char *btname)
{
  DISK_ISVALID valid = DISK_ERROR;
  VPID r_vpid;			/* root page identifier */
  PAGE_PTR r_pgptr = NULL;	/* root page pointer */
  BTID_INT btid_int;
  BTREE_ROOT_HEADER *root_header = NULL;

  /* Fetch the root page */
  r_vpid.pageid = btid->root_pageid;
  r_vpid.volid = btid->vfid.volid;
  r_pgptr = pgbuf_fix (thread_p, &r_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		       PGBUF_UNCONDITIONAL_LATCH);
  if (r_pgptr == NULL)
    {
      valid = DISK_ERROR;
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, r_pgptr, PAGE_BTREE);

  root_header = btree_get_root_header (r_pgptr);
  if (root_header == NULL)
    {
      valid = DISK_ERROR;
      goto error;
    }

  btid_int.sys_btid = btid;
  if (btree_glean_root_header_info (thread_p, root_header, &btid_int) !=
      NO_ERROR)
    {
      goto error;
    }

  valid = btree_check_pages (thread_p, &btid_int, r_pgptr, &r_vpid);
  if (valid != DISK_VALID)
    {
      goto error;
    }

  pgbuf_unfix_and_init (thread_p, r_pgptr);

  /* Now check for the logical correctness of the tree */
  return btree_verify_tree (thread_p, class_oid_p, &btid_int, btname);

error:

  if (r_pgptr)
    {
      pgbuf_unfix_and_init (thread_p, r_pgptr);
    }
  return valid;
}

/*
 * btree_check_by_btid () -
 *   btid(in): B+tree index identifier
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *
 * Note: Verify that all pages of a btree indices are valid.
 */
DISK_ISVALID
btree_check_by_btid (THREAD_ENTRY * thread_p, BTID * btid)
{
  DISK_ISVALID valid = DISK_ERROR;
  char area[FILE_DUMP_DES_AREA_SIZE];
  char *fd = area;
  int fd_size = FILE_DUMP_DES_AREA_SIZE, size;
  FILE_BTREE_DES *btree_des;
  char *btname;
  VPID vpid;

  size = file_get_descriptor (thread_p, &btid->vfid, fd, fd_size);
  if (size < 0)
    {
      fd_size = -size;
      fd = (char *) db_private_alloc (thread_p, fd_size);
      if (fd == NULL)
	{
	  fd = area;
	  fd_size = FILE_DUMP_DES_AREA_SIZE;
	}
      else
	{
	  size = file_get_descriptor (thread_p, &btid->vfid, fd, fd_size);
	}
    }
  btree_des = (FILE_BTREE_DES *) fd;

  /* get the index name of the index key */
  if (heap_get_indexinfo_of_btid (thread_p, &(btree_des->class_oid),
				  btid, NULL, NULL, NULL,
				  NULL, &btname, NULL) != NO_ERROR)
    {
      goto exit_on_end;
    }

  if (file_find_nthpages (thread_p, &btid->vfid, &vpid, 0, 1) != 1)
    {
      goto exit_on_end;
    }

  btid->root_pageid = vpid.pageid;

  valid = btree_check_tree (thread_p, &(btree_des->class_oid), btid, btname);

exit_on_end:
  if (fd != area)
    {
      db_private_free_and_init (thread_p, fd);
    }
  if (btname)
    {
      free_and_init (btname);
    }

  return valid;
}

/*
 * btree_check_by_class_oid () -
 *   cls_oid(in):
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *
 */
DISK_ISVALID
btree_check_by_class_oid (THREAD_ENTRY * thread_p, OID * cls_oid,
			  BTID * idx_btid)
{
  OR_CLASSREP *cls_repr;
  OR_INDEX *curr;
  BTID btid;
  int i;
  int cache_idx = -1;
  DISK_ISVALID rv = DISK_VALID;

  cls_repr =
    heap_classrepr_get (thread_p, cls_oid, NULL, 0, &cache_idx, true);
  if (cls_repr == NULL)
    {
      assert (er_errid () != NO_ERROR);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_errid (), 0);
      return DISK_ERROR;
    }

  for (i = 0, curr = cls_repr->indexes; i < cls_repr->n_indexes; i++, curr++)
    {
      if (curr == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_UNEXPECTED, 0);
	  rv = DISK_ERROR;
	  break;
	}

      if (idx_btid != NULL && !BTID_IS_EQUAL (&curr->btid, idx_btid))
	{
	  continue;
	}

      BTID_COPY (&btid, &curr->btid);
      if (btree_check_by_btid (thread_p, &btid) != DISK_VALID)
	{
	  rv = DISK_ERROR;
	  break;
	}
    }

  if (cls_repr)
    {
      heap_classrepr_free (cls_repr, &cache_idx);
    }

  return rv;
}

/*
 * btree_repair_prev_link_by_btid () -
 *   btid(in) :
 *   repair(in) :
 *   index_name(in) :
 *   return:
 */
static DISK_ISVALID
btree_repair_prev_link_by_btid (THREAD_ENTRY * thread_p, BTID * btid,
				bool repair, char *index_name)
{
  PAGE_PTR current_pgptr, next_pgptr, root_pgptr;
  VPID current_vpid, next_vpid;
  int valid = DISK_VALID;
  int request_mode;
  int retry_count = 0;
  char output[LINE_MAX];
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  VPID_SET_NULL (&next_vpid);

  snprintf (output, LINE_MAX, "%s - %s... ",
	    repair ? "repair index" : "check index", index_name);
  xcallback_console_print (thread_p, output);

  current_pgptr = NULL;
  next_pgptr = NULL;
  root_pgptr = NULL;

  request_mode = repair ? PGBUF_LATCH_WRITE : PGBUF_LATCH_READ;

  /* root page */
  VPID_SET (&current_vpid, btid->vfid.volid, btid->root_pageid);
  root_pgptr = pgbuf_fix (thread_p, &current_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
  if (root_pgptr == NULL)
    {
      valid = DISK_ERROR;
      goto exit_repair;
    }

  (void) pgbuf_check_page_ptype (thread_p, root_pgptr, PAGE_BTREE);

retry_repair:
  if (retry_count > 10)
    {
      valid = DISK_ERROR;
      goto exit_repair;
    }

  if (current_pgptr)
    {
      pgbuf_unfix_and_init (thread_p, current_pgptr);
    }
  if (next_pgptr)
    {
      pgbuf_unfix_and_init (thread_p, next_pgptr);
    }

  while (!VPID_ISNULL (&current_vpid))
    {
      current_pgptr = pgbuf_fix (thread_p, &current_vpid, OLD_PAGE,
				 request_mode, PGBUF_CONDITIONAL_LATCH);
      if (current_pgptr == NULL)
	{
	  retry_count++;
	  goto retry_repair;
	}

      (void) pgbuf_check_page_ptype (thread_p, current_pgptr, PAGE_BTREE);

      header = btree_get_node_header (current_pgptr);
      if (header == NULL)
	{
	  goto exit_repair;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      if (node_type == BTREE_LEAF_NODE)
	{
	  next_vpid = header->next_vpid;
	  break;
	}
      else
	{
	  RECDES rec;
	  NON_LEAF_REC non_leaf_rec;

	  if (spage_get_record (current_pgptr, 1, &rec, PEEK) != S_SUCCESS)
	    {
	      valid = DISK_ERROR;
	      goto exit_repair;
	    }
	  btree_read_fixed_portion_of_non_leaf_record (&rec, &non_leaf_rec);
	  current_vpid = non_leaf_rec.pnt;
	  pgbuf_unfix_and_init (thread_p, current_pgptr);
	}
    }

  assert (node_type == BTREE_LEAF_NODE);
  assert (header != NULL);

  while (!VPID_ISNULL (&next_vpid))
    {
      next_pgptr = pgbuf_fix (thread_p, &next_vpid, OLD_PAGE,
			      request_mode, PGBUF_CONDITIONAL_LATCH);
      if (next_pgptr == NULL)
	{
	  retry_count++;
	  goto retry_repair;
	}

      (void) pgbuf_check_page_ptype (thread_p, next_pgptr, PAGE_BTREE);

      header = btree_get_node_header (next_pgptr);
      if (header == NULL)
	{
	  goto exit_repair;
	}

      if (!VPID_EQ (&header->prev_vpid, &current_vpid))
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_BTREE_CORRUPT_PREV_LINK, 3, index_name,
		  next_vpid.volid, next_vpid.pageid);

	  if (repair)
	    {
	      BTID_INT bint;

	      log_start_system_op (thread_p);
	      bint.sys_btid = btid;
	      if (btree_set_vpid_previous_vpid (thread_p, &bint, next_pgptr,
						&current_vpid) != NO_ERROR)
		{
		  valid = DISK_ERROR;
		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
		  goto exit_repair;
		}
	      valid = DISK_INVALID;
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	      er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
		      ER_BTREE_REPAIR_PREV_LINK, 3, index_name,
		      next_vpid.volid, next_vpid.pageid);
	    }
	  else
	    {
	      valid = DISK_INVALID;
	      goto exit_repair;
	    }
	}
      pgbuf_unfix_and_init (thread_p, current_pgptr);

      /* move to next page */
      current_vpid = next_vpid;
      next_vpid = header->next_vpid;
      current_pgptr = next_pgptr;
      next_pgptr = NULL;
    }

exit_repair:
  if (root_pgptr)
    {
      pgbuf_unfix (thread_p, root_pgptr);
    }
  if (current_pgptr)
    {
      pgbuf_unfix (thread_p, current_pgptr);
    }
  if (next_pgptr)
    {
      pgbuf_unfix (thread_p, next_pgptr);
    }

  if (valid == DISK_ERROR)
    {
      xcallback_console_print (thread_p, (char *) "error\n");
    }
  else if (valid == DISK_VALID)
    {
      xcallback_console_print (thread_p, (char *) "pass\n");
    }
  else
    {
      if (repair)
	{
	  xcallback_console_print (thread_p, (char *) "repaired\n");
	}
      else
	{
	  xcallback_console_print (thread_p, (char *) "repair needed\n");
	}
    }

  return valid;
}

/*
 * btree_repair_prev_link_by_class_oid () -
 *   oid(in) :
 *   repair(in) :
 *   return:
 */
static DISK_ISVALID
btree_repair_prev_link_by_class_oid (THREAD_ENTRY * thread_p, OID * oid,
				     BTID * index_btid, bool repair)
{
  OR_CLASSREP *cls_repr;
  OR_INDEX *curr;
  int i;
  int cache_idx = -1;
  DISK_ISVALID valid = DISK_VALID;
  char *index_name;

  cls_repr = heap_classrepr_get (thread_p, oid, NULL, 0, &cache_idx, true);

  if (cls_repr == NULL)
    {
      return DISK_ERROR;
    }

  for (i = 0, curr = cls_repr->indexes;
       i < cls_repr->n_indexes && curr && valid == DISK_VALID; i++, curr++)
    {
      if (index_btid != NULL && !BTID_IS_EQUAL (&curr->btid, index_btid))
	{
	  continue;
	}

      heap_get_indexinfo_of_btid (thread_p, oid, &curr->btid, NULL, NULL,
				  NULL, NULL, &index_name, NULL);
      valid =
	btree_repair_prev_link_by_btid (thread_p, &curr->btid, repair,
					index_name);
      if (index_name)
	{
	  free_and_init (index_name);
	}
    }

  if (cls_repr)
    {
      heap_classrepr_free (cls_repr, &cache_idx);
    }

  return valid;
}

/*
 * btree_repair_prev_link () -
 *   oid(in) :
 *   index_btid(in) :
 *   repair(in) :
 *   return:
 */
DISK_ISVALID
btree_repair_prev_link (THREAD_ENTRY * thread_p, OID * oid, BTID * index_btid,
			bool repair)
{
  int num_files;
  BTID btid;
  FILE_TYPE file_type;
  DISK_ISVALID valid;
  int i;
  char *index_name;
  VPID vpid;

  if (oid != NULL && !OID_ISNULL (oid))
    {
      return btree_repair_prev_link_by_class_oid (thread_p, oid, index_btid,
						  repair);
    }

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files < 0)
    {
      return DISK_ERROR;
    }

  valid = DISK_VALID;

  /* Go to each file, check only the btree files */
  for (i = 0; i < num_files && valid != DISK_ERROR; i++)
    {
      INT64 fix_count = 0;
      char area[FILE_DUMP_DES_AREA_SIZE];
      char *fd = area;
      int fd_size = FILE_DUMP_DES_AREA_SIZE, size;
      FILE_BTREE_DES *btree_des;

      if (file_find_nthfile (thread_p, &btid.vfid, i) != 1)
	{
	  valid = DISK_ERROR;
	  break;
	}

      file_type = file_get_type (thread_p, &btid.vfid);
      if (file_type == FILE_UNKNOWN_TYPE)
	{
	  valid = DISK_ERROR;
	  break;
	}

      if (file_type != FILE_BTREE)
	{
	  continue;
	}

      size = file_get_descriptor (thread_p, &btid.vfid, fd, fd_size);
      if (size < 0)
	{
	  fd_size = -size;
	  fd = (char *) malloc (fd_size);
	  if (fd == NULL)
	    {
	      return DISK_ERROR;
	    }
	  size = file_get_descriptor (thread_p, &btid.vfid, fd, fd_size);
	}
      btree_des = (FILE_BTREE_DES *) fd;

      /* get the index name of the index key */
      if (heap_get_indexinfo_of_btid (thread_p, &(btree_des->class_oid),
				      &btid, NULL, NULL, NULL,
				      NULL, &index_name, NULL) != NO_ERROR)
	{
	  if (fd != area)
	    {
	      free_and_init (fd);
	    }
	  return DISK_ERROR;
	}

      if (file_find_nthpages (thread_p, &btid.vfid, &vpid, 0, 1) != 1)
	{
	  if (fd != area)
	    {
	      free_and_init (fd);
	    }
	  if (index_name)
	    {
	      free_and_init (index_name);
	    }
	  return DISK_ERROR;
	}

      btid.root_pageid = vpid.pageid;
      valid = btree_repair_prev_link_by_btid (thread_p, &btid, repair,
					      index_name);

      if (fd != area)
	{
	  free_and_init (fd);
	}
      if (index_name)
	{
	  free_and_init (index_name);
	}
    }

  return valid;
}

/*
 * btree_check_all () -
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *
 * Note: Verify that all pages of all btree indices are valid.
 */
DISK_ISVALID
btree_check_all (THREAD_ENTRY * thread_p)
{
  int num_files;		/* Number of files in the system */
  BTID btid;			/* Btree index identifier        */
  DISK_ISVALID valid, allvalid;	/* Validation return code        */
  FILE_TYPE file_type;		/* TYpe of file                  */
  int i;			/* Loop counter                  */

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files < 0)
    {
      return DISK_ERROR;
    }

  allvalid = DISK_VALID;

  /* Go to each file, check only the btree files */
  for (i = 0; i < num_files && allvalid != DISK_ERROR; i++)
    {
      if (file_find_nthfile (thread_p, &btid.vfid, i) != 1)
	{
	  break;
	}

      file_type = file_get_type (thread_p, &btid.vfid);
      if (file_type == FILE_UNKNOWN_TYPE)
	{
	  allvalid = DISK_ERROR;
	  break;
	}

      if (file_type != FILE_BTREE)
	{
	  continue;
	}

      valid = btree_check_by_btid (thread_p, &btid);
      if (valid != DISK_VALID)
	{
	  allvalid = valid;
	}
    }

  return allvalid;
}

/*
 * btree_keyoid_checkscan_start () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   btscan(out): Set to key-oid check scan structure.
 *
 * Note: Start a <key-oid> check scan on the index.
 */
int
btree_keyoid_checkscan_start (THREAD_ENTRY * thread_p, BTID * btid,
			      BTREE_CHECKSCAN * btscan)
{
  assert (btid != NULL);

  /* initialize scan structure */
  btscan->btid = *btid;
  BTREE_INIT_SCAN (&btscan->btree_scan);
  btscan->oid_area_size = ISCAN_OID_BUFFER_SIZE;
  btscan->oid_cnt = 0;
  btscan->oid_ptr = (OID *) os_malloc (btscan->oid_area_size);
  if (btscan->oid_ptr == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, btscan->oid_area_size);
      return ER_FAILED;
    }

  return NO_ERROR;
}

/*
 * btree_keyoid_checkscan_check () -
 *   return: either: DISK_INVALID, DISK_VALID, DISK_ERROR
 *   btscan(in): B+tree key-oid check scan structure.
 *   cls_oid(in):
 *   key(in): Key pointer
 *   oid(in): Object identifier for the key
 *
 * Note: Check if the given key-oid pair exists in the index.
 */
DISK_ISVALID
btree_keyoid_checkscan_check (THREAD_ENTRY * thread_p,
			      BTREE_CHECKSCAN * btscan, OID * cls_oid,
			      DB_VALUE * key, OID * oid)
{
  int k;			/* Loop iteration variable */
  INDX_SCAN_ID isid;
  DISK_ISVALID status;
  KEY_VAL_RANGE key_val_range;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;

  if (mvcc_Enabled)
    {
      mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
      if (mvcc_snapshot == NULL)
	{
	  return DISK_INVALID;
	}
    }

  /* initialize scan structure */
  BTREE_INIT_SCAN (&btscan->btree_scan);

  scan_init_index_scan (&isid, btscan->oid_ptr, mvcc_snapshot);

  assert (!pr_is_set_type (DB_VALUE_DOMAIN_TYPE (key)));

  PR_SHARE_VALUE (key, &key_val_range.key1);
  PR_SHARE_VALUE (key, &key_val_range.key2);
  key_val_range.range = GE_LE;
  key_val_range.num_index_term = 0;

  do
    {
      /* search index */
      btscan->oid_cnt = btree_keyval_search (thread_p, &btscan->btid,
					     S_SELECT,
					     &btscan->btree_scan,
					     &key_val_range,
					     cls_oid, btscan->oid_ptr,
					     btscan->oid_area_size, NULL,
					     &isid, false);

      if (DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY
	  && key->data.midxkey.domain == NULL)
	{
	  /* set the appropriate domain, as it might be needed for printing
	   * if the given key-oid pair does not exist in the index. */
	  key->data.midxkey.domain = btscan->btree_scan.btid_int.key_type;
	}

      if (btscan->oid_cnt == -1)
	{
	  btscan->oid_ptr = isid.oid_list.oidp;
	  status = DISK_ERROR;
	  goto end;
	}

      btscan->oid_ptr = isid.oid_list.oidp;

      /* search current set of OIDs to see if given <key-oid> pair exists */
      for (k = 0; k < btscan->oid_cnt; k++)
	{
	  if (OID_EQ (&btscan->oid_ptr[k], oid))
	    {			/* <key-oid> pair found */
	      status = DISK_VALID;
	      goto end;
	    }
	}
    }
  while (!BTREE_END_OF_SCAN (&btscan->btree_scan));

  /* indicate <key_oid> pair is not found */
  status = DISK_INVALID;

end:

  btree_scan_clear_key (&btscan->btree_scan);

  /* do not use copy_buf for key-val scan, only use for key-range scan */

  return status;
}

/*
 * btree_keyoid_checkscan_end () -
 *   return:
 *   btscan(in): B+tree key-oid check scan structure.
 *
 * Note: End the <key-oid> check scan on the index.
 */
void
btree_keyoid_checkscan_end (THREAD_ENTRY * thread_p, BTREE_CHECKSCAN * btscan)
{
  /* Deallocate allocated areas */
  if (btscan->oid_ptr)
    {
      os_free_and_init (btscan->oid_ptr);
      btscan->oid_area_size = 0;
    }
}

/*
 *       		     b+tree space routines
 */

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * btree_estimate_total_numpages () -
 *   return: int
 *   dis_key_cnt(in): Distinct number of key values
 *   avg_key_len(in): Average key length
 *   tot_val_cnt(in): Total value count
 *   blt_pgcnt_est(out): Set to index built(not-loaded) total page cnt estimate
 *   blt_wrs_pgcnt_est(out): Set to index built(not-loaded) worst case pgcnt
 *                           estimate
 *
 * Note: Estimate and return total number of pages for the B+tree to
 * be constructed.
 */
int
btree_estimate_total_numpages (THREAD_ENTRY * thread_p, int dis_key_cnt,
			       int avg_key_len, int tot_val_cnt,
			       int *blt_pgcnt_est, int *blt_wrs_pgcnt_est)
{
  int load_pgcnt_est;
  int rec_oid_cnt;
  int avg_rec_len;
  int avg_nrec_len;
  int page_size;
  int ovfl_page_size;
  int nrecs_leaf_page;
  int nrecs_nleaf_page;
  int num_leaf_pages;
  int num_ovfl_pages;
  int num_nleaf_pages;
  int order;
  int nlevel_cnt;
  int nlevel_pg_cnt;
  int num_pages;
  int k, s;
  float unfill_factor;

  /* initializations */
  load_pgcnt_est = -1;
  *blt_pgcnt_est = -1;
  *blt_wrs_pgcnt_est = -1;

  /* check for passed parameters */
  if (dis_key_cnt == 0)
    {
      dis_key_cnt++;
    }
  if (tot_val_cnt < dis_key_cnt)
    {
      tot_val_cnt = dis_key_cnt;
    }

  /* find average leaf record length */
  /* LEAF RECORD: Key-Length : Ovfl_vpid :    key   :  oid1 : oid2 ... */
  rec_oid_cnt = CEIL_PTVDIV (tot_val_cnt, dis_key_cnt);
  rec_oid_cnt = MAX (1, rec_oid_cnt);
  avg_rec_len = LEAF_RECORD_SIZE;
  avg_rec_len += avg_key_len;
  avg_rec_len = DB_ALIGN (avg_rec_len, OR_INT_SIZE);	/* OK */
  avg_rec_len += (rec_oid_cnt * OR_OID_SIZE);

  /* find average non-leaf record length */
  /* NLEAF RECORD: Child_vpid : key_len : key */
  avg_nrec_len = NON_LEAF_RECORD_SIZE;
  avg_nrec_len += avg_key_len;

  /* find actually available page size for index records:
   * The index pages are usually 80% full and each one contains
   * a node header (sizeof (BTREE_NODE_HEADER)).
   *
   * Reserved space: page-header-overhead + header record +
   *                 one record size (the one not to be inserted) +
   *                 free area reserved in the page
   */

  /* Do the estimations for three cases.
   * Regular index loading, use index unfill factor,
   * Regular index built (one at a time), assume 30% free in pages,
   * Worst case index built, assume 50% free space in pages.
   */
  for (s = 0; s < 3; s++)
    {
      if (s == 0)
	{
	  unfill_factor =
	    (float) (prm_get_float_value (PRM_ID_BT_UNFILL_FACTOR) + 0.05);
	}
      else if (s == 1)
	{
	  unfill_factor = (float) (0.30 + 0.05);
	}
      else
	{
	  unfill_factor = (float) (0.50 + 0.05);
	}
      page_size = (int) (DB_PAGESIZE - (spage_header_size () +
					(sizeof (BTREE_NODE_HEADER) +
					 spage_slot_size ()) +
					(DB_PAGESIZE * unfill_factor)));

      /* find the number of records per index page */
      if (avg_rec_len >= page_size)
	{
	  /* records will use overflow pages, so each leaf page will get
	   * one record, plus number overflow pages
	   */
	  nrecs_leaf_page = 1;
	  ovfl_page_size = DB_PAGESIZE - (spage_header_size () +
					  (DISK_VPID_SIZE +
					   spage_slot_size ()) +
					  spage_slot_size ());
	  num_ovfl_pages =
	    dis_key_cnt *
	    (CEIL_PTVDIV (avg_rec_len - page_size, ovfl_page_size));
	}
      else
	{
	  /* consider the last record size not to be put in page */
	  page_size -= (avg_rec_len + spage_slot_size ());
	  nrecs_leaf_page = page_size / (avg_rec_len + spage_slot_size ());
	  nrecs_leaf_page = MAX (1, nrecs_leaf_page);
	  num_ovfl_pages = 0;
	}
      nrecs_nleaf_page = page_size / (avg_nrec_len + spage_slot_size ());
      nrecs_nleaf_page = MAX (2, nrecs_nleaf_page);

      /* find the number of leaf pages */
      num_leaf_pages = CEIL_PTVDIV (dis_key_cnt, nrecs_leaf_page);
      num_leaf_pages = MAX (1, num_leaf_pages);

      /* find the number of nleaf pages */
      num_nleaf_pages = 1;
      order = 1;
      do
	{
	  nlevel_cnt = 1;
	  for (k = 0; k < order; k++)
	    {
	      nlevel_cnt *= ((int) nrecs_nleaf_page);
	    }
	  nlevel_pg_cnt = (num_leaf_pages / nlevel_cnt);
	  num_nleaf_pages += nlevel_pg_cnt;
	  order++;
	}
      while (nlevel_pg_cnt > 1);

      /* find total number of index tree pages, one page is added for the
       * file manager overhead.
       */
      num_pages = num_leaf_pages + num_ovfl_pages + num_nleaf_pages;
      num_pages += file_guess_numpages_overhead (thread_p, NULL, num_pages);

      /* record corresponding estimation */
      if (s == 0)
	{
	  load_pgcnt_est = num_pages;
	}
      else if (s == 1)
	{
	  *blt_pgcnt_est = num_pages;
	}
      else
	{
	  *blt_wrs_pgcnt_est = num_pages;
	}

    }				/* for */

  /* make sure that built tree estimations are not lower than loaded
   * tree estimations.
   */
  if (*blt_pgcnt_est < load_pgcnt_est)
    {
      *blt_pgcnt_est = load_pgcnt_est;
    }
  if (*blt_wrs_pgcnt_est < *blt_pgcnt_est)
    {
      *blt_wrs_pgcnt_est = *blt_pgcnt_est;
    }

  return load_pgcnt_est;
}
#endif

/*
 * btree_get_subtree_capacity () -
 *   return: NO_ERROR
 *   btid(in):
 *   pg_ptr(in):
 *   cpc(in):
 */
static int
btree_get_subtree_capacity (THREAD_ENTRY * thread_p, BTID_INT * btid,
			    PAGE_PTR pg_ptr, BTREE_CAPACITY * cpc)
{
  RECDES rec;			/* Page record descriptor */
  int free_space;		/* Total free space of the Page */
  int key_cnt;			/* Page key count */
  NON_LEAF_REC nleaf_ptr;	/* NonLeaf Record pointer */
  VPID page_vpid;		/* Child page identifier */
  PAGE_PTR page = NULL;		/* Child page pointer */
  int i;			/* Loop counter */
  int offset;			/* Offset to the beginning of OID list */
  int oid_cnt;			/* Number of OIDs */
  VPID ovfl_vpid;		/* Overflow page identifier */
  RECDES orec;			/* Overflow record descriptor */
  LEAF_REC leaf_pnt;

  bool clear_key = false;
  PAGE_PTR ovfp = NULL;
  DB_VALUE key1;
  int oid_size;
  int ret = NO_ERROR;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  /* initialize */
  leaf_pnt.key_len = 0;
  VPID_SET_NULL (&leaf_pnt.ovfl);

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  /* initialize capacity structure */
  cpc->dis_key_cnt = 0;
  cpc->tot_val_cnt = 0;
  cpc->avg_val_per_key = 0;
  cpc->leaf_pg_cnt = 0;
  cpc->nleaf_pg_cnt = 0;
  cpc->tot_pg_cnt = 0;
  cpc->height = 0;
  cpc->sum_rec_len = 0;
  cpc->sum_key_len = 0;
  cpc->avg_key_len = 0;
  cpc->avg_rec_len = 0;
  cpc->tot_free_space = 0;
  cpc->tot_space = 0;
  cpc->tot_used_space = 0;
  cpc->avg_pg_key_cnt = 0;
  cpc->avg_pg_free_sp = 0;

  free_space = spage_get_free_space (thread_p, pg_ptr);

  key_cnt = btree_node_number_of_keys (pg_ptr);

  header = btree_get_node_header (pg_ptr);
  if (header == NULL)
    {
      goto exit_on_error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {				/* a non-leaf page */
      BTREE_CAPACITY cpc2;

      /* traverse all the subtrees of this non_leaf page and accumulate
       * the statistical data in the cpc structure
       */
      for (i = 1; i <= key_cnt; i++)
	{
	  if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	    {
	      goto exit_on_error;
	    }
	  btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf_ptr);
	  page_vpid = nleaf_ptr.pnt;
	  page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			    PGBUF_UNCONDITIONAL_LATCH);
	  if (page == NULL)
	    {
	      goto exit_on_error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, page, PAGE_BTREE);

	  ret = btree_get_subtree_capacity (thread_p, btid, page, &cpc2);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  /* form the cpc structure for a non-leaf node page */
	  cpc->dis_key_cnt += cpc2.dis_key_cnt;
	  cpc->tot_val_cnt += cpc2.tot_val_cnt;
	  cpc->leaf_pg_cnt += cpc2.leaf_pg_cnt;
	  cpc->nleaf_pg_cnt += cpc2.nleaf_pg_cnt;
	  cpc->tot_pg_cnt += cpc2.tot_pg_cnt;
	  cpc->height = cpc2.height + 1;
	  cpc->sum_rec_len += cpc2.sum_rec_len;
	  cpc->sum_key_len += cpc2.sum_key_len;
	  cpc->tot_free_space += cpc2.tot_free_space;
	  cpc->tot_space += cpc2.tot_space;
	  cpc->tot_used_space += cpc2.tot_used_space;
	  pgbuf_unfix_and_init (thread_p, page);
	}			/* for */
      cpc->avg_val_per_key = ((cpc->dis_key_cnt > 0) ?
			      (cpc->tot_val_cnt / cpc->dis_key_cnt) : 0);
      cpc->nleaf_pg_cnt += 1;
      cpc->tot_pg_cnt += 1;
      cpc->tot_free_space += free_space;
      cpc->tot_space += DB_PAGESIZE;
      cpc->tot_used_space += (DB_PAGESIZE - free_space);
      cpc->avg_key_len = ((cpc->dis_key_cnt > 0) ?
			  ((int) (cpc->sum_key_len / cpc->dis_key_cnt)) : 0);
      cpc->avg_rec_len = ((cpc->dis_key_cnt > 0) ?
			  ((int) (cpc->sum_rec_len / cpc->dis_key_cnt)) : 0);
      cpc->avg_pg_key_cnt = ((cpc->leaf_pg_cnt > 0) ?
			     ((int) (cpc->dis_key_cnt / cpc->leaf_pg_cnt)) :
			     0);
      cpc->avg_pg_free_sp = ((cpc->tot_pg_cnt > 0) ?
			     (cpc->tot_free_space / cpc->tot_pg_cnt) : 0);
    }
  else
    {				/* a leaf page */

      /* form the cpc structure for a leaf node page */
      cpc->dis_key_cnt = key_cnt;
      cpc->leaf_pg_cnt = 1;
      cpc->nleaf_pg_cnt = 0;
      cpc->tot_pg_cnt = 1;
      cpc->height = 1;
      for (i = 1; i <= cpc->dis_key_cnt; i++)
	{
	  if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	    {
	      goto exit_on_error;
	    }
	  cpc->sum_rec_len += rec.length;

	  /* read the current record key */
	  btree_read_record (thread_p, btid, pg_ptr, &rec, &key1, &leaf_pnt,
			     BTREE_LEAF_NODE, &clear_key, &offset,
			     PEEK_KEY_VALUE, NULL);
	  cpc->sum_key_len += btree_get_key_length (&key1);
	  btree_clear_key_value (&clear_key, &key1);

	  /* find the value (OID) count for the record */
	  oid_cnt = btree_leaf_get_num_oids (&rec, offset,
					     BTREE_LEAF_NODE, oid_size);

	  ovfl_vpid = leaf_pnt.ovfl;
	  if (!VPID_ISNULL (&ovfl_vpid))
	    {			/* overflow pages exist */
	      do
		{
		  ovfp = pgbuf_fix (thread_p, &ovfl_vpid, OLD_PAGE,
				    PGBUF_LATCH_READ,
				    PGBUF_UNCONDITIONAL_LATCH);
		  if (ovfp == NULL)
		    {
		      goto exit_on_error;
		    }

		  (void) pgbuf_check_page_ptype (thread_p, ovfp, PAGE_BTREE);

		  btree_get_next_overflow_vpid (ovfp, &ovfl_vpid);

		  if (spage_get_record (ovfp, 1, &orec, PEEK) != S_SUCCESS)
		    {
		      goto exit_on_error;
		    }

		  assert (mvcc_Enabled || oid_size == OR_OID_SIZE);
		  oid_cnt += btree_leaf_get_num_oids (&orec, 0,
						      BTREE_OVERFLOW_NODE,
						      oid_size);
		  pgbuf_unfix_and_init (thread_p, ovfp);
		}
	      while (!VPID_ISNULL (&ovfl_vpid));
	    }			/* if */
	  cpc->tot_val_cnt += oid_cnt;

	}			/* for */
      cpc->avg_val_per_key = ((cpc->dis_key_cnt > 0) ?
			      (cpc->tot_val_cnt / cpc->dis_key_cnt) : 0);
      cpc->avg_key_len = ((cpc->dis_key_cnt > 0) ?
			  ((int) (cpc->sum_key_len / cpc->dis_key_cnt)) : 0);
      cpc->avg_rec_len = ((cpc->dis_key_cnt > 0) ?
			  ((int) (cpc->sum_rec_len / cpc->dis_key_cnt)) : 0);
      cpc->tot_free_space = (float) free_space;
      cpc->tot_space = DB_PAGESIZE;
      cpc->tot_used_space = (cpc->tot_space - cpc->tot_free_space);
      cpc->avg_pg_key_cnt = ((cpc->leaf_pg_cnt > 0) ?
			     (cpc->dis_key_cnt / cpc->leaf_pg_cnt) : 0);
      cpc->avg_pg_free_sp = ((cpc->tot_pg_cnt > 0) ?
			     (cpc->tot_free_space / cpc->tot_pg_cnt) : 0);

    }				/* if-else */

  return ret;

exit_on_error:

  if (page)
    {
      pgbuf_unfix_and_init (thread_p, page);
    }
  if (ovfp)
    {
      pgbuf_unfix_and_init (thread_p, ovfp);
    }

  btree_clear_key_value (&clear_key, &key1);

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_index_capacity () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   cpc(out): Set to contain index capacity information
 *
 * Note: Form and return index capacity/space related information
 */
int
btree_index_capacity (THREAD_ENTRY * thread_p, BTID * btid,
		      BTREE_CAPACITY * cpc)
{
  VPID root_vpid;		/* root page identifier */
  PAGE_PTR root = NULL;		/* root page pointer */
  BTID_INT btid_int;
  BTREE_ROOT_HEADER *root_header = NULL;
  int ret = NO_ERROR;

  /* read root page */
  root_vpid.pageid = btid->root_pageid;
  root_vpid.volid = btid->vfid.volid;
  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  btid_int.sys_btid = btid;
  ret = btree_glean_root_header_info (thread_p, root_header, &btid_int);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* traverse the tree and store the capacity info */
  ret = btree_get_subtree_capacity (thread_p, &btid_int, root, cpc);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  pgbuf_unfix_and_init (thread_p, root);

  return ret;

exit_on_error:

  if (root)
    {
      pgbuf_unfix_and_init (thread_p, root);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_dump_capacity () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *
 * Note: Dump index capacity/space information.
 */
int
btree_dump_capacity (THREAD_ENTRY * thread_p, FILE * fp, BTID * btid)
{
  BTREE_CAPACITY cpc;
  int ret = NO_ERROR;
  char area[FILE_DUMP_DES_AREA_SIZE];
  char *file_des = NULL;
  char *index_name = NULL;
  char *class_name = NULL;
  int file_des_size = 0;
  int size = 0;
  OID class_oid;

  assert (fp != NULL && btid != NULL);

  /* get index capacity information */
  ret = btree_index_capacity (thread_p, btid, &cpc);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* get class_name and index_name */
  file_des = area;
  file_des_size = FILE_DUMP_DES_AREA_SIZE;

  size = file_get_descriptor (thread_p, &btid->vfid, file_des, file_des_size);
  if (size <= 0)
    {
      goto exit_on_error;
    }

  class_oid = ((FILE_HEAP_DES *) file_des)->class_oid;

  class_name = heap_get_class_name (thread_p, &class_oid);

  /* get index name */
  if (heap_get_indexinfo_of_btid (thread_p, &class_oid, btid,
				  NULL, NULL, NULL, NULL, &index_name,
				  NULL) != NO_ERROR)
    {
      goto exit_on_error;
    }

  fprintf (fp,
	   "\n--------------------------------------------------"
	   "-----------\n");
  fprintf (fp, "BTID: {{%d, %d}, %d}, %s ON %s, CAPACITY INFORMATION:\n",
	   btid->vfid.volid, btid->vfid.fileid, btid->root_pageid,
	   (index_name == NULL) ? "*UNKOWN_INDEX*" : index_name,
	   (class_name == NULL) ? "*UNKOWN_CLASS*" : class_name);

  /* dump the capacity information */
  fprintf (fp, "\nDistinct Key Count: %d\n", cpc.dis_key_cnt);
  fprintf (fp, "Total Value Count: %d\n", cpc.tot_val_cnt);
  fprintf (fp, "Average Value Count Per Key: %d\n", cpc.avg_val_per_key);
  fprintf (fp, "Total Page Count: %d\n", cpc.tot_pg_cnt);
  fprintf (fp, "Leaf Page Count: %d\n", cpc.leaf_pg_cnt);
  fprintf (fp, "NonLeaf Page Count: %d\n", cpc.nleaf_pg_cnt);
  fprintf (fp, "Height: %d\n", cpc.height);
  fprintf (fp, "Average Key Length: %d\n", cpc.avg_key_len);
  fprintf (fp, "Average Record Length: %d\n", cpc.avg_rec_len);
  fprintf (fp, "Total Index Space: %.0f bytes\n", cpc.tot_space);
  fprintf (fp, "Used Index Space: %.0f bytes\n", cpc.tot_used_space);
  fprintf (fp, "Free Index Space: %.0f bytes\n", cpc.tot_free_space);
  fprintf (fp, "Average Page Free Space: %.0f bytes\n", cpc.avg_pg_free_sp);
  fprintf (fp, "Average Page Key Count: %d\n", cpc.avg_pg_key_cnt);
  fprintf (fp, "--------------------------------------------------"
	   "-----------\n");

end:

  if (class_name != NULL)
    {
      free_and_init (class_name);
    }

  if (index_name != NULL)
    {
      free_and_init (index_name);
    }

  return ret;

exit_on_error:

  assert (ret != NO_ERROR);
  if (ret == NO_ERROR)
    {
      assert (er_errid () != NO_ERROR);
      ret = er_errid ();
      if (ret == NO_ERROR)
	{
	  ret = ER_FAILED;
	}
    }

  goto end;
}

/*
 * btree_dump_capacity_all () -
 *   return: NO_ERROR
 *
 * Note: Dump the capacity/space information of all indices.
 */
int
btree_dump_capacity_all (THREAD_ENTRY * thread_p, FILE * fp)
{
  int num_files;		/* Number of files in the system */
  BTID btid;			/* Btree index identifier */
  VPID vpid;			/* Index root page identifier */
  int i;			/* Loop counter */
  int ret = NO_ERROR;

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files < 0)
    {
      goto exit_on_error;
    }

  /* Go to each file, check only the btree files */
  for (i = 0; i < num_files; i++)
    {
      if (file_find_nthfile (thread_p, &btid.vfid, i) != 1)
	{
	  break;
	}

      if (file_get_type (thread_p, &btid.vfid) != FILE_BTREE)
	{
	  continue;
	}

      if (file_find_nthpages (thread_p, &btid.vfid, &vpid, 0, 1) != 1)
	{
	  goto exit_on_error;
	}

      btid.root_pageid = vpid.pageid;

      ret = btree_dump_capacity (thread_p, fp, &btid);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }				/* for */

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * b+tree dump routines
 */

/*
 * btree_print_space () -
 *   return:
 *   n(in):
 */
static void
btree_print_space (FILE * fp, int n)
{

  while (n--)			/* print n space character */
    {
      fprintf (fp, " ");
    }

}

/*
 * btree_dump_page () -
 *   return: nothing
 *   btid(in): B+tree index identifier
 *   page_ptr(in): Page pointer
 *   pg_vpid(in): Page identifier
 *   n(in): Identation left margin (number of preceding blanks)
 *   level(in):
 *
 * Note: Dumps the content of the given page of the tree.
 */
static void
btree_dump_page (THREAD_ENTRY * thread_p, FILE * fp,
		 const OID * class_oid_p, BTID_INT * btid,
		 const char *btname, PAGE_PTR page_ptr, VPID * pg_vpid,
		 int depth, int level)
{
  int key_cnt;
  int i;
  RECDES rec;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  VPID vpid;

  if (pg_vpid == NULL)
    {
      pgbuf_get_vpid (page_ptr, &vpid);
      pg_vpid = &vpid;
    }

  key_cnt = btree_node_number_of_keys (page_ptr);

  /* get the header record */
  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      btree_print_space (fp, depth * 4);
      fprintf (fp, "btree_dump_page: get node header failure: %d\n", key_cnt);
      return;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  btree_print_space (fp, depth * 4);
  fprintf (fp,
	   "[%s PAGE {%d, %d}, level: %d, depth: %d, keys: %d, "
	   "Prev: {%d, %d}, Next: {%d, %d}, Max key len: %d]\n",
	   node_type_to_string (node_type),
	   pg_vpid->volid, pg_vpid->pageid,
	   header->node_level, depth, key_cnt,
	   header->prev_vpid.volid, header->prev_vpid.pageid,
	   header->next_vpid.volid, header->next_vpid.pageid,
	   header->max_key_len);

  if (class_oid_p && !OID_ISNULL (class_oid_p))
    {
      char *class_name_p = NULL;
      class_name_p = heap_get_class_name (thread_p, class_oid_p);

      btree_print_space (fp, depth * 4);
      fprintf (fp, "INDEX %s ON CLASS %s (CLASS_OID:%2d|%4d|%2d) \n\n",
	       (btname) ? btname : "*UNKNOWN-INDEX*",
	       (class_name_p) ? class_name_p : "*UNKNOWN-CLASS*",
	       class_oid_p->volid, class_oid_p->pageid, class_oid_p->slotid);
      if (class_name_p)
	{
	  free_and_init (class_name_p);
	}
    }

  fflush (fp);

  if (key_cnt < 0)
    {
      btree_print_space (fp, depth * 4);
      fprintf (fp,
	       "btree_dump_page: node key count underflow: %d\n", key_cnt);
      return;
    }

  if (level > 1)
    {
      /* output the content of each record */
      for (i = 1; i <= key_cnt; i++)
	{
	  (void) spage_get_record (page_ptr, i, &rec, PEEK);
	  if (node_type == BTREE_LEAF_NODE)
	    {
	      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
		{
		  fprintf (fp, "(F)");
		}
	      else
		{
		  fprintf (fp, "   ");
		}

	      btree_dump_leaf_record (thread_p, fp, btid, &rec, depth);
	    }
	  else
	    {
	      btree_dump_non_leaf_record (thread_p, fp, btid, &rec, depth, 1);
	    }
	  /* fprintf (fp, "\n"); */
	}
    }

  fprintf (fp, "\n");
}

/*
 * btree_dump_page_with_subtree () -
 *   return: nothing
 *   btid(in): B+tree index identifier
 *   pg_ptr(in): Page pointer
 *   pg_vpid(in): Page identifier
 *   n(in): Identation left margin (number of preceding blanks)
 *   level(in):
 *
 * Note: Dumps the content of the given page together with its subtrees
 */
static void
btree_dump_page_with_subtree (THREAD_ENTRY * thread_p, FILE * fp,
			      BTID_INT * btid, PAGE_PTR pg_ptr,
			      VPID * pg_vpid, int depth, int level)
{
  int key_cnt;
  int i;
  NON_LEAF_REC nleaf_ptr;
  VPID page_vpid;
  PAGE_PTR page = NULL;
  RECDES rec;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  key_cnt = btree_node_number_of_keys (pg_ptr);

  btree_dump_page (thread_p, fp, NULL, btid, NULL, pg_ptr, pg_vpid, depth, level);	/* dump current page */

  /* get the header record */
  header = btree_get_node_header (pg_ptr);
  if (header == NULL)
    {
      fprintf (fp,
	       "btree_dump_page_with_subtree: get node header failure: %d.\n",
	       key_cnt);
      return;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {				/* page is non_leaf */
#if !defined(NDEBUG)
      if (key_cnt < 0)
	{
	  fprintf (fp,
		   "btree_dump_page_with_subtree: node key count underflow: %d.\n",
		   key_cnt);
	  return;
	}
#endif

      /* for each child page pointer in this non_leaf page,
       * dump the corresponding subtree
       */
      for (i = 1; i <= key_cnt; i++)
	{
	  (void) spage_get_record (pg_ptr, i, &rec, PEEK);
	  btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf_ptr);
	  page_vpid = nleaf_ptr.pnt;
	  page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			    PGBUF_UNCONDITIONAL_LATCH);
	  if (page == NULL)
	    {
	      return;
	    }
	  btree_dump_page_with_subtree (thread_p, fp, btid, page, &page_vpid,
					depth + 1, level);
	  pgbuf_unfix_and_init (thread_p, page);
	}
    }

  return;
}

/*
 * btree_dump () -
 *   return: nothing
 *   btid(in): B+tree index identifier
 *   level(in):
 *
 * Note: Dumps the content of the each page in the B+tree by
 * traversing the tree in an "inorder" manner. The header
 * information, as well as the content of each record in a page
 * are dumped. The header information for a non_leaf page
 * contains the key count and maximum key length information.
 * Maximum key length refers to the longest key in the page and
 * in its subtrees. The header information for a leaf page
 * contains also the next_page information, which is the page
 * identifier of the next sibling page, and the overflow page
 * count information. root header information contains
 * statistical data for the whole tree. These consist of total
 * key count of the tree, total page count, leaf page count,
 * non_leaf page count, total overflow page count and the height
 * of the tree. Total key count refers only to those keys that
 * are stored in the leaf pages of the tree. The index key type
 * is also stored in the root header.
 */
void
btree_dump (THREAD_ENTRY * thread_p, FILE * fp, BTID * btid, int level)
{
  VPID p_vpid;
  PAGE_PTR root = NULL;
  BTID_INT btid_int;
  BTREE_ROOT_HEADER *root_header = NULL;

  p_vpid.pageid = btid->root_pageid;	/* read root page */
  p_vpid.volid = btid->vfid.volid;
  root = pgbuf_fix (thread_p, &p_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      return;
    }

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      goto end;			/* do nothing */
    }

  btid_int.sys_btid = btid;
  if (btree_glean_root_header_info (thread_p, root_header, &btid_int) !=
      NO_ERROR)
    {
      goto end;			/* do nothing */
    }

  fprintf (fp,
	   "\n------------ The B+Tree Index Dump Start ---------------------\n\n\n");
  btree_dump_root_header (fp, root);	/* output root header information */

  if (level != 0)
    {
      btree_dump_page_with_subtree (thread_p, fp, &btid_int, root, &p_vpid, 0,
				    level);
    }

  fprintf (fp,
	   "\n------------ The B+Tree Index Dump End ---------------------\n\n\n");

end:
  pgbuf_unfix_and_init (thread_p, root);

  return;
}


/*
 * btree_read_key_type () -
 *   return:
 *   btid(in):
 */
TP_DOMAIN *
btree_read_key_type (THREAD_ENTRY * thread_p, BTID * btid)
{
  VPID p_vpid;
  PAGE_PTR root = NULL;
  TP_DOMAIN *key_type = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;

  p_vpid.pageid = btid->root_pageid;	/* read root page */
  p_vpid.volid = btid->vfid.volid;
  root = pgbuf_fix (thread_p, &p_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      return NULL;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      pgbuf_unfix_and_init (thread_p, root);
      return NULL;
    }

  (void) or_unpack_domain (root_header->packed_key_domain, &key_type, 0);

  pgbuf_unfix_and_init (thread_p, root);

  return key_type;
}

/*
 * btree_delete_key_from_leaf () -
 *   return:
 *   btid(in):
 *   leaf_pg(in):
 *   slot_id(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   leaf_rec(in):
 *   leafrec_pnt(in):
 */
static int
btree_delete_key_from_leaf (THREAD_ENTRY * thread_p, BTID_INT * btid,
			    PAGE_PTR leaf_pg, INT16 slot_id,
			    DB_VALUE * key, OID * oid, OID * class_oid,
			    RECDES * leaf_rec, LEAF_REC * leafrec_pnt,
			    MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int key_cnt;
  BTREE_NODE_HEADER *header = NULL;

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (leafrec_pnt->key_len < 0
      && (logtb_is_current_active (thread_p) == true
	  || btree_is_new_file (btid)))
    {
      ret = btree_delete_overflow_key (thread_p, btid, leaf_pg, slot_id,
				       BTREE_LEAF_NODE);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
				 BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_DEL,
			     &btid->sys_btid->vfid, leaf_pg, slot_id,
			     rv_data_len, rv_data);
    }
  else
    {
      log_append_undoredo_data2 (thread_p, RVBT_KEYVAL_DEL_LFRECORD_DEL,
				 &btid->sys_btid->vfid, leaf_pg, slot_id,
				 rv_key_len, 0, rv_key, NULL);
    }

  RANDOM_EXIT (thread_p);

  header = btree_get_node_header (leaf_pg);
  if (header == NULL)
    {
      goto exit_on_error;
    }

  /* now delete the btree slot */
  assert (slot_id > 0);
  if (spage_delete (thread_p, leaf_pg, slot_id) != slot_id)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      btree_node_header_undo_log (thread_p, &btid->sys_btid->vfid, leaf_pg);
    }

  RANDOM_EXIT (thread_p);

  /* key deleted, update node header */
  key_cnt = btree_node_number_of_keys (leaf_pg);
  if (key_cnt == 0)
    {
      header->max_key_len = 0;
    }

  RANDOM_EXIT (thread_p);

  if (btree_is_new_file (btid))
    {
      log_append_redo_data2 (thread_p, RVBT_KEYVAL_DEL_LFRECORD_DEL,
			     &btid->sys_btid->vfid,
			     leaf_pg, slot_id, 0, NULL);
    }

  pgbuf_set_dirty (thread_p, leaf_pg, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, leaf_pg);
#endif

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_swap_first_oid_with_ovfl_rec () -
 *   return:
 *   btid(in):
 *   leaf_page(in):
 *   slot_id(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   leaf_rec(in):
 *   ovfl_vpid(in):
 */
static int
btree_swap_first_oid_with_ovfl_rec (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    PAGE_PTR leaf_page, INT16 slot_id,
				    DB_VALUE * key, OID * oid,
				    OID * class_oid, RECDES * leaf_rec,
				    VPID * ovfl_vpid,
				    MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  OID last_oid, last_class_oid;
  PAGE_PTR ovfl_page = NULL;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  RECDES ovfl_copy_rec;
  char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  VPID next_ovfl_vpid;
  int oid_cnt;
  MVCC_REC_HEADER last_oid_mvcc_header, *p_last_oid_mvcc_header = NULL;
  int last_oid_mvcc_offset = 0, *p_last_oid_mvcc_offset = NULL;
  int oid_size = OR_OID_SIZE;

  assert (btid != NULL && leaf_page != NULL && slot_id >= 0
	  && leaf_rec != NULL);
  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size += OR_OID_SIZE;
    }

  ovfl_copy_rec.area_size = DB_PAGESIZE;
  ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  ovfl_page = pgbuf_fix (thread_p, ovfl_vpid, OLD_PAGE,
			 PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
  if (ovfl_page == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

  if (spage_get_record (ovfl_page, 1, &ovfl_copy_rec, COPY) != S_SUCCESS)
    {
      goto exit_on_error;
    }

  assert (ovfl_copy_rec.length % 4 == 0);

  btree_get_next_overflow_vpid (ovfl_page, &next_ovfl_vpid);

  oid_cnt = btree_leaf_get_num_oids (&ovfl_copy_rec, 0,
				     BTREE_OVERFLOW_NODE, oid_size);
  assert (oid_cnt >= 1);

  if (mvcc_Enabled)
    {
      p_last_oid_mvcc_header = &last_oid_mvcc_header;
      p_last_oid_mvcc_offset = &last_oid_mvcc_offset;
    }

  btree_leaf_get_last_oid (btid, &ovfl_copy_rec, BTREE_OVERFLOW_NODE, 0,
			   &last_oid, &last_class_oid, p_last_oid_mvcc_header,
			   p_last_oid_mvcc_offset);

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
				 BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, leaf_page, slot_id,
			     rv_data_len, rv_data);
    }
  else
    {
      log_append_undo_data2 (thread_p, RVBT_KEYVAL_DEL,
			     &btid->sys_btid->vfid,
			     ovfl_page, 1, rv_key_len, rv_key);
    }

  if (oid_cnt > 1)
    {
      if (btree_is_new_file (btid))
	{
	  btree_rv_write_log_record (rv_data, &rv_data_len, &ovfl_copy_rec,
				     BTREE_LEAF_NODE);
	  log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
				 &btid->sys_btid->vfid, ovfl_page, 1,
				 rv_data_len, rv_data);
	}

      btree_leaf_remove_last_oid (btid, &ovfl_copy_rec, BTREE_OVERFLOW_NODE,
				  oid_size, last_oid_mvcc_offset);
      assert (ovfl_copy_rec.length % 4 == 0);

#if !defined (NDEBUG)
      btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				BTREE_OVERFLOW_NODE, key);
#endif

      if (spage_update (thread_p, ovfl_page, 1, &ovfl_copy_rec) != SP_SUCCESS)
	{
	  goto exit_on_error;
	}

      btree_rv_write_log_record (rv_data, &rv_data_len, &ovfl_copy_rec,
				 BTREE_LEAF_NODE);
      log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, ovfl_page, 1,
			     rv_data_len, rv_data);

      pgbuf_set_dirty (thread_p, ovfl_page, FREE);
      ovfl_page = NULL;
    }
  else
    {
      pgbuf_unfix_and_init (thread_p, ovfl_page);

      ret = file_dealloc_page (thread_p, &btid->sys_btid->vfid, ovfl_vpid);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      if (!VPID_ISNULL (&next_ovfl_vpid))
	{
	  btree_leaf_update_overflow_oids_vpid (leaf_rec, &next_ovfl_vpid);
	}
      else
	{
	  leaf_rec->length -= DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
	  btree_leaf_clear_flag (leaf_rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS);
	  if (mvcc_Enabled)
	    {
	      /* Both insert and delete MVCCID's exist and must make sure the
	       * correct flags are set.
	       */
	      btree_leaf_key_oid_set_mvcc_flag (leaf_rec->data,
						BTREE_LEAF_OID_HAS_MVCC_INSID
						|
						BTREE_LEAF_OID_HAS_MVCC_DELID);
	    }
	}
    }

  assert (mvcc_Enabled || OID_ISNULL (&last_class_oid));

  btree_leaf_change_first_oid (leaf_rec, btid, &last_oid, &last_class_oid,
			       p_last_oid_mvcc_header, NULL);
  assert (leaf_rec->length % 4 == 0);

  assert (slot_id > 0);
  if (spage_update (thread_p, leaf_page, slot_id, leaf_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
			     BTREE_LEAF_NODE);
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD, &btid->sys_btid->vfid,
			 leaf_page, slot_id, rv_data_len, rv_data);
  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  if (ovfl_page)
    {
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_delete_oid_from_leaf () -
 *   return:
 *   btid(in):
 *   leaf_page(in):
 *   slot_id(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   leaf_rec(in):
 *   del_oid_offset(in):
 *   oid_list_offset(in/out): oid list offset, may be changed when delete first
 *			    OID
 */
static int
btree_delete_oid_from_leaf (THREAD_ENTRY * thread_p, BTID_INT * btid,
			    PAGE_PTR leaf_page, INT16 slot_id,
			    DB_VALUE * key, OID * oid, OID * class_oid,
			    RECDES * leaf_rec, int del_oid_offset,
			    int *oid_list_offset,
			    MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  OID last_oid, last_class_oid;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len, oid_size;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  MVCC_REC_HEADER last_oid_mvcc_header, *p_last_oid_mvcc_header = NULL;
  int last_oid_mvcc_offset, *p_last_oid_mvcc_offset = NULL;
  int key_offset;

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  if (mvcc_args == NULL
      || mvcc_args->purpose != MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO)
    {
      if (btree_is_new_file (btid))
	{
	  btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
				     BTREE_LEAF_NODE);
	  log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
				 &btid->sys_btid->vfid, leaf_page, slot_id,
				 rv_data_len, rv_data);
	}
      else
	{
	  /* Do not log undo operation for relocating an object from leaf
	   * to overflow.
	   */
	  log_append_undo_data2 (thread_p, RVBT_KEYVAL_DEL,
				 &btid->sys_btid->vfid,
				 leaf_page, slot_id, rv_key_len, rv_key);
	}
    }

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  if (mvcc_Enabled && !BTREE_IS_UNIQUE (btid->unique_pk)
      && del_oid_offset != 0)
    {
      /* Where are here because:
       * 1. MVCC is enabled.
       * 2. B-tree is not unique.
       * 3. Object is not first.
       * In this case, the total occupied size of deleted object is not
       * constant due to variable MVCC information. We cannot just replace it
       * with last object because their size may not be equal. It is better to
       * just remove the object by moving the rest of the record.
       * Note that if the object being deleted is first is a special case, it
       * still must be replaced with the last object.
       */
      btree_delete_mvcc_object (leaf_rec, del_oid_offset, oid_size, false);
    }
  else
    {
      /* Replace the deleted object with the last object in the record */
      if (mvcc_Enabled)
	{
	  /* Besides OID/class OID of last object, we will also need the
	   * offset to it (it can have variable size) and its MVCC info.
	   */
	  p_last_oid_mvcc_header = &last_oid_mvcc_header;
	  p_last_oid_mvcc_offset = &last_oid_mvcc_offset;
	}
      /* Get last object from record */
      btree_leaf_get_last_oid (btid, leaf_rec, BTREE_LEAF_NODE,
			       *oid_list_offset, &last_oid, &last_class_oid,
			       p_last_oid_mvcc_header, &last_oid_mvcc_offset);

      /* Remove last object */
      btree_leaf_remove_last_oid (btid, leaf_rec, BTREE_LEAF_NODE, oid_size,
				  last_oid_mvcc_offset);
      assert (leaf_rec->length % 4 == 0);

      if (!OID_EQ (oid, &last_oid))
	{
	  /* There would be nothing else to do if the deleted object was last.
	   */
	  if (del_oid_offset == 0)
	    {
	      /* Delete first oid */
	      btree_leaf_change_first_oid (leaf_rec, btid, &last_oid,
					   &last_class_oid,
					   p_last_oid_mvcc_header,
					   &key_offset);
	      (*oid_list_offset) += key_offset;
	    }
	  else
	    {
	      /* Delete a non-first and non-last object */
	      if (mvcc_Enabled)
		{
		  /* We are here because:
		   * 1. MVCC is enabled.
		   * 2. B-tree is unique.
		   * 3. This is not the first or the last object.
		   * We must set both flags in OID, add the OID, the class
		   * OID, and insert/delete MVCCID.
		   */
		  last_oid.volid |=
		    (BTREE_LEAF_OID_HAS_MVCC_INSID
		     | BTREE_LEAF_OID_HAS_MVCC_DELID);
		  OR_PUT_OID ((leaf_rec->data + del_oid_offset), &last_oid);
		  OR_PUT_OID ((leaf_rec->data + del_oid_offset + OR_OID_SIZE),
			      &last_class_oid);
		  OR_PUT_MVCCID (leaf_rec->data + del_oid_offset
				 + 2 * OR_OID_SIZE,
				 &MVCC_GET_INSID (p_last_oid_mvcc_header));
		  OR_PUT_MVCCID (leaf_rec->data + del_oid_offset
				 + 2 * OR_OID_SIZE + OR_MVCCID_SIZE,
				 &MVCC_GET_DELID (p_last_oid_mvcc_header));
		}
	      else
		{
		  /* We are here because:
		   * 1. MVCC is not enabled.
		   * 2. The object is not first and is not last.
		   */
		  OR_PUT_OID ((leaf_rec->data + del_oid_offset), &last_oid);
		}
	    }
	}
    }
#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, leaf_rec, BTREE_LEAF_NODE, key);
#endif

  assert (slot_id > 0);
  if (spage_update (thread_p, leaf_page, slot_id, leaf_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
			     BTREE_LEAF_NODE);
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD, &btid->sys_btid->vfid,
			 leaf_page, slot_id, rv_data_len, rv_data);

  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_modify_leaf_ovfl_vpid () -
 *   return:
 *   btid(in):
 *   leaf_page(in):
 *   slot_id(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   leaf_rec(in):
 *   next_ovfl_vpid(in):
 */
static int
btree_modify_leaf_ovfl_vpid (THREAD_ENTRY * thread_p, BTID_INT * btid,
			     PAGE_PTR leaf_page, INT16 slot_id,
			     DB_VALUE * key, OID * oid, OID * class_oid,
			     RECDES * leaf_rec, VPID * next_ovfl_vpid,
			     MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
				 BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, leaf_page, slot_id,
			     rv_data_len, rv_data);
    }

  if (!VPID_ISNULL (next_ovfl_vpid))
    {
      btree_leaf_update_overflow_oids_vpid (leaf_rec, next_ovfl_vpid);
    }
  else
    {
      leaf_rec->length -= DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
      btree_leaf_clear_flag (leaf_rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS);
    }

  btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
			     BTREE_LEAF_NODE);
  assert (leaf_rec->length % 4 == 0);

  if (btree_is_new_file (btid) != true)
    {
      log_append_undoredo_data2 (thread_p, RVBT_KEYVAL_DEL_NDRECORD_UPD,
				 &btid->sys_btid->vfid, leaf_page, slot_id,
				 rv_key_len, rv_data_len, rv_key, rv_data);
    }

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, leaf_rec, BTREE_LEAF_NODE, key);
#endif

  assert (slot_id > 0);
  if (spage_update (thread_p, leaf_page, slot_id, leaf_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      log_append_redo_data2 (thread_p, RVBT_KEYVAL_DEL_NDRECORD_UPD,
			     &btid->sys_btid->vfid, leaf_page,
			     slot_id, rv_data_len, rv_data);
    }

  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_modify_overflow_link () -
 *   return:
 *   btid(in):
 *   ovfl_page(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   ovfl_rec(in):
 *   next_ovfl_vpid(in):
 */
static int
btree_modify_overflow_link (THREAD_ENTRY * thread_p, BTID_INT * btid,
			    PAGE_PTR ovfl_page, DB_VALUE * key, OID * oid,
			    OID * class_oid, RECDES * ovfl_rec,
			    VPID * next_ovfl_vpid,
			    MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  BTREE_OVERFLOW_HEADER ovf_header_info, *ovf_header = NULL;

  ovf_header = &ovf_header_info;

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, ovfl_rec,
				 BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, ovfl_page,
			     HEADER, rv_data_len, rv_data);
    }

  ovf_header->next_vpid = *next_ovfl_vpid;
  memcpy (ovfl_rec->data, ovf_header, sizeof (BTREE_OVERFLOW_HEADER));
  ovfl_rec->length = sizeof (BTREE_OVERFLOW_HEADER);

  if (btree_is_new_file (btid) != true)
    {
      log_append_undoredo_data2 (thread_p, RVBT_KEYVAL_DEL_NDHEADER_UPD,
				 &btid->sys_btid->vfid,
				 ovfl_page, HEADER, rv_key_len,
				 ovfl_rec->length, rv_key, ovfl_rec->data);
    }

  if (spage_update (thread_p, ovfl_page, HEADER, ovfl_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      log_append_redo_data2 (thread_p, RVBT_KEYVAL_DEL_NDHEADER_UPD,
			     &btid->sys_btid->vfid, ovfl_page, HEADER,
			     ovfl_rec->length, ovfl_rec->data);
    }

  pgbuf_set_dirty (thread_p, ovfl_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_delete_oid_from_ovfl () -
 *   return:
 *   btid(in):
 *   ovfl_page(in):
 *   key(in):
 *   oid(in):
 *   class_oid(in):
 *   ovfl_rec(in):
 *   del_oid_offset(in):
 */
static int
btree_delete_oid_from_ovfl (THREAD_ENTRY * thread_p, BTID_INT * btid,
			    PAGE_PTR ovfl_page, DB_VALUE * key,
			    OID * oid, OID * class_oid,
			    RECDES * ovfl_rec, int del_oid_offset,
			    MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  OID last_oid, last_class_oid;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  MVCC_REC_HEADER last_oid_mvcc_header;
  int size = OR_OID_SIZE;

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  ret = btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			      &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  btree_leaf_get_last_oid (btid, ovfl_rec, BTREE_OVERFLOW_NODE, 0,
			   &last_oid, &last_class_oid, &last_oid_mvcc_header,
			   NULL);

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, ovfl_rec,
				 BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, ovfl_page, 1,
			     rv_data_len, rv_data);
    }
  else
    {
      log_append_undo_data2 (thread_p, RVBT_KEYVAL_DEL, &btid->sys_btid->vfid,
			     ovfl_page, 1, rv_key_len, rv_key);
    }

  if (mvcc_Enabled)
    {
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  size = 2 * OR_OID_SIZE + 2 * OR_MVCCID_SIZE;
	}
      else
	{
	  size = OR_OID_SIZE + 2 * OR_MVCCID_SIZE;
	}
    }

  if (!OID_EQ (oid, &last_oid))
    {
      BTREE_LEAF_MOVE_INSIDE_RECORD (ovfl_rec, del_oid_offset,
				     del_oid_offset + size);
    }
  else
    {
      ovfl_rec->length -= size;
    }

  assert (ovfl_rec->length % 4 == 0);

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, ovfl_rec, BTREE_OVERFLOW_NODE,
			    key);
#endif

  if (spage_update (thread_p, ovfl_page, 1, ovfl_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  btree_rv_write_log_record (rv_data, &rv_data_len, ovfl_rec,
			     BTREE_LEAF_NODE);

  log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD,
			 &btid->sys_btid->vfid, ovfl_page, 1,
			 rv_data_len, rv_data);

  pgbuf_set_dirty (thread_p, ovfl_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_delete_from_leaf () -
 *   return: NO_ERROR
 *   key_deleted(out):
 *   btid(in):
 *   leaf_vpid(in):
 *   key(in):
 *   class_oid(in):
 *   oid(in):
 *   leaf_slot_id(in):
 *   mvcc_args(in): arguments used in MVCC context only.
 *
 * LOGGING Note: When the btree is new, splits and merges will
 * not be committed, but will be attached.  If the transaction
 * is rolled back, the merge and split actions will be rolled
 * back as well.  The undo (and redo) logging for splits and
 * merges are page based (physical) logs, thus the rest of the
 * logs for the undo session must be page based as well.  When
 * the btree is old, splits and merges are committed and all
 * the rest of the logging must be logical (non page based)
 * since pages may change as splits and merges are performed.
 *
 * LOGGING Note2: We adopts a new concept of log, that is a combined log of
 * logical undo and physical redo log, for performance reasons.
 * For key delete, this will be written only when the btree is old.
 * However each undo log and redo log will be written as it is in the rest of
 * the cases(need future work).
 * Condition:
 *     When the btree is old
 * Algorithm:
 *     // find and remove the last oid, shorten OID list
 *     do {
 *	   oid_cnt--;
 *	   if (oid_cnt == 0) {
 *	       if (last_oid_is_in_leaf) {
 *	           logical_undo_physical_redo(); // CASE-A
 *	       } else { // last_oid_is_in_overflow, and overflow is empty
 *		   if (prev_is_leaf) {
 *		       logical_undo_physical_redo(); // CASE-B-1
 *		   } else { // prev_is_still_overflow
 *		       logical_undo_physical_redo(); // CASE-B-2
 *		   }
 *	       }
 *	   } else { // we still some OIDs in the list
 *	       logical_undo_physical_redo(); // CASE-C
 *	   }
 *     } while ();
 *
 *     if (oid != last_oid) {
 *         // replace deleting oid with the last oid
 *     }
 */
static int
btree_delete_from_leaf (THREAD_ENTRY * thread_p, bool * key_deleted,
			BTID_INT * btid, VPID * leaf_vpid, DB_VALUE * key,
			OID * class_oid, OID * oid, INT16 leaf_slot_id,
			MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  PAGE_PTR leaf_page, ovfl_page, prev_page;
  LEAF_REC leaf_rec;
  VPID ovfl_vpid, next_ovfl_vpid;
  RECDES leaf_copy_rec;
  char leaf_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  RECDES ovfl_copy_rec;
  char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int oid_size, oid_offset, oid_list_offset, oid_cnt;
  bool dummy;
  bool mvcc_key_deleted =
    mvcc_Enabled && (key_deleted != NULL)
    && (mvcc_args == NULL || mvcc_args->purpose == MVCC_BTREE_DELETE_OBJECT);
  int num_visible_oids = 0;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;

  mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;

  VPID_SET_NULL (&leaf_rec.ovfl);
  leaf_rec.key_len = 0;
  leaf_copy_rec.area_size = DB_PAGESIZE;
  leaf_copy_rec.data = PTR_ALIGN (leaf_copy_rec_buf, BTREE_MAX_ALIGN);
  ovfl_copy_rec.area_size = DB_PAGESIZE;
  ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);

  leaf_page = pgbuf_fix (thread_p, leaf_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			 PGBUF_UNCONDITIONAL_LATCH);
  if (leaf_page == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, leaf_page, PAGE_BTREE);

  /* find the slot for the key */
  if (leaf_slot_id == NULL_SLOTID)
    {
      if (!btree_search_leaf_page (thread_p, btid, leaf_page,
				   key, &leaf_slot_id))
	{
	  /* key does not exist */
	  log_append_redo_data2 (thread_p, RVBT_NOOP, &btid->sys_btid->vfid,
				 leaf_page, -1, 0, NULL);
	  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);
	  if (!thread_is_vacuum_worker (thread_p))
	    {
	      btree_set_unknown_key_error (thread_p, btid->sys_btid, key,
					   "btree_delete_from_leaf: "
					   "btree_search_leaf_page fails.");
	    }

	  pgbuf_unfix_and_init (thread_p, leaf_page);
	  goto exit_on_error;
	}
    }

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  /* leaf page */
  assert (leaf_slot_id > 0);
  if (spage_get_record (leaf_page, leaf_slot_id, &leaf_copy_rec,
			COPY) != S_SUCCESS)
    {
      pgbuf_unfix_and_init (thread_p, leaf_page);
      goto exit_on_error;
    }

  assert (leaf_copy_rec.length % 4 == 0);

  btree_read_record (thread_p, btid, leaf_page, &leaf_copy_rec, NULL,
		     &leaf_rec, BTREE_LEAF_NODE, &dummy, &oid_list_offset,
		     PEEK_KEY_VALUE, NULL);

  oid_offset =
    btree_find_oid_from_leaf (btid, &leaf_copy_rec, oid_list_offset, oid,
			      mvcc_args, NULL);

  if (oid_offset != NOT_FOUND)
    {
      if (MVCC_BTREE_DELETE_REMOVE_DELID (mvcc_args))
	{
	  /* remove only del_id from leaf page */
	  assert (mvcc_Enabled == true);

	  ret =
	    btree_delete_mvcc_delid_from_page (thread_p, btid, leaf_page,
					       BTREE_LEAF_NODE, leaf_slot_id,
					       key, oid, class_oid,
					       &leaf_copy_rec, oid_offset,
					       mvcc_args);
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &leaf_copy_rec,
				    BTREE_LEAF_NODE, key);
#endif
	  pgbuf_unfix_and_init (thread_p, leaf_page);
	  return ret;
	}
      else if (MVCC_BTREE_DELETE_VACUUM_INSID (mvcc_args))
	{
	  ret =
	    btree_delete_mvcc_insid_from_page (thread_p, btid, leaf_page,
					       BTREE_LEAF_NODE, leaf_slot_id,
					       key, oid, class_oid,
					       &leaf_copy_rec, oid_offset,
					       mvcc_args);
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &leaf_copy_rec,
				    BTREE_LEAF_NODE, key);
#endif
	  pgbuf_unfix_and_init (thread_p, leaf_page);
	  return ret;
	}
      else
	{
	  oid_cnt = btree_leaf_get_num_oids (&leaf_copy_rec, oid_list_offset,
					     BTREE_LEAF_NODE, oid_size);
	  if (oid_cnt == 1)
	    {
	      if (VPID_ISNULL (&leaf_rec.ovfl))
		{
		  if (key_deleted != NULL)
		    {
		      *key_deleted = true;
		    }
		  ret = btree_delete_key_from_leaf (thread_p, btid, leaf_page,
						    leaf_slot_id, key, oid,
						    class_oid, &leaf_copy_rec,
						    &leaf_rec, mvcc_args);
		}
	      else
		{
		  if (mvcc_Enabled == true)
		    {
		      assert (btree_leaf_key_oid_is_mvcc_flaged
			      (leaf_copy_rec.data,
			       BTREE_LEAF_OID_HAS_MVCC_INSID)
			      &&
			      btree_leaf_key_oid_is_mvcc_flaged
			      (leaf_copy_rec.data,
			       BTREE_LEAF_OID_HAS_MVCC_DELID));
		    }

		  if (mvcc_key_deleted)
		    {
		      ret = btree_mvcc_get_num_visible_oids_from_all_ovf
			(thread_p, btid, &leaf_rec.ovfl, &num_visible_oids,
			 NULL, &mvcc_snapshot_dirty);
		    }

		  if (ret == NO_ERROR)
		    {
		      ret =
			btree_swap_first_oid_with_ovfl_rec (thread_p, btid,
							    leaf_page,
							    leaf_slot_id, key,
							    oid, class_oid,
							    &leaf_copy_rec,
							    &leaf_rec.ovfl,
							    mvcc_args);
		      if (ret == NO_ERROR && mvcc_key_deleted
			  && !num_visible_oids)
			{
			  *key_deleted = true;
			}
		    }
#if !defined (NDEBUG)
		  btree_check_valid_record (thread_p, btid, &leaf_copy_rec,
					    BTREE_LEAF_NODE, key);
#endif
		}
	    }
	  else
	    {
	      ret = btree_delete_oid_from_leaf (thread_p, btid, leaf_page,
						leaf_slot_id, key, oid,
						class_oid, &leaf_copy_rec,
						oid_offset, &oid_list_offset,
						mvcc_args);
#if !defined (NDEBUG)
	      btree_check_valid_record (thread_p, btid, &leaf_copy_rec,
					BTREE_LEAF_NODE, key);
#endif
	      if (mvcc_key_deleted)
		{
		  num_visible_oids =
		    btree_leaf_mvcc_get_num_visible_oids (thread_p, btid,
							  &leaf_copy_rec,
							  oid_list_offset,
							  BTREE_LEAF_NODE,
							  NULL,
							  &mvcc_snapshot_dirty);

		  if (ret == NO_ERROR && !num_visible_oids)
		    {
		      if (VPID_ISNULL (&leaf_rec.ovfl))
			{
			  *key_deleted = true;
			}
		      else
			{
			  ret = btree_mvcc_get_num_visible_oids_from_all_ovf
			    (thread_p, btid, &leaf_rec.ovfl,
			     &num_visible_oids, NULL, &mvcc_snapshot_dirty);
			  if (ret == NO_ERROR && !num_visible_oids)
			    {
			      *key_deleted = true;
			    }
			}
		    }
		}

	      if (mvcc_Enabled && MVCC_BTREE_DELETE_VACUUM_OBJECT (mvcc_args))
		{
		  vacuum_er_log (VACUUM_ER_LOG_BTREE | VACUUM_ER_LOG_WORKER,
				 "VACUUM: thread(%d) remove insert MVCCID: ",
				 "btid(%d, %d %d), oid(%d, %d, %d), "
				 "class_oid(%d, %d, %d), "
				 "mvccid=%lld",
				 thread_get_current_entry_index (),
				 (int) btid->sys_btid->root_pageid,
				 (int) btid->sys_btid->vfid.volid,
				 (int) btid->sys_btid->vfid.fileid,
				 (int) oid->volid, (int) oid->pageid,
				 (int) oid->slotid, (int) class_oid->volid,
				 (int) class_oid->pageid,
				 (int) class_oid->slotid,
				 mvcc_args->delete_mvccid);
		}
	    }

	  pgbuf_unfix_and_init (thread_p, leaf_page);
	  return ret;
	}
    }
  else
    {
      if (mvcc_key_deleted)
	{
	  num_visible_oids =
	    btree_leaf_mvcc_get_num_visible_oids (thread_p, btid,
						  &leaf_copy_rec,
						  oid_list_offset,
						  BTREE_LEAF_NODE, NULL,
						  &mvcc_snapshot_dirty);
	}
    }

  /* overflow page */

  ovfl_vpid = leaf_rec.ovfl;
  prev_page = leaf_page;
  ovfl_page = NULL;

  while (!VPID_ISNULL (&ovfl_vpid))
    {
      ovfl_page = pgbuf_fix (thread_p, &ovfl_vpid, OLD_PAGE,
			     PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
      if (ovfl_page == NULL)
	{
	  pgbuf_unfix_and_init (thread_p, prev_page);
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

      if (spage_get_record (ovfl_page, 1, &ovfl_copy_rec, COPY) != S_SUCCESS)
	{
	  pgbuf_unfix_and_init (thread_p, prev_page);
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  goto exit_on_error;
	}

      assert (ovfl_copy_rec.length % 4 == 0);

      if (btree_get_next_overflow_vpid (ovfl_page, &next_ovfl_vpid) !=
	  NO_ERROR)
	{
	  pgbuf_unfix_and_init (thread_p, prev_page);
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  goto exit_on_error;
	}

      oid_offset =
	btree_find_oid_from_ovfl (&ovfl_copy_rec, oid, oid_size, mvcc_args);
      if (oid_offset == NOT_FOUND)
	{
	  if (!num_visible_oids && mvcc_key_deleted)
	    {
	      num_visible_oids =
		btree_leaf_mvcc_get_num_visible_oids (thread_p, btid,
						      &ovfl_copy_rec, 0,
						      BTREE_OVERFLOW_NODE,
						      NULL,
						      &mvcc_snapshot_dirty);
	    }

	  pgbuf_unfix_and_init (thread_p, prev_page);
	  prev_page = ovfl_page;
	  ovfl_vpid = next_ovfl_vpid;
	  continue;
	}

      if (MVCC_BTREE_DELETE_REMOVE_DELID (mvcc_args))
	{
	  /* remove only del_id from leaf page */
	  assert (mvcc_Enabled == true);

	  pgbuf_unfix_and_init (thread_p, prev_page);
	  ret =
	    btree_delete_mvcc_delid_from_page (thread_p, btid, ovfl_page,
					       BTREE_OVERFLOW_NODE, 1, key,
					       oid, class_oid, &ovfl_copy_rec,
					       oid_offset, mvcc_args);
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				    BTREE_OVERFLOW_NODE, key);
#endif
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  return ret;
	}
      else if (MVCC_BTREE_DELETE_VACUUM_INSID (mvcc_args))
	{
	  /* Remove insert ID */
	  assert (mvcc_Enabled);
	  pgbuf_unfix_and_init (thread_p, prev_page);
	  ret =
	    btree_delete_mvcc_insid_from_page (thread_p, btid, ovfl_page,
					       BTREE_OVERFLOW_NODE, 1, key,
					       oid, class_oid, &ovfl_copy_rec,
					       oid_offset, mvcc_args);
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				    BTREE_OVERFLOW_NODE, key);
#endif
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  return ret;
	}

      assert (mvcc_Enabled || oid_size == OR_OID_SIZE);
      oid_cnt = btree_leaf_get_num_oids (&ovfl_copy_rec, 0,
					 BTREE_OVERFLOW_NODE, oid_size);
      assert (oid_cnt > 0);

      if (oid_cnt == 1)
	{
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  ret = file_dealloc_page (thread_p, &btid->sys_btid->vfid,
				   &ovfl_vpid);
	  if (ret != NO_ERROR)
	    {
	      pgbuf_unfix_and_init (thread_p, prev_page);
	      goto exit_on_error;
	    }

	  /* notification */
	  BTREE_SET_DELETED_OVERFLOW_PAGE_NOTIFICATION (thread_p, key,
							oid, class_oid,
							btid->sys_btid);

	  if (prev_page == leaf_page)
	    {
	      ret = btree_modify_leaf_ovfl_vpid (thread_p, btid, prev_page,
						 leaf_slot_id, key, oid,
						 class_oid, &leaf_copy_rec,
						 &next_ovfl_vpid, mvcc_args);
	    }
	  else
	    {
	      ret = btree_modify_overflow_link (thread_p, btid, prev_page,
						key, oid, class_oid,
						&ovfl_copy_rec,
						&next_ovfl_vpid, mvcc_args);
	    }

	  if (mvcc_key_deleted && !num_visible_oids)
	    {
	      ret =
		btree_mvcc_get_num_visible_oids_from_all_ovf
		(thread_p, btid, &next_ovfl_vpid, &num_visible_oids, NULL,
		 &mvcc_snapshot_dirty);
	      if (ret == NO_ERROR && !num_visible_oids)
		{
		  *key_deleted = true;
		}
	    }

	  pgbuf_unfix_and_init (thread_p, prev_page);
	}
      else
	{
	  pgbuf_unfix_and_init (thread_p, prev_page);

	  ret = btree_delete_oid_from_ovfl (thread_p, btid, ovfl_page,
					    key, oid, class_oid,
					    &ovfl_copy_rec, oid_offset,
					    mvcc_args);
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				    BTREE_OVERFLOW_NODE, key);
#endif
	  if (mvcc_key_deleted && !num_visible_oids)
	    {
	      ret = btree_mvcc_get_num_visible_oids_from_all_ovf
		(thread_p, btid, &ovfl_vpid, &num_visible_oids, NULL,
		 &mvcc_snapshot_dirty);
	      if (ret == NO_ERROR && !num_visible_oids)
		{
		  *key_deleted = true;
		}
	    }

	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	}

      if (mvcc_Enabled && MVCC_BTREE_DELETE_VACUUM_OBJECT (mvcc_args))
	{
	  vacuum_er_log (VACUUM_ER_LOG_BTREE | VACUUM_ER_LOG_WORKER,
			 "VACUUM: thread(%d) remove insert MVCCID: ",
			 "btid(%d, %d %d), oid(%d, %d, %d), class_oid(%d, %d, %d), "
			 "mvccid=%lld", thread_get_current_entry_index (),
			 (int) btid->sys_btid->root_pageid,
			 (int) btid->sys_btid->vfid.volid,
			 (int) btid->sys_btid->vfid.fileid, (int) oid->volid,
			 (int) oid->pageid, (int) oid->slotid,
			 (int) class_oid->volid, (int) class_oid->pageid,
			 (int) class_oid->slotid, mvcc_args->delete_mvccid);
	}

      return ret;
    }

  /* OID does not exist */
  log_append_redo_data2 (thread_p, RVBT_NOOP, &btid->sys_btid->vfid,
			 prev_page, -1, 0, NULL);
  pgbuf_set_dirty (thread_p, prev_page, FREE);
  if (!thread_is_vacuum_worker (thread_p))
    {
      btree_set_unknown_key_error (thread_p, btid->sys_btid, key,
				   "btree_delete_from_leaf: "
				   "caused by del_oid_offset == not found.");
    }

  return ER_BTREE_UNKNOWN_KEY;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  return ret;
}

/*
 * btree_delete_meta_record - delete record for merge
 *
 *   return: (error code)
 *   thread_p(in):
 *   btid(in): B+tree index identifier
 *   page_ptr(in):
 *   slot_id(in):
 *   node_type:
 *
 */
static int
btree_delete_meta_record (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  PAGE_PTR page_ptr, int slot_id)
{
  int ret = NO_ERROR;
  RECDES rec;
  int dummy_offset;
  bool dummy_clear_key;
  NON_LEAF_REC nleaf_pnt = { {NULL_PAGEID, NULL_VOLID}, 0 };
  LEAF_REC leaf_pnt = { {NULL_PAGEID, NULL_VOLID}, 0 };
  char *recset_data;
  int recset_data_length;
  PGLENGTH log_addr_offset;
  char recset_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  /* init */
  recset_data = PTR_ALIGN (recset_data_buf, BTREE_MAX_ALIGN);

  assert (slot_id > 0);
  if (spage_get_record (page_ptr, slot_id, &rec, PEEK) != S_SUCCESS)
    {
      goto exit_on_error;
    }

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      goto exit_on_error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  assert (node_type == BTREE_NON_LEAF_NODE);

  if (node_type == BTREE_NON_LEAF_NODE)
    {
      btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf_pnt);

      /* prepare undo log record */
      btree_rv_write_log_record (recset_data, &recset_data_length, &rec,
				 BTREE_NON_LEAF_NODE);

      if (nleaf_pnt.key_len < 0)
	{			/* overflow key */
	  /* get the overflow manager to delete the key */
	  ret = btree_delete_overflow_key (thread_p, btid, page_ptr, slot_id,
					   BTREE_NON_LEAF_NODE);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }
  else
    {
      btree_read_record (thread_p, btid, page_ptr, &rec, NULL, &leaf_pnt,
			 BTREE_LEAF_NODE, &dummy_clear_key, &dummy_offset,
			 PEEK_KEY_VALUE, NULL);

      /* prepare undo log record */
      btree_rv_write_log_record (recset_data, &recset_data_length, &rec,
				 BTREE_LEAF_NODE);

      if (leaf_pnt.key_len < 0)
	{			/* overflow key */
	  /* get the overflow manager to delete the key */
	  ret = btree_delete_overflow_key (thread_p, btid, page_ptr, slot_id,
					   BTREE_LEAF_NODE);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }

  assert (slot_id > 0);
  if (spage_delete (thread_p, page_ptr, slot_id) != slot_id)
    {
      goto exit_on_error;
    }

  /* log the deleted slot_id for undo/redo purposes */
  log_addr_offset = slot_id;
  log_append_undoredo_data2 (thread_p, RVBT_NDRECORD_DEL,
			     &btid->sys_btid->vfid, page_ptr, log_addr_offset,
			     recset_data_length,
			     sizeof (log_addr_offset), recset_data,
			     &log_addr_offset);

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_write_default_split_info() -
 *   return:
 *   info(in/out):
 */
static void
btree_write_default_split_info (BTREE_NODE_SPLIT_INFO * info)
{
  assert (info != NULL);

  info->pivot = BTREE_SPLIT_DEFAULT_PIVOT;
  info->index = 1;
}

/*
 * btree_merge_root () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   P(in): Page pointer for the root to be merged
 *   Q(in): Page pointer for the root child page to be merged
 *   R(in): Page pointer for the root child page to be merged
 *   P_vpid(in): Page identifier for page P
 *   Q_vpid(in): Page identifier for page Q
 *   R_vpid(in): Page identifier for page R
 *
 * Note: When the root page has only two children (non_leaf)
 * that can be merged together, then they are merged through
 * this specific root merge operation. The main distinction of
 * this routine from the regular merge operation is that in this
 * the content of the two child pages are moved to the root, in
 * order not to change the originial root page. The root can also
 * be a specific non-leaf page, that is, it may have only one key
 * and one child page pointer. In this case, R_id, the page
 * identifier for the page R is NULL_PAGEID. In both cases, the
 * height of the tree is reduced by one, after the merge
 * operation. The two (one) child pages are not deallocated by
 * this routine. Deallocation of these pages are left to the
 * calling routine.
 *
 * Note:  Page Q and Page R contents are not changed by this routine,
 * since these pages will be deallocated by the calling routine.
 */
static int
btree_merge_root (THREAD_ENTRY * thread_p, BTID_INT * btid, PAGE_PTR P,
		  PAGE_PTR Q, PAGE_PTR R)
{
  int left_cnt, right_cnt;
  RECDES peek_rec;
  NON_LEAF_REC nleaf_pnt = { {NULL_PAGEID, NULL_VOLID}, 0 };
  int i, j;
  char *recset_data;		/* for recovery purposes */
  int recset_length;		/* for recovery purposes */
  RECSET_HEADER recset_header;	/* for recovery purposes */
  int ret = NO_ERROR;
  char recset_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  LOG_DATA_ADDR addr;
  BTREE_ROOT_HEADER *root_header = NULL;
  int Q_end, R_start;
#if !defined(NDEBUG)
  int p_level, q_level, r_level;
#endif

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_SIMPLE)
    {
      VPID *P_vpid = pgbuf_get_vpid_ptr (P);
      VPID *Q_vpid = pgbuf_get_vpid_ptr (Q);
      VPID *R_vpid = pgbuf_get_vpid_ptr (R);

      printf ("btree_merge_root: P{%d, %d}, Q{%d, %d}, R{%d, %d}\n",
	      P_vpid->volid, P_vpid->pageid,
	      Q_vpid->volid, Q_vpid->pageid, R_vpid->volid, R_vpid->pageid);
    }

  p_level = btree_get_node_level (P);
  assert (p_level > 2);

  q_level = btree_get_node_level (Q);
  assert (q_level > 1);

  r_level = btree_get_node_level (R);
  assert (r_level > 1);

  assert (q_level == r_level);
  assert (p_level == q_level + 1);
  assert (p_level == r_level + 1);

  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, Q);
  btree_verify_node (thread_p, btid, R);
#endif

  /* initializations */
  recset_data = NULL;

  /* log the P record contents for undo purposes,
   * if a crash happens the records of root page P will be inserted
   * back. There is no need for undo logs for pages Q and R,
   * since they are not changed by this routine, because they will be
   * deallocated after a succesful merge operation. There is also no need
   * for redo logs for pages Q and R, since these pages will be deallocated
   * by the caller routine.
   */

  /* for recovery purposes */
  recset_data = PTR_ALIGN (recset_data_buf, BTREE_MAX_ALIGN);
  assert (recset_data != NULL);

  /* remove fence key for merge */
  left_cnt = btree_node_number_of_keys (Q);
  right_cnt = btree_node_number_of_keys (R);

  Q_end = left_cnt;
  if (left_cnt > 0)
    {
      /* read the last record to check upper fence_key */
      if (spage_get_record (Q, left_cnt, &peek_rec, PEEK) != S_SUCCESS)
	{
	  goto exit_on_error;
	}

      /* delete left page upper fence_key before merge */
      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE))
	{
	  assert_release (left_cnt >= 1);
	  assert (!btree_leaf_is_flaged
		  (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));
	  Q_end--;
	}
    }

  left_cnt = Q_end;

  R_start = 1;
  if (right_cnt > 0)
    {
      /* read the first record to check lower fence_key */
      if (spage_get_record (R, 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  goto exit_on_error;
	}

      /* delete right page lower fence_key before merge */
      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE))
	{
	  assert_release (right_cnt >= 1);
	  assert (!btree_leaf_is_flaged
		  (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));
	  R_start++;
	}
    }

  right_cnt = right_cnt - (R_start + 1);

  /* delete all records in P (should be just 2)
   */

  /* delete second record */
  ret = btree_delete_meta_record (thread_p, btid, P, 2);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* delete first record */
  ret = btree_delete_meta_record (thread_p, btid, P, 1);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* Log the page Q records for undo/redo purposes on page P.
   */
  recset_header.rec_cnt = left_cnt;
  recset_header.first_slotid = 1;
  ret = btree_rv_util_save_page_records (Q, 1, Q_end, 1, recset_data,
					 &recset_length);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* move content of the left page to the root page */
  for (i = 1; i <= Q_end; i++)
    {
      if (spage_get_record (Q, i, &peek_rec, PEEK) != S_SUCCESS
	  || spage_insert_at (thread_p, P, i, &peek_rec) != SP_SUCCESS)
	{
	  if (i > 1)
	    {
	      recset_header.rec_cnt = i - 1;
	      recset_header.first_slotid = 1;
	      log_append_undo_data2 (thread_p, RVBT_INS_PGRECORDS,
				     &btid->sys_btid->vfid, P, -1,
				     sizeof (RECSET_HEADER), &recset_header);
	    }
	  goto exit_on_error;
	}
    }				/* for */

  log_append_undoredo_data2 (thread_p, RVBT_INS_PGRECORDS,
			     &btid->sys_btid->vfid, P, -1,
			     sizeof (RECSET_HEADER), recset_length,
			     &recset_header, recset_data);

  /* increment lsa of the page to be deallocated */
  addr.vfid = NULL;
  addr.pgptr = Q;
  addr.offset = 0;
  log_skip_logging_set_lsa (thread_p, &addr);
  pgbuf_set_dirty (thread_p, Q, DONT_FREE);

  /* Log the page R records for undo purposes on page P.
   */
  right_cnt = btree_node_number_of_keys (R);

  recset_header.rec_cnt = right_cnt;
  recset_header.first_slotid = left_cnt + 1;

  ret = btree_rv_util_save_page_records (R, R_start, right_cnt, left_cnt + 1,
					 recset_data, &recset_length);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* move content of the right page to the root page */
  assert (R_start > 0);
  for (i = R_start, j = 1; j <= right_cnt; i++, j++)
    {
      assert (left_cnt + j > 0);
      if (spage_get_record (R, i, &peek_rec, PEEK) != S_SUCCESS
	  || spage_insert_at (thread_p, P, left_cnt + j, &peek_rec)
	  != SP_SUCCESS)
	{
	  if (j > 1)
	    {
	      recset_header.rec_cnt = j - 1;
	      recset_header.first_slotid = left_cnt + 1;
	      log_append_undo_data2 (thread_p, RVBT_INS_PGRECORDS,
				     &btid->sys_btid->vfid, P, -1,
				     sizeof (RECSET_HEADER), &recset_header);
	    }
	  goto exit_on_error;
	}
    }				/* for */

  log_append_undoredo_data2 (thread_p, RVBT_INS_PGRECORDS,
			     &btid->sys_btid->vfid, P, -1,
			     sizeof (RECSET_HEADER), recset_length,
			     &recset_header, recset_data);

  /* increment lsa of the page to be deallocated */
  addr.vfid = NULL;
  addr.pgptr = R;
  addr.offset = 0;
  log_skip_logging_set_lsa (thread_p, &addr);
  pgbuf_set_dirty (thread_p, R, DONT_FREE);

  /* update root page */
  root_header = btree_get_root_header (P);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  btree_node_header_undo_log (thread_p, &btid->sys_btid->vfid, P);

  VPID_SET_NULL (&root_header->node.prev_vpid);
  VPID_SET_NULL (&root_header->node.next_vpid);
  btree_write_default_split_info (&(root_header->node.split_info));
  root_header->node.node_level--;
  assert_release (root_header->node.node_level > 1);

  btree_node_header_redo_log (thread_p, &btid->sys_btid->vfid, P);

#if !defined(NDEBUG)
  {
    BTREE_NODE_HEADER *qheader = NULL, *rheader = NULL;

    qheader = btree_get_node_header (Q);
    assert (qheader != NULL);

    rheader = btree_get_node_header (R);
    assert (rheader != NULL);

    assert (root_header->node.max_key_len ==
	    MAX (qheader->max_key_len, rheader->max_key_len));
  }
#endif

  pgbuf_set_dirty (thread_p, P, DONT_FREE);

  mnt_bt_merges (thread_p);

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, Q);
  btree_verify_node (thread_p, btid, R);
#endif

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_merge_node () -
 *   return: NO_ERROR
 *   btid(in): The B+tree index identifier
 *   P(in): Page pointer for the parent page of page Q
 *   Q(in): Page pointer for the child page of P that will be merged
 *   R(in): Page pointer for the left or right sibling of page Q
 *   next_page(in):
 *   P_vpid(in): Page identifier for page P
 *   Q_vpid(in): Page identifier for page Q
 *   R_vpid(in): Page identifier for page R
 *   p_slot_id(in): The slot of parent page P which points page to
		be merged (right page)
 *   child_vpid(in): Child page identifier to be followed, Q or R.
 *
 * Note: Page Q is merged with page R which may be its left or right
 * sibling. Depending on the efficiency of the merge operation
 * the merge operation may take place on Page Q or on page R to
 * reduce the size of the data that will moved. After the merge
 * operation either page Q or page R becomes ready for
 * deallocation. Deallocation is left to the calling routine.
 *
 * Note:  The page which will be deallocated by the caller after a
 * succesful merge operation is not changed by this routine.
 */
static int
btree_merge_node (THREAD_ENTRY * thread_p, BTID_INT * btid, PAGE_PTR P,
		  PAGE_PTR left_pg, PAGE_PTR right_pg, INT16 p_slot_id,
		  VPID * child_vpid, BTREE_MERGE_STATUS status)
{
  int left_cnt, right_cnt;
  int i, ret = NO_ERROR;
  int key_len;
  VPID *left_vpid = pgbuf_get_vpid_ptr (left_pg);

  /* record decoding */
  RECDES peek_rec;
  NON_LEAF_REC nleaf_pnt;
  LEAF_REC leaf_pnt;
  DB_VALUE key, lf_key;
  bool clear_key = false, lf_clear_key = false;
  int offset;

  OID instance_oid, class_oid;

  /* recovery */
  LOG_DATA_ADDR addr;
  char *recset_data;		/* for recovery purposes */
  int recset_length;		/* for recovery purposes */
  char recset_data_buf[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  BTREE_NODE_HEADER *qheader = NULL, *rheader = NULL;

  /* merge & recompress buff */
  DB_VALUE uncomp_key;
  int L_prefix, R_prefix;
  int L_used, R_used, total_size;
  RECDES rec[MAX_LEAF_REC_NUM];
  int rec_idx;
  char merge_buf[(IO_MAX_PAGE_SIZE * 4) + MAX_ALIGNMENT];
  char *merge_buf_ptr = merge_buf;
  int merge_buf_size = sizeof (merge_buf);
  int merge_idx = 0;
  bool fence_record;
  bool left_upper_fence = false;

  PGSLOTID sp_id;
  int sp_ret;

  MVCC_REC_HEADER mvcc_header, *mvcc_header_p = NULL;

  if (mvcc_Enabled)
    {
      mvcc_header_p = &mvcc_header;
    }

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_SIMPLE)
    {
      VPID *P_vpid = pgbuf_get_vpid_ptr (P);
      VPID *right_vpid = pgbuf_get_vpid_ptr (right_pg);

      printf ("btree_merge_node: P{%d, %d}, Q{%d, %d}, R{%d, %d}\n",
	      P_vpid->volid, P_vpid->pageid,
	      left_vpid->volid, left_vpid->pageid, right_vpid->volid,
	      right_vpid->pageid);
    }
#endif

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, left_pg);
  btree_verify_node (thread_p, btid, right_pg);
#endif

  /***********************************************************
   ***  STEP 0: initializations, save undo image of left
   *** 		calculate uncompress size & memory alloc
   ***********************************************************/
  /* initializations */
  VPID_SET_NULL (child_vpid);
  recset_data = PTR_ALIGN (recset_data_buf, MAX_ALIGNMENT);

  left_cnt = btree_node_number_of_keys (left_pg);
  right_cnt = btree_node_number_of_keys (right_pg);

  L_used = btree_node_size_uncompressed (thread_p, btid, left_pg);
  L_prefix = btree_node_common_prefix (thread_p, btid, left_pg);

  R_used = btree_node_size_uncompressed (thread_p, btid, right_pg);
  R_prefix = btree_node_common_prefix (thread_p, btid, right_pg);

  total_size = L_used + R_used + MAX_MERGE_ALIGN_WASTE;
  if (total_size > (int) sizeof (merge_buf))
    {
      merge_buf_size = total_size;
      merge_buf_ptr = (char *) db_private_alloc (thread_p, merge_buf_size);
    }

  /* this means left (or right) empty or only one fence key remain */
  if (status == BTREE_MERGE_L_EMPTY)
    {
      left_cnt = 0;
    }

  if (status == BTREE_MERGE_R_EMPTY)
    {
      right_cnt = 0;
    }

  /***********************************************************
   ***  STEP 1: uncompress left
   ***********************************************************/

  for (i = 1, rec_idx = 0; i <= left_cnt; i++, rec_idx++)
    {
      rec[rec_idx].data =
	PTR_ALIGN (&merge_buf_ptr[merge_idx], BTREE_MAX_ALIGN);
      merge_idx = rec[rec_idx].data - merge_buf_ptr;	/* advance align */
      rec[rec_idx].area_size = merge_buf_size - merge_idx;
      rec[rec_idx].type = REC_HOME;

      /* peek original record */
      spage_get_record (left_pg, i, &peek_rec, PEEK);
      fence_record =
	btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE);

      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY) ||
	  (fence_record == false && L_prefix == 0) ||
	  status == BTREE_MERGE_R_EMPTY)
	{
	  /* use peeked data for overflow key */
	  memcpy (rec[rec_idx].data, peek_rec.data, peek_rec.length);
	  rec[rec_idx].length = peek_rec.length;
	  merge_idx += rec[rec_idx].length;
	  continue;
	}

      if (fence_record == true && i == left_cnt)
	{
	  if (i == 1)
	    {
	      RECDES tmp_rec;
	      spage_get_record (right_pg, 1, &tmp_rec, PEEK);
	      if (btree_leaf_is_flaged (&tmp_rec, BTREE_LEAF_RECORD_FENCE))
		{
		  /* skip upper fence key */
		  left_upper_fence = true;
		  break;
		}
	      else
		{
		  /* lower fence key */
		}
	    }
	  else
	    {
	      /* skip upper fence key */
	      left_upper_fence = true;
	      break;
	    }
	}

      /* read original key */
      btree_read_record_helper (thread_p, btid, &peek_rec, &key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_key,
				&offset, PEEK_KEY_VALUE);
      key_len = btree_get_key_length (&key);
      btree_leaf_get_first_oid (btid, &peek_rec, &instance_oid, &class_oid,
				mvcc_header_p);

      if (fence_record == true)
	{
	  assert (i == 1);	/* lower fence key */

	  lf_key = key;

	  /* use current fence record */
	  memcpy (rec[rec_idx].data, peek_rec.data, peek_rec.length);
	  rec[rec_idx].length = peek_rec.length;
	  merge_idx += rec[rec_idx].length;
	  continue;
	}

      /* uncompress key */
      assert (L_prefix > 0);

      pr_midxkey_add_prefix (&uncomp_key, &lf_key, &key, L_prefix);
      btree_clear_key_value (&clear_key, &key);

      key_len = btree_get_key_length (&uncomp_key);
      clear_key = true;

      /* make record */
      btree_write_record (thread_p, btid, NULL, &uncomp_key, BTREE_LEAF_NODE,
			  BTREE_NORMAL_KEY, key_len, false, &class_oid,
			  &instance_oid, mvcc_header_p, &rec[rec_idx]);
      btree_clear_key_value (&clear_key, &uncomp_key);

      /* save oid lists (& overflow vpid if exists) */
      memcpy (rec[rec_idx].data + rec[rec_idx].length, peek_rec.data + offset,
	      peek_rec.length - offset);
      rec[rec_idx].length += peek_rec.length - offset;

      merge_idx += rec[rec_idx].length;

      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	{
	  btree_leaf_set_flag (&rec[rec_idx],
			       BTREE_LEAF_RECORD_OVERFLOW_OIDS);
	}

#if !defined (NDEBUG)
      btree_check_valid_record (thread_p, btid, &rec[rec_idx],
				BTREE_LEAF_NODE, NULL);
#endif

      assert (merge_idx < merge_buf_size);
      assert (rec_idx < (int) (sizeof (rec) / sizeof (rec[0])));
    }

  btree_clear_key_value (&lf_clear_key, &lf_key);

  /***********************************************************
   ***  STEP 2: uncompress right
   ***********************************************************/

  for (i = 1; i <= right_cnt; i++, rec_idx++)
    {
      rec[rec_idx].data = PTR_ALIGN (&merge_buf[merge_idx], BTREE_MAX_ALIGN);
      merge_idx = rec[rec_idx].data - merge_buf;	/* advance align */
      rec[rec_idx].area_size = sizeof (merge_buf) - merge_idx;
      rec[rec_idx].type = REC_HOME;

      /* peek original record */
      spage_get_record (right_pg, i, &peek_rec, PEEK);
      fence_record =
	btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE);

      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY) ||
	  (fence_record == false && R_prefix == 0) ||
	  status == BTREE_MERGE_L_EMPTY)
	{
	  /* use peeked data for overflow key */
	  memcpy (rec[rec_idx].data, peek_rec.data, peek_rec.length);
	  rec[rec_idx].length = peek_rec.length;
	  merge_idx += rec[rec_idx].length;
	  continue;
	}

      if (fence_record && i == right_cnt)
	{
	  if (i == 1 && left_upper_fence == true)
	    {
	      /* lower fence key */
	    }
	  else
	    {
	      /* upper fence key, use current fence record */
	      memcpy (rec[rec_idx].data, peek_rec.data, peek_rec.length);
	      rec[rec_idx].length = peek_rec.length;
	      merge_idx += rec[rec_idx].length;
	      continue;
	    }
	}

      /* read orginal key */
      btree_read_record_helper (thread_p, btid, &peek_rec, &key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_key,
				&offset, PEEK_KEY_VALUE);
      key_len = btree_get_key_length (&key);
      btree_leaf_get_first_oid (btid, &peek_rec, &instance_oid, &class_oid,
				mvcc_header_p);

      if (fence_record)
	{
	  assert (i == 1);	/* lower fence key */

	  lf_key = key;

	  /* skip lower fence key */
	  rec_idx--;
	  continue;
	}

      /* uncompress key */
      assert (R_prefix > 0);

      pr_midxkey_add_prefix (&uncomp_key, &lf_key, &key, R_prefix);
      btree_clear_key_value (&clear_key, &key);

      key_len = btree_get_key_length (&key);
      clear_key = true;

      /* make record */
      btree_write_record (thread_p, btid, NULL, &uncomp_key, BTREE_LEAF_NODE,
			  BTREE_NORMAL_KEY, key_len, false, &class_oid,
			  &instance_oid, &mvcc_header, &rec[rec_idx]);
      btree_clear_key_value (&clear_key, &uncomp_key);

      /* save oid lists (& overflow vpid if exists) */
      memcpy (rec[rec_idx].data + rec[rec_idx].length, peek_rec.data + offset,
	      peek_rec.length - offset);
      rec[rec_idx].length += peek_rec.length - offset;

      merge_idx += rec[rec_idx].length;

      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
	{
	  btree_leaf_set_flag (&rec[rec_idx],
			       BTREE_LEAF_RECORD_OVERFLOW_OIDS);
	}

#if !defined (NDEBUG)
      btree_check_valid_record (thread_p, btid, &rec[rec_idx],
				BTREE_LEAF_NODE, NULL);
#endif

      assert (merge_idx < merge_buf_size);
      assert (rec_idx < (int) (sizeof (rec) / sizeof (rec[0])));
    }

  btree_clear_key_value (&lf_clear_key, &lf_key);

  /***********************************************************
   ***  STEP 3: recompress all
   ***
   ***    recompress records with changed prefix
   ***********************************************************/

  if (TP_DOMAIN_TYPE (btid->key_type) == DB_TYPE_MIDXKEY
      && btree_leaf_is_flaged (&rec[0], BTREE_LEAF_RECORD_FENCE)
      && btree_leaf_is_flaged (&rec[rec_idx - 1], BTREE_LEAF_RECORD_FENCE))
    {
      /* recompress */
      btree_compress_records (thread_p, btid, rec, rec_idx);
    }

  /***********************************************************
   ***  STEP 4: replace left slots
   ***
   ***    remove all records
   ***    insert recompressed records
   ***********************************************************/

  /* add undo logging for left_pg */
  log_append_undo_data2 (thread_p, RVBT_COPYPAGE,
			 &btid->sys_btid->vfid, left_pg, -1,
			 DB_PAGESIZE, left_pg);

  for (i = left_cnt; i >= 1; i--)
    {
      assert (i > 0);
      if (spage_delete (thread_p, left_pg, i) != i)
	{
	  assert (false);
	  goto exit_on_error;
	}
    }

  assert (spage_number_of_records (left_pg) == 1);

  /* Log the right page records for undo purposes on the left page. */
  for (i = 0; i < rec_idx; i++)
    {
      sp_ret = spage_insert (thread_p, left_pg, &rec[i], &sp_id);
      if (sp_ret != SP_SUCCESS)
	{
#if !defined(NDEBUG)
	  btree_dump_page (thread_p, stdout, NULL, btid,
			   NULL, left_pg, left_vpid, 2, 2);
#endif
	  assert (false);
	  goto exit_on_error;
	}

      assert (sp_id > 0);
    }

  /***********************************************************
   ***  STEP 5: update child link of page P
   ***********************************************************/

  /* get and log the old node record to be deleted for undo purposes */
  assert (p_slot_id > 0);
  if (spage_get_record (P, p_slot_id, &peek_rec, PEEK) != S_SUCCESS)
    {
      assert (false);
      goto exit_on_error;
    }

  btree_read_fixed_portion_of_non_leaf_record (&peek_rec, &nleaf_pnt);

  if (nleaf_pnt.key_len < 0)
    {				/* overflow key */
      /* get the overflow manager to delete the key */
      ret = btree_delete_overflow_key (thread_p, btid, P, p_slot_id,
				       BTREE_NON_LEAF_NODE);
      if (ret != NO_ERROR)
	{
	  assert (false);
	  goto exit_on_error;
	}
    }

  btree_rv_write_log_record (recset_data, &recset_length,
			     &peek_rec, BTREE_NON_LEAF_NODE);
  log_append_undoredo_data2 (thread_p, RVBT_NDRECORD_DEL,
			     &btid->sys_btid->vfid, P, p_slot_id,
			     recset_length,
			     sizeof (p_slot_id), recset_data, &p_slot_id);
  RANDOM_EXIT (thread_p);

  assert (p_slot_id > 0);
  if (spage_delete (thread_p, P, p_slot_id) != p_slot_id)
    {
      assert (false);
      goto exit_on_error;
    }

  pgbuf_set_dirty (thread_p, P, DONT_FREE);

  *child_vpid = *left_vpid;

  /***********************************************************
   ***  STEP 6: update left page header info
   ***          write redo log for left
   ***********************************************************/

  /* get right page header information */
  rheader = btree_get_node_header (right_pg);
  if (rheader == NULL)
    {
      goto exit_on_error;
    }

  /* update left page header information */
  qheader = btree_get_node_header (left_pg);
  if (qheader == NULL)
    {
      goto exit_on_error;
    }

  qheader->next_vpid = rheader->next_vpid;
  qheader->max_key_len = MAX (qheader->max_key_len, rheader->max_key_len);
  btree_write_default_split_info (&(qheader->split_info));

  /* add redo logging for left_pg */
  log_append_redo_data2 (thread_p, RVBT_COPYPAGE,
			 &btid->sys_btid->vfid, left_pg, -1,
			 DB_PAGESIZE, left_pg);

  pgbuf_set_dirty (thread_p, left_pg, DONT_FREE);

  /***********************************************************
   ***  STEP 7: increment lsa of right page to be deallocated
   ***          verify P, left, right
   ***********************************************************/

  addr.vfid = NULL;
  addr.pgptr = right_pg;
  addr.offset = 0;
  log_skip_logging_set_lsa (thread_p, &addr);
  pgbuf_set_dirty (thread_p, right_pg, DONT_FREE);

  mnt_bt_merges (thread_p);

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, left_pg);
  btree_verify_node (thread_p, btid, right_pg);
#endif

  if (merge_buf_ptr != merge_buf)
    {
      db_private_free_and_init (thread_p, merge_buf_ptr);
    }

  return ret;

exit_on_error:

  if (merge_buf_ptr != merge_buf)
    {
      db_private_free_and_init (thread_p, merge_buf_ptr);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_delete_lock_curr_key_next_pseudo_oid () - lock the next pseudo oid
 *						   of current key in
 *						   btree_delete
 *
 *   return: (error code)
 *   thread_p(in):
 *   btid(in): B+tree index identifier
 *   rec(in): leaf record
 *   offset(in): offset of oid(s) following the key
 *   P_vpid(in): vpid of page where rec reside
 *   P_page(in/out): page where rec reside
 *		     *P_page can be modified when resume after uncond locking
 *   first_ovfl_vpid(in): first overflow vpid of leaf record
 *   class_oid(in): class oid
 *   search_without_locking(out): true when the lock is acquired and
 *				 the page where the key reside must
 *				 be searched again
 *				  if true, *P_page is set to NULL
 *
 * Note:
 *  This function must be called by btree_delete if the following conditions
 * are satisfied:
 *  1. curr key lock commit duration is needed (non unique btree, at least
 *     2 OIDs)
 *  2. need to delete the first key buffer OID
 *  3. the pseudo-OID attached to the first key buffer OID has been
 *     NX_LOCK-ed by the current transaction
 */
static int
btree_delete_lock_curr_key_next_pseudo_oid (THREAD_ENTRY * thread_p,
					    BTID_INT * btid_int,
					    RECDES * rec, int offset,
					    VPID * P_vpid,
					    PAGE_PTR * P_page,
					    VPID * first_ovfl_vpid,
					    OID * class_oid,
					    bool * search_without_locking)
{
  PAGE_PTR O_page = NULL;
  int ret_val = NO_ERROR, lock_ret;
  RECDES peek_recdes;
  OID temp_oid, last_oid;
  VPID O_vpid;
  PAGE_PTR P = NULL;
  int oids_cnt;
  int oid_size = OR_OID_SIZE;

  assert_release (btid_int != NULL);
  assert_release (rec != NULL);
  assert_release (first_ovfl_vpid != NULL);
  assert_release (class_oid != NULL);
  assert_release (P_vpid != NULL);
  assert_release (P_page != NULL);
  assert_release (search_without_locking != NULL);
  assert_release (*P_page != NULL);
  assert (mvcc_Enabled == false);

  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  OID_SET_NULL (&last_oid);
  *search_without_locking = false;

  O_vpid = *first_ovfl_vpid;
  oids_cnt = btree_leaf_get_num_oids (rec, offset, BTREE_LEAF_NODE, oid_size);
  if (oids_cnt == 1 && !VPID_ISNULL (&O_vpid))
    {
      /* get the last oid from first overflow page */
      O_page = pgbuf_fix (thread_p, &O_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
      if (O_page == NULL)
	{
	  return ER_FAILED;
	}

      (void) pgbuf_check_page_ptype (thread_p, O_page, PAGE_BTREE);

      if (spage_get_record (O_page, 1, &peek_recdes, PEEK) != S_SUCCESS)
	{
	  pgbuf_unfix_and_init (thread_p, O_page);
	  ret_val = ER_FAILED;
	  goto end;
	}

      assert (peek_recdes.length % 4 == 0);
      btree_leaf_get_last_oid (btid_int, &peek_recdes, BTREE_OVERFLOW_NODE, 0,
			       &last_oid, &temp_oid, NULL, NULL);

      pgbuf_unfix_and_init (thread_p, O_page);
    }
  else
    {
      /* get the last oid from leaf node record */
      btree_leaf_get_last_oid (btid_int, rec, BTREE_LEAF_NODE, 0,
			       &last_oid, &temp_oid, NULL, NULL);
    }

  if (OID_ISNULL (&last_oid))
    {
      ret_val = ER_FAILED;
      goto end;
    }

  btree_make_pseudo_oid (last_oid.pageid, last_oid.slotid,
			 last_oid.volid, btid_int->sys_btid, &last_oid);

  assert (!OID_ISNULL (&last_oid));
  assert (!OID_ISNULL (class_oid));
  lock_ret = lock_object_with_btid (thread_p, &last_oid, class_oid,
				    btid_int->sys_btid, NX_LOCK,
				    LK_COND_LOCK);
  if (lock_ret == LK_GRANTED)
    {
      goto end;
    }
  else if (lock_ret == LK_NOTGRANTED_DUE_TIMEOUT)
    {
      /* acquire uncond lock */
      LOG_LSA cur_leaf_lsa;

      LSA_COPY (&cur_leaf_lsa, pgbuf_get_lsa (*P_page));
      pgbuf_unfix_and_init (thread_p, *P_page);

      /* UNCONDITIONAL lock request */
      assert (!OID_ISNULL (&last_oid));
      assert (!OID_ISNULL (class_oid));
      lock_ret = lock_object_with_btid (thread_p, &last_oid, class_oid,
					btid_int->sys_btid, NX_LOCK,
					LK_UNCOND_LOCK);
      if (lock_ret != LK_GRANTED)
	{
	  ret_val = ER_FAILED;
	  goto end;
	}

      *P_page = pgbuf_fix (thread_p, P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			   PGBUF_UNCONDITIONAL_LATCH);
      if (*P_page == NULL)
	{
	  ret_val = ER_FAILED;
	  goto end;
	}

      if (!btree_leaf_lsa_eq (thread_p,
			      &cur_leaf_lsa, pgbuf_get_lsa (*P_page)))
	{
	  /* current page changed during unconditional lock request
	   * the page where the key reside must be searched again
	   * the locks have been acquired
	   */
	  pgbuf_unfix_and_init (thread_p, *P_page);
	  *search_without_locking = true;
	  goto end;
	}

      (void) pgbuf_check_page_ptype (thread_p, *P_page, PAGE_BTREE);
    }
  else
    {
      ret_val = ER_FAILED;
      goto end;
    }

end:
  return ret_val;
}

/*
 * btree_node_size_uncompressed -
 *
 *   return:
 *   thread_p(in):
 *   btid(in):
 *   page_ptr(in):
 *
 */
static int
btree_node_size_uncompressed (THREAD_ENTRY * thread_p, BTID_INT * btid,
			      PAGE_PTR page_ptr)
{
  int used_size, key_cnt = 0, prefix, prefix_len, offset;
  RECDES rec;
  DB_VALUE key;
  bool clear_key = false;
  DB_MIDXKEY *midx_key = NULL;
  LEAF_REC leaf_pnt;

  used_size = DB_PAGESIZE - spage_get_free_space (thread_p, page_ptr);

  prefix = btree_node_common_prefix (thread_p, btid, page_ptr);
  if (prefix > 0)
    {
#if !defined(NDEBUG)
      BTREE_NODE_HEADER *header = NULL;

      header = btree_get_node_header (page_ptr);

      assert (header != NULL);
      assert (header->node_level == 1);	/* BTREE_LEAF_NODE */
#endif

      if (spage_get_record (page_ptr, 1, &rec, PEEK) != S_SUCCESS)
	{
	  assert (false);
	  return used_size;
	}

      btree_read_record_helper (thread_p, btid, &rec, &key,
				&leaf_pnt, BTREE_LEAF_NODE,
				&clear_key, &offset, PEEK_KEY_VALUE);

      assert (!btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));
      assert (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE));
      assert (DB_VALUE_TYPE (&key) == DB_TYPE_MIDXKEY);

      midx_key = DB_PULL_MIDXKEY (&key);

      btree_clear_key_value (&clear_key, &key);

      prefix_len = pr_midxkey_get_element_offset (midx_key, prefix);

      /* at here, we can not calculate aligned size of uncompressed rec.
       * alignment is already included in FIXED_EMPTY
       */
      key_cnt = btree_node_number_of_keys (page_ptr);
      used_size += (key_cnt - 2) * prefix_len;
    }

  return used_size;
}

/*
 * btree_node_mergeable -
 *
 *   return:
 *   thread_p(in):
 *   btid(in):
 *   L_page(in):
 *   R_page(in):
 *
 */
static BTREE_MERGE_STATUS
btree_node_mergeable (THREAD_ENTRY * thread_p, BTID_INT * btid,
		      PAGE_PTR L_page, PAGE_PTR R_page)
{
  BTREE_NODE_HEADER *l_header = NULL, *r_header = NULL;
  BTREE_NODE_TYPE l_node_type, r_node_type;
  int L_used, R_used, L_cnt, R_cnt;

  /* case 1 : one of page is empty */

  L_cnt = btree_node_number_of_keys (L_page);
  R_cnt = btree_node_number_of_keys (R_page);

  if (L_cnt == 0)
    {
      return BTREE_MERGE_L_EMPTY;
    }

  if (R_cnt == 0)
    {
      return BTREE_MERGE_R_EMPTY;
    }

  /* case 2 : size */

  l_header = btree_get_node_header (L_page);
  if (l_header == NULL)
    {
      return BTREE_MERGE_NO;
    }

  r_header = btree_get_node_header (R_page);
  if (r_header == NULL)
    {
      return BTREE_MERGE_NO;
    }

  l_node_type =
    (l_header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;
  r_node_type =
    (r_header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  assert (l_node_type == r_node_type);

  L_used = DB_PAGESIZE - spage_get_free_space (thread_p, L_page);
  R_used = DB_PAGESIZE - spage_get_free_space (thread_p, R_page);
  if (L_used + R_used + FIXED_EMPTY < DB_PAGESIZE)
    {
      /* check uncompressed size */
      if (l_node_type == BTREE_LEAF_NODE)
	{
	  /* recalculate uncompressed left size */
	  L_used = btree_node_size_uncompressed (thread_p, btid, L_page);

	  if (L_used + R_used + FIXED_EMPTY >= DB_PAGESIZE)
	    {
	      return BTREE_MERGE_NO;
	    }

	  /* recalculate uncompressed right size */
	  R_used = btree_node_size_uncompressed (thread_p, btid, R_page);

	  if (L_used + R_used + FIXED_EMPTY >= DB_PAGESIZE)
	    {
	      /* can recalculate used size after recompress with new fence key
	       * (can return true in some cases)
	       *
	       * but split and merge will be done more frequently
	       * (trade off of space and SMO)
	       */
	      return BTREE_MERGE_NO;
	    }
	}

      return BTREE_MERGE_SIZE;
    }

  return BTREE_MERGE_NO;
}


/*
 * btree_delete () -
 *   return: (the specified key on success, or NULL on failure)
 *   btid(in): B+tree index identifier
 *   key(in): Key from which the specified value will be deleted
 *   cls_oid(in):
 *   oid(in): Object identifier to be deleted
 *   locked_keys(in): keys already locked by the current transaction
 *		      when search
 *   unique(in):
 *   op_type(in):
 *   unique_stat_info(in):
 *   mvcc_args(in): arguments used in the MVCC context.
 */
DB_VALUE *
btree_delete (THREAD_ENTRY * thread_p, BTID * btid, DB_VALUE * key,
	      OID * cls_oid, OID * oid, BTREE_LOCKED_KEYS locked_keys,
	      int *unique, int op_type, BTREE_UNIQUE_STATS * unique_stat_info,
	      MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int offset, p_key_cnt, root_level = 0;
  VPID P_vpid, Q_vpid, R_vpid, child_vpid, Left_vpid, Right_vpid;
  PAGE_PTR P = NULL, Q = NULL, R = NULL, Left = NULL, Right = NULL;
  PAGE_PTR next_page = NULL;
  RECDES peek_recdes1, peek_recdes2;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  NON_LEAF_REC nleaf_pnt;
  LEAF_REC leaf_pnt;
  INT16 p_slot_id;
  int top_op_active = 0;
  DB_VALUE mid_key;
  bool clear_key = false;
  BTREE_NODE_TYPE node_type;
  BTID_INT btid_int;
  OID class_oid;
  int ret_val, oid_size, curr_key_oids_cnt;
  LOG_LSA saved_plsa, saved_nlsa;
  LOG_LSA *temp_lsa;
  bool old_check_interrupt;

  /* for locking */
  bool is_active;
  LOCK class_lock = NULL_LOCK;
  int tran_index;
  bool curr_key_lock_commit_duration = false;
  bool delete_first_key_oid = false;
  bool search_without_locking = false;
  TRAN_ISOLATION tran_isolation;

  /* for next key lock */
  bool next_page_flag = false;
  bool next_lock_flag = false;
  bool curr_lock_flag = false;
  PAGE_PTR N = NULL;
  VPID N_vpid;
  OID N_oid, saved_N_oid;
  OID C_oid, saved_C_oid, curr_key_first_oid;
  OID N_class_oid, C_class_oid;
  OID saved_N_class_oid, saved_C_class_oid;
  int nextkey_lock_request;
  BTREE_SCAN tmp_bts;
  bool is_last_key;

  /* for merge */
  int Q_used, R_used;
  bool merged, key_deleted, *pkey_deleted = NULL;
  BTREE_MERGE_STATUS merge_status;

  assert (key != NULL);
  assert (oid != NULL);
  assert (unique != NULL);

  old_check_interrupt = thread_set_check_interrupt (thread_p, false);

  is_active = logtb_is_current_active (thread_p);
  tran_isolation = logtb_find_current_isolation (thread_p);

#if !defined(NDEBUG)
  if (BTREE_INVALID_INDEX_ID (btid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_INDEX_ID, 3,
	      btid->vfid.fileid, btid->vfid.volid, btid->root_pageid);
      goto error;
    }
#endif

  P_vpid.volid = btid->vfid.volid;	/* read the root page */
  P_vpid.pageid = btid->root_pageid;

  P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		 PGBUF_UNCONDITIONAL_LATCH);
  if (P == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

  root_header = btree_get_root_header (P);
  if (root_header == NULL)
    {
      goto error;
    }

  btid_int.sys_btid = btid;
  if (btree_glean_root_header_info (thread_p,
				    root_header, &btid_int) != NO_ERROR)
    {
      goto error;
    }

  if (key && DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY)
    {
      key->data.midxkey.domain = btid_int.key_type;	/* set complete setdomain */
    }

  *unique = BTREE_IS_UNIQUE (btid_int.unique_pk) ? 1 : 0;
  if (BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      oid_size = 2 * OR_OID_SIZE;
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      /* update root header statistics if it's a Btree for uniques.
       * this only wants to be done if the transaction is active.  that
       * is, if we are aborting a transaction the statistics are "rolled
       * back by their own logging.
       *
       * unique statistics for non null keys will be updated after we
       * find out if we have deleted a key or not.
       */
      if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk)
	  && !thread_is_vacuum_worker (thread_p))
	{
	  assert (!MVCC_BTREE_DELETE_REMOVE_DELID (mvcc_args));
	  if (op_type == SINGLE_ROW_DELETE || op_type == SINGLE_ROW_UPDATE
	      || op_type == SINGLE_ROW_MODIFY)
	    {
	      if (mvcc_Enabled && !heap_is_mvcc_disabled_for_class (cls_oid))
		{
		  if (logtb_mvcc_update_class_unique_stats
		      (thread_p, &btid_int.topclass_oid, btid, 0, -1, -1,
		       true) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else
		{
		  /* update the root header */
		  ret_val =
		    btree_change_root_header_delta (thread_p, &btid->vfid, P,
						    -1, -1, 0);
		  if (ret_val != NO_ERROR)
		    {
		      goto error;
		    }

		  pgbuf_set_dirty (thread_p, P, DONT_FREE);
		}
	    }
	  else
	    {
	      /* Multiple instances will be deleted.
	       * Update local statistics. */
	      if (unique_stat_info == NULL)
		{
		  goto error;
		}

	      unique_stat_info->num_nulls--;
	      unique_stat_info->num_oids--;
	    }
	}

      /* nothing more to do -- this is not an error */
      pgbuf_unfix_and_init (thread_p, P);

      mnt_bt_deletes (thread_p);

      (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

#if !defined(NDEBUG)
      if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) &
	  BTREE_DEBUG_DUMP_FULL)
	{
	  btree_dump (thread_p, stdout, btid, 2);
	}
#endif

      return key;
    }

  /* update root header statistics if it is a Btree for uniques.
   * this will happen only when the transaction is active.
   * that is, if we are aborting a transaction the statistics are "rolled back"
   * by their own logging.
   *
   * NOTE that we are optimistically updating the header statistics in advance.
   * When we encounter the failure to delete a new key, updates are rollbacked
   * by not an adjusting routine but the top operation.
   * Also NOTE that users to see the header statistics may have the transient
   * values.
   */
  if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk)
      && !thread_is_vacuum_worker (thread_p))
    {
      if (op_type == SINGLE_ROW_DELETE || op_type == SINGLE_ROW_UPDATE
	  || op_type == SINGLE_ROW_MODIFY)
	{
	  if (mvcc_Enabled && !heap_is_mvcc_disabled_for_class (cls_oid))
	    {
	      if (logtb_mvcc_update_class_unique_stats
		  (thread_p, &btid_int.topclass_oid, btid, -1, -1, 0,
		   true) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      /* update the root header
	       * guess existing key delete
	       */
	      ret_val =
		btree_change_root_header_delta (thread_p, &btid->vfid, P, 0,
						-1, -1);
	      if (ret_val != NO_ERROR)
		{
		  goto error;
		}

	      pgbuf_set_dirty (thread_p, P, DONT_FREE);
	    }
	}
      else
	{
	  /* Multiple instances will be deleted.
	   * Therefore, update local statistical information.
	   */
	  if (unique_stat_info == NULL)
	    {
	      goto error;
	    }

	  unique_stat_info->num_oids--;
	  unique_stat_info->num_keys--;	/* guess existing key delete */
	}
    }

  /* Decide whether key range locking must be performed.
   * If class_oid is transferred through a new argument,
   * this operation will be performed more efficiently.
   */
  if (cls_oid != NULL && !OID_ISNULL (cls_oid))
    {
      /* cls_oid can be NULL_OID in case of non-unique index.
       * But it does not make problem.
       */
      COPY_OID (&class_oid, cls_oid);
    }
  else
    {
      if (is_active)
	{
	  if (heap_get_class_oid (thread_p, &class_oid, oid,
				  DONT_NEED_SNAPSHOT) == NULL)
	    {
	      goto error;
	      /* nextkey_lock_request = true; goto start_point; */
	    }
	}
      else
	{
	  assert (false);	/* is impossible */
	  OID_SET_NULL (&class_oid);
	}
    }

  if (mvcc_Enabled == true)
    {
      nextkey_lock_request = false;
    }
  else if (is_active)
    {
      /* initialize saved_N_oid */
      OID_SET_NULL (&saved_N_oid);
      OID_SET_NULL (&saved_N_class_oid);
      OID_SET_NULL (&saved_C_oid);
      OID_SET_NULL (&saved_C_class_oid);

      /* Find the lock that is currently acquired on the class */
      tran_index = LOG_FIND_THREAD_TRAN_INDEX (thread_p);
      class_lock = lock_get_object_lock (&class_oid, oid_Root_class_oid,
					 tran_index);

      /* get nextkey_lock_request from the class lock mode */
      switch (class_lock)
	{
	case SCH_M_LOCK:
	case X_LOCK:
	case SIX_LOCK:
	case IX_LOCK:
	  nextkey_lock_request = true;
	  break;
	case S_LOCK:
	case IS_LOCK:
	case NULL_LOCK:
	default:
	  assert (false);
	  goto error;
	}
      assert (nextkey_lock_request == true);

      if (!BTREE_IS_UNIQUE (btid_int.unique_pk))
	{			/* non-unique index */
	  assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	  if (IS_WRITE_EXCLUSIVE_LOCK (class_lock))
	    {
	      nextkey_lock_request = false;
	    }
	  else
	    {
	      nextkey_lock_request = true;
	    }
	}
    }
  else
    {
      /* total rollback, partial rollback, undo phase in recovery */
      nextkey_lock_request = false;
    }

  COPY_OID (&N_class_oid, &class_oid);
  COPY_OID (&C_class_oid, &class_oid);

start_point:

  if (search_without_locking == false)
    {
      curr_key_lock_commit_duration = false;
    }
  else
    {
      /* preserve values of curr_key_lock_commit_duration, curr_lock_flag
       */
    }

  if (next_lock_flag == true || curr_lock_flag == true)
    {
      P_vpid.volid = btid->vfid.volid;	/* read the root page */
      P_vpid.pageid = btid->root_pageid;
      P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      root_header = btree_get_root_header (P);
      if (root_header == NULL)
	{
	  goto error;
	}

      root_level = root_header->node.node_level;
      node_type = (root_level) > 1 ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;
    }

  if (root_level > 2 && btree_node_number_of_keys (P) == 2)
    {				/* root merge may be needed */

      /* read the first record */
      if (spage_get_record (P, 1, &peek_recdes1, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      btree_read_fixed_portion_of_non_leaf_record (&peek_recdes1, &nleaf_pnt);

      Q_vpid = nleaf_pnt.pnt;

      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (Q == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);

      Q_used = DB_PAGESIZE - spage_get_free_space (thread_p, Q);

      header = btree_get_node_header (Q);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;
      assert (root_level == header->node_level + 1);
      assert (node_type == BTREE_NON_LEAF_NODE);

      /* read the second record */
      if (spage_get_record (P, 2, &peek_recdes2, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      btree_read_record (thread_p, &btid_int, P, &peek_recdes2, &mid_key,
			 &nleaf_pnt, BTREE_NON_LEAF_NODE, &clear_key, &offset,
			 PEEK_KEY_VALUE, NULL);

      R_vpid = nleaf_pnt.pnt;
      R = pgbuf_fix (thread_p, &R_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (R == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, R, PAGE_BTREE);

      R_used = DB_PAGESIZE - spage_get_free_space (thread_p, R);

      /* we need to take into consideration the largest key size since the
       * merge will use a key from the root page as the mid_key.  It may be
       * longer than any key on Q or R.  This is a little bit overly
       * pessimistic, which is probably not bad for root merges.
       */
      if ((Q_used + R_used + FIXED_EMPTY + root_header->node.max_key_len) <
	  DB_PAGESIZE)
	{
	  /* root merge possible */

	  /* Start system permanent operation */
	  log_start_system_op (thread_p);
	  top_op_active = 1;

	  if (btree_merge_root (thread_p, &btid_int, P, Q, R) != NO_ERROR)
	    {
	      goto error;
	    }

#if !defined(NDEBUG)
	  (void) spage_check_num_slots (thread_p, P);
#endif

	  if (btree_is_new_file (&btid_int))
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
	    }
	  else
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	    }

	  top_op_active = 0;

	  assert (VFID_EQ (&btid->vfid, &btid_int.sys_btid->vfid));

	  pgbuf_unfix_and_init (thread_p, Q);

	  if (btree_dealloc_page (thread_p, &btid_int, &Q_vpid) != NO_ERROR)
	    {
	      pgbuf_unfix_and_init (thread_p, R);
	      goto error;
	    }

	  pgbuf_unfix_and_init (thread_p, R);

	  if (btree_dealloc_page (thread_p, &btid_int, &R_vpid) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      else
	{			/* merge not possible */
	  int c;

	  c = btree_compare_key (key, &mid_key, btid_int.key_type,
				 1, 1, NULL);
	  assert (c == DB_LT || c == DB_EQ || c == DB_GT);

	  if (c == DB_UNK)
	    {
	      /* error should have been set */
	      goto error;
	    }

	  if (c < 0)
	    {
	      /* choose left child */
	      pgbuf_unfix_and_init (thread_p, R);
	    }
	  else
	    {			/* choose right child */
	      pgbuf_unfix_and_init (thread_p, Q);
	      Q = R;
	      R = NULL;
	      Q_vpid = R_vpid;
	    }

	  pgbuf_unfix_and_init (thread_p, P);
	  P = Q;
	  Q = NULL;
	  P_vpid = Q_vpid;
	}

      btree_clear_key_value (&clear_key, &mid_key);
    }

  while (node_type == BTREE_NON_LEAF_NODE)
    {
      /* find and get the child page to be followed */
      if (btree_search_nonleaf_page (thread_p, &btid_int, P, key,
				     &p_slot_id, &Q_vpid) != NO_ERROR)
	{
	  goto error;
	}

      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (Q == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);

      merged = false;

      p_key_cnt = btree_node_number_of_keys (P);

      /* read the header record */
      header = btree_get_node_header (Q);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      if (p_slot_id < p_key_cnt)
	{			/* right merge */
	  if (spage_get_record (P, p_slot_id + 1, &peek_recdes1, PEEK)
	      != S_SUCCESS)
	    {
	      goto error;
	    }

	  btree_read_fixed_portion_of_non_leaf_record (&peek_recdes1,
						       &nleaf_pnt);
	  Right_vpid = nleaf_pnt.pnt;
	  Right = pgbuf_fix (thread_p, &Right_vpid, OLD_PAGE,
			     PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
	  if (Right == NULL)
	    {
	      goto error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, Right, PAGE_BTREE);

	  merge_status = btree_node_mergeable (thread_p, &btid_int, Q, Right);
	  if (merge_status != BTREE_MERGE_NO)
	    {			/* right merge possible */

	      /* start system permanent operation */
	      log_start_system_op (thread_p);
	      top_op_active = 1;

	      if (btree_merge_node (thread_p, &btid_int, P, Q, Right,
				    p_slot_id + 1, &child_vpid,
				    merge_status) != NO_ERROR)
		{
		  goto error;
		}

	      merged = true;

#if !defined(NDEBUG)
	      (void) spage_check_num_slots (thread_p, P);
	      (void) spage_check_num_slots (thread_p, Q);
#endif

	      if (VPID_EQ (&child_vpid, &Q_vpid))
		{
		  assert (next_page == NULL);

		  if (node_type == BTREE_LEAF_NODE)
		    {
		      next_page = btree_get_next_page (thread_p, Q);
		      if (next_page != NULL)
			{
			  (void) pgbuf_check_page_ptype (thread_p, next_page,
							 PAGE_BTREE);

			  if (btree_set_vpid_previous_vpid (thread_p,
							    &btid_int,
							    next_page,
							    &Q_vpid)
			      != NO_ERROR)
			    {
			      goto error;
			    }
			}
		    }

		  if (btree_is_new_file (&btid_int))
		    {
		      log_end_system_op (thread_p,
					 LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
		    }
		  else
		    {
		      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
		    }

		  if (next_page)
		    {
		      pgbuf_unfix_and_init (thread_p, next_page);
		    }

		  top_op_active = 0;

		  /* child page to be followed is Q */
		  pgbuf_unfix_and_init (thread_p, Right);

		  if (btree_dealloc_page (thread_p, &btid_int, &Right_vpid)
		      != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else
		{
		  assert (false);	/* is error ? */

		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
		  top_op_active = 0;

		  pgbuf_unfix_and_init (thread_p, P);
		  pgbuf_unfix_and_init (thread_p, Q);
		  pgbuf_unfix_and_init (thread_p, Right);

		  (void) thread_set_check_interrupt (thread_p,
						     old_check_interrupt);

		  return NULL;
		}
	    }
	  else
	    {			/* not merged */
	      pgbuf_unfix_and_init (thread_p, Right);
	    }
	}

      if (!merged && (p_slot_id > 1))
	{			/* left sibling accessible */
	  assert (p_slot_id - 1 > 0);
	  if (spage_get_record (P, p_slot_id - 1, &peek_recdes1, PEEK)
	      != S_SUCCESS)
	    {
	      goto error;
	    }

	  btree_read_fixed_portion_of_non_leaf_record (&peek_recdes1,
						       &nleaf_pnt);
	  Left_vpid = nleaf_pnt.pnt;

	  /* try to fix sibling page conditionally */
	  Left = pgbuf_fix (thread_p, &Left_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			    PGBUF_CONDITIONAL_LATCH);
	  if (Left == NULL)
	    {
	      /* unfix Q page */
	      pgbuf_unfix_and_init (thread_p, Q);

	      Left = pgbuf_fix (thread_p, &Left_vpid, OLD_PAGE,
				PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
	      if (Left == NULL)
		{
		  goto error;
		}

	      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			     PGBUF_UNCONDITIONAL_LATCH);
	      if (Q == NULL)
		{
		  goto error;
		}

	      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);

	      /* follow code may not be required */
	    }

	  (void) pgbuf_check_page_ptype (thread_p, Left, PAGE_BTREE);

	  merge_status = btree_node_mergeable (thread_p, &btid_int, Left, Q);
	  if (merge_status != BTREE_MERGE_NO)
	    {			/* left merge possible */

	      /* Start system permanent operation */
	      log_start_system_op (thread_p);
	      top_op_active = 1;

	      if (btree_merge_node
		  (thread_p, &btid_int, P, Left, Q, p_slot_id, &child_vpid,
		   merge_status) != NO_ERROR)
		{
		  goto error;
		}

	      merged = true;

#if !defined(NDEBUG)
	      (void) spage_check_num_slots (thread_p, P);
	      (void) spage_check_num_slots (thread_p, Left);
#endif

	      if (VPID_EQ (&child_vpid, &Left_vpid))
		{
		  assert (next_page == NULL);

		  if (node_type == BTREE_LEAF_NODE)
		    {
		      next_page = btree_get_next_page (thread_p, Left);
		      if (next_page != NULL)
			{
			  (void) pgbuf_check_page_ptype (thread_p, next_page,
							 PAGE_BTREE);

			  if (btree_set_vpid_previous_vpid (thread_p,
							    &btid_int,
							    next_page,
							    &Left_vpid)
			      != NO_ERROR)
			    {
			      goto error;
			    }
			}
		    }

		  if (btree_is_new_file (&btid_int))
		    {
		      log_end_system_op (thread_p,
					 LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
		    }
		  else
		    {
		      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
		    }

		  if (next_page)
		    {
		      pgbuf_unfix_and_init (thread_p, next_page);
		    }

		  top_op_active = 0;

		  /* child page to be followed is Left */
		  pgbuf_unfix_and_init (thread_p, Q);

		  if (btree_dealloc_page (thread_p, &btid_int, &Q_vpid) !=
		      NO_ERROR)
		    {
		      goto error;
		    }

		  Q = Left;
		  Left = NULL;
		  Q_vpid = Left_vpid;
		}
	      else
		{
		  assert (false);

		  /* do not unfix P, Q, R before topop rollback */
		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
		  top_op_active = 0;

		  pgbuf_unfix_and_init (thread_p, P);
		  pgbuf_unfix_and_init (thread_p, Q);
		  pgbuf_unfix_and_init (thread_p, Left);

		  (void) thread_set_check_interrupt (thread_p,
						     old_check_interrupt);

		  return NULL;
		}
	    }
	  else
	    {			/* not merged */
	      pgbuf_unfix_and_init (thread_p, Left);
	    }
	}

      pgbuf_unfix_and_init (thread_p, P);
      P = Q;
      Q = NULL;
      P_vpid = Q_vpid;
    }

  p_slot_id = NULL_SLOTID;

  if (nextkey_lock_request == false || search_without_locking == true
      || MVCC_BTREE_DELETE_NO_LOCK_REQUIRED (mvcc_args))
    {
      goto key_deletion;
    }

  /* keys(the number of keyvals) of the leaf page is valid */

  if (locked_keys == BTREE_ALL_KEYS_LOCKED)
    {
      if (btree_search_leaf_page (thread_p, &btid_int, P, key, &p_slot_id))
	{
	  assert (p_slot_id > 0);
	  if (spage_get_record (P, p_slot_id, &peek_recdes1, PEEK) !=
	      S_SUCCESS)
	    {
	      goto error;
	    }
	  btree_read_record (thread_p, &btid_int, P, &peek_recdes1,
			     NULL, &leaf_pnt, BTREE_LEAF_NODE,
			     &clear_key, &offset, PEEK_KEY_VALUE, NULL);
	  (void) btree_leaf_get_first_oid (&btid_int, &peek_recdes1,
					   &curr_key_first_oid, &C_class_oid,
					   NULL);
	  if (!BTREE_IS_UNIQUE (btid_int.unique_pk))
	    {
	      assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	      COPY_OID (&C_class_oid, &class_oid);
	    }
	  delete_first_key_oid = OID_EQ (&curr_key_first_oid, oid);
	  btree_make_pseudo_oid (curr_key_first_oid.pageid,
				 curr_key_first_oid.slotid,
				 curr_key_first_oid.volid, btid_int.sys_btid,
				 &C_oid);
	  curr_key_oids_cnt =
	    btree_leaf_get_num_oids (&peek_recdes1, offset, BTREE_LEAF_NODE,
				     oid_size);
	  if (curr_key_oids_cnt > 1
	      || (curr_key_oids_cnt == 1 && !VPID_ISNULL (&(leaf_pnt.ovfl))))
	    {
	      curr_key_lock_commit_duration = true;
	    }
	  else
	    {
	      curr_key_lock_commit_duration = false;
	    }

	  if (curr_key_lock_commit_duration && delete_first_key_oid)
	    {
	      /* at least 2 OIDs, and the first one is removed
	         lock the second OID */
	      if (btree_delete_lock_curr_key_next_pseudo_oid (thread_p,
							      &btid_int,
							      &peek_recdes1,
							      offset,
							      &P_vpid, &P,
							      &leaf_pnt.ovfl,
							      &C_class_oid,
							      &search_without_locking)
		  != NO_ERROR)
		{
		  goto error;
		}

	      if (search_without_locking)
		{
		  curr_lock_flag = true;
		  goto start_point;
		}
	    }

	  /* remove the current key lock */
	  curr_lock_flag = true;
	  goto key_deletion;
	}
      else
	{
	  log_append_redo_data2 (thread_p, RVBT_NOOP, &btid->vfid, P, -1, 0,
				 NULL);
	  pgbuf_set_dirty (thread_p, P, DONT_FREE);
	  btree_set_unknown_key_error (thread_p, btid, key,
				       "btree_delete: "
				       "btree_search_leaf_page fails, "
				       "current key not found.");
	  goto error;
	}
    }

  /* save node info. of the leaf page
   * Header must be calculated again.
   * because, SMO might have been occurred.
   */

  header = btree_get_node_header (P);
  if (header == NULL)
    {
      goto error;
    }

  /* find next key */
  if (btree_search_leaf_page (thread_p, &btid_int, P, key, &p_slot_id))
    {
      if (!BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	  assert (p_slot_id > 0);
	  if (spage_get_record (P, p_slot_id, &peek_recdes1, PEEK) !=
	      S_SUCCESS)
	    {
	      goto error;
	    }

	  btree_read_record (thread_p, &btid_int, P, &peek_recdes1,
			     NULL, &leaf_pnt, BTREE_LEAF_NODE,
			     &clear_key, &offset, PEEK_KEY_VALUE, NULL);
	  curr_key_oids_cnt =
	    btree_leaf_get_num_oids (&peek_recdes1, offset,
				     BTREE_LEAF_NODE, oid_size);
	  if (curr_key_oids_cnt > 1
	      || (curr_key_oids_cnt == 1 && !VPID_ISNULL (&(leaf_pnt.ovfl))))
	    {
	      /* at least 2 OIDs, lock current key commit duration */
	      curr_key_lock_commit_duration = true;
	      /* if not SERIALIZABLE, lock the current key only */
	      if (tran_isolation != TRAN_SERIALIZABLE)
		{
		  if (next_lock_flag == true)
		    {
		      /* remove the next key lock */
		      assert (!OID_ISNULL (&saved_N_oid));
		      assert (!OID_ISNULL (&saved_N_class_oid));
		      lock_remove_object_lock (thread_p, &saved_N_oid,
					       &saved_N_class_oid, NX_LOCK);
		      next_lock_flag = false;
		      OID_SET_NULL (&saved_N_oid);
		      OID_SET_NULL (&saved_N_class_oid);
		    }

		  OID_SET_NULL (&N_oid);
		  goto curr_key_locking;
		}
	    }
	}
    }
  else
    {
      log_append_redo_data2 (thread_p, RVBT_NOOP, &btid->vfid, P, -1, 0,
			     NULL);
      pgbuf_set_dirty (thread_p, P, DONT_FREE);
      btree_set_unknown_key_error (thread_p, btid, key,
				   "btree_delete: btree_search_leaf_page fails, next key not found.");
      goto error;
    }

  memset (&tmp_bts, 0, sizeof (BTREE_SCAN));
  BTREE_INIT_SCAN (&tmp_bts);
  tmp_bts.C_page = P;
  tmp_bts.slot_id = p_slot_id;

  ret_val =
    btree_find_next_index_record_holding_current (thread_p, &tmp_bts,
						  &peek_recdes1);

  if (ret_val != NO_ERROR)
    {
      goto error;
    }

  is_last_key = false;		/* init */
  if (tmp_bts.C_page != NULL && tmp_bts.C_page != P)
    {
      next_page_flag = true;
      N_vpid = tmp_bts.C_vpid;
      N = tmp_bts.C_page;
    }
  else
    {
      next_page_flag = false;
      /* tmp_bts.C_page is NULL if next record is not exists */
      if (tmp_bts.C_page == NULL)
	{
	  is_last_key = true;
	}
    }

  tmp_bts.C_page = NULL;	/* this page is pointed by P (or N) */

  if (is_last_key)
    {
      assert (next_page_flag == false);
      /* the first entry of the root page is used as the next OID */
      N_oid.volid = btid->vfid.volid;
      N_oid.pageid = btid->root_pageid;
      N_oid.slotid = 0;
      if (BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  COPY_OID (&N_class_oid, &btid_int.topclass_oid);
	}
      else
	{
	  COPY_OID (&N_class_oid, &class_oid);
	}
      assert (!OID_ISNULL (&N_class_oid));
    }
  else
    {
      btree_read_record (thread_p, &btid_int, P, &peek_recdes1, NULL,
			 &leaf_pnt, BTREE_LEAF_NODE, &clear_key,
			 &offset, PEEK_KEY_VALUE, NULL);

      (void) btree_leaf_get_first_oid (&btid_int, &peek_recdes1, &N_oid,
				       &N_class_oid, NULL);
      btree_make_pseudo_oid (N_oid.pageid, N_oid.slotid, N_oid.volid,
			     btid_int.sys_btid, &N_oid);

      if (BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  assert (!OID_ISNULL (&N_class_oid));

	  if (OID_EQ (&N_class_oid, &class_oid)
	      && IS_WRITE_EXCLUSIVE_LOCK (class_lock))
	    {
	      if (next_lock_flag == true)
		{
		  /* remove the next key lock */
		  assert (!OID_ISNULL (&saved_N_oid));
		  assert (!OID_ISNULL (&saved_N_class_oid));
		  lock_remove_object_lock (thread_p, &saved_N_oid,
					   &saved_N_class_oid, NX_LOCK);
		  next_lock_flag = false;
		  OID_SET_NULL (&saved_N_oid);
		  OID_SET_NULL (&saved_N_class_oid);
		}

	      if (N != NULL)
		{
		  pgbuf_unfix_and_init (thread_p, N);
		}
	      goto curr_key_locking;
	    }
	}
      else
	{
	  COPY_OID (&N_class_oid, &class_oid);
	}
    }

  if (next_lock_flag == true)
    {
      if (OID_EQ (&saved_N_oid, &N_oid))
	{
	  if (next_page_flag == true)
	    {
	      pgbuf_unfix_and_init (thread_p, N);
	      next_page_flag = false;
	    }
	  goto curr_key_locking;
	}

      /* remove the next key lock */
      assert (!OID_ISNULL (&saved_N_oid));
      assert (!OID_ISNULL (&saved_N_class_oid));
      lock_remove_object_lock (thread_p, &saved_N_oid, &saved_N_class_oid,
			       NX_LOCK);
      next_lock_flag = false;
      OID_SET_NULL (&saved_N_oid);
      OID_SET_NULL (&saved_N_class_oid);
    }

  /* CONDITIONAL lock request */
  assert (!OID_ISNULL (&N_oid));
  assert (!OID_ISNULL (&N_class_oid));
  ret_val = lock_object_with_btid (thread_p, &N_oid, &N_class_oid, btid,
				   NX_LOCK, LK_COND_LOCK);
  if (ret_val == LK_GRANTED)
    {
      next_lock_flag = true;
      if (next_page_flag == true)
	{
	  pgbuf_unfix_and_init (thread_p, N);
	  next_page_flag = false;
	}
    }
  else if (ret_val == LK_NOTGRANTED_DUE_TIMEOUT)
    {
      /* save some information for validation checking
       * after UNCONDITIONAL lock request
       */
      temp_lsa = pgbuf_get_lsa (P);
      LSA_COPY (&saved_plsa, temp_lsa);
      pgbuf_unfix_and_init (thread_p, P);
      if (next_page_flag == true)
	{
	  temp_lsa = pgbuf_get_lsa (N);
	  LSA_COPY (&saved_nlsa, temp_lsa);
	  pgbuf_unfix_and_init (thread_p, N);
	}
      COPY_OID (&saved_N_oid, &N_oid);
      COPY_OID (&saved_N_class_oid, &N_class_oid);

      assert (P == NULL);
      assert (Q == NULL);
      assert (R == NULL);
      assert (N == NULL);

      assert (!OID_ISNULL (&N_oid));
      assert (!OID_ISNULL (&N_class_oid));
      ret_val = lock_object_with_btid (thread_p, &N_oid, &N_class_oid,
				       btid, NX_LOCK, LK_UNCOND_LOCK);
      /* UNCONDITIONAL lock request */
      if (ret_val != LK_GRANTED)
	{
	  goto error;
	}

      next_lock_flag = true;

      /* Validation checking after the unconditional lock acquisition
       * In this implementation, only PageLSA of the page is checked.
       * It means that if the PageLSA has not been changed,
       * the page image does not changed
       * during the unconditional next key lock acquisition.
       * So, the next lock that is acquired is valid.
       * If we give more accurate and precise checking condition,
       * the operation that traverse the tree can be reduced.
       */
      P = pgbuf_fix_without_validation (thread_p, &P_vpid, OLD_PAGE,
					PGBUF_LATCH_WRITE,
					PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      temp_lsa = pgbuf_get_lsa (P);
      if (!btree_leaf_lsa_eq (thread_p, &saved_plsa, temp_lsa))
	{
	  pgbuf_unfix_and_init (thread_p, P);
	  next_page_flag = false;
	  goto start_point;
	}

      /* the first leaf page is valid */

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      if (next_page_flag == true)
	{
	  N = pgbuf_fix_without_validation (thread_p, &N_vpid, OLD_PAGE,
					    PGBUF_LATCH_READ,
					    PGBUF_UNCONDITIONAL_LATCH);
	  if (N == NULL)
	    {
	      goto error;
	    }

	  temp_lsa = pgbuf_get_lsa (N);
	  if (!btree_leaf_lsa_eq (thread_p, &saved_nlsa, temp_lsa))
	    {
	      pgbuf_unfix_and_init (thread_p, P);
	      pgbuf_unfix_and_init (thread_p, N);
	      next_page_flag = false;
	      goto start_point;
	    }

	  /* the next leaf page is valid */

	  (void) pgbuf_check_page_ptype (thread_p, N, PAGE_BTREE);

	  pgbuf_unfix_and_init (thread_p, N);
	  next_page_flag = false;
	}

      /* only page P is currently locked and fetched */
    }
  else
    {
      goto error;
    }

curr_key_locking:
  if (!curr_key_lock_commit_duration || tran_isolation == TRAN_SERIALIZABLE)
    {
      assert (p_slot_id > 0);
      if (spage_get_record (P, p_slot_id, &peek_recdes1, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      btree_read_record (thread_p, &btid_int, P, &peek_recdes1, NULL,
			 &leaf_pnt, BTREE_LEAF_NODE, &clear_key,
			 &offset, PEEK_KEY_VALUE, NULL);
    }

  (void) btree_leaf_get_first_oid (&btid_int, &peek_recdes1,
				   &curr_key_first_oid, &C_class_oid, NULL);
  btree_make_pseudo_oid (curr_key_first_oid.pageid, curr_key_first_oid.slotid,
			 curr_key_first_oid.volid, btid_int.sys_btid, &C_oid);

  delete_first_key_oid = OID_EQ (&curr_key_first_oid, oid);

  if (BTREE_IS_UNIQUE (btid_int.unique_pk))	/* unique index */
    {
      assert (!OID_ISNULL (&C_class_oid));

      if (OID_EQ (&C_class_oid, &class_oid)
	  && IS_WRITE_EXCLUSIVE_LOCK (class_lock))
	{
	  goto key_deletion;
	}
    }
  else
    {
      COPY_OID (&C_class_oid, &class_oid);
    }

  if (curr_lock_flag == true)
    {
      if (OID_EQ (&saved_C_oid, &C_oid))
	{
	  /* current key already locked, goto curr key locking consistency */
	  goto curr_key_lock_consistency;
	}

      /* remove current key lock */
      assert (!OID_ISNULL (&saved_C_oid));
      assert (!OID_ISNULL (&saved_C_class_oid));
      lock_remove_object_lock (thread_p, &saved_C_oid, &saved_C_class_oid,
			       NX_LOCK);
      curr_lock_flag = false;
      OID_SET_NULL (&saved_C_oid);
      OID_SET_NULL (&saved_C_class_oid);
    }

  if (locked_keys == BTREE_CURRENT_KEYS_LOCKED)
    {
      /* current key lock already granted */
      assert (lock_has_lock_on_object (&C_oid, &C_class_oid,
				       LOG_FIND_THREAD_TRAN_INDEX (thread_p),
				       NX_LOCK) == 1);
      ret_val = LK_GRANTED;
      curr_lock_flag = true;
    }
  else if ((curr_key_lock_commit_duration == true)
	   && (!delete_first_key_oid || locked_keys == BTREE_NO_KEY_LOCKED))
    {
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));
      ret_val = lock_object_with_btid (thread_p, &C_oid, &C_class_oid, btid,
				       NX_LOCK, LK_COND_LOCK);
      if (ret_val == LK_NOTGRANTED_DUE_TIMEOUT)
	{
	  ret_val = LK_NOTGRANTED;
	}
      else if (ret_val != LK_GRANTED)
	{
	  goto error;
	}
      else
	{
	  curr_lock_flag = true;
	}
    }
  else
    {
      ret_val = lock_hold_object_instant (thread_p, &C_oid, &C_class_oid,
					  NX_LOCK);
    }

  if (ret_val == LK_GRANTED)
    {
      /* nothing to do */
    }
  else if (ret_val == LK_NOTGRANTED)
    {
      temp_lsa = pgbuf_get_lsa (P);
      LSA_COPY (&saved_plsa, temp_lsa);
      pgbuf_unfix_and_init (thread_p, P);

      COPY_OID (&saved_C_oid, &C_oid);
      COPY_OID (&saved_C_class_oid, &C_class_oid);

      if (OID_ISNULL (&saved_N_oid))
	{
	  /* save the next pseudo oid */
	  COPY_OID (&saved_N_oid, &N_oid);
	  COPY_OID (&saved_N_class_oid, &N_class_oid);
	}

      assert (P == NULL);
      assert (Q == NULL);
      assert (R == NULL);
      assert (N == NULL);

      /* UNCONDITIONAL lock request */
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));
      ret_val = lock_object_with_btid (thread_p, &C_oid, &C_class_oid, btid,
				       NX_LOCK, LK_UNCOND_LOCK);
      if (ret_val != LK_GRANTED)
	{
	  goto error;
	}
      curr_lock_flag = true;

      P = pgbuf_fix_without_validation (thread_p, &P_vpid, OLD_PAGE,
					PGBUF_LATCH_WRITE,
					PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      temp_lsa = pgbuf_get_lsa (P);
      if (!btree_leaf_lsa_eq (thread_p, &saved_plsa, temp_lsa))
	{
	  pgbuf_unfix_and_init (thread_p, P);
	  goto start_point;
	}

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      if (curr_key_lock_commit_duration == true && delete_first_key_oid)
	{
	  /* still have to lock one pseudo OID, peek the record again */
	  assert (p_slot_id > 0);
	  if (spage_get_record (P, p_slot_id, &peek_recdes1, PEEK) !=
	      S_SUCCESS)
	    {
	      goto error;
	    }

	  btree_read_record (thread_p, &btid_int, P, &peek_recdes1,
			     NULL, &leaf_pnt, BTREE_LEAF_NODE,
			     &clear_key, &offset, PEEK_KEY_VALUE, NULL);
	}
    }
  else
    {
      goto error;
    }

curr_key_lock_consistency:
  if (curr_key_lock_commit_duration == true && delete_first_key_oid)
    {
      assert (search_without_locking == false);
      assert (curr_lock_flag == true);

      if (btree_delete_lock_curr_key_next_pseudo_oid (thread_p, &btid_int,
						      &peek_recdes1, offset,
						      &P_vpid, &P,
						      &leaf_pnt.ovfl,
						      &C_class_oid,
						      &search_without_locking)
	  != NO_ERROR)
	{
	  goto error;
	}

      if (search_without_locking)
	{
	  goto start_point;
	}
    }

  /* valid point for key deletion
   * only the page P is currently locked and fetched
   */

key_deletion:

  /* a leaf page is reached, perform the deletion
   */
  if (!mvcc_Enabled || BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      pkey_deleted = &key_deleted;
      key_deleted = false;
    }

  if (prm_get_bool_value (PRM_ID_LOG_BTREE_OPS))
    {
      if (mvcc_args)
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: delete oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d)) with "
			 "mvcc_info=%lld | %lld, purpose=%d",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid, mvcc_args->insert_mvccid,
			 mvcc_args->delete_mvccid, mvcc_args->purpose);
	}
      else
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: non-mvcc delete object "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d))",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid);
	}
    }
  if (btree_delete_from_leaf (thread_p, pkey_deleted, &btid_int,
			      &P_vpid, key, &class_oid, oid,
			      p_slot_id, mvcc_args) != NO_ERROR)
    {
      if (prm_get_bool_value (PRM_ID_LOG_BTREE_OPS))
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: Failed delete "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d)) with "
			 "mvcc_info=%lld | %lld, purpose=%d",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid);
	}
      goto error;
    }
  if (prm_get_bool_value (PRM_ID_LOG_BTREE_OPS))
    {
      _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: Successful delete "
		     "oid(%d, %d, %d) "
		     "class_oid(%d, %d, %d) and btid(%d, (%d, %d)) with "
		     "mvcc_info=%lld | %lld, purpose=%d",
		     oid->volid, oid->pageid, oid->slotid,
		     cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
		     btid->root_pageid, btid->vfid.volid, btid->vfid.fileid);
    }

#if !defined(NDEBUG)
  (void) spage_check_num_slots (thread_p, P);
#endif

  pgbuf_unfix_and_init (thread_p, P);

  if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk)
      && !thread_is_vacuum_worker (thread_p))
    {
      if (op_type == SINGLE_ROW_DELETE || op_type == SINGLE_ROW_UPDATE
	  || op_type == SINGLE_ROW_MODIFY)
	{
	  /* at here, do nothing.
	   * later, undo root header statistics
	   */
	}
      else
	{
	  if (unique_stat_info == NULL)
	    {
	      goto error;
	    }
	  else if (key_deleted == false)
	    {
	      /* revert local statistical information */
	      unique_stat_info->num_keys++;
	    }
	}
    }

  if (curr_lock_flag == true && (curr_key_lock_commit_duration == false ||
				 delete_first_key_oid))
    {
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));
      lock_remove_object_lock (thread_p, &C_oid, &C_class_oid, NX_LOCK);
    }

  mnt_bt_deletes (thread_p);

  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_FULL)
    {
      btree_dump (thread_p, stdout, btid, 2);
    }
#endif

  return key;

error:

  /* do not unfix P, Q, R before topop rollback */
  if (top_op_active)
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
    }

  if (P)
    {
      pgbuf_unfix_and_init (thread_p, P);
    }
  if (Q)
    {
      pgbuf_unfix_and_init (thread_p, Q);
    }
  if (R)
    {
      pgbuf_unfix_and_init (thread_p, R);
    }
  if (N)
    {
      pgbuf_unfix_and_init (thread_p, N);
    }
  if (next_page)
    {
      pgbuf_unfix_and_init (thread_p, next_page);
    }

  if (curr_lock_flag)
    {
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));
      lock_remove_object_lock (thread_p, &C_oid, &C_class_oid, NX_LOCK);
    }

  /* even if an error occurs,
   * the next-key lock acquired already must not be released.
   * if the next-key lock is to be released,
   * a new key that has same key value with one of deleted key values
   * can be inserted before current transaction aborts the deletion.
   * therefore, uniqueness violation could be occurred.
   */
  if (Left)
    {
      pgbuf_unfix_and_init (thread_p, Left);
    }
  if (Right)
    {
      pgbuf_unfix_and_init (thread_p, Right);
    }

  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

  return NULL;
}

/*
 * btree_insert_oid_with_new_key () -
 *   return:
 *   btid(in): B+tree index identifier
 *   leaf_page(in): Leaf page pointer to which the key is to be inserted
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   slot_id(in):
 *   p_mvcc_rec_header(in): MVCC record header
 */
static int
btree_insert_oid_with_new_key (THREAD_ENTRY * thread_p, BTID_INT * btid,
			       PAGE_PTR leaf_page, DB_VALUE * key,
			       OID * cls_oid, OID * oid, INT16 slot_id,
			       MVCC_REC_HEADER * p_mvcc_rec_header)
{
  int ret = NO_ERROR;
  int key_type = BTREE_NORMAL_KEY;
  int key_len, max_free;
  RECDES rec;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int key_cnt;
  BTREE_NODE_HEADER *header = NULL;
  DB_VALUE *new_key = key;
  LOG_RCVINDEX rcvindex;

  rec.type = REC_HOME;
  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  max_free = spage_max_space_for_new_record (thread_p, leaf_page);
  key_len = btree_get_key_length (key);

  /* form a new leaf record */
  if (key_len >= BTREE_MAX_KEYLEN_INPAGE)
    {
      key_type = BTREE_OVERFLOW_KEY;
    }


  /* put a LOGICAL log to undo the insertion of <key, oid> pair
   * to the B+tree index. This will be a call to delete this pair
   * from the index. Put this logical log here, because now we know
   * that the <key, oid> pair to be inserted is not already in the index.
   */
  if (btree_is_new_file (btid) != true)
    {
      if (p_mvcc_rec_header != NULL
	  && MVCC_IS_HEADER_INSID_NOT_ALL_VISIBLE (p_mvcc_rec_header))
	{
	  MVCC_BTREE_OP_ARGUMENTS mvcc_args;
	  mvcc_args.purpose = MVCC_BTREE_INSERT_OBJECT;
	  mvcc_args.insert_mvccid = MVCC_GET_INSID (p_mvcc_rec_header);
	  ret =
	    btree_rv_save_keyval (btid, key, cls_oid, oid, &mvcc_args,
				  &rv_key, &rv_key_len);
	}
      else
	{
	  ret =
	    btree_rv_save_keyval (btid, key, cls_oid, oid, NULL, &rv_key,
				  &rv_key_len);
	}
      if (ret != NO_ERROR)
	{
	  return ret;
	}
      if (key_type == BTREE_OVERFLOW_KEY)
	{
	  rcvindex =
	    p_mvcc_rec_header !=
	    NULL ? RVBT_KEYVAL_MVCC_INS : RVBT_KEYVAL_INS;
	  log_append_undo_data2 (thread_p, rcvindex, &btid->sys_btid->vfid,
				 NULL, -1, rv_key_len, rv_key);
	}
    }

  /* do not compress overflow key */
  if (key_type != BTREE_OVERFLOW_KEY)
    {
      int diff_column;

      diff_column = btree_node_common_prefix (thread_p, btid, leaf_page);
      if (diff_column > 0)
	{
	  new_key = db_value_copy (key);
	  pr_midxkey_remove_prefix (new_key, diff_column);
	}
    }

  ret = btree_write_record (thread_p, btid, NULL, new_key, BTREE_LEAF_NODE,
			    key_type, key_len, false, cls_oid, oid,
			    p_mvcc_rec_header, &rec);

  if (new_key != key)
    {
      db_value_clear (new_key);
    }

  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (rec.length > max_free)
    {
      /* if this block is entered, that means there is not enough space
       * in the leaf page for a new key. This shows a bug in the algorithm.
       */
      char *ptr = NULL;
      FILE *fp = NULL;
      size_t sizeloc;

      fp = port_open_memstream (&ptr, &sizeloc);
      if (fp)
	{
	  VPID *vpid = pgbuf_get_vpid_ptr (leaf_page);

	  btree_dump_page (thread_p, fp, cls_oid, btid,
			   NULL, leaf_page, vpid, 2, 2);
	  spage_dump (thread_p, fp, leaf_page, true);
	  port_close_memstream (fp, &ptr, &sizeloc);
	}

      er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_BTREE_NO_SPACE,
	      2, rec.length, ptr);

      if (ptr)
	{
	  free (ptr);
	}

      assert_release (false);
      ret = ER_BTREE_NO_SPACE;

      goto exit_on_error;
    }

  /* save the inserted record for redo purposes,
   * in the case of redo, the record will be inserted.
   */
  btree_rv_write_log_record_for_key_insert (rv_data, &rv_data_len,
					    (INT16) key_len, &rec);

  if (btree_is_new_file (btid) != true && key_type == BTREE_NORMAL_KEY)
    {
      assert (rv_key != NULL);
      rcvindex =
	p_mvcc_rec_header !=
	NULL ? RVBT_KEYVAL_MVCC_INS_LFRECORD_KEYINS :
	RVBT_KEYVAL_INS_LFRECORD_KEYINS;
      log_append_undoredo_data2 (thread_p, rcvindex, &btid->sys_btid->vfid,
				 leaf_page, slot_id, rv_key_len, rv_data_len,
				 rv_key, rv_data);
    }

  /* insert the new record */
  assert (rec.length % 4 == 0);
  assert (slot_id > 0);
  if (spage_insert_at (thread_p, leaf_page, slot_id, &rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  key_cnt = btree_node_number_of_keys (leaf_page);

  /* update the page header */
  header = btree_get_node_header (leaf_page);
  if (header == NULL)
    {
      goto exit_on_error;
    }

  key_len = BTREE_GET_KEY_LEN_IN_PAGE (key_len);

  /* do not write log (for update max_key_len) because redo log for
   * RVBT_LFRECORD_KEY_INS will change max_key_len if needed
   */
  header->max_key_len = MAX (header->max_key_len, key_len);

  assert (header->split_info.pivot >= 0 && key_cnt > 0);
  btree_split_next_pivot (&header->split_info, (float) slot_id / key_cnt,
			  key_cnt);

  /* log the new record insertion and update to the header record for
   * undo/redo purposes.  This can be after the insert/update since we
   * still have the page pinned.
   */
  if (btree_is_new_file (btid))
    {
      log_append_undoredo_data2 (thread_p, RVBT_LFRECORD_KEYINS,
				 &btid->sys_btid->vfid, leaf_page, slot_id,
				 sizeof (slot_id), rv_data_len,
				 &slot_id, rv_data);
    }
  else
    {
      if (key_type == BTREE_OVERFLOW_KEY)
	{
	  log_append_redo_data2 (thread_p, RVBT_LFRECORD_KEYINS,
				 &btid->sys_btid->vfid, leaf_page, slot_id,
				 rv_data_len, rv_data);
	}
    }

  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_insert_oid_into_leaf_rec () -
 *   return:
 *   btid(in): B+tree index identifier
 *   leaf_page(in): Leaf page pointer to which the key is to be inserted
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   p_mvcc_rec_header: MVCC record header
 *   insoid_mode(in):  insert OID mode
 */
static int
btree_insert_oid_into_leaf_rec (THREAD_ENTRY * thread_p, BTID_INT * btid,
				PAGE_PTR leaf_page, DB_VALUE * key,
				OID * cls_oid, OID * oid, INT16 slot_id,
				RECDES * rec, VPID * first_ovfl_vpid,
				MVCC_REC_HEADER * p_mvcc_rec_header,
				INT16 insoid_mode)
{
#define BTREE_INSERT_OID_INTO_LEAF_REDO_CRUMBS_MAX 3
#define BTREE_INSERT_OID_INTO_LEAF_UNDO_CRUMBS_MAX 1
  int ret = NO_ERROR;
  int key_type = BTREE_NORMAL_KEY;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  RECINS_STRUCT recins;
  MVCC_REC_HEADER saved_first_mvcc_rec_header, local_mvcc_header;
  LOG_RCVINDEX rcvindex;
  OID saved_first_oid, saved_first_class_oid;
  LOG_DATA_ADDR addr;
  LOG_CRUMB redo_crumbs[BTREE_INSERT_OID_INTO_LEAF_REDO_CRUMBS_MAX];
  LOG_CRUMB undo_crumbs[BTREE_INSERT_OID_INTO_LEAF_UNDO_CRUMBS_MAX];
  int n_redo_crumbs = 0, n_undo_crumbs = 0;
  MVCC_BTREE_OP_ARGUMENTS mvcc_args, *mvcc_args_p = NULL;
#if !defined (NDEBUG)
  char domain_buf[BTID_DOMAIN_BUFFER_SIZE], *domain_ptr = NULL;
  int domain_size = or_packed_domain_size (btid->key_type, 0);
#endif

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  /* Initialize log address data */
  addr.pgptr = leaf_page;
  addr.offset = slot_id;
  addr.vfid = &btid->sys_btid->vfid;

  recins.flags = 0;
  BTREE_INSERT_RCV_SET_RECORD_REGULAR (&recins);
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_OID_INSERTED);
  COPY_OID (&recins.oid, oid);

  rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
  if (p_mvcc_rec_header != NULL)
    {
      assert ((mvcc_Enabled == true) &&
	      (p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)
	      && !(p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID));

      /* Now that we passed the assert check, in case insert MVCCID is all
       * visible clear the flag for valid insert MVCCID to avoid storing it.
       */
      if (!MVCC_IS_HEADER_INSID_NOT_ALL_VISIBLE (p_mvcc_rec_header))
	{
	  /* Clear insert MVCCID flag... We don't need to save or log it */
	  MVCC_CLEAR_FLAG_BITS (p_mvcc_rec_header, OR_MVCC_FLAG_VALID_INSID);
	}
      else
	{
	  /* We need to also log MVCCID for undo/redo purpose */
	  /* TODO: We have an exception for undo operation in case of
	   *	   new files which we need to fix in the future (in order to
	   *	   allow vacuum find our object).
	   */
	  mvcc_args_p = &mvcc_args;
	  mvcc_args_p->purpose = MVCC_BTREE_INSERT_OBJECT;
	  mvcc_args_p->insert_mvccid = MVCC_GET_INSID (p_mvcc_rec_header);
	  rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
	}
    }

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_UNIQUE);
      COPY_OID (&recins.class_oid, cls_oid);
      /* in MVCC, in case of unique indexes, do not insert at end */
      assert (mvcc_Enabled == false
	      || insoid_mode != BTREE_INSERT_OID_MODE_DEFAULT);

      if (insoid_mode == BTREE_INSERT_OID_MODE_REPLACE_FIRST
	  || !VPID_ISNULL (first_ovfl_vpid))
	{
	  if (p_mvcc_rec_header == NULL)
	    {
	      BTREE_INIT_MVCC_HEADER (&local_mvcc_header);
	      p_mvcc_rec_header = &local_mvcc_header;
	    }

	  /* overflow VPID exists or will be created */
	  BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_rec_header);
	}
    }
  else
    {
      OID_SET_NULL (&recins.class_oid);
    }

  recins.ovfl_vpid = *first_ovfl_vpid;

  /* Set insert OID mode */
  BTREE_INSERT_RCV_SET_FLAGS (&recins, insoid_mode);

#if !defined (NDEBUG)
  if (domain_size > BTID_DOMAIN_BUFFER_SIZE)
    {
      domain_ptr = (char *) db_private_alloc (thread_p, domain_size);
      if (domain_ptr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, domain_size);
	  goto exit_on_error;
	}
    }
  else
    {
      domain_ptr = domain_buf;
    }
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_KEY_DOMAIN);
#endif

  /* Create redo crumbs */
  redo_crumbs[n_redo_crumbs].length = sizeof (recins);
  redo_crumbs[n_redo_crumbs++].data = &recins;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      /* Add top class OID */
      redo_crumbs[n_redo_crumbs].length = sizeof (btid->topclass_oid);
      redo_crumbs[n_redo_crumbs++].data = &btid->topclass_oid;
    }

#if !defined (NDEBUG)
  /* Add key domain */
  (void) or_pack_domain (domain_ptr, btid->key_type, 0, 0);
  redo_crumbs[n_redo_crumbs].length = domain_size;
  redo_crumbs[n_redo_crumbs++].data = domain_ptr;
#endif

  assert (n_redo_crumbs <= BTREE_INSERT_OID_INTO_LEAF_REDO_CRUMBS_MAX);

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, rec, BTREE_LEAF_NODE);

      log_append_undo_data (thread_p, RVBT_NDRECORD_UPD, &addr,
			    rv_data_len, rv_data);
    }
  else
    {
      ret =
	btree_rv_save_keyval (btid, key, cls_oid, oid, mvcc_args_p, &rv_key,
			      &rv_key_len);
      if (ret != NO_ERROR)
	{
	  return ret;
	}

      /* Create undo crumbs */
      undo_crumbs[n_undo_crumbs].length = rv_key_len;
      undo_crumbs[n_undo_crumbs++].data = rv_key;

      assert (n_undo_crumbs <= BTREE_INSERT_OID_INTO_LEAF_UNDO_CRUMBS_MAX);

      log_append_undoredo_crumbs (thread_p, rcvindex, &addr,
				  n_undo_crumbs, n_redo_crumbs,
				  undo_crumbs, redo_crumbs);
    }

  if (insoid_mode != BTREE_INSERT_OID_MODE_DEFAULT)
    {
      /* insert or replace in first position needed
       * save first oid that will be moved at the end of the buffer
       * save MVCC rec header of first OID. Set fixed size to saved MVCC rec
       * header since the saved first OID is moved from first position
       */
      btree_leaf_get_first_oid (btid, rec, &saved_first_oid,
				&saved_first_class_oid,
				&saved_first_mvcc_rec_header);
      BTREE_MVCC_SET_HEADER_FIXED_SIZE (&saved_first_mvcc_rec_header);

      /* replace first OID with the new OID */
      btree_leaf_change_first_oid (rec, btid, oid, cls_oid,
				   p_mvcc_rec_header, NULL);

      /* prepare for insertion of saved first OID at the end of the buffer */
      if (insoid_mode == BTREE_INSERT_OID_MODE_AT_BEGINNING)
	{
	  oid = &saved_first_oid;
	  cls_oid = &saved_first_class_oid;
	  p_mvcc_rec_header = &saved_first_mvcc_rec_header;
	}
    }

  if (insoid_mode == BTREE_INSERT_OID_MODE_REPLACE_FIRST)
    {
      /* nothing to do */
    }
  else if (VPID_ISNULL (first_ovfl_vpid))
    {
      int oid_size = OR_OID_SIZE;
      btree_append_oid (rec, oid);

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  btree_append_oid (rec, cls_oid);
	  oid_size += OR_OID_SIZE;
	}

      if (p_mvcc_rec_header != NULL)
	{
	  btree_append_mvcc_info (rec, p_mvcc_rec_header, oid_size);
	}
    }
  else
    {
      assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
      assert (mvcc_Enabled == true
	      || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));
      btree_insert_oid_in_front_of_ovfl_vpid (rec, oid, cls_oid,
					      BTREE_IS_UNIQUE (btid->
							       unique_pk),
					      first_ovfl_vpid,
					      p_mvcc_rec_header);
    }

  RANDOM_EXIT (thread_p);

  assert (rec->length % 4 == 0);
  assert (slot_id > 0);

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, rec, BTREE_LEAF_NODE, key);
#endif

  if (spage_update (thread_p, leaf_page, slot_id, rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      log_append_redo_crumbs (thread_p, rcvindex, &addr, n_redo_crumbs,
			      redo_crumbs);
    }

  RANDOM_EXIT (thread_p);

  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_append_overflow_oids_page () -
 *   return:
 *   btid(in): B+tree index identifier
 *   ovfl_page(in): Leaf page pointer to which the key is to be inserted
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   p_mvcc_rec_header(in): MVCC tree info
 *   skip_overflow_undo(in): it true, skip undo logging
 */
static int
btree_append_overflow_oids_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				 PAGE_PTR leaf_page, DB_VALUE * key,
				 OID * cls_oid, OID * oid, INT16 slot_id,
				 RECDES * leaf_rec, VPID * near_vpid,
				 VPID * first_ovfl_vpid,
				 MVCC_REC_HEADER * p_mvcc_rec_header,
				 bool skip_overflow_undo)
{
#define BTREE_APPEND_OVF_OIDS_PAGE_REDO_CRUMBS_MAX 3
#define BTREE_APPEND_OVF_OIDS_PAGE_UNDO_CRUMBS_MAX 1
  int ret = NO_ERROR;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  RECINS_STRUCT recins;
  VPID ovfl_vpid;
  PAGE_PTR ovfl_page = NULL;
  MVCC_REC_HEADER local_mvcc_header;
  LOG_RCVINDEX rcvindex;
  LOG_DATA_ADDR addr;
  LOG_CRUMB redo_crumbs[BTREE_APPEND_OVF_OIDS_PAGE_REDO_CRUMBS_MAX];
  LOG_CRUMB undo_crumbs[BTREE_APPEND_OVF_OIDS_PAGE_UNDO_CRUMBS_MAX];
  int n_redo_crumbs = 0, n_undo_crumbs = 0;
#if !defined (NDEBUG)
  char domain_buf[BTID_DOMAIN_BUFFER_SIZE], *domain_ptr = NULL;
  int domain_size = or_packed_domain_size (btid->key_type, 0);
#endif

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  if (mvcc_Enabled == true)
    {
      if (p_mvcc_rec_header == NULL)
	{
	  local_mvcc_header.mvcc_flag = 0;
	  p_mvcc_rec_header = &local_mvcc_header;
	}

      /* MVCCID has fixed size in overflow page */
      BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_rec_header);
    }

  ret = btree_start_overflow_page (thread_p, btid, &ovfl_vpid, &ovfl_page,
				   near_vpid, oid, cls_oid, first_ovfl_vpid,
				   p_mvcc_rec_header);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* notification */
  BTREE_SET_CREATED_OVERFLOW_PAGE_NOTIFICATION (thread_p, key, oid, cls_oid,
						btid->sys_btid);

  /* log the changes to the leaf node record for redo purposes */
  addr.offset = slot_id;
  addr.pgptr = leaf_page;
  addr.vfid = &btid->sys_btid->vfid;

  recins.flags = 0;

  BTREE_INSERT_RCV_SET_RECORD_REGULAR (&recins);
  recins.ovfl_vpid = ovfl_vpid;
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_OVFL_CHANGED);
  OID_SET_NULL (&recins.oid);
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_UNIQUE);
      COPY_OID (&recins.class_oid, cls_oid);
    }
  else
    {
      OID_SET_NULL (&recins.class_oid);
    }

  if (VPID_ISNULL (first_ovfl_vpid))
    {
      BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_NEW_OVFLPG);
    }

  BTREE_INSERT_RCV_SET_INSMODE_DEFAULT (&recins);

#if !defined (NDEBUG)
  if (domain_size > BTID_DOMAIN_BUFFER_SIZE)
    {
      domain_ptr = (char *) db_private_alloc (thread_p, domain_size);
      if (domain_ptr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, domain_size);
	  goto exit_on_error;
	}
    }
  else
    {
      domain_ptr = domain_buf;
    }
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_KEY_DOMAIN);
#endif

  /* Create redo crumbs */
  redo_crumbs[n_redo_crumbs].length = sizeof (recins);
  redo_crumbs[n_redo_crumbs++].data = &recins;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      /* Add top class OID */
      redo_crumbs[n_redo_crumbs].length = sizeof (btid->topclass_oid);
      redo_crumbs[n_redo_crumbs++].data = &btid->topclass_oid;
    }

#if !defined (NDEBUG)
  /* Add key domain */
  (void) or_pack_domain (domain_ptr, btid->key_type, 0, 0);
  redo_crumbs[n_redo_crumbs].length = domain_size;
  redo_crumbs[n_redo_crumbs++].data = domain_ptr;
#endif

  assert (n_redo_crumbs <= BTREE_APPEND_OVF_OIDS_PAGE_REDO_CRUMBS_MAX);

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, leaf_rec,
				 BTREE_LEAF_NODE);
      if (skip_overflow_undo == false)
	{
	  log_append_undo_data (thread_p, RVBT_NDRECORD_UPD, &addr,
				rv_data_len, rv_data);
	}
    }
  else if (skip_overflow_undo)
    {
      if (p_mvcc_rec_header != NULL
	  && (MVCC_GET_INSID (p_mvcc_rec_header) != MVCCID_ALL_VISIBLE))
	{
	  rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
	}
      else
	{
	  rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
	}
      log_append_redo_crumbs (thread_p, rcvindex, &addr, n_redo_crumbs,
			      redo_crumbs);
    }
  else
    {
      if (p_mvcc_rec_header != NULL
	  && MVCC_IS_HEADER_INSID_NOT_ALL_VISIBLE (p_mvcc_rec_header))
	{
	  /* This is an MVCC operation and must be logged accordingly to be
	   * processed by vacuum.
	   * If insert MVCCID is "all visible", vacuum is no longer required.
	   */
	  MVCC_BTREE_OP_ARGUMENTS mvcc_args;
	  mvcc_args.purpose = MVCC_BTREE_INSERT_OBJECT;
	  mvcc_args.insert_mvccid = MVCC_GET_INSID (p_mvcc_rec_header);
	  ret =
	    btree_rv_save_keyval (btid, key, cls_oid, oid, &mvcc_args,
				  &rv_key, &rv_key_len);
	  rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
	}
      else
	{
	  ret =
	    btree_rv_save_keyval (btid, key, cls_oid, oid, NULL, &rv_key,
				  &rv_key_len);
	  rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
	}
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* Create undo crumbs */
      undo_crumbs[n_undo_crumbs].length = rv_key_len;
      undo_crumbs[n_undo_crumbs++].data = rv_key;

      assert (n_undo_crumbs <= BTREE_APPEND_OVF_OIDS_PAGE_UNDO_CRUMBS_MAX);

      log_append_undoredo_crumbs (thread_p, rcvindex, &addr,
				  n_undo_crumbs, n_redo_crumbs,
				  undo_crumbs, redo_crumbs);
    }

  if (VPID_ISNULL (first_ovfl_vpid))
    {
      btree_leaf_new_overflow_oids_vpid (leaf_rec, &ovfl_vpid,
					 BTREE_IS_UNIQUE (btid->unique_pk),
					 cls_oid);
    }
  else
    {
      btree_leaf_update_overflow_oids_vpid (leaf_rec, &ovfl_vpid);
    }

  assert (leaf_rec->length % 4 == 0);
  assert (slot_id > 0);

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, leaf_rec, BTREE_LEAF_NODE, key);
#endif

  if (spage_update (thread_p, leaf_page, slot_id, leaf_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      /* TODO: Investigate if an MVCC log record type must be used for
       *       new files too.
       */
      log_append_redo_crumbs (thread_p, RVBT_KEYVAL_INS_LFRECORD_OIDINS,
			      &addr, n_redo_crumbs, redo_crumbs);
    }

  pgbuf_set_dirty (thread_p, ovfl_page, DONT_FREE);
  pgbuf_set_dirty (thread_p, leaf_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  if (ovfl_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_insert_oid_overflow_page () -
 *   return:
 *   btid(in): B+tree index identifier
 *   ovfl_page(in): Leaf page pointer to which the key is to be inserted
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   p_mvcc_rec_header(in): MVCC tree info
 *   skip_overflow_undo(in): it true, skip undo logging
 */
static int
btree_insert_oid_overflow_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				PAGE_PTR ovfl_page, DB_VALUE * key,
				OID * cls_oid, OID * oid,
				MVCC_REC_HEADER * p_mvcc_rec_header,
				bool skip_overflow_undo)
{
#define BTREE_INSERT_OID_OVF_REDO_CRUMBS_MAX 3
#define BTREE_INSERT_OID_OVF_UNDO_CRUMBS_MAX 1
  int ret = NO_ERROR;
  RECDES ovfl_rec;
  char ovfl_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  RECINS_STRUCT recins;
  MVCC_REC_HEADER local_mvcc_header;
  LOG_RCVINDEX rcvindex;
  LOG_DATA_ADDR addr;
  LOG_CRUMB redo_crumbs[BTREE_INSERT_OID_OVF_REDO_CRUMBS_MAX];
  LOG_CRUMB undo_crumbs[BTREE_INSERT_OID_OVF_UNDO_CRUMBS_MAX];
  int n_redo_crumbs = 0, n_undo_crumbs = 0;
  MVCC_BTREE_OP_ARGUMENTS mvcc_args, *mvcc_args_p = NULL;
#if !defined (NDEBUG)
  char domain_buf[BTID_DOMAIN_BUFFER_SIZE], *domain_ptr = NULL;
  int domain_size = or_packed_domain_size (btid->key_type, 0);
#endif

  assert (mvcc_Enabled == true || !BTREE_IS_UNIQUE (btid->unique_pk));
  assert (mvcc_Enabled == true || !BTREE_IS_PRIMARY_KEY (btid->unique_pk));

  ovfl_rec.type = REC_HOME;
  ovfl_rec.area_size = DB_PAGESIZE;
  ovfl_rec.data = PTR_ALIGN (ovfl_rec_buf, BTREE_MAX_ALIGN);

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  if (spage_get_record (ovfl_page, 1, &ovfl_rec, COPY) != S_SUCCESS)
    {
      goto exit_on_error;
    }

  assert (ovfl_rec.length >= OR_OID_SIZE);
  assert (ovfl_rec.length % 4 == 0);

  /* log the new node record for redo purposes */
  addr.offset = 1;
  addr.pgptr = ovfl_page;
  addr.vfid = &btid->sys_btid->vfid;

  recins.flags = 0;

  BTREE_INSERT_RCV_SET_RECORD_OVERFLOW (&recins);
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_OID_INSERTED);
  recins.oid = *oid;
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_UNIQUE);
      COPY_OID (&recins.class_oid, cls_oid);
    }
  else
    {
      OID_SET_NULL (&recins.class_oid);
    }
  VPID_SET_NULL (&recins.ovfl_vpid);

  BTREE_INSERT_RCV_SET_INSMODE_DEFAULT (&recins);

  if (mvcc_Enabled == true)
    {
      if (p_mvcc_rec_header == NULL)
	{
	  local_mvcc_header.mvcc_flag = 0;
	  p_mvcc_rec_header = &local_mvcc_header;
	}

      /* MVCCID has fixed size in overflow page */
      BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_rec_header);
    }

  rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
  if (p_mvcc_rec_header != NULL
      && MVCC_IS_HEADER_INSID_NOT_ALL_VISIBLE (p_mvcc_rec_header))
    {
      mvcc_args_p = &mvcc_args;
      mvcc_args_p->purpose = MVCC_BTREE_INSERT_OBJECT;
      mvcc_args_p->insert_mvccid = MVCC_GET_INSID (p_mvcc_rec_header);
      /* Set to MVCC log record type to log MVCCID */
      rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
    }

#if !defined (NDEBUG)
  if (domain_size > BTID_DOMAIN_BUFFER_SIZE)
    {
      domain_ptr = (char *) db_private_alloc (thread_p, domain_size);
      if (domain_ptr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, domain_size);
	  goto exit_on_error;
	}
    }
  else
    {
      domain_ptr = domain_buf;
    }
  BTREE_INSERT_RCV_SET_FLAGS (&recins, BTREE_INSERT_RCV_FLAG_KEY_DOMAIN);
#endif

  /* Create redo crumbs */
  redo_crumbs[n_redo_crumbs].length = sizeof (recins);
  redo_crumbs[n_redo_crumbs++].data = &recins;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      /* Add top class OID */
      redo_crumbs[n_redo_crumbs].length = sizeof (btid->topclass_oid);
      redo_crumbs[n_redo_crumbs++].data = &btid->topclass_oid;
    }

#if !defined (NDEBUG)
  /* Add key domain */
  (void) or_pack_domain (domain_ptr, btid->key_type, 0, 0);
  redo_crumbs[n_redo_crumbs].length = domain_size;
  redo_crumbs[n_redo_crumbs++].data = domain_ptr;
#endif

  assert (n_redo_crumbs <= BTREE_INSERT_OID_OVF_REDO_CRUMBS_MAX);

  if (btree_is_new_file (btid))
    {
      btree_rv_write_log_record (rv_data, &rv_data_len, &ovfl_rec,
				 BTREE_LEAF_NODE);
      if (skip_overflow_undo == false)
	{
	  log_append_undo_data (thread_p, RVBT_NDRECORD_UPD, &addr,
				rv_data_len, rv_data);
	}
    }
  else if (skip_overflow_undo)
    {
      if (p_mvcc_rec_header != NULL
	  && (MVCC_GET_INSID (p_mvcc_rec_header) != MVCCID_ALL_VISIBLE))
	{
	  rcvindex = RVBT_KEYVAL_MVCC_INS_LFRECORD_OIDINS;
	}
      else
	{
	  rcvindex = RVBT_KEYVAL_INS_LFRECORD_OIDINS;
	}
      log_append_redo_crumbs (thread_p, rcvindex, &addr, n_redo_crumbs,
			      redo_crumbs);
    }
  else
    {
      ret =
	btree_rv_save_keyval (btid, key, cls_oid, oid, mvcc_args_p, &rv_key,
			      &rv_key_len);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* Create undo crumbs */
      undo_crumbs[n_undo_crumbs].length = rv_key_len;
      undo_crumbs[n_undo_crumbs++].data = rv_key;

      assert (n_undo_crumbs <= BTREE_INSERT_OID_OVF_UNDO_CRUMBS_MAX);

      log_append_undoredo_crumbs (thread_p, rcvindex, &addr,
				  n_undo_crumbs, n_redo_crumbs,
				  undo_crumbs, redo_crumbs);
    }

  if (btree_insert_oid_with_order (&ovfl_rec, oid, cls_oid,
				   BTREE_IS_UNIQUE (btid->unique_pk),
				   p_mvcc_rec_header) != NO_ERROR)
    {
      goto exit_on_error;
    }

  assert (ovfl_rec.length % 4 == 0);

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, &ovfl_rec, BTREE_OVERFLOW_NODE,
			    key);
#endif

  if (spage_update (thread_p, ovfl_page, 1, &ovfl_rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      /* TODO: Investigate if an MVCC log record type must be used for
       *       new files too.
       */
      log_append_redo_crumbs (thread_p, rcvindex, &addr, n_redo_crumbs,
			      redo_crumbs);
    }

  pgbuf_set_dirty (thread_p, ovfl_page, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_insert_into_leaf () -
 *   return: NO_ERROR
 *   key_added_deleted(out): true if the key was logical deleted or inserted,
 *			     false otherwise.
 *   btid(in): B+tree index identifier
 *   page_ptr(in): Leaf page pointer to which the key is to be inserted
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   nearp_vpid(in): Near page identifier that may be used in allocating a new
 *                   overflow page. (Note: it may be ignored.)
 *   op_type(in):
 *   key_found(in):
 *   slot_id(in):
 *   p_mvcc_rec_header(in/out): MVCC record header
 *
 * Note: Insert the given < key, oid > pair into the leaf page
 * specified. If the key is a new one, it assumes that there is
 * enough space in the page to make insertion, otherwise an
 * error condition is raised. If the key is an existing one,
 * inserting "oid" may necessitate the use of overflow pages.
 *
 * LOGGING Note: When the btree is new, splits and merges will
 * not be committed, but will be attached.  If the transaction
 * is rolled back, the merge and split actions will be rolled
 * back as well.  The undo (and redo) logging for splits and
 * merges are page based (physical) logs, thus the rest of the
 * logs for the undo session must be page based as well.  When
 * the btree is old, splits and merges are committed and all
 * the rest of the logging must be logical (non page based)
 * since pages may change as splits and merges are performed.
 *
 * LOGGING Note2: We adopts a new concept of log, that is a combined log of
 * logical undo and physical redo log, for performance reasons.
 * For key insert, this will be written only when the btree is old and
 * the given key is not an overflow-key.
 * However each undo log and redo log will be written as it is in the rest of
 * the cases(need future work).
 * Condition:
 *     When the b-tree is old and the given key is not overflow-key
 * Algorithm:
 *     if (key_does_not_exist) {
 *         logical_undo_physical_redo(); // CASE-A
 *         spage_insert_at(); // insert <key,oid>
 *     } else { // key_already_exists
 *         if (no_overflow_page) {
 *             if (enough_space_in_page) {
 *                 logical_undo_physical_redo();  // CASE-B-1
 *                 spage_update(); // append oid
 *             } else { // needs an overflow page
 *                 btree_start_overflow_page();
 *                 logical_undo_physical_redo(); // CASE-B-2
 *                 spage_update(); // append oid
 *             }
 *         } else { // overflow page exists
 *             do {
 *                 // find the last overflow page
 *             } while ();
 *             if (enough_space_in_last_overflow_page) {
 *                 logical_undo_physical_redo();  // CASE-C-1
 *                 spage_update(); // append oid
 *             } else { // needs a new overflow page
 *                 btree_start_overflow_page();
 *                 logical_undo_physical_redo(); // CASE-C-2
 *                 change BTREE_OVERFLOW_HEADER.next_vpid // link overflow
 *             }
 *         }
 *    }
 */
static int
btree_insert_into_leaf (THREAD_ENTRY * thread_p, int *key_added_deleted,
			BTID_INT * btid, PAGE_PTR page_ptr, DB_VALUE * key,
			OID * cls_oid, OID * oid, VPID * nearp_vpid,
			int op_type, bool key_found, INT16 slot_id,
			MVCC_REC_HEADER * p_mvcc_rec_header)
{
  RECDES rec;
  LEAF_REC leafrec_pnt;
  int oid_size;
  bool dummy;
  int max_free, oid_length_in_rec;
  int offset, key_len, key_len_in_page;
  int ret = NO_ERROR;
  PAGE_PTR ovfl_page, last_ovfl_page = NULL;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  MVCC_REC_HEADER last_oid_mvcc_header, *p_mvcc_ovf_rec_header = NULL;
  int num_oids;
  bool swap_last_oid_to_overflow = false, delete_swapped_object = false;
  OID last_oid, last_class_oid;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;
  int mvccid_size = 0;
  INT16 insoid_mode = BTREE_INSERT_OID_MODE_DEFAULT;
#if 0
  OID saved_first_class_oid, saved_first_oid;
  MVCC_REC_HEADER saved_first_oid_mvcc_header;
#endif

  mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;
  key_len = btree_get_key_length (key);
  key_len_in_page = BTREE_GET_KEY_LEN_IN_PAGE (key_len);

#if !defined(NDEBUG)
  if (!key || DB_IS_NULL (key))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_NULL_KEY, 0);
      return ER_BTREE_NULL_KEY;
    }
#endif

  /* In an unique index, each OID information in leaf entry
   * is composed of <instance OID, class OID>.
   * In a non-unique index, each OID information in leaf entry
   * is composed of <instance OID>.
   */
  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      /* <inst OID, class OID> */
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      /* <inst OID only> */
      oid_size = OR_OID_SIZE;
    }

  if (slot_id == NULL_SLOTID)
    {
      key_found = btree_search_leaf_page (thread_p, btid,
					  page_ptr, key, &slot_id);
      if (slot_id == NULL_SLOTID)
	{
	  return (ret = er_errid ()) == NO_ERROR ? ER_FAILED : ret;
	}
    }

  if (!key_found)
    {
      /* key does not exist */
      if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
	{
	  /* Couldn't find the object we are trying to delete! */
	  /* TODO: Find a more appropriate error message */
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  assert (false);
	  return ER_FAILED;
	}
      *key_added_deleted = 1;
      ret = btree_insert_oid_with_new_key (thread_p, btid, page_ptr, key,
					   cls_oid, oid, slot_id,
					   p_mvcc_rec_header);
#if !defined(NDEBUG)
      if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) &
	  BTREE_DEBUG_DUMP_SIMPLE)
	{
	  VPID *vpid = pgbuf_get_vpid_ptr (page_ptr);
	  fprintf (stdout, "btree insert at (%d:%d:%d) with key:",
		   vpid->volid, vpid->pageid, slot_id);
	  db_value_print (key);
	  fprintf (stdout, "\n");
	}
#endif

#if !defined(NDEBUG)
      btree_verify_node (thread_p, btid, page_ptr);
#endif

      return ret;
    }

  VPID_SET_NULL (&leafrec_pnt.ovfl);
  leafrec_pnt.key_len = -1;

  rec.type = REC_HOME;
  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  /* read the record that contains the key */
  assert (slot_id > 0);
  if (spage_get_record (page_ptr, slot_id, &rec, COPY) != S_SUCCESS)
    {
      return (ret = er_errid ()) == NO_ERROR ? ER_FAILED : ret;
    }
  assert (rec.length % 4 == 0);

  btree_read_record (thread_p, btid, page_ptr, &rec, NULL, &leafrec_pnt,
		     BTREE_LEAF_NODE, &dummy, &offset, PEEK_KEY_VALUE, NULL);

  if (BTREE_IS_UNIQUE (btid->unique_pk)
      && !BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
    {
      if (mvcc_Enabled == true)
	{
	  num_oids =
	    btree_leaf_mvcc_get_num_visible_oids (thread_p, btid, &rec,
						  offset, BTREE_LEAF_NODE,
						  NULL, &mvcc_snapshot_dirty);

	  if (num_oids < 0)
	    {
	      /* Error */
	      return num_oids;
	    }

	  if (num_oids <= 1 && !VPID_ISNULL (&leafrec_pnt.ovfl))
	    {
	      int num_ovf_oids;
	      if (btree_mvcc_get_num_visible_oids_from_all_ovf
		  (thread_p, btid, &(leafrec_pnt.ovfl), &num_ovf_oids, NULL,
		   &mvcc_snapshot_dirty) != NO_ERROR)
		{
		  goto error;
		}

	      num_oids += num_ovf_oids;
	    }
	  if (num_oids == 0)
	    {
	      *key_added_deleted = 1;
	    }
	}
      else
	{
	  num_oids = btree_leaf_get_num_oids (&rec, offset, BTREE_LEAF_NODE,
					      oid_size);
	}

      if (num_oids == 0)
	{
	  /* need to insert into OID list */
	}
      else if (BTREE_NEED_UNIQUE_CHECK (thread_p, op_type) || num_oids >= 2)
	{
	  if (prm_get_bool_value (PRM_ID_UNIQUE_ERROR_KEY_VALUE))
	    {
	      char *keyval = pr_valstring (key);

	      ret = ER_UNIQUE_VIOLATION_WITHKEY;
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1,
		      (keyval == NULL) ? "(null)" : keyval);
	      if (keyval)
		{
		  free_and_init (keyval);
		}
	    }
	  else
	    {
	      BTREE_SET_UNIQUE_VIOLATION_ERROR (thread_p, key, oid, cls_oid,
						btid->sys_btid, NULL);
	      ret = ER_BTREE_UNIQUE_FAILED;
	    }

	  return ret;
	}
      else if (op_type == MULTI_ROW_UPDATE
	       && prm_get_integer_value (PRM_ID_HA_MODE) != HA_MODE_OFF)
	{
	  ret = ER_REPL_MULTI_UPDATE_UNIQUE_VIOLATION;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 0);
	  return ret;
	}
    }

  /* TO DO - adapt btree_check_duplicate_oid to MVCC - disable for now */
  if (log_is_in_crash_recovery () && mvcc_Enabled == false)
    {
      ret = btree_check_duplicate_oid (thread_p, btid, page_ptr, slot_id,
				       &rec, offset, oid, &leafrec_pnt.ovfl);
      if (ret != NO_ERROR)
	{
	  return ret;
	}
    }

  /* get the free space size in page */
  max_free = spage_max_space_for_new_record (thread_p, page_ptr);

  oid_length_in_rec = oid_size + rec.length - offset;

  if (mvcc_Enabled)
    {
      mvccid_size = btree_packed_mvccinfo_size (p_mvcc_rec_header);

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  /* need to add OR_OID_SIZE since the first OID may be moved to the
	   * end of key buffer - fixed size
	   */
	  mvccid_size += OR_OID_SIZE;
	}

      /* add size of MVCCIDs associated with first OID */
      if (btree_leaf_key_oid_is_mvcc_flaged (rec.data,
					     BTREE_LEAF_OID_HAS_MVCC_INSID))
	{
	  oid_length_in_rec += OR_MVCCID_SIZE;
	}
      else
	{
	  /* first OID may be moved at the end of key buffer - fixed size */
	  if (BTREE_IS_UNIQUE (btid->unique_pk))
	    {
	      mvccid_size += OR_MVCCID_SIZE;
	    }
	}

      if (btree_leaf_key_oid_is_mvcc_flaged (rec.data,
					     BTREE_LEAF_OID_HAS_MVCC_DELID))
	{
	  oid_length_in_rec += OR_MVCCID_SIZE;
	}
      else
	{
	  if (BTREE_IS_UNIQUE (btid->unique_pk))
	    {
	      /* first OID may be moved at the end of key buffer - fixed size */
	      mvccid_size += OR_MVCCID_SIZE;
	    }
	}
    }

#if !defined(NDEBUG)
  if (p_mvcc_rec_header != NULL)
    {
      /* MVCC insertion, MVCC logical deletion or MVCC visible for all */
      assert (!(p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)
	      || !(p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_DELID));
    }
#endif

  if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
    {
      /* if want to insert only del_id, find the leaf page where associated OID
       * reside
       */
      /* the current b-tree insertion is in fact a logical deletion */
      int oid_offset;
      int last_oid_offset = 0;

      assert (mvcc_Enabled == true);
      oid_offset = btree_find_oid_from_leaf (btid, &rec, offset, oid, NULL,
					     NULL);

      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  num_oids =
	    btree_leaf_mvcc_get_num_visible_oids (thread_p, btid, &rec,
						  offset, BTREE_LEAF_NODE,
						  NULL, &mvcc_snapshot_dirty);

	  if (!num_oids || (num_oids == 1 && oid_offset != NOT_FOUND))
	    {
	      if (VPID_ISNULL (&leafrec_pnt.ovfl))
		{
		  *key_added_deleted = 1;
		}
	      else
		{
		  if (btree_mvcc_get_num_visible_oids_from_all_ovf
		      (thread_p, btid, &(leafrec_pnt.ovfl), &num_oids,
		       NULL, &mvcc_snapshot_dirty) != NO_ERROR)
		    {
		      goto error;
		    }
		  if (!num_oids)
		    {
		      *key_added_deleted = 1;
		    }
		}
	    }
	}

      if (oid_offset != NOT_FOUND)
	{
	  MVCC_BTREE_OP_ARGUMENTS mvcc_args_for_delete;
	  if (btree_leaf_key_oid_is_mvcc_flaged (rec.data + oid_offset,
						 BTREE_LEAF_OID_HAS_MVCC_DELID)
	      || (max_free > OR_MVCCID_SIZE
		  && oid_length_in_rec < BTREE_MAX_OIDLEN_INPAGE)
	      || (BTREE_IS_UNIQUE (btid->unique_pk) && offset > 0))
	    {
	      /* MVCC del_id can be added without relocation */
	      ret =
		btree_insert_mvcc_delid_into_page (thread_p, btid,
						   page_ptr,
						   BTREE_LEAF_NODE,
						   key,
						   cls_oid, oid,
						   slot_id, &rec,
						   oid_offset,
						   p_mvcc_rec_header);
#if !defined(NDEBUG)
	      btree_check_valid_record (thread_p, btid, &rec, BTREE_LEAF_NODE,
					key);
	      btree_verify_node (thread_p, btid, page_ptr);
#endif
	      return ret;
	    }

	  /* We are here because we don't have enough space to add delete
	   * MVCCID into leaf record. We will move the last object into
	   * an overflow page. For now just remove last object from leaf
	   * record.
	   */
	  /* need to move from leaf to overflow page */
	  assert (btree_leaf_get_num_oids (&rec, offset, BTREE_LEAF_NODE,
					   oid_size) >= 2);

	  /* Get last object from leaf */
	  btree_leaf_get_last_oid (btid, &rec, BTREE_LEAF_NODE, offset,
				   &last_oid, &last_class_oid,
				   &last_oid_mvcc_header, &last_oid_offset);

	  /* Physically remove last object for relocation. In this case only
	   * redo logging will be appended.
	   * TODO: What happens if an error occurs after deleting the object
	   *       and before inserting into overflow?
	   */
	  mvcc_args_for_delete.purpose =
	    MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO;
	  mvcc_args_for_delete.insert_mvccid =
	    MVCC_GET_INSID (&last_oid_mvcc_header);
	  mvcc_args_for_delete.delete_mvccid =
	    MVCC_GET_DELID (&last_oid_mvcc_header);
	  ret =
	    btree_delete_oid_from_leaf (thread_p, btid, page_ptr, slot_id,
					key, &last_oid, &last_class_oid, &rec,
					last_oid_offset, &offset,
					&mvcc_args_for_delete);
	  if (ret != NO_ERROR)
	    {
	      goto error;
	    }
#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &rec, BTREE_LEAF_NODE,
				    key);
#endif

	  /* Now we can have two cases:
	   * 1. The last object was actually the one we required to be
	   *    deleted. We will have to first move it to overflow page, then
	   *    add delete MVCCID (so it will be handled later).
	   * 2. The last object was not the one we required to delete. We can
	   *    now delete our object. This case can be handled here.
	   */
	  if (OID_EQ (oid, &last_oid))
	    {
	      delete_swapped_object = true;
	    }
	  else
	    {
	      /* Since have enough space, we can insert delid */
	      ret =
		btree_insert_mvcc_delid_into_page (thread_p, btid, page_ptr,
						   BTREE_LEAF_NODE, key,
						   cls_oid, oid, slot_id,
						   &rec, oid_offset,
						   p_mvcc_rec_header);
	      if (ret != NO_ERROR)
		{
		  goto error;
		}
#if !defined (NDEBUG)
	      btree_check_valid_record (thread_p, btid, &rec, BTREE_LEAF_NODE,
					key);
#endif
	      /* prepare for last oid insertion in overflow page */
	      oid = &last_oid;
	      cls_oid = &last_class_oid;
	    }

	  /* Get header for moving to overflow */
	  p_mvcc_ovf_rec_header = &last_oid_mvcc_header;

	  /* Must announce the insert into overflow that this is actually an
	   * object swapping. Undo logging will be skipped.
	   */
	  swap_last_oid_to_overflow = true;

	  /* prepare for insertion in overflow */
	  BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_ovf_rec_header);
	}
      else
	{
	  RECDES ovfl_copy_rec;
	  char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
	  /* not found in leaf page - search in overflow page */
	  ovfl_page = NULL;

	  ovfl_copy_rec.area_size = DB_PAGESIZE;
	  ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);

	  ret =
	    btree_find_overflow_page (thread_p, oid, &leafrec_pnt.ovfl,
				      &ovfl_page, &ovfl_copy_rec,
				      &oid_offset, oid_size);
	  if (ret != NO_ERROR)
	    {
	      return ret;
	    }

	  if (oid_offset == NOT_FOUND)
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE,
		      ER_BTREE_UNKNOWN_OID, 3, oid->volid, oid->pageid,
		      oid->slotid);
	      return ER_BTREE_UNKNOWN_OID;
	    }

	  /* MVCCID has fixed size, so no need to check space */
	  ret = btree_insert_mvcc_delid_into_page (thread_p, btid,
						   ovfl_page,
						   BTREE_OVERFLOW_NODE, key,
						   cls_oid, oid, 1,
						   &ovfl_copy_rec, oid_offset,
						   p_mvcc_rec_header);

	  pgbuf_unfix_and_init (thread_p, ovfl_page);

#if !defined(NDEBUG)
	  btree_verify_node (thread_p, btid, page_ptr);
	  btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				    BTREE_OVERFLOW_NODE, key);
#endif
	  return ret;
	}
    }
  else
    {
      if (mvcc_Enabled && BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  /* insert in first position */
	  insoid_mode = BTREE_INSERT_OID_MODE_AT_BEGINNING;

	  /* when insert in position 0, the minimum free space required in
	   * page is OR_OID_SIZE + 2 * OR_MVCCID_SIZE + DISK_VPID_SIZE.
	   * Need this free space when first OID is moved into overflow OIDs
	   * page and newly inserted object has fixed size. If there is not
	   * enough free space it means a bug in the algorithm.
	   */
	  if (max_free <
	      OR_OID_SIZE + 2 * OR_MVCCID_SIZE
	      + (int) (DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT)))
	    {
	      char *ptr = NULL;
	      FILE *fp = NULL;
	      size_t sizeloc;

	      fp = port_open_memstream (&ptr, &sizeloc);
	      if (fp)
		{
		  VPID *vpid = pgbuf_get_vpid_ptr (page_ptr);

		  btree_dump_page (thread_p, fp, cls_oid, btid, NULL,
				   page_ptr, vpid, 2, 2);
		  spage_dump (thread_p, fp, page_ptr, true);
		  port_close_memstream (fp, &ptr, &sizeloc);
		}

	      er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
		      ER_BTREE_NO_SPACE, 2, rec.length, ptr);

	      if (ptr)
		{
		  free (ptr);
		}

	      assert_release (false);

	      return ER_BTREE_NO_SPACE;
	    }
	}

      /* insert oid in leaf rec */
      if ((max_free >
	   oid_size + mvccid_size
	   + (int) (DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT)))
	  && (oid_length_in_rec < BTREE_MAX_OIDLEN_INPAGE))
	{
	  ret =
	    btree_insert_oid_into_leaf_rec (thread_p, btid, page_ptr, key,
					    cls_oid, oid, slot_id, &rec,
					    &leafrec_pnt.ovfl,
					    p_mvcc_rec_header, insoid_mode);

#if !defined(NDEBUG)
	  btree_verify_node (thread_p, btid, page_ptr);
#endif

	  return ret;
	}

#if 0
      if (mvcc_Enabled && BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  /* replace first oid in key buffer  */
	  insoid_mode = KEY_INSOID_REPLACE_FIRST_OID;

	  /* save first OID with its class OID and MVCC header before replace */
	  btree_leaf_get_first_oid (btid, &rec, &saved_first_oid,
				    &saved_first_class_oid,
				    &saved_first_oid_mvcc_header);
	  BTREE_MVCC_SET_HEADER_FIXED_SIZE (&saved_first_oid_mvcc_header);

	  ret =
	    btree_insert_oid_into_leaf_rec (thread_p, btid, page_ptr, key,
					    cls_oid, oid, slot_id, &rec,
					    &leafrec_pnt.ovfl,
					    p_mvcc_rec_header, insoid_mode);
	  if (ret != NO_ERROR)
	    {
	      return ret;
	    }

#if !defined(NDEBUG)
	  btree_verify_node (thread_p, btid, page_ptr);
#endif

	  /* prepare replaced OID for insertion in overflow page */
	  oid = &saved_first_oid;
	  cls_oid = &saved_first_class_oid;
	  p_mvcc_rec_header = &saved_first_oid_mvcc_header;
	}
#endif

      /* insert oid in overflow page */
      if (VPID_ISNULL (&leafrec_pnt.ovfl))
	{
	  assert (btree_leaf_get_num_oids (&rec, offset, BTREE_LEAF_NODE,
					   oid_size) >= 2);

	  ret = btree_append_overflow_oids_page (thread_p, btid, page_ptr,
						 key, cls_oid, oid, slot_id,
						 &rec, nearp_vpid,
						 &leafrec_pnt.ovfl,
						 p_mvcc_rec_header,
						 swap_last_oid_to_overflow);

#if !defined(NDEBUG)
	  btree_verify_node (thread_p, btid, page_ptr);
#endif

	  return ret;
	}

      /* prepare for oid insertion in overflow page */
      p_mvcc_ovf_rec_header = p_mvcc_rec_header;
    }

  ovfl_page = btree_find_free_overflow_oids_page (thread_p, btid,
						  &leafrec_pnt.ovfl);

  /* append or insert overflow page */
  if (ovfl_page == NULL)
    {
      ret = btree_append_overflow_oids_page (thread_p, btid, page_ptr,
					     key, cls_oid, oid, slot_id,
					     &rec, nearp_vpid,
					     &leafrec_pnt.ovfl,
					     p_mvcc_ovf_rec_header,
					     swap_last_oid_to_overflow);
    }
  else
    {
      ret = btree_insert_oid_overflow_page (thread_p, btid, ovfl_page,
					    key, cls_oid, oid,
					    p_mvcc_ovf_rec_header,
					    swap_last_oid_to_overflow);
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  if (delete_swapped_object)
    {
      /* Delete was postponed because we had to move the object to an overflow
       * page first. Delete it now.
       * TODO: This was a quick fix for a serious bug. It can be optimized
       *       to do the delete at the same time with the relocation.
       */
      RECDES ovfl_copy_rec;
      char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
      int oid_offset;

      ovfl_page = NULL;
      ovfl_copy_rec.area_size = DB_PAGESIZE;
      ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);

      if (VPID_ISNULL (&leafrec_pnt.ovfl))
	{
	  /* The overflow page was just created */
	  (void) btree_leaf_get_vpid_for_overflow_oids (&rec,
							&leafrec_pnt.ovfl);
	}
      ret =
	btree_find_overflow_page (thread_p, oid, &leafrec_pnt.ovfl,
				  &ovfl_page, &ovfl_copy_rec, &oid_offset,
				  oid_size);
      if (ret != NO_ERROR)
	{
	  return ret;
	}

      if (oid_offset == NOT_FOUND)
	{
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_BTREE_UNKNOWN_OID,
		  3, oid->volid, oid->pageid, oid->slotid);
	  return ER_BTREE_UNKNOWN_OID;
	}

      /* MVCCID has fixed size, so no need to check space */
      ret =
	btree_insert_mvcc_delid_into_page (thread_p, btid, ovfl_page,
					   BTREE_OVERFLOW_NODE, key, cls_oid,
					   oid, 1, &ovfl_copy_rec, oid_offset,
					   p_mvcc_rec_header);
      pgbuf_unfix_and_init (thread_p, ovfl_page);

#if !defined(NDEBUG)
      btree_check_valid_record (thread_p, btid, &ovfl_copy_rec,
				BTREE_OVERFLOW_NODE, key);
#endif
      return ret;
    }

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, page_ptr);
#endif

  return ret;

error:
  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  return ret;
}

/*
 *
 */
static int
btree_rv_write_log_record_for_key_insert (char *log_rec, int *log_length,
					  INT16 key_len, RECDES * recp)
{
  *(INT16 *) ((char *) log_rec + LOFFS1) = key_len;
  *(INT16 *) ((char *) log_rec + LOFFS2) = BTREE_LEAF_NODE;
  *(INT16 *) ((char *) log_rec + LOFFS3) = recp->type;
  memcpy ((char *) log_rec + LOFFS4, recp->data, recp->length);

  *log_length = recp->length + LOFFS4;

  return NO_ERROR;
}

static int
btree_rv_write_log_record (char *log_rec, int *log_length, RECDES * recp,
			   BTREE_NODE_TYPE node_type)
{
  assert (node_type == BTREE_LEAF_NODE || node_type == BTREE_NON_LEAF_NODE);

  *(INT16 *) ((char *) log_rec + OFFS1) = node_type;
  *(INT16 *) ((char *) log_rec + OFFS2) = recp->type;
  memcpy ((char *) log_rec + OFFS3, recp->data, recp->length);

  *log_length = recp->length + OFFS3;

  return NO_ERROR;
}

/*
 * btree_find_free_overflow_oids_page () -
 *   return :
 *  thread_p(in): thread entry
 *  btid(in): B+tree index identifier
 *  first_ovfl_vpid(in): first overflow vpid of leaf record
 */
static PAGE_PTR
btree_find_free_overflow_oids_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    VPID * first_ovfl_vpid)
{
  PAGE_PTR ovfl_page;
  VPID ovfl_vpid;
  int space_needed = OR_OID_SIZE;

  assert (first_ovfl_vpid != NULL);

  if (mvcc_Enabled)
    {
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  space_needed += OR_OID_SIZE;
	}
      space_needed += (2 * OR_MVCCID_SIZE);
    }

  ovfl_vpid = *first_ovfl_vpid;

  while (!VPID_ISNULL (&ovfl_vpid))
    {
      ovfl_page = pgbuf_fix (thread_p, &ovfl_vpid, OLD_PAGE,
			     PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
      if (ovfl_page == NULL)
	{
	  return NULL;
	}

      (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

      if (spage_max_space_for_new_record (thread_p, ovfl_page) > space_needed)
	{
	  return ovfl_page;
	}

      btree_get_next_overflow_vpid (ovfl_page, &ovfl_vpid);

      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return NULL;
}

/*
 * btree_find_free_overflow_oids_page () -
 *   return :
 *
 */
static int
btree_find_overflow_page (THREAD_ENTRY * thread_p,
			  OID * oid, VPID * first_ovfl_vpid,
			  PAGE_PTR * ovfl_page, RECDES * ovfl_copy_rec,
			  int *ovfl_rec_offset, int oid_size)
{
  VPID next_ovfl_vpid;
  int ret = NO_ERROR;

  assert (first_ovfl_vpid != NULL);

  assert (mvcc_Enabled == true && ovfl_page != NULL && ovfl_copy_rec != NULL
	  && ovfl_rec_offset != NULL && oid != NULL);

  *ovfl_page = NULL;
  next_ovfl_vpid = *first_ovfl_vpid;
  /* search for OID into overflow page */
  while (!VPID_ISNULL (&next_ovfl_vpid))
    {
      *ovfl_page = pgbuf_fix (thread_p, &next_ovfl_vpid, OLD_PAGE,
			      PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (*ovfl_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, *ovfl_page, PAGE_BTREE);

      if (spage_get_record (*ovfl_page, 1, ovfl_copy_rec, COPY) != S_SUCCESS)
	{
	  goto error;
	}
      assert (ovfl_copy_rec->length % 4 == 0);

      *ovfl_rec_offset =
	btree_find_oid_from_ovfl (ovfl_copy_rec, oid, oid_size, NULL);
      if (*ovfl_rec_offset != NOT_FOUND)
	{
	  return NO_ERROR;
	}

      btree_get_next_overflow_vpid (*ovfl_page, &next_ovfl_vpid);
      pgbuf_unfix_and_init (thread_p, *ovfl_page);
    }

  return ret;

error:

  if (*ovfl_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, *ovfl_page);
    }

  ret = er_errid ();
  if (ret == NO_ERROR)
    {
      ret = ER_FAILED;
    }

  return ret;
}


/*
 * btree_check_duplicate_oid () -
 *   return :
 *
 */
static int
btree_check_duplicate_oid (THREAD_ENTRY * thread_p, BTID_INT * btid,
			   PAGE_PTR leaf_page, INT16 slot_id,
			   RECDES * leaf_rec_p, int oid_list_offset,
			   OID * oid, VPID * ovfl_vpid)
{
  PAGE_PTR ovfl_page, redo_page;
  INT16 redo_slot_id;
  VPID next_ovfl_vpid;
  RECDES orec;
  int oid_size = OR_OID_SIZE;

  assert (btid != NULL);

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      oid_size += OR_OID_SIZE;
    }

  if (btree_find_oid_from_leaf (btid, leaf_rec_p, oid_list_offset,
				oid, NULL, NULL) != NOT_FOUND)
    {
      redo_page = leaf_page;
      redo_slot_id = slot_id;

      goto redo_log;
    }

  next_ovfl_vpid = *ovfl_vpid;

  while (!VPID_ISNULL (&next_ovfl_vpid))
    {
      ovfl_page = pgbuf_fix (thread_p, &next_ovfl_vpid, OLD_PAGE,
			     PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (ovfl_page == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  return er_errid ();
	}

      (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

      (void) spage_get_record (ovfl_page, 1, &orec, PEEK);
      assert (orec.length % 4 == 0);

      if (btree_find_oid_from_ovfl (&orec, oid, oid_size, NULL) != NOT_FOUND)
	{
	  redo_page = ovfl_page;
	  redo_slot_id = 1;

	  goto redo_log;
	}

      btree_get_next_overflow_vpid (ovfl_page, &next_ovfl_vpid);

      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return NO_ERROR;

redo_log:

  /* put a NOOP redo log here, which does NOTHING, this is used
   * to accompany the corresponding logical undo log, if there is
   * any, which caused this routine to be called.
   */
  log_append_redo_data2 (thread_p, RVBT_NOOP, &btid->sys_btid->vfid,
			 redo_page, redo_slot_id, 0, NULL);
  pgbuf_set_dirty (thread_p, redo_page, DONT_FREE);

  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_BTREE_DUPLICATE_OID,
	  3, oid->volid, oid->pageid, oid->slotid);

  if (redo_page != leaf_page)
    {
      pgbuf_unfix_and_init (thread_p, redo_page);
    }

  return ER_BTREE_DUPLICATE_OID;
}

/*
 * btree_find_oid_from_leaf () -
 *   return : offset of oid or NOT_FOUND
 */
static int
btree_find_oid_from_leaf (BTID_INT * btid, RECDES * rec_p,
			  int oid_list_offset, OID * oid,
			  MVCC_BTREE_OP_ARGUMENTS * mvcc_args,
			  MVCC_REC_HEADER * p_mvcc_rec_header)
{
  OR_BUF buf;
  char *save_buf_ptr = NULL;
  OID inst_oid, class_oid;
  int vpid_size = 0, error_code = NO_ERROR;
  short mvcc_flags;
  bool check_insid = MVCC_BTREE_DELETE_VACUUM_INSID (mvcc_args);
  bool check_delid =
    MVCC_BTREE_DELETE_REMOVE_DELID (mvcc_args)
    || MVCC_BTREE_DELETE_VACUUM_OBJECT (mvcc_args);
  bool first_object = true;
  MVCC_REC_HEADER mvcc_header;

  assert (btid != NULL);
  assert (oid_list_offset != 0);

  if (btree_leaf_is_flaged (rec_p, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      /* Record ends with the VPID of first overflow page */
      vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
    }

  or_init (&buf, rec_p->data, rec_p->length - vpid_size);
  while (buf.ptr < buf.endptr)
    {
      /* If the object has fixed size, it is forced to have both insert MVCCID
       * and delete MVCCID. This can happen if:
       * 1. The index is unique, and this is not the key's first object.
       * 2. The keys has overflow OID's and this is the first object.
       * In any other cases follow the MVCC flags.
       */
      save_buf_ptr = buf.ptr;
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);

      /* Get OID */
      if (or_get_oid (&buf, &inst_oid) != NO_ERROR)
	{
	  assert (false);
	  return NOT_FOUND;
	}

      if (BTREE_IS_UNIQUE (btid->unique_pk)
	  && (!first_object
	      || btree_leaf_is_flaged (rec_p, BTREE_LEAF_RECORD_SUBCLASS)))
	{
	  /* Class OID may also be saved when b-tree is unique. Class OID is
	   * saved if:
	   * 1. This is the first entry and the object is flagged with
	   *    BTREE_LEAF_RECORD_SUBCLASS flag.
	   * 2. All other entries have class OID's saved too.
	   */
	  /* Get class OID */
	  if (or_get_oid (&buf, &class_oid) != NO_ERROR)
	    {
	      assert (false);
	      return NOT_FOUND;
	    }
	}

      BTREE_CLEAR_MVCC_FLAGS_FROM_OID (&inst_oid);
      BTREE_CLEAR_RECORD_FLAGS_FROM_OID (&inst_oid);

      if (mvcc_Enabled)
	{
	  if (btree_or_get_mvccinfo (&buf, &mvcc_header, mvcc_flags)
	      != NO_ERROR)
	    {
	      assert (false);
	      return NOT_FOUND;
	    }
	}

      if (OID_EQ (&inst_oid, oid)
	  && (!check_insid
	      || (mvcc_args->insert_mvccid == MVCC_GET_INSID (&mvcc_header)))
	  && (!check_delid
	      || (mvcc_args->delete_mvccid == MVCC_GET_DELID (&mvcc_header))))
	{
	  /* Found object */
	  if (mvcc_args != NULL
	      && mvcc_args->purpose == MVCC_BTREE_DELETE_OBJECT)
	    {
	      /* Save insert MVCCID */
	      mvcc_args->insert_mvccid = MVCC_GET_INSID (&mvcc_header);
	    }

	  if (p_mvcc_rec_header)
	    {
	      memcpy (p_mvcc_rec_header, &mvcc_header,
		      sizeof (MVCC_REC_HEADER));
	    }
	  return CAST_BUFLEN (save_buf_ptr - rec_p->data);
	}
      else
	{
	  /* Even if inst_oid is the same as the searched oid, in some cases
	   * it can be a duplicate or a different object that replaced the
	   * previous one. This is specific to MVCC, and it can happen due to
	   * postponed object physical deletion and vacuum. In these cases,
	   * insert or delete MVCCID must also match.
	   */

	  if (first_object)
	    {
	      /* Must skip key value */
	      if (or_seek (&buf, oid_list_offset) != NO_ERROR)
		{
		  assert (false);
		  return NOT_FOUND;
		}
	    }
	}
      first_object = false;
    }

  return NOT_FOUND;
}

/*
 * btree_find_oid_from_ovfl () -
 *   return : offset of oid or NOT_FOUND
 *  rec_p(in): leaf record
 *  oid(in): oid to search for
 */
static int
btree_find_oid_from_ovfl (RECDES * rec_p, OID * oid, int oid_size,
			  MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  OID inst_oid;
  int min, mid, max, num_oids;
  char *base_ptr, *oid_ptr;
  int size = oid_size;
  MVCCID insid, delid;
  bool check_insid = MVCC_BTREE_DELETE_VACUUM_INSID (mvcc_args);
  bool check_delid =
    MVCC_BTREE_DELETE_REMOVE_DELID (mvcc_args)
    || MVCC_BTREE_DELETE_VACUUM_OBJECT (mvcc_args);

  /* check first oid */
  BTREE_GET_OID (rec_p->data, &inst_oid);
  assert ((inst_oid.slotid & BTREE_LEAF_RECORD_MASK) == 0);
  assert ((inst_oid.volid & BTREE_LEAF_OID_MVCC_MASK) == 0);

  if (OID_LT (oid, &inst_oid))
    {
      return NOT_FOUND;
    }
  else if (OID_EQ (oid, &inst_oid))
    {
      oid_ptr = rec_p->data;
      if (check_insid)
	{
	  /* Read insert MVCCID and check if it is the expected one */
	  OR_GET_MVCCID (oid_ptr + oid_size, &insid);
	  if (insid != mvcc_args->insert_mvccid)
	    {
	      /* This is not the expected OID */
	      return NOT_FOUND;
	    }
	}
      else if (check_delid)
	{
	  /*  Read delete MVCCID and check if it is the expected one */
	  OR_GET_MVCCID (oid_ptr + oid_size + OR_MVCCID_SIZE, &delid);
	  if (delid != mvcc_args->delete_mvccid)
	    {
	      /* This is not the expected OID */
	      return NOT_FOUND;
	    }
	}

      return 0;
    }

  if (mvcc_Enabled == true)
    {
      /* MVCCID fixed size */
      size += 2 * OR_MVCCID_SIZE;
    }

  /* check last oid */
  BTREE_GET_OID (rec_p->data + rec_p->length - size, &inst_oid);
  assert ((inst_oid.slotid & BTREE_LEAF_RECORD_MASK) == 0);
  assert ((inst_oid.volid & BTREE_LEAF_OID_MVCC_MASK) == 0);

  if (OID_GT (oid, &inst_oid))
    {
      return NOT_FOUND;
    }
  else if (OID_EQ (oid, &inst_oid))
    {
      if (check_insid)
	{
	  OR_GET_MVCCID (rec_p->data + rec_p->length - 2 * OR_MVCCID_SIZE,
			 &insid);
	  if (insid != mvcc_args->insert_mvccid)
	    {
	      /* Not our object */
	      return NOT_FOUND;
	    }
	}
      else if (check_delid)
	{
	  OR_GET_MVCCID (rec_p->data + rec_p->length - OR_MVCCID_SIZE,
			 &delid);
	  if (delid != mvcc_args->delete_mvccid)
	    {
	      /* Not our object */
	      return NOT_FOUND;
	    }
	}
      return rec_p->length - size;
    }

  num_oids = btree_leaf_get_num_oids (rec_p, 0, BTREE_OVERFLOW_NODE,
				      oid_size);
  base_ptr = rec_p->data;
  min = 0;
  max = num_oids - 1;

  while (min <= max)
    {
      mid = (min + max) / 2;
      oid_ptr = base_ptr + (size * mid);
      BTREE_GET_OID (oid_ptr, &inst_oid);
      assert ((inst_oid.slotid & BTREE_LEAF_RECORD_MASK) == 0);
      assert ((inst_oid.volid & BTREE_LEAF_OID_MVCC_MASK) == 0);

      if (OID_EQ (oid, &inst_oid))
	{
	  if (check_insid)
	    {
	      /* Read insert MVCCID and check if it is the expected one */
	      OR_GET_MVCCID (oid_ptr + oid_size, &insid);
	      if (insid != mvcc_args->insert_mvccid)
		{
		  /* This is not the expected OID */
		  return NOT_FOUND;
		}
	    }
	  else if (check_delid)
	    {
	      /*  Read delete MVCCID and check if it is the expected one */
	      OR_GET_MVCCID (oid_ptr + oid_size + OR_MVCCID_SIZE, &delid);
	      if (delid != mvcc_args->delete_mvccid)
		{
		  /* This is not the expected OID */
		  return NOT_FOUND;
		}
	    }
	  return CAST_BUFLEN (oid_ptr - rec_p->data);
	}
      else if (OID_GT (oid, &inst_oid))
	{
	  min = mid + 1;
	}
      else
	{
	  assert (OID_LT (oid, &inst_oid));
	  max = mid - 1;
	}
    }

  return NOT_FOUND;
}

/*
 * btree_get_prefix_separator () -
 *   return: db_value containing the prefix key.  This must be
 *           cleared when it is done being used.
 *   key1(in): first key
 *   key2(in): second key
 *   prefix_key(in):
 *
 * Note: This function finds the prefix (the separator) of two strings.
 * Currently this is only done for one of the six string types,
 * but with multi-column indexes and uniques coming, we may want
 * to do prefix keys for sequences as well.
 *
 * The purpose of this routine is to find a prefix that is
 * greater than or equal to the first key but strictly less
 * than the second key.  This routine assumes that the second
 * key is strictly greater than the first key.
 *
 * If this function could not generate common prefix key
 * (ex: key domain == integer)
 * copy key2 to prefix_key (because Index separator use key2 in general case)
 */
/* TODO: change key generation
 * (db_string_unique_prefix, pr_midxkey_unique_prefix)
 */
int
btree_get_prefix_separator (const DB_VALUE * key1, const DB_VALUE * key2,
			    DB_VALUE * prefix_key, TP_DOMAIN * key_domain)
{
  int c;
  int err = NO_ERROR;

  assert (DB_IS_NULL (key1) ||
	  (DB_VALUE_DOMAIN_TYPE (key1) == DB_VALUE_DOMAIN_TYPE (key2)));
  assert (!DB_IS_NULL (key2));
  assert_release (key_domain != NULL);

#if !defined(NDEBUG)
  c =
    btree_compare_key ((DB_VALUE *) key1, (DB_VALUE *) key2, key_domain, 1, 1,
		       NULL);
  assert (c == DB_LT);
#endif

  if (DB_VALUE_DOMAIN_TYPE (key1) == DB_TYPE_MIDXKEY)
    {
      assert_release (TP_DOMAIN_TYPE (key_domain) == DB_TYPE_MIDXKEY);

      err = pr_midxkey_unique_prefix (key1, key2, prefix_key);
    }
  else if (pr_is_string_type (DB_VALUE_DOMAIN_TYPE (key1)))
    {
      assert_release (TP_DOMAIN_TYPE (key_domain) != DB_TYPE_MIDXKEY);

      err = db_string_unique_prefix (key1, key2, prefix_key, key_domain);
    }
  else
    {
      /* In this case, key2 is used as separator in B+tree
       * so, copy key2 to prefix_key
       */
      err = pr_clone_value (key2, prefix_key);
    }

  if (err != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  c =
    btree_compare_key ((DB_VALUE *) key1, prefix_key, key_domain, 1, 1, NULL);

  if (c != DB_LT)
    {
      assert_release (false);
      return ER_FAILED;
    }

  c =
    btree_compare_key (prefix_key, (DB_VALUE *) key2, key_domain, 1, 1, NULL);

  if (!(c == DB_LT || c == DB_EQ))
    {
      assert_release (false);
      return ER_FAILED;
    }

  return err;
}

/*
 * btree_find_split_point () -
 *   return: the key or key separator (prefix) to be moved to the
 *           parent page, or NULL_KEY. The length of the returned
 *           key, or prefix, is set in mid_keylen. The parameter
 *           mid_slot is set to the record number of the split point record.
 *   btid(in):
 *   page_ptr(in): Pointer to the page
 *   mid_slot(out): Set to contain the record number for the split point slot
 *   key(in): Key to be inserted to the index
 *   clear_midkey(in):
 *
 * Note: Finds the split point of the given page by considering the
 * length of the existing records and the length of the key.
 * For a leaf page split operation, if there are n keys in the
 * page, then mid_slot can be set to :
 *
 *              0 : all the records in the page are to be moved to the newly
 *                  allocated page, key is to be inserted into the original
 *                  page. Mid_key is between key and the first record key.
 *
 *              n : all the records will be kept in the original page. Key is
 *                  to be inserted to the newly allocated page. Mid_key is
 *                  between the last record key and the key.
 *      otherwise : slot point is in the range 1 to n-1, inclusive. The page
 *                  is to be split into half.
 *
 * Note: the returned db_value should be cleared and FREED by the caller.
 */
static DB_VALUE *
btree_find_split_point (THREAD_ENTRY * thread_p,
			BTID_INT * btid, PAGE_PTR page_ptr,
			int *mid_slot, DB_VALUE * key, bool * clear_midkey)
{
  RECDES rec;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  INT16 slot_id;
  int ent_size;
  int key_cnt, key_len, offset;
  INT16 tot_rec, sum, max_split_size;
  int i, mid_size;
  bool m_clear_key, n_clear_key;
  DB_VALUE *mid_key = NULL, *next_key = NULL, *prefix_key = NULL, *tmp_key;
  bool key_read, found;
  NON_LEAF_REC nleaf_pnt;
  LEAF_REC leaf_pnt;

  key_cnt = btree_node_number_of_keys (page_ptr);
  if (key_cnt <= 0)
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_find_split_point: node key count underflow: %d",
		    key_cnt);
      goto error;
    }

  /* get the page header */
  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_find_split_point: get node header failure: %d",
		    key_cnt);
      goto error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  key_len = btree_get_key_length (key);
  key_len = BTREE_GET_KEY_LEN_IN_PAGE (key_len);

  key_read = false;

  /* find the slot position of the key if it is to be located in the page */
  if (node_type == BTREE_LEAF_NODE)
    {
      found = btree_search_leaf_page (thread_p, btid, page_ptr,
				      key, &slot_id);
      if (slot_id == NULL_SLOTID)	/* leaf search failed */
	{
	  assert (false);
	  goto error;
	}
    }
  else
    {
      found = 0;
      slot_id = NULL_SLOTID;
    }

  /* first find out the size of the data on the page, don't count the
   * header record.
   */
  for (i = 1, tot_rec = 0; i <= key_cnt; i++)
    {
      tot_rec += spage_get_space_for_record (page_ptr, i);
    }
  max_split_size = BTREE_NODE_MAX_SPLIT_SIZE (page_ptr);

  if (node_type == BTREE_LEAF_NODE && !found)
    {				/* take key length into consideration */
      if (mvcc_Enabled)
	{
	  ent_size = MVCC_LEAFENTSZ (key_len);
	}
      else
	{
	  ent_size = LEAFENTSZ (key_len);
	}

      tot_rec += ent_size;

      mid_size = MIN (max_split_size - ent_size,
		      btree_split_find_pivot (tot_rec,
					      &(header->split_info)));
      for (i = 1, sum = 0; i < slot_id && sum < mid_size; i++)
	{
	  sum += spage_get_space_for_record (page_ptr, i);
	}

      if (sum < mid_size)
	{
	  int len;

	  /* new key insert into left node */
	  sum += ent_size;
	  key_read = true;

	  for (; sum < mid_size && i <= key_cnt; i++)
	    {
	      len = spage_get_space_for_record (page_ptr, i);
	      if (sum + len >= mid_size)
		{
		  break;
		}

	      sum += len;
	    }
	}
      else
	{
	  while (sum < ent_size)
	    {
	      if (i == slot_id)
		{
		  /* new key insert into left node */
		  sum += ent_size;
		  key_read = true;
		}
	      else
		{
		  sum += spage_get_space_for_record (page_ptr, i);
		  i++;
		}
	    }
	}
    }
  else
    {				/* consider only the length of the records in the page */
      mid_size = btree_split_find_pivot (tot_rec, &(header->split_info));
      for (i = 1, sum = 0;
	   sum < mid_size && sum < max_split_size && i <= key_cnt; i++)
	{
	  sum += spage_get_space_for_record (page_ptr, i);
	}
    }

  *mid_slot = i - 1;

  /* We used to have a check here to make sure that the key could be
   * inserted into one of the pages after the split operation.  It must
   * always be the case that the key can be inserted into one of the
   * pages after split because keys can be no larger than
   * BTREE_MAX_KEYLEN_INPAGE
   * and the determination of the splitpoint above
   * should always guarantee that both pages have at least that much
   * free (usually closer to half the page, certainly more than 2 *
   * BTREE_MAX_KEYLEN_INPAGE.
   */

  if (node_type == BTREE_LEAF_NODE)
    {
      /* adjust right-end split point */
      if (*mid_slot >= (key_cnt - 1) && slot_id <= key_cnt)
	{
	  *mid_slot = key_cnt - 2;
	}

      /* adjust left-end split point */
      /* new key will be inserted at right page,
       * so adjust to prevent empty(or only fence) left
       */
      if (*mid_slot <= 1)
	{
	  *mid_slot = 2;
	}
    }
  else
    {
      /* adjust right-end split point */
      if (*mid_slot == key_cnt)
	{
	  *mid_slot = key_cnt - 1;
	}

      /* adjust left-end split point */
      /* new key will be inserted at right page,
       * so adjust to prevent empty left
       */
      if (*mid_slot == 0)
	{
	  *mid_slot = 1;
	}
    }

  assert (*mid_slot != 0);

  mid_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (mid_key == NULL)
    {
      goto error;
    }

  db_make_null (mid_key);

  if (*mid_slot != key_cnt && *mid_slot == (slot_id - 1) && key_read)
    {
      /* the new key is the split key */
      PR_TYPE *pr_type;

      if (node_type == BTREE_LEAF_NODE)
	{
	  pr_type = btid->key_type->type;
	}
      else
	{
	  pr_type = btid->nonleaf_key_type->type;
	}

      m_clear_key = false;

      (*(pr_type->setval)) (mid_key, key, m_clear_key);
    }
  else
    {
      /* the split key is one of the keys on the page */
      assert (*mid_slot > 0);
      if (spage_get_record (page_ptr, *mid_slot, &rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      /* we copy the key here because rec lives on the stack and mid_key
       * is returned from this routine.
       */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, mid_key,
			     (void *) &leaf_pnt, node_type, &m_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
      else
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, mid_key,
			     (void *) &nleaf_pnt, node_type, &m_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
    }

  /* The determination of the prefix key is dependent on the next key */
  next_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (next_key == NULL)
    {
      goto error;
    }

  db_make_null (next_key);

  if (*mid_slot == key_cnt && slot_id == (key_cnt + 1))
    {
      assert (node_type == BTREE_LEAF_NODE);

      n_clear_key = true;
      if (pr_clone_value (key, next_key) != NO_ERROR)
	{
	  goto error;
	}
    }
  else
    {
      /* The next key is one of the keys on the page */
      assert ((*mid_slot) + 1 > 0);
      if (spage_get_record (page_ptr, (*mid_slot) + 1, &rec, PEEK)
	  != S_SUCCESS)
	{
	  goto error;
	}

      /* we copy the key here because rec lives on the stack and mid_key
       * is returned from this routine.
       */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, next_key,
			     (void *) &leaf_pnt, node_type, &n_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
      else
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, next_key,
			     (void *) &nleaf_pnt, node_type, &n_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
    }

  /* now that we have the mid key and the next key, we can determine the
   * prefix key.
   */
  prefix_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (prefix_key == NULL)
    {
      goto error;
    }

  /* Check if we can make use of prefix keys.  We can't use them in the
   * upper levels of the trees because the algorithm will fall apart.  We
   * can only use them when splitting a leaf page.
   */
  if (node_type == BTREE_LEAF_NODE)
    {
      if ((btree_get_key_length (mid_key) >= BTREE_MAX_KEYLEN_INPAGE)
	  || (btree_get_key_length (next_key) >= BTREE_MAX_KEYLEN_INPAGE))
	{
	  /* if one of key is overflow key
	   * prefix key could be longer then max_key_len in page
	   * (that means insert could be failed)
	   * so, in this case use next key itself as prefix key
	   */
	  pr_clone_value (next_key, prefix_key);
	}
      else
	{
	  if (btree_get_prefix_separator (mid_key, next_key, prefix_key,
					  btid->key_type) != NO_ERROR)
	    {
	      goto error;
	    }
	}
    }
  else
    {
      /* return the next_key */
      pr_clone_value (next_key, prefix_key);
    }

  *clear_midkey = true;		/* we must always clear prefix keys */

  /* replace the mid_key with the prefix_key */
  tmp_key = mid_key;
  mid_key = prefix_key;
  prefix_key = tmp_key;		/* this makes sure we clear/free the old mid key */
  goto success;

  /* error handling and cleanup. */
error:

  if (mid_key)
    {
      btree_clear_key_value (&m_clear_key, mid_key);
      db_private_free_and_init (thread_p, mid_key);
    }
  mid_key = NULL;

  /* fall through */

success:

  if (next_key)
    {
      btree_clear_key_value (&n_clear_key, next_key);
      db_private_free_and_init (thread_p, next_key);
    }
  if (prefix_key)
    {
      pr_clear_value (prefix_key);
      db_private_free_and_init (thread_p, prefix_key);
    }

  return mid_key;
}

/*
 * btree_split_find_pivot () -
 *   return:
 *   total(in):
 *   split_info(in):
 */
static int
btree_split_find_pivot (int total, BTREE_NODE_SPLIT_INFO * split_info)
{
  int split_point;

  if (split_info->pivot == 0
      || (split_info->pivot > BTREE_SPLIT_LOWER_BOUND
	  && split_info->pivot < BTREE_SPLIT_UPPER_BOUND))
    {
      split_point = CEIL_PTVDIV (total, 2);
    }
  else
    {
      split_point = (int) (total * MAX (MIN (split_info->pivot,
					     BTREE_SPLIT_MAX_PIVOT),
					BTREE_SPLIT_MIN_PIVOT));
    }

  return split_point;
}

/*
 * btree_split_next_pivot () -
 *   return:
 *   split_info(in):
 *   new_value(in):
 *   max_index(in):
 */
static int
btree_split_next_pivot (BTREE_NODE_SPLIT_INFO * split_info,
			float new_value, int max_index)
{
  float new_pivot;

  assert (0.0f <= split_info->pivot);
  assert (split_info->pivot <= 1.0f);

  split_info->index = MIN (split_info->index + 1, max_index);

  if (split_info->pivot == 0)
    {
      new_pivot = new_value;
    }
  else
    {
      /* cumulative moving average(running average) */
      new_pivot = split_info->pivot;
      new_pivot = (new_pivot + ((new_value - new_pivot) / split_info->index));
    }

  split_info->pivot = MAX (0.0f, MIN (1.0f, new_pivot));

  return NO_ERROR;
}

static bool
btree_node_is_compressed (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  PAGE_PTR page_ptr)
{
  int key_cnt;
  RECDES peek_rec;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  assert (btid != NULL);

  if (TP_DOMAIN_TYPE (btid->key_type) != DB_TYPE_MIDXKEY)
    {
      return false;
    }

  key_cnt = btree_node_number_of_keys (page_ptr);
  if (key_cnt < 2)
    {
      return false;
    }

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      return false;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {
      return false;
    }

  /* check iff lower fence key */
  if (spage_get_record (page_ptr, 1, &peek_rec, PEEK) != S_SUCCESS)
    {
      assert (false);
    }
  if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE) == false)
    {
      return false;
    }

  /* check iff upper fence key */
  assert (key_cnt > 0);
  if (spage_get_record (page_ptr, key_cnt, &peek_rec, PEEK) != S_SUCCESS)
    {
      assert (false);
    }
  if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE) == false)
    {
      return false;
    }

  return true;
}

static int
btree_node_common_prefix (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  PAGE_PTR page_ptr)
{
  RECDES peek_rec;
  int diff_column;
  int key_cnt;
  bool dom_is_desc = false, next_dom_is_desc = false;

  DB_VALUE lf_key, uf_key;
  bool lf_clear_key, uf_clear_key;
  int offset;
  LEAF_REC leaf_pnt;

  if (btree_node_is_compressed (thread_p, btid, page_ptr) == false)
    {
      return 0;
    }

  key_cnt = btree_node_number_of_keys (page_ptr);
  assert (key_cnt >= 2);

  spage_get_record (page_ptr, 1, &peek_rec, PEEK);
  btree_read_record_helper (thread_p, btid, &peek_rec, &lf_key,
			    &leaf_pnt, BTREE_LEAF_NODE, &lf_clear_key,
			    &offset, PEEK_KEY_VALUE);
  assert (!btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));
  assert (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE));
  assert (DB_VALUE_TYPE (&lf_key) == DB_TYPE_MIDXKEY);

  assert (key_cnt > 0);
  spage_get_record (page_ptr, key_cnt, &peek_rec, PEEK);
  btree_read_record_helper (thread_p, btid, &peek_rec, &uf_key,
			    &leaf_pnt, BTREE_LEAF_NODE, &uf_clear_key,
			    &offset, PEEK_KEY_VALUE);
  assert (!btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY));
  assert (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE));
  assert (DB_VALUE_TYPE (&uf_key) == DB_TYPE_MIDXKEY);

  diff_column = pr_midxkey_common_prefix (&lf_key, &uf_key);

  /* clean up */
  btree_clear_key_value (&lf_clear_key, &lf_key);
  btree_clear_key_value (&uf_clear_key, &uf_key);

  return diff_column;
}

static int
btree_compress_records (THREAD_ENTRY * thread_p, BTID_INT * btid,
			RECDES * rec, int key_cnt)
{
  int key_len, new_key_len;
  int i, diff_column;
  DB_VALUE key, lf_key, uf_key;
  bool clear_key = false, lf_clear_key = false, uf_clear_key = false;
  int offset, new_offset;
  int key_type = BTREE_NORMAL_KEY;
  LEAF_REC leaf_pnt;

  assert (rec != NULL);
  assert (key_cnt >= 2);
  assert (!btree_leaf_is_flaged (&rec[0], BTREE_LEAF_RECORD_OVERFLOW_KEY));
  assert (btree_leaf_is_flaged (&rec[0], BTREE_LEAF_RECORD_FENCE));
  assert (!btree_leaf_is_flaged
	  (&rec[key_cnt - 1], BTREE_LEAF_RECORD_OVERFLOW_KEY));
  assert (btree_leaf_is_flaged (&rec[key_cnt - 1], BTREE_LEAF_RECORD_FENCE));

  btree_read_record_helper (thread_p, btid, &rec[0], &lf_key,
			    &leaf_pnt, BTREE_LEAF_NODE, &lf_clear_key,
			    &offset, PEEK_KEY_VALUE);
  assert (lf_clear_key == false);

  btree_read_record_helper (thread_p, btid, &rec[key_cnt - 1], &uf_key,
			    &leaf_pnt, BTREE_LEAF_NODE, &uf_clear_key,
			    &offset, PEEK_KEY_VALUE);
  assert (uf_clear_key == false);

  diff_column = pr_midxkey_common_prefix (&lf_key, &uf_key);

  if (diff_column > 0)
    {
      /* compress prefix */
      for (i = 1; i < key_cnt - 1; i++)
	{
	  assert (!btree_leaf_is_flaged (&rec[i], BTREE_LEAF_RECORD_FENCE));

	  if (btree_leaf_is_flaged (&rec[i], BTREE_LEAF_RECORD_OVERFLOW_KEY))
	    {
	      /* do not compress overflow key */
	      continue;
	    }

	  btree_read_record_helper (thread_p, btid, &rec[i], &key,
				    &leaf_pnt, BTREE_LEAF_NODE, &clear_key,
				    &offset, PEEK_KEY_VALUE);
	  assert (clear_key == false);

	  key_len = btree_get_key_length (&key);
	  pr_midxkey_remove_prefix (&key, diff_column);
	  new_key_len = btree_get_key_length (&key);

	  new_offset = offset + (new_key_len - key_len);
	  new_offset = DB_ALIGN (new_offset, INT_ALIGNMENT);

	  /* move remaining part of oids */
	  memmove (rec[i].data + new_offset, rec[i].data + offset,
		   rec[i].length - offset);
	  rec[i].length = new_offset + (rec[i].length - offset);

#if !defined (NDEBUG)
	  btree_check_valid_record (thread_p, btid, &rec[i], BTREE_LEAF_NODE,
				    NULL);
#endif

	  btree_clear_key_value (&clear_key, &key);
	}
    }

  return diff_column;
}

static int
btree_compress_node (THREAD_ENTRY * thread_p, BTID_INT * btid,
		     PAGE_PTR page_ptr)
{
  int i, key_cnt, diff_column;
  RECDES peek_rec, rec;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  DB_VALUE key;
  bool clear_key = false;
  int offset, new_offset, key_len, new_key_len;
  int key_type = BTREE_NORMAL_KEY;
  LEAF_REC leaf_pnt;

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, page_ptr);
#endif

  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  key_cnt = btree_node_number_of_keys (page_ptr);

  diff_column = btree_node_common_prefix (thread_p, btid, page_ptr);
  if (diff_column == 0)
    {
      return diff_column;
    }

  /* compress prefix */
  for (i = 2; i < key_cnt; i++)
    {
      (void) spage_get_record (page_ptr, i, &peek_rec, PEEK);

      assert (!btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_FENCE));

      if (btree_leaf_is_flaged (&peek_rec, BTREE_LEAF_RECORD_OVERFLOW_KEY))
	{
	  /* do not compress overflow key */
	  continue;
	}

      (void) spage_get_record (page_ptr, i, &rec, COPY);

      btree_read_record_helper (thread_p, btid, &rec, &key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_key,
				&offset, PEEK_KEY_VALUE);
      assert (clear_key == false);

      key_len = btree_get_key_length (&key);
      pr_midxkey_remove_prefix (&key, diff_column);
      new_key_len = btree_get_key_length (&key);

      new_offset = offset + new_key_len - key_len;
      new_offset = DB_ALIGN (new_offset, INT_ALIGNMENT);

      if (new_offset != offset)
	{
	  /* move the remaining part of record */
	  memmove (rec.data + new_offset, rec.data + offset,
		   rec.length - offset);
	  rec.length = new_offset + (rec.length - offset);
	}

#if !defined (NDEBUG)
      btree_check_valid_record (thread_p, btid, &rec, BTREE_LEAF_NODE, &key);
#endif

      spage_update (thread_p, page_ptr, i, &rec);
      btree_clear_key_value (&clear_key, &key);
    }

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, page_ptr);
#endif

  return diff_column;
}

/*
 * btree_split_node () -
 *   return: NO_ERROR
 *           child_vpid is set to page identifier for the child page to be
 *           followed, Q or R, or the page identifier of a newly allocated
 *           page to insert the key, or NULL_PAGEID. The parameter key is
 *           set to the middle key that will be put into the parent page P.
 *   btid(in): The index identifier
 *   P(in): Page pointer for the parent page of page Q
 *   Q(in): Page pointer for the page to be split
 *   R(in): Page pointer for the newly allocated page
 *   next_page(in):
 *   P_vpid(in): Page identifier for page Q
 *   Q_vpid(in): Page identifier for page Q
 *   R_vpid(in): Page identifier for page R
 *   p_slot_id(in): The slot of parent page P which points to page Q
 *   node_type(in): shows whether page Q is a leaf page, or not
 *   key(out): Set to contain the middle key of the split operation
 *   child_vpid(out): Set to the child page identifier
 *
 * Note: Page Q is split into two pages: Q and R. The second half of
 * of the page Q is move to page R. The middle key of of the
 * split operation is moved to parent page P. Depending on the
 * split point, the whole page Q may be moved to page R, or the
 * whole page content may be kept in page Q. If the key can not
 * fit into one of the pages after the split, a new page is
 * allocated for the key and its page identifier is returned.
 * The headers of all pages are updated, accordingly.
 */
static int
btree_split_node (THREAD_ENTRY * thread_p, BTID_INT * btid, PAGE_PTR P,
		  PAGE_PTR Q, PAGE_PTR R, VPID * P_vpid,
		  VPID * Q_vpid, VPID * R_vpid, INT16 p_slot_id,
		  BTREE_NODE_TYPE node_type, DB_VALUE * key,
		  VPID * child_vpid)
{
  int key_cnt, leftcnt, rightcnt;
  RECDES peek_rec, rec;
  NON_LEAF_REC nleaf_rec;
  BTREE_NODE_HEADER *pheader = NULL, *qheader = NULL;
  BTREE_NODE_HEADER right_header_info, *rheader = NULL;
  int i, j, c;
  int sep_key_len, key_len;
  bool clear_sep_key;
  DB_VALUE *sep_key;
  int ret = NO_ERROR;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int key_type;

  bool flag_fence_insert = false;
  OID dummy_oid = { NULL_PAGEID, 0, 0 };
  int leftsize, rightsize;
  VPID right_next_vpid;
  int right_max_key_len;

  /* for recovery purposes */
  char *p_redo_data;
  int p_redo_length;
  char p_redo_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];

  rheader = &right_header_info;

  /***********************************************************
   ***  STEP 0: initializations
   ***********************************************************/
  p_redo_data = NULL;

  rec.data = NULL;

  /* initialize child page identifier */
  VPID_SET_NULL (child_vpid);
  sep_key = NULL;

#if !defined(NDEBUG)
  if ((!P || !Q || !R) || VPID_ISNULL (P_vpid)
      || VPID_ISNULL (Q_vpid) || VPID_ISNULL (R_vpid))
    {
      goto exit_on_error;
    }
#endif

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_SIMPLE)
    {
      printf ("btree_split_node: P{%d, %d}, Q{%d, %d}, R{%d, %d}\n",
	      P_vpid->volid, P_vpid->pageid,
	      Q_vpid->volid, Q_vpid->pageid, R_vpid->volid, R_vpid->pageid);
    }
#endif

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, Q);
#endif
  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  key_cnt = btree_node_number_of_keys (Q);
  if (key_cnt <= 0)
    {
      goto exit_on_error;
    }

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_TEST_SPLIT)
    {
      btree_split_test (thread_p, btid, key, Q_vpid, Q, node_type);
    }
#endif

  /********************************************************************
   ***  STEP 1: find split point & sep_key
   ***          make fence key to be inserted
   ***
   ***   find the middle record of the page Q and find the number of
   ***   keys after split in pages Q and R, respectively
   ********************************************************************/
  qheader = btree_get_node_header (Q);
  if (qheader == NULL)
    {
      goto exit_on_error;
    }

  sep_key = btree_find_split_point (thread_p, btid, Q, &leftcnt, key,
				    &clear_sep_key);

  if (sep_key == NULL || DB_IS_NULL (sep_key))
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_split_node: Null middle key after split."
		    " Operation Ignored.\n");
      goto exit_on_error;
    }

  /* make fence record */
  if (node_type == BTREE_LEAF_NODE)
    {
      PR_TYPE *pr_type = btid->key_type->type;
      if (pr_type->index_lengthval)
	{
	  sep_key_len = (*pr_type->index_lengthval) (sep_key);
	}
      else
	{
	  sep_key_len = pr_type->disksize;
	}

      if (sep_key_len < BTREE_MAX_KEYLEN_INPAGE
	  && sep_key_len <= qheader->max_key_len)
	{
	  ret = btree_write_record (thread_p, btid, NULL, sep_key,
				    BTREE_LEAF_NODE, BTREE_NORMAL_KEY,
				    sep_key_len, false,
				    &btid->topclass_oid, &dummy_oid,
				    NULL, &rec);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  btree_leaf_set_flag (&rec, BTREE_LEAF_RECORD_FENCE);

	  flag_fence_insert = true;
	}
      else
	{
	  /* do not insert fence key if sep_key is overflow key */
	  flag_fence_insert = false;
	}
    }

  if (prm_get_bool_value (PRM_ID_USE_BTREE_FENCE_KEY) == false)
    {
      flag_fence_insert = false;
    }

  rightcnt = key_cnt - leftcnt;

  assert (leftcnt > 0);

  /*********************************************************************
   ***  STEP 2: save undo image of Q
   ***		update Q, R header info
   *********************************************************************/
  /* add undo logging for page Q */
  log_append_undo_data2 (thread_p, RVBT_COPYPAGE,
			 &btid->sys_btid->vfid, Q, -1, DB_PAGESIZE, Q);

  /* We may need to update the max_key length if the mid key is larger than
   * the max key length.  This can happen due to disk padding when the
   * prefix key length approaches the fixed key length.
   */
  sep_key_len = btree_get_key_length (sep_key);
  sep_key_len = BTREE_GET_KEY_LEN_IN_PAGE (sep_key_len);
  qheader->max_key_len = MAX (sep_key_len, qheader->max_key_len);

  /* set rheader max_key_len as qheader max_key_len */
  right_max_key_len = qheader->max_key_len;
  right_next_vpid = qheader->next_vpid;

  if (node_type == BTREE_LEAF_NODE)
    {
      qheader->next_vpid = *R_vpid;
    }
  else
    {
      VPID_SET_NULL (&qheader->next_vpid);
    }

  assert_release (leftcnt != 0);
  if (leftcnt == 0)		/* defence */
    {
      qheader->max_key_len = 0;
    }

  qheader->split_info.index = 1;

  rheader->node_level = qheader->node_level;
  rheader->max_key_len = right_max_key_len;
  if (key_cnt - leftcnt == 0 && flag_fence_insert == false)
    {
      rheader->max_key_len = 0;
    }

  rheader->next_vpid = right_next_vpid;

  if (node_type == BTREE_LEAF_NODE)
    {
      rheader->prev_vpid = *Q_vpid;
    }
  else
    {
      VPID_SET_NULL (&(rheader->prev_vpid));
    }

  rheader->split_info = qheader->split_info;

  RANDOM_EXIT (thread_p);

  if (btree_init_node_header (thread_p, &btid->sys_btid->vfid,
			      R, rheader, false) != NO_ERROR)
    {
      goto exit_on_error;
    }

  /*******************************************************************
   ***   STEP 3: move second half of page Q to page R
   ***           insert fence key to Q
   ***           make redo image for Q
   *******************************************************************/
  /* lower fence key for R */
  rightsize = 0;
  j = 1;
  if (flag_fence_insert == true)
    {
      rightsize = j;
      assert (j > 0);
      if (spage_insert_at (thread_p, R, j++, &rec) != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
    }

  /* move the second half of page Q to page R */
  for (i = 1; i <= rightcnt; i++, j++)
    {
      assert (leftcnt + 1 > 0);
      if (spage_get_record (Q, leftcnt + 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  ret = ER_FAILED;
	  goto exit_on_error;
	}

      assert (j > 0);
      if (spage_insert_at (thread_p, R, j, &peek_rec) != SP_SUCCESS)
	{
	  ret = ER_FAILED;
	  goto exit_on_error;
	}

      rightsize = j;

      assert (leftcnt + 1 > 0);
      if (spage_delete (thread_p, Q, leftcnt + 1) != leftcnt + 1)
	{
	  ret = ER_FAILED;
	  goto exit_on_error;
	}
    }

  leftsize = leftcnt;
  /* upper fence key for Q */
  if (flag_fence_insert == true)
    {
      assert (leftcnt + 1 > 0);
      if (spage_insert_at (thread_p, Q, leftcnt + 1, &rec) != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
      leftsize++;
    }

  RANDOM_EXIT (thread_p);

  btree_compress_node (thread_p, btid, Q);

  /* add redo logging for page Q */
  log_append_redo_data2 (thread_p, RVBT_COPYPAGE,
			 &btid->sys_btid->vfid, Q, -1, DB_PAGESIZE, Q);

  /***************************************************************************
   ***   STEP 4: add redo log for R
   ***    Log the second half of page Q for redo purposes on Page R,
   ***    the records on the second half of page Q will be inserted to page R
   ***************************************************************************/

  btree_compress_node (thread_p, btid, R);

  log_append_redo_data2 (thread_p, RVBT_COPYPAGE, &btid->sys_btid->vfid,
			 R, -1, DB_PAGESIZE, R);

  RANDOM_EXIT (thread_p);

  /****************************************************************************
   ***   STEP 5: insert sep_key to P
   ***           add undo/redo log for page P
   ***
   ***    update the parent page P to keep the middle key and to point to
   ***    pages Q and R.  Remember that this mid key will be on a non leaf page
   ***    regardless of whether we are splitting a leaf or non leaf page.
   ****************************************************************************/
  nleaf_rec.pnt = *R_vpid;
  key_len = btree_get_key_length (sep_key);
  if (key_len < BTREE_MAX_KEYLEN_INPAGE)
    {
      key_type = BTREE_NORMAL_KEY;
      nleaf_rec.key_len = key_len;
    }
  else
    {
      key_type = BTREE_OVERFLOW_KEY;
      nleaf_rec.key_len = -1;
    }

  ret = btree_write_record (thread_p, btid, &nleaf_rec, sep_key,
			    BTREE_NON_LEAF_NODE, key_type, key_len, false,
			    NULL, NULL, NULL, &rec);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  p_slot_id++;

  RANDOM_EXIT (thread_p);

  /* add undo/redo logging for page P */
  assert (p_slot_id > 0);
  if (spage_insert_at (thread_p, P, p_slot_id, &rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  p_redo_data = PTR_ALIGN (p_redo_data_buf, BTREE_MAX_ALIGN);

  btree_rv_write_log_record (p_redo_data, &p_redo_length, &rec,
			     BTREE_NON_LEAF_NODE);
  log_append_undoredo_data2 (thread_p, RVBT_NDRECORD_INS,
			     &btid->sys_btid->vfid, P, p_slot_id,
			     sizeof (p_slot_id), p_redo_length, &p_slot_id,
			     p_redo_data);

  RANDOM_EXIT (thread_p);

  key_cnt = btree_node_number_of_keys (P);
  assert_release (key_cnt > 0);

  pheader = btree_get_node_header (P);
  if (pheader == NULL)
    {
      goto exit_on_error;
    }

  assert_release (pheader->split_info.pivot >= 0);

  btree_node_header_undo_log (thread_p, &btid->sys_btid->vfid, P);

  btree_split_next_pivot (&pheader->split_info, (float) p_slot_id / key_cnt,
			  key_cnt);

  /* We may need to update the max_key length if the mid key is larger than
   * the max key length. This can happen due to disk padding when the
   * prefix key length approaches the fixed key length.
   */
  sep_key_len = btree_get_key_length (sep_key);
  sep_key_len = BTREE_GET_KEY_LEN_IN_PAGE (sep_key_len);
  pheader->max_key_len = MAX (sep_key_len, pheader->max_key_len);

  btree_node_header_redo_log (thread_p, &btid->sys_btid->vfid, P);

  /* find the child page to be followed */
  c = btree_compare_key (key, sep_key, btid->key_type, 1, 1, NULL);
  assert (c == DB_LT || c == DB_EQ || c == DB_GT);

  if (c == DB_UNK)
    {
      goto exit_on_error;
    }
  else if (c < 0)
    {
      /* set child page pointer */
      *child_vpid = *Q_vpid;
    }
  else
    {
      /* set child page pointer */
      *child_vpid = *R_vpid;
    }

  /* TODO : update child_vpid max_key_len */
  if (sep_key)
    {
      btree_clear_key_value (&clear_sep_key, sep_key);
      db_private_free_and_init (thread_p, sep_key);
    }

  pgbuf_set_dirty (thread_p, P, DONT_FREE);
  pgbuf_set_dirty (thread_p, Q, DONT_FREE);
  pgbuf_set_dirty (thread_p, R, DONT_FREE);

  RANDOM_EXIT (thread_p);

  mnt_bt_splits (thread_p);

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, Q);
  btree_verify_node (thread_p, btid, R);
#endif

  return ret;

exit_on_error:

  if (sep_key)
    {
      btree_clear_key_value (&clear_sep_key, sep_key);
      db_private_free_and_init (thread_p, sep_key);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

#if !defined(NDEBUG)
/*
 * btree_set_split_point () -
 *   return: the key or key separator (prefix) to be moved to the
 *           parent page, or NULL_KEY. The length of the returned
 *           key, or prefix, is set in mid_keylen. The parameter
 *           mid_slot is set to the record number of the split point record.
 *   btid(in):
 *   page_ptr(in): Pointer to the page
 *   mid_slot(in): Set to contain the record number for the split point slot
 *   key(in): Key to be inserted to the index
 *   clear_midkey(in):
 *
 */
static DB_VALUE *
btree_set_split_point (THREAD_ENTRY * thread_p,
		       BTID_INT * btid, PAGE_PTR page_ptr,
		       INT16 mid_slot, DB_VALUE * key, bool * clear_midkey)
{
  RECDES rec;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  INT16 slot_id;
  int key_cnt, offset;
  bool m_clear_key, n_clear_key;
  DB_VALUE *mid_key = NULL, *next_key = NULL, *prefix_key = NULL, *tmp_key;
  NON_LEAF_REC nleaf_pnt;
  LEAF_REC leaf_pnt;

  key_cnt = btree_node_number_of_keys (page_ptr);
  if (key_cnt <= 0)
    {
      assert (false);
    }

  /* get the page header */
  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      assert (false);
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  /* find the slot position of the key if it is to be located in the page */
  if (node_type == BTREE_LEAF_NODE)
    {
      (void) btree_search_leaf_page (thread_p, btid, page_ptr, key, &slot_id);
      if (slot_id == NULL_SLOTID)	/* leaf search failed */
	{
	  assert (false);
	}
    }
  else
    {
      slot_id = NULL_SLOTID;
    }

  mid_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (mid_key == NULL)
    {
      assert (false);
    }

  db_make_null (mid_key);

  /* the split key is one of the keys on the page */
  assert (mid_slot > 0);
  if (spage_get_record (page_ptr, mid_slot, &rec, PEEK) != S_SUCCESS)
    {
      assert (false);
    }

  /* we copy the key here because rec lives on the stack and mid_key
   * is returned from this routine.
   */
  if (node_type == BTREE_LEAF_NODE)
    {
      btree_read_record (thread_p, btid, page_ptr, &rec, mid_key,
			 (void *) &leaf_pnt, node_type, &m_clear_key,
			 &offset, COPY_KEY_VALUE, NULL);
    }
  else
    {
      btree_read_record (thread_p, btid, page_ptr, &rec, mid_key,
			 (void *) &nleaf_pnt, node_type, &m_clear_key,
			 &offset, COPY_KEY_VALUE, NULL);
    }

  /* The determination of the prefix key is dependent on the next key */
  next_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (next_key == NULL)
    {
      assert (false);
    }

  db_make_null (next_key);

  if (mid_slot == key_cnt && slot_id == (key_cnt + 1))
    {
      /* the next key is the new key, we don't have to read it */
      n_clear_key = true;
      if (pr_clone_value (key, next_key) != NO_ERROR)
	{
	  assert (false);
	}
    }
  else
    {
      /* The next key is one of the keys on the page */
      assert (mid_slot + 1 > 0);
      if (spage_get_record (page_ptr, mid_slot + 1, &rec, PEEK) != S_SUCCESS)
	{
	  assert (false);
	}

      /* we copy the key here because rec lives on the stack and mid_key
       * is returned from this routine.
       */
      if (node_type == BTREE_LEAF_NODE)
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, next_key,
			     (void *) &leaf_pnt, node_type, &n_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
      else
	{
	  btree_read_record (thread_p, btid, page_ptr, &rec, next_key,
			     (void *) &nleaf_pnt, node_type, &n_clear_key,
			     &offset, COPY_KEY_VALUE, NULL);
	}
    }

  /* now that we have the mid key and the next key, we can determine the
   * prefix key.
   */

  prefix_key = (DB_VALUE *) db_private_alloc (thread_p, sizeof (DB_VALUE));
  if (prefix_key == NULL)
    {
      assert (false);
    }

  /* Check if we can make use of prefix keys.  We can't use them in the
   * upper levels of the trees because the algorithm will fall apart.  We
   * can only use them when splitting a leaf page.
   */
  if (node_type == BTREE_LEAF_NODE)
    {
      if (btree_get_prefix_separator (mid_key, next_key, prefix_key,
				      btid->key_type) != NO_ERROR)
	{
	  assert (false);
	}
    }
  else
    {
      /* return the next_key */
      pr_clone_value (next_key, prefix_key);
    }

  *clear_midkey = true;		/* we must always clear prefix keys */

  /* replace the mid_key with the prefix_key */
  tmp_key = mid_key;
  mid_key = prefix_key;
  prefix_key = tmp_key;		/* this makes sure we clear/free the old mid key */

  if (next_key)
    {
      btree_clear_key_value (&n_clear_key, next_key);
      db_private_free_and_init (thread_p, next_key);
    }
  if (prefix_key)
    {
      pr_clear_value (prefix_key);
      db_private_free_and_init (thread_p, prefix_key);
    }

  return mid_key;
}

/*
 * btree_split_test () -
 *
 *   btid(in):
 *   key(in):
 *   S_vpid(in):
 *   S_page(in):
 *   node_type(in):
 */
static void
btree_split_test (THREAD_ENTRY * thread_p, BTID_INT * btid, DB_VALUE * key,
		  VPID * S_vpid, PAGE_PTR S_page, BTREE_NODE_TYPE node_type)
{
  RECDES rec, peek_rec;
  int i, j, key_cnt, lcnt, rcnt, sep_key_len, ret;
  PAGE_PTR L_page, R_page;
  VPID L_vpid, R_vpid;
  BTREE_NODE_HEADER header_info, *header = NULL;
  DB_VALUE *sep_key;
  bool fence_insert = false;
  bool clear_sep_key = true;
  OID dummy_oid = { NULL_PAGEID, 0, 0 };
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];

  header = &header_info;

  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  key_cnt = btree_node_number_of_keys (S_page);
  assert (key_cnt > 0);

  L_page = btree_get_new_page (thread_p, btid, &L_vpid, S_vpid);
  R_page = btree_get_new_page (thread_p, btid, &R_vpid, S_vpid);

  /* dummy header */
  memset (header, 0, sizeof (BTREE_NODE_HEADER));
  btree_init_node_header (thread_p, &btid->sys_btid->vfid,
			  L_page, header, false);
  btree_init_node_header (thread_p, &btid->sys_btid->vfid,
			  R_page, header, false);

  for (lcnt = 1; lcnt < key_cnt; i++)
    {
      fence_insert = false;
      sep_key = btree_set_split_point (thread_p, btid, S_page, lcnt, key,
				       &clear_sep_key);
      assert (sep_key != NULL);

      if (node_type == BTREE_LEAF_NODE)
	{
	  PR_TYPE *pr_type;

	  pr_type = btid->key_type->type;
	  sep_key_len = pr_type->disksize;

	  if (pr_type->index_lengthval)
	    {
	      sep_key_len = (*pr_type->index_lengthval) (sep_key);
	    }

	  if (sep_key_len < BTREE_MAX_KEYLEN_INPAGE)
	    {
	      ret = btree_write_record (thread_p, btid, NULL, sep_key,
					BTREE_LEAF_NODE, BTREE_NORMAL_KEY,
					sep_key_len, false,
					&btid->topclass_oid, &dummy_oid,
					NULL, &rec);

	      btree_leaf_set_flag (&rec, BTREE_LEAF_RECORD_FENCE);
	      fence_insert = true;
	    }
	}

      rcnt = key_cnt - lcnt;

      /* Right page test */
      j = 1;
      /* lower fence key for Right */
      if (fence_insert == true)
	{
	  assert (j > 0);
	  ret = spage_insert_at (thread_p, R_page, j++, &rec);
	  if (ret != SP_SUCCESS)
	    {
	      assert (false);
	    }
	}

      /* move the second half of page P to page R */
      for (i = 1; i <= rcnt; i++, j++)
	{
	  assert (lcnt + i > 0);
	  ret = spage_get_record (S_page, lcnt + i, &peek_rec, PEEK);
	  if (ret != S_SUCCESS)
	    {
	      assert (false);
	    }

	  assert (j > 0);
	  ret = spage_insert_at (thread_p, R_page, j, &peek_rec);
	  if (ret != SP_SUCCESS)
	    {
	      assert (false);
	    }
	}

      /* Left page test */
      for (i = 1; i <= lcnt; i++)
	{
	  ret = spage_get_record (S_page, i, &peek_rec, PEEK);
	  if (ret != S_SUCCESS)
	    {
	      assert (false);
	    }

	  ret = spage_insert_at (thread_p, L_page, i, &peek_rec);
	  if (ret != SP_SUCCESS)
	    {
	      assert (false);
	    }
	}

      /* upper fence key for Left */
      if (fence_insert == true)
	{
	  assert (i > 0);
	  ret = spage_insert_at (thread_p, L_page, i, &rec);
	  if (ret != SP_SUCCESS)
	    {
	      assert (false);
	    }
	}

      /* clean up */
      if (fence_insert == true)
	{
	  lcnt++, rcnt++;
	}

      assert (btree_node_number_of_keys (L_page) == lcnt);
      assert (btree_node_number_of_keys (R_page) == rcnt);

      for (i = 1; i <= lcnt; i++)
	{
	  ret = spage_delete (thread_p, L_page, 1);
	  if (ret != 1)
	    {
	      assert (false);
	    }
	}

      for (i = 1; i <= rcnt; i++)
	{
	  ret = spage_delete (thread_p, R_page, 1);
	  if (ret != 1)
	    {
	      assert (false);
	    }
	}

      assert (btree_node_number_of_keys (L_page) == 0);
      assert (btree_node_number_of_keys (R_page) == 0);

      btree_clear_key_value (&clear_sep_key, sep_key);
      db_private_free_and_init (thread_p, sep_key);
    }

  pgbuf_unfix_and_init (thread_p, L_page);
  pgbuf_unfix_and_init (thread_p, R_page);

  if (file_dealloc_page (thread_p, &btid->sys_btid->vfid, &L_vpid) !=
      NO_ERROR)
    {
      assert (false);
    }

  if (file_dealloc_page (thread_p, &btid->sys_btid->vfid, &R_vpid) !=
      NO_ERROR)
    {
      assert (false);
    }
}
#endif

/*
 * btree_split_root () -
 *   return: NO_ERROR
 *           child_vpid parameter is set to the child page to be followed
 *           after the split operation, or the page identifier of a newly
 *           allocated page for future key insertion, or NULL_PAGEID.
 *           The parameter key is set to the middle key of the split operation.
 *   btid(in): B+tree index identifier
 *   P(in): Page pointer for the root to be split
 *   Q(in): Page pointer for the newly allocated page
 *   R(in): Page pointer for the newly allocated page
 *   P_vpid(in): Page identifier for root page P
 *   Q_vpid(in): Page identifier for page Q
 *   R_vpid(in): Page identifier for page R
 *   node_type(in): shows whether root is currenly a leaf page,
 *                  or not
 *   key(out): Set to contain the middle key of the split operation
 *   child_vpid(out): Set to the child page identifier
 *
 * Note: The root page P is split into two pages: Q and R. In order
 * not to change the actual root page, the first half of the page
 * is moved to page Q and the second half is moved to page R.
 * Depending on the split point found, the whole root page may be
 * moved to Q, or R, leaving the other one empty for future  key
 * insertion. If the key cannot fit into either Q or R after the
 * split, a new page is allocated and its page identifier is
 * returned. Two new records are formed within root page to point
 * to pages Q and R. The headers of all pages are updated.
 */
static int
btree_split_root (THREAD_ENTRY * thread_p, BTID_INT * btid, PAGE_PTR P,
		  PAGE_PTR Q, PAGE_PTR R, VPID * P_vpid,
		  VPID * Q_vpid, VPID * R_vpid, BTREE_NODE_TYPE node_type,
		  DB_VALUE * key, VPID * child_vpid)
{
  int key_cnt, leftcnt, rightcnt;
  RECDES rec, peek_rec;
  NON_LEAF_REC nleaf_rec;
  BTREE_ROOT_HEADER *pheader = NULL;
  BTREE_NODE_HEADER q_header_info, *qheader = NULL;
  BTREE_NODE_HEADER r_header_info, *rheader = NULL;
  int i, j, c;
  int sep_key_len, key_len;
  bool clear_sep_key;
  DB_VALUE *sep_key;
  DB_VALUE *neg_inf_key = NULL;
  char *recset_data;		/* for recovery purposes */
  RECSET_HEADER recset_header;	/* for recovery purposes */
  int recset_length;		/* for recovery purposes */
  int sp_success;
  PGLENGTH log_addr_offset;
  int ret = NO_ERROR;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  char recset_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int key_type;
  BTREE_NODE_SPLIT_INFO split_info;
  int node_level;
  bool flag_fence_insert = false;
  OID dummy_oid = { NULL_PAGEID, 0, 0 };
  int leftsize, rightsize;

  qheader = &q_header_info;
  rheader = &r_header_info;

  /***********************************************************
   ***  STEP 0: initializations
   ***********************************************************/
  recset_data = NULL;
  rec.data = NULL;

  /* initialize child page identifier */
  VPID_SET_NULL (child_vpid);
  sep_key = NULL;

#if !defined(NDEBUG)
  if ((!P || !Q || !R) || VPID_ISNULL (P_vpid)
      || VPID_ISNULL (Q_vpid) || VPID_ISNULL (R_vpid))
    {
      goto exit_on_error;
    }
#endif

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_SIMPLE)
    {
      printf ("btree_split_root: P{%d, %d}, Q{%d, %d}, R{%d, %d}\n",
	      P_vpid->volid, P_vpid->pageid,
	      Q_vpid->volid, Q_vpid->pageid, R_vpid->volid, R_vpid->pageid);
    }
#endif

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
#endif

  /* initializations */
  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  /* log the whole root page P for undo purposes. */
  log_append_undo_data2 (thread_p, RVBT_COPYPAGE, &btid->sys_btid->vfid, P,
			 -1, DB_PAGESIZE, P);

  /* get the number of keys in the root page P */
  key_cnt = btree_node_number_of_keys (P);
  if (key_cnt <= 0)
    {
      goto exit_on_error;
    }

#if !defined(NDEBUG)
  node_level = btree_get_node_level (P);
  assert (node_level >= 1);
#endif

  pheader = btree_get_root_header (P);
  if (pheader == NULL)
    {
      goto exit_on_error;
    }

  split_info = pheader->node.split_info;
  split_info.index = 1;

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_TEST_SPLIT)
    {
      btree_split_test (thread_p, btid, key, P_vpid, P, node_type);
    }
#endif

  /********************************************************************
   ***  STEP 1: find split point & sep_key
   ***          make fence key to be inserted
   ***
   ***   find the middle record of the page Q and find the number of
   ***   keys after split in pages Q and R, respectively
   ********************************************************************/

  sep_key = btree_find_split_point (thread_p, btid, P, &leftcnt, key,
				    &clear_sep_key);
  if (sep_key == NULL || DB_IS_NULL (sep_key))
    {
      er_log_debug (ARG_FILE_LINE,
		    "btree_split_root: Null middle key after split."
		    " Operation Ignored.\n");
      goto exit_on_error;
    }

  /* make fence record */
  if (node_type == BTREE_LEAF_NODE)
    {
      PR_TYPE *pr_type;

      pr_type = btid->key_type->type;

      if (pr_type->index_lengthval)
	{
	  sep_key_len = (*pr_type->index_lengthval) (sep_key);
	}
      else
	{
	  sep_key_len = pr_type->disksize;
	}

      if (sep_key_len < BTREE_MAX_KEYLEN_INPAGE
	  && sep_key_len <= pheader->node.max_key_len)
	{
	  ret = btree_write_record (thread_p, btid, NULL, sep_key,
				    BTREE_LEAF_NODE, BTREE_NORMAL_KEY,
				    sep_key_len, false,
				    &btid->topclass_oid, &dummy_oid,
				    NULL, &rec);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  btree_leaf_set_flag (&rec, BTREE_LEAF_RECORD_FENCE);
	  flag_fence_insert = true;
	}
      else
	{
	  /* do not insert fence key if sep_key is overflow key */
	  flag_fence_insert = false;
	}
    }

  if (prm_get_bool_value (PRM_ID_USE_BTREE_FENCE_KEY) == false)
    {
      flag_fence_insert = false;
    }

  /* neg-inf key is dummy key which is not used in comparison
   * so set it as sep_key */
  neg_inf_key = sep_key;

  rightcnt = key_cnt - leftcnt;

  /*********************************************************************
   ***  STEP 2: update P, Q, R header info
   *********************************************************************/
  /* update page P header */
  pheader->node.node_level++;

  /* We may need to update the max_key length if the sep key is larger than
   * the max key length. This can happen due to disk padding when the
   * prefix key length approaches the fixed key length.
   */
  sep_key_len = btree_get_key_length (sep_key);
  sep_key_len = BTREE_GET_KEY_LEN_IN_PAGE (sep_key_len);
  pheader->node.max_key_len = MAX (sep_key_len, pheader->node.max_key_len);
  btree_write_default_split_info (&(pheader->node.split_info));

  btree_node_header_redo_log (thread_p, &btid->sys_btid->vfid, P);

  /* update page Q header */
  qheader->node_level = pheader->node.node_level - 1;
  qheader->max_key_len = pheader->node.max_key_len;
  if (leftcnt == 0 && flag_fence_insert == false)
    {
      qheader->max_key_len = 0;
    }

  VPID_SET_NULL (&(qheader->prev_vpid));	/* non leaf or first leaf node */

  if (node_type == BTREE_LEAF_NODE)
    {
      qheader->next_vpid = *R_vpid;
    }
  else
    {
      VPID_SET_NULL (&(qheader->next_vpid));
    }

  qheader->split_info = split_info;

  if (btree_init_node_header
      (thread_p, &btid->sys_btid->vfid, Q, qheader, true) != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* update page R header */
  rheader->node_level = pheader->node.node_level - 1;
  rheader->max_key_len = pheader->node.max_key_len;
  if (key_cnt - leftcnt == 0 && flag_fence_insert == false)
    {
      rheader->max_key_len = 0;
    }

  VPID_SET_NULL (&(rheader->next_vpid));	/* non leaf or last leaf node */

  if (node_type == BTREE_LEAF_NODE)
    {
      rheader->prev_vpid = *Q_vpid;
    }
  else
    {
      VPID_SET_NULL (&(rheader->prev_vpid));
    }

  rheader->split_info = split_info;

  if (btree_init_node_header
      (thread_p, &btid->sys_btid->vfid, R, rheader, true) != NO_ERROR)
    {
      goto exit_on_error;
    }


  /*******************************************************************
   ***   STEP 3: move second half of page P to page R
   ***           insert fence key to R
   ***           add undo / redo log for R
   *******************************************************************/
  /* move the second half of root page P to page R
   */
  assert (btree_node_number_of_keys (P) == leftcnt + rightcnt);

  j = 1;
  /* lower fence key for page R */
  if (flag_fence_insert == true)
    {
      rightsize = j;
      assert (j > 0);
      if (spage_insert_at (thread_p, R, j++, &rec) != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
    }

  for (i = 1; i <= rightcnt; i++, j++)
    {
      assert (leftcnt + 1 > 0);
      if (spage_get_record (P, leftcnt + 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  goto exit_on_error;
	}

      assert (j > 0);
      sp_success = spage_insert_at (thread_p, R, j, &peek_rec);
      if (sp_success != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
      rightsize = j;

      assert (leftcnt + 1 > 0);
      if (spage_delete (thread_p, P, leftcnt + 1) != leftcnt + 1)
	{
	  goto exit_on_error;
	}
    }

  /* for recovery purposes */
  recset_data = PTR_ALIGN (recset_data_buf, BTREE_MAX_ALIGN);

  /* Log page R records for redo purposes */
  ret = btree_rv_util_save_page_records (R, 1, j - 1, 1, recset_data,
					 &recset_length);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  log_append_redo_data2 (thread_p, RVBT_INS_PGRECORDS, &btid->sys_btid->vfid,
			 R, -1, recset_length, recset_data);


  /*******************************************************************
   ***   STEP 4: move first half of page P to page Q
   ***           insert fence key to Q
   ***           add undo / redo log for Q
   *******************************************************************/
  /* move the first half of root page P to page Q */

  for (i = 1; i <= leftcnt; i++)
    {
      if (spage_get_record (P, 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  goto exit_on_error;
	}

      sp_success = spage_insert_at (thread_p, Q, i, &peek_rec);
      if (sp_success != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
      leftsize = i;

      if (spage_delete (thread_p, P, 1) != 1)
	{
	  goto exit_on_error;
	}
    }

  /* upper fence key for Q */
  if (flag_fence_insert == true)
    {
      assert (i > 0);
      if (spage_insert_at (thread_p, Q, i, &rec) != SP_SUCCESS)
	{
	  goto exit_on_error;
	}
      leftsize = i;
    }
  else
    {
      i--;
    }

  /* Log page Q records for redo purposes */
  ret = btree_rv_util_save_page_records (Q, 1, i, 1, recset_data,
					 &recset_length);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }
  log_append_redo_data2 (thread_p, RVBT_INS_PGRECORDS, &btid->sys_btid->vfid,
			 Q, -1, recset_length, recset_data);

  /****************************************************************************
   ***   STEP 5: insert sep_key to P
   ***           add redo log for page P
   ****************************************************************************/

  /* Log deletion of all page P records (except the header!!)
   * for redo purposes
   */
  recset_header.rec_cnt = key_cnt;
  recset_header.first_slotid = 1;
  log_append_redo_data2 (thread_p, RVBT_DEL_PGRECORDS, &btid->sys_btid->vfid,
			 P, -1, sizeof (RECSET_HEADER), &recset_header);

  /* update the root page P to keep the middle key and to point to
   * page Q and R.  Remember that this mid key will be on a non leaf page
   * regardless of whether we are splitting a leaf or non leaf page.
   */
  nleaf_rec.pnt = *Q_vpid;
  key_len = btree_get_key_length (neg_inf_key);
  if (key_len < BTREE_MAX_KEYLEN_INPAGE)
    {
      key_type = BTREE_NORMAL_KEY;
      nleaf_rec.key_len = key_len;
    }
  else
    {
      key_type = BTREE_OVERFLOW_KEY;
      nleaf_rec.key_len = -1;
    }

  ret = btree_write_record (thread_p, btid, &nleaf_rec, neg_inf_key,
			    BTREE_NON_LEAF_NODE, key_type, key_len, false,
			    NULL, NULL, NULL, &rec);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (spage_insert_at (thread_p, P, 1, &rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  /* log the inserted record for undo/redo purposes, */
  btree_rv_write_log_record (recset_data, &recset_length, &rec,
			     BTREE_NON_LEAF_NODE);

  log_addr_offset = 1;
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_INS,
			 &btid->sys_btid->vfid, P, log_addr_offset,
			 recset_length, recset_data);

  nleaf_rec.pnt = *R_vpid;
  key_len = btree_get_key_length (sep_key);
  if (key_len < BTREE_MAX_KEYLEN_INPAGE)
    {
      key_type = BTREE_NORMAL_KEY;
      nleaf_rec.key_len = key_len;
    }
  else
    {
      key_type = BTREE_OVERFLOW_KEY;
      nleaf_rec.key_len = -1;
    }

  ret = btree_write_record (thread_p, btid, &nleaf_rec, sep_key,
			    BTREE_NON_LEAF_NODE, key_type, key_len, false,
			    NULL, NULL, NULL, &rec);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (spage_insert_at (thread_p, P, 2, &rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  /* log the inserted record for undo/redo purposes, */
  btree_rv_write_log_record (recset_data, &recset_length, &rec,
			     BTREE_NON_LEAF_NODE);

  log_addr_offset = 2;
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_INS,
			 &btid->sys_btid->vfid, P, log_addr_offset,
			 recset_length, recset_data);

  /* find the child page to be followed */

  c = btree_compare_key (key, sep_key, btid->key_type, 1, 1, NULL);
  assert (c == DB_LT || c == DB_EQ || c == DB_GT);

  if (c == DB_UNK)
    {
      goto exit_on_error;
    }
  else if (c < 0)
    {
      /* set child page identifier */
      *child_vpid = *Q_vpid;

    }
  else
    {
      /* set child page identifier */
      *child_vpid = *R_vpid;
    }

  if (sep_key)
    {
      btree_clear_key_value (&clear_sep_key, sep_key);
      db_private_free_and_init (thread_p, sep_key);
    }

  pgbuf_set_dirty (thread_p, P, DONT_FREE);
  pgbuf_set_dirty (thread_p, Q, DONT_FREE);
  pgbuf_set_dirty (thread_p, R, DONT_FREE);

  mnt_bt_splits (thread_p);

#if !defined(NDEBUG)
  btree_verify_node (thread_p, btid, P);
  btree_verify_node (thread_p, btid, Q);
  btree_verify_node (thread_p, btid, R);
#endif

  return ret;

exit_on_error:

  if (sep_key)
    {
      btree_clear_key_value (&clear_sep_key, sep_key);
      db_private_free_and_init (thread_p, sep_key);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_insert_lock_curr_key_remaining_pseudo_oid - lock the remaining pseudo
 *						     oids of current key in
 *						     btree_insert
 *
 *   return: (error code)
 *   thread_p(in):
 *   btid_int(in): B+tree index identifier
 *   rec(in): leaf record
 *   offset(in): offset of oid(s) following the key
 *   oid_start_pos(in) : position of first OID that will be locked
 *   first_ovfl_vpid(in): first overflow vpid of leaf record
 *   oid (in): object OID
 *   class_oid(in): class oid
 *
 * Note:
 *  This function must be called by btree_insert if the following conditions
 * are satisfied:
 *  1. current key locking require many pseudo-oids locks (non-unique B-tree,
 *     key already exists in B-tree, the total transactions hold mode is
 *     NS-lock on PSEUDO-OID attached to the first key buffer OID)
 *  2. the PSEUDO-OID attached to the first key buffer OID has been NS-locked
 *     by the current transaction
 *  3. next key not previously S-locked or NX-locked
 *
 *   The function iterate through OID-list starting with the second OID.
 * The attached pseudo-OID of each OID is conditionally NS-locked.
 * The function stops if pseudo-OID is not locked by any active transaction
 */
static int
btree_insert_lock_curr_key_remaining_pseudo_oid (THREAD_ENTRY * thread_p,
						 BTID_INT * btid_int,
						 RECDES * rec, int offset,
						 VPID * first_ovfl_vpid,
						 OID * oid, OID * class_oid)
{
  char *rec_oid_ptr = NULL;
  int oid_pos, oids_cnt;
  OID temp_pseudo_oid;
  PAGE_PTR O_page = NULL;
  int ret_val = NO_ERROR;
  LOCK prev_tot_hold_mode;
  VPID O_vpid;
  RECDES peek_rec;
  int oid_size;

  assert_release (btid_int != NULL);
  assert_release (btid_int->sys_btid != NULL);
  assert_release (rec != NULL);
  assert_release (rec->data != NULL);
  assert_release (first_ovfl_vpid != NULL);
  assert_release (class_oid != NULL);
  assert_release (!mvcc_Enabled);

  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  oids_cnt = btree_leaf_get_num_oids (rec, offset, BTREE_LEAF_NODE, oid_size);

  /* starting with the second oid */
  rec_oid_ptr = rec->data + offset;
  for (oid_pos = 1; oid_pos < oids_cnt; oid_pos++)
    {
      OR_GET_OID (rec_oid_ptr, &temp_pseudo_oid);
      /* make pseudo OID */
      btree_make_pseudo_oid (temp_pseudo_oid.pageid, temp_pseudo_oid.slotid,
			     temp_pseudo_oid.volid, btid_int->sys_btid,
			     &temp_pseudo_oid);

      /* don't care about key lock escalation on remaining PSEUDO-OIDS */
      if (lock_btid_object_get_prev_total_hold_mode (thread_p,
						     &temp_pseudo_oid,
						     class_oid,
						     btid_int->sys_btid,
						     NS_LOCK, LK_COND_LOCK,
						     &prev_tot_hold_mode,
						     NULL) != LK_GRANTED)
	{
	  return ER_FAILED;
	}

      if (prev_tot_hold_mode == NULL_LOCK)
	{
	  /* no need to lock the others pseudo-OIDs */
	  return NO_ERROR;
	}

      rec_oid_ptr += OR_OID_SIZE;
    }

  /* get OIDS from overflow OID page, create and lock pseudo OIDs */
  O_vpid = *first_ovfl_vpid;
  while (!VPID_ISNULL (&O_vpid))
    {
      O_page = pgbuf_fix (thread_p, &O_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
      if (O_page == NULL)
	{
	  return ER_FAILED;
	}

      (void) pgbuf_check_page_ptype (thread_p, O_page, PAGE_BTREE);

      if (spage_get_record (O_page, 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  ret_val = ER_FAILED;
	  goto end;
	}

      assert (peek_rec.length % 4 == 0);
      oids_cnt = btree_leaf_get_num_oids (&peek_rec, 0, BTREE_OVERFLOW_NODE,
					  oid_size);
      rec_oid_ptr = peek_rec.data;
      for (oid_pos = 0; oid_pos < oids_cnt; oid_pos++)
	{
	  OR_GET_OID (rec_oid_ptr, &temp_pseudo_oid);
	  /* make pseudo OID */
	  btree_make_pseudo_oid (temp_pseudo_oid.pageid,
				 temp_pseudo_oid.slotid,
				 temp_pseudo_oid.volid,
				 btid_int->sys_btid, &temp_pseudo_oid);

	  /* don't care about key lock escalation on remaining PSEUDO-OIDS */
	  if (lock_btid_object_get_prev_total_hold_mode
	      (thread_p, &temp_pseudo_oid, class_oid, btid_int->sys_btid,
	       NS_LOCK, LK_COND_LOCK, &prev_tot_hold_mode,
	       NULL) != LK_GRANTED)
	    {
	      ret_val = ER_FAILED;
	      goto end;
	    }

	  if (prev_tot_hold_mode == NULL_LOCK)
	    {
	      /* no need to lock the others pseudo-OIDs */
	      goto end;
	    }

	  rec_oid_ptr += OR_OID_SIZE;
	}

      /* advance to the next page */
      btree_get_next_overflow_vpid (O_page, &O_vpid);

      pgbuf_unfix_and_init (thread_p, O_page);
    }

  /* lock Pseudo OID attached to oid */
  btree_make_pseudo_oid (oid->pageid, oid->slotid, oid->volid,
			 btid_int->sys_btid, &temp_pseudo_oid);

  /* don't care about key lock escalation on remaining PSEUDO-OIDS */
  if (lock_btid_object_get_prev_total_hold_mode (thread_p, &temp_pseudo_oid,
						 class_oid,
						 btid_int->sys_btid, NS_LOCK,
						 LK_COND_LOCK,
						 &prev_tot_hold_mode,
						 NULL) != LK_GRANTED)
    {
      ret_val = ER_FAILED;
    }

end:
  if (O_page)
    {
      pgbuf_unfix_and_init (thread_p, O_page);
    }

  return ret_val;
}

/*
 * btree_insert_unlock_curr_key_remaining_pseudo_oid - unlock the remaining
 *						       pseudo oids of current
 *						       key in btree_insert
 *
 *   return: (error code)
 *   thread_p(in):
 *   btid_int(in): B+tree index identifier
 *   rec(in): leaf record
 *   offset(in): offset of oid(s) following the key
 *   first_ovfl_vpid(in): first overflow vpid of leaf record
 *   class_oid(in): class oid
 *
 * Note:
 *  This function must be called by btree_insert if the following conditions
 * are satisfied:
 *  1. current key locking require many pseudo-oids locks (non-unique B-tree,
 *     key already exists in B-tree, the total transactions hold mode is
 *     NS-lock on PSEUDO-OID attached to the first key buffer OID).
 *  2. many pseudo-oids locks has been already NS-locked, and the first
 *     pseudo-oid has been already escalated to NX-lock.
 *
 *   The function iterate through OID-list starting with the second OID.
 * The attached pseudo-OIDs that was previously NS-locked by the current
 * transaction, is unlocked now.
 *   When insert in tree, the PSEUDO-OIDs are NS-locked in the order they
 * are found in current key lock buffer
 */
static int
btree_insert_unlock_curr_key_remaining_pseudo_oid (THREAD_ENTRY * thread_p,
						   BTID_INT * btid_int,
						   RECDES * rec, int offset,
						   VPID * first_ovfl_vpid,
						   OID * class_oid)
{
  char *rec_oid_ptr = NULL;
  int oid_pos, oids_cnt;
  OID temp_pseudo_oid;
  PAGE_PTR O_page = NULL;
  int ret_val = NO_ERROR;
  VPID O_vpid;
  RECDES peek_rec;
  int oid_size;

  assert_release (btid_int != NULL);
  assert_release (btid_int->sys_btid != NULL);
  assert_release (rec != NULL);
  assert_release (rec->data != NULL);
  assert_release (first_ovfl_vpid != NULL);
  assert_release (class_oid != NULL);
  assert_release (!mvcc_Enabled);

  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  oids_cnt = btree_leaf_get_num_oids (rec, offset, BTREE_LEAF_NODE, oid_size);

  /* starting with the second oid */
  rec_oid_ptr = rec->data + offset;
  for (oid_pos = 1; oid_pos < oids_cnt; oid_pos++)
    {
      OR_GET_OID (rec_oid_ptr, &temp_pseudo_oid);
      /* make pseudo OID */
      btree_make_pseudo_oid (temp_pseudo_oid.pageid, temp_pseudo_oid.slotid,
			     temp_pseudo_oid.volid, btid_int->sys_btid,
			     &temp_pseudo_oid);
      if (ret_val != NO_ERROR)
	{
	  return ret_val;
	}

      assert (!OID_ISNULL (&temp_pseudo_oid));
      assert (!OID_ISNULL (class_oid));
      lock_remove_object_lock (thread_p, &temp_pseudo_oid, class_oid,
			       NS_LOCK);

      rec_oid_ptr += OR_OID_SIZE;
    }

  /* get OIDS from overflow OID page, create and lock pseudo OIDs */
  O_vpid = *first_ovfl_vpid;
  while (!VPID_ISNULL (&O_vpid))
    {
      O_page = pgbuf_fix (thread_p, &O_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
      if (O_page == NULL)
	{
	  return ER_FAILED;
	}

      (void) pgbuf_check_page_ptype (thread_p, O_page, PAGE_BTREE);

      if (spage_get_record (O_page, 1, &peek_rec, PEEK) != S_SUCCESS)
	{
	  ret_val = ER_FAILED;
	  goto end;
	}

      assert (peek_rec.length % 4 == 0);
      oids_cnt = btree_leaf_get_num_oids (&peek_rec, 0, BTREE_OVERFLOW_NODE,
					  oid_size);
      rec_oid_ptr = peek_rec.data;
      for (oid_pos = 0; oid_pos < oids_cnt; oid_pos++)
	{
	  OR_GET_OID (rec_oid_ptr, &temp_pseudo_oid);
	  /* make pseudo OID */
	  btree_make_pseudo_oid (temp_pseudo_oid.pageid,
				 temp_pseudo_oid.slotid,
				 temp_pseudo_oid.volid,
				 btid_int->sys_btid, &temp_pseudo_oid);

	  assert (!OID_ISNULL (&temp_pseudo_oid));
	  assert (!OID_ISNULL (class_oid));
	  lock_remove_object_lock (thread_p, &temp_pseudo_oid, class_oid,
				   NS_LOCK);

	  rec_oid_ptr += OR_OID_SIZE;
	}

      /* advance to the next page */
      btree_get_next_overflow_vpid (O_page, &O_vpid);

      pgbuf_unfix_and_init (thread_p, O_page);
    }

end:
  if (O_page)
    {
      pgbuf_unfix_and_init (thread_p, O_page);
    }

  return ret_val;
}

/*
 * btree_insert () -
 *   return: (the key to be inserted or NULL)
 *   btid(in): B+tree index identifier
 *   key(in): Key to be inserted
 *   cls_oid(in): To find out the lock mode of corresponding class.
 *                The lock mode is used to make a decision about if the key
 *                range locking must be performed.
 *   oid(in): Object identifier to be inserted for the key
 *   op_type(in): operation types
 *                SINGLE_ROW_INSERT, SINGLE_ROW_UPDATE, SINGLE_ROW_MODIFY
 *                MULTI_ROW_INSERT, MULTI_ROW_UPDATE
 *   unique_stat_info(in):
 *            When multiple rows are inserted, unique_stat_info maintains
 *            the local statistical infomation related to uniqueness checking
 *            such as num_nulls, num_keys, and num_oids, and that is locally
 *            updated during the process of one INSERT or UPDATE statement.
 *            After those rows are inserted correctly, the local information
 *            would be reflected into global information saved in root page.
 *   unique(out):
 *   p_mvcc_rec_header(in):
 *
 */
static DB_VALUE *
btree_insert (THREAD_ENTRY * thread_p, BTID * btid, DB_VALUE * key,
	      OID * cls_oid, OID * oid, int op_type,
	      BTREE_UNIQUE_STATS * unique_stat_info, int *unique,
	      MVCC_REC_HEADER * p_mvcc_rec_header)
{
  VPID P_vpid, Q_vpid, R_vpid, child_vpid;
  PAGE_PTR P = NULL, Q = NULL, R = NULL, next_page = NULL;
  RECDES peek_rec;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  int root_level = 0, key_len, max_entry, key_len_in_page;
  int pnt_max_key_len, max_key_len;
  int pnt_node_level;
  INT16 p_slot_id, slot_id;
  int top_op_active = 0;
  int max_free;
  PAGEID_STRUCT pageid_struct;	/* for recovery purposes */
  int key_added_deleted;
  BTID_INT btid_int;
  bool next_page_flag = false;
  bool next_lock_flag = false;
  bool curr_lock_flag = false;
  OID class_oid;
  LOCK class_lock = NULL_LOCK;
  int tran_index = 0;
  int nextkey_lock_request;
  int mvcc_lock_request;
  int offset = -1;
  bool dummy;
  int ret_val;
  LEAF_REC leaf_pnt;
  PAGE_PTR N = NULL;
  BTREE_NODE_TYPE node_type;
  int key_cnt;
  VPID N_vpid;
  OID N_oid, saved_N_oid;
  OID C_oid, saved_C_oid;
  LOG_LSA saved_plsa, saved_nlsa;
  LOG_LSA *temp_lsa;
  OID N_class_oid, C_class_oid;
  OID saved_N_class_oid, saved_C_class_oid;
  int alignment;
  bool is_active;
  bool key_found = false;
  LOCK next_key_granted_mode = NULL_LOCK;
  LOCK current_lock = NULL_LOCK, prev_tot_hold_mode = NULL_LOCK;
  bool curr_key_many_locks_needed = false;
  KEY_LOCK_ESCALATION curr_key_lock_escalation = NO_KEY_LOCK_ESCALATION;
  BTREE_SCAN tmp_bts;
  bool is_last_key;
  bool old_check_interrupt;
  int retry_btree_no_space = 0;
  BTREE_SEARCH result;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;	/* used in RR or SERIALIZABLE */
  MVCC_REC_HEADER mvcc_local_rec_header;

  assert (key != NULL);

  old_check_interrupt = thread_set_check_interrupt (thread_p, false);

  is_active = logtb_is_current_active (thread_p);

#if !defined(NDEBUG)
  if (BTREE_INVALID_INDEX_ID (btid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_INDEX_ID, 3,
	      btid->vfid.fileid, btid->vfid.volid, btid->root_pageid);
      goto error;
    }
#endif

  P_vpid.volid = btid->vfid.volid;	/* read the root page */
  P_vpid.pageid = btid->root_pageid;
  P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		 PGBUF_UNCONDITIONAL_LATCH);
  if (P == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

  root_header = btree_get_root_header (P);
  if (root_header == NULL)
    {
      goto error;
    }

  btid_int.sys_btid = btid;
  if (btree_glean_root_header_info (thread_p, root_header, &btid_int) !=
      NO_ERROR)
    {
      goto error;
    }

  if (key && DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY)
    {
      /* set complete setdomain */
      key->data.midxkey.domain = btid_int.key_type;
    }

  if (unique)
    {
      *unique = BTREE_IS_UNIQUE (btid_int.unique_pk) ? 1 : 0;
    }

  alignment = BTREE_MAX_ALIGN;

  key_len = btree_get_key_length (key);

  if (key_len >= BTREE_MAX_KEYLEN_INPAGE && VFID_ISNULL (&btid_int.ovfid))
    {
      if (log_start_system_op (thread_p) == NULL)
	{
	  goto error;
	}

      if (btree_create_overflow_key_file (thread_p, &btid_int) != NO_ERROR)
	{
	  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
	  goto error;
	}

      /* notification */
      BTREE_SET_CREATED_OVERFLOW_KEY_NOTIFICATION (thread_p, key, oid,
						   cls_oid, btid, NULL);

      log_append_undoredo_data2 (thread_p, RVBT_UPDATE_OVFID,
				 &btid_int.sys_btid->vfid, P, HEADER,
				 sizeof (VFID), sizeof (VFID),
				 &root_header->ovfid, &btid_int.ovfid);

      /* update the root header */
      VFID_COPY (&root_header->ovfid, &btid_int.ovfid);

      pgbuf_set_dirty (thread_p, P, DONT_FREE);

      if (btree_is_new_file (&btid_int))
	{
	  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
	}
      else
	{
	  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	  file_new_declare_as_old (thread_p, &btid_int.ovfid);
	}
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  max_key_len = root_header->node.max_key_len;
  key_len_in_page = BTREE_GET_KEY_LEN_IN_PAGE (key_len);

  if (key_len_in_page > max_key_len)
    {
      root_header->node.max_key_len = key_len_in_page;
      max_key_len = key_len_in_page;

      ret_val =
	btree_change_root_header_delta (thread_p, &btid->vfid, P, 0, 0, 0);
      if (ret_val != NO_ERROR)
	{
	  goto error;
	}

      pgbuf_set_dirty (thread_p, P, DONT_FREE);
    }

  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      /* update root header statistics if it's a unique Btree
       * and the transaction is active.
       *
       * unique statistics for non null keys will be updated after
       * we find out if we have a new key or not.
       */
      if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
	    {
	      assert (mvcc_Enabled == true
		      && !heap_is_mvcc_disabled_for_class (cls_oid));
	      if (op_type == SINGLE_ROW_DELETE || op_type == SINGLE_ROW_UPDATE
		  || op_type == SINGLE_ROW_MODIFY)
		{
		  if (logtb_mvcc_update_class_unique_stats
		      (thread_p, &btid_int.topclass_oid, btid, 0, -1, -1,
		       true) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else		/* MULTI_ROW_DELETE, MULTI_ROW_UPDATE */
		{
		  if (unique_stat_info == NULL)
		    {
		      goto error;
		    }
		  unique_stat_info->num_nulls--;
		  unique_stat_info->num_oids--;
		}
	    }
	  else if (op_type == SINGLE_ROW_INSERT
		   || op_type == SINGLE_ROW_UPDATE
		   || op_type == SINGLE_ROW_MODIFY)
	    {
	      if (mvcc_Enabled && !heap_is_mvcc_disabled_for_class (cls_oid))
		{
		  if (logtb_mvcc_update_class_unique_stats
		      (thread_p, &btid_int.topclass_oid, btid, 0, 1, 1,
		       true) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else
		{
		  ret_val =
		    btree_change_root_header_delta (thread_p, &btid->vfid, P,
						    1, 1, 0);
		  if (ret_val != NO_ERROR)
		    {
		      goto error;
		    }

		  pgbuf_set_dirty (thread_p, P, DONT_FREE);
		}
	    }
	  else
	    {			/* MULTI_ROW_INSERT, MULTI_ROW_UPDATE */
	      if (unique_stat_info == NULL)
		{
		  goto error;
		}

	      unique_stat_info->num_nulls++;
	      unique_stat_info->num_oids++;
	    }
	}

      /* nothing more to do -- this is not an error */
      pgbuf_unfix_and_init (thread_p, P);

      mnt_bt_inserts (thread_p);

      (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

#if !defined(NDEBUG)
      if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) &
	  BTREE_DEBUG_DUMP_FULL)
	{
	  btree_dump (thread_p, stdout, btid, 2);
	}
#endif

      return key;
    }

  /* update root header statistics if its a Btree for uniques.
   * this only wants to be done if the transaction is active.  That
   * is, if we are aborting a transaction the statistics are "rolled back"
   * by their own logging.
   *
   * NOTE that we are optimistically updating the header statistics in advance.
   * When we encounter the failure to insert a new key, updates are rollbacked
   * by not an adjusting routine but the top operation.
   * Also NOTE that users to see the header statistics may have the transient
   * values.
   */
  if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
	{
	  if (op_type == SINGLE_ROW_DELETE || op_type == SINGLE_ROW_UPDATE
	      || op_type == SINGLE_ROW_MODIFY)
	    {
	      if (logtb_mvcc_update_class_unique_stats
		  (thread_p, &btid_int.topclass_oid, btid, -1, -1, 0,
		   true) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      if (unique_stat_info == NULL)
		{
		  goto error;
		}

	      unique_stat_info->num_oids--;
	      unique_stat_info->num_keys--;
	    }
	}
      else if (op_type == SINGLE_ROW_INSERT || op_type == SINGLE_ROW_UPDATE
	       || op_type == SINGLE_ROW_MODIFY)
	{
	  if (mvcc_Enabled && !heap_is_mvcc_disabled_for_class (cls_oid))
	    {
	      if (logtb_mvcc_update_class_unique_stats
		  (thread_p, &btid_int.topclass_oid, btid, 1, 1, 0,
		   true) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      /* update the root header */
	      ret_val =
		btree_change_root_header_delta (thread_p, &btid->vfid, P, 0,
						1, 1);
	      if (ret_val != NO_ERROR)
		{
		  goto error;
		}

	      pgbuf_set_dirty (thread_p, P, DONT_FREE);
	    }
	}
      else
	{
	  if (unique_stat_info == NULL)
	    {
	      goto error;
	    }

	  unique_stat_info->num_oids++;
	  unique_stat_info->num_keys++;	/* guess new key insert */
	}
    }

  /* decide whether key range locking must be performed.
   * if class_oid is transferred through a new argument,
   * this operation will be performed more efficiently.
   */
  if (cls_oid != NULL && !OID_ISNULL (cls_oid))
    {
      COPY_OID (&class_oid, cls_oid);
      /* cls_oid might be NULL_OID. But it does not make problem. */
    }
  else
    {
      if (is_active)
	{
	  if (heap_get_class_oid (thread_p, &class_oid, oid,
				  DONT_NEED_SNAPSHOT) == NULL)
	    {
	      goto error;
	    }
	}
      else
	{
	  assert (false);	/* is impossible */
	  OID_SET_NULL (&class_oid);
	}
    }

  if (is_active)
    {
      /* initialize saved_N_oid */
      OID_SET_NULL (&saved_N_oid);
      OID_SET_NULL (&saved_N_class_oid);
      OID_SET_NULL (&saved_C_oid);
      OID_SET_NULL (&saved_C_class_oid);
    }

  if (btree_insert_init_locks (thread_p, is_active, &class_oid, &btid_int,
			       &class_lock, &nextkey_lock_request,
			       &mvcc_lock_request) != NO_ERROR)
    {
      goto error;
    }

  COPY_OID (&N_class_oid, &class_oid);
  COPY_OID (&C_class_oid, &class_oid);

  /* Get transaction index */
  tran_index = LOG_FIND_THREAD_TRAN_INDEX (thread_p);

start_point:
  if (next_lock_flag == true || curr_lock_flag == true
      || retry_btree_no_space > 0)
    {
      P_vpid.volid = btid->vfid.volid;	/* read the root page */
      P_vpid.pageid = btid->root_pageid;
      P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      root_header = btree_get_root_header (P);
      if (root_header == NULL)
	{
	  goto error;
	}

      root_level = root_header->node.node_level;
      node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      max_key_len = root_header->node.max_key_len;
    }

  /* find the maximum entry size that may need to be inserted to the root */
  if (node_type == BTREE_LEAF_NODE)
    {
      if (mvcc_Enabled)
	{
	  max_entry = 2 * MVCC_LEAFENTSZ (max_key_len);
	}
      else
	{
	  max_entry = 2 * LEAFENTSZ (max_key_len);
	}
    }
  else
    {
      max_entry = NLEAFENTSZ (max_key_len);
    }
  /* slotted page overhead */
  max_entry += (alignment +	/* sphdr->alignment */
		spage_slot_size ());	/* slot size */

  /* free space in the root node */
  max_free = spage_max_space_for_new_record (thread_p, P);

  key_cnt = btree_node_number_of_keys (P);

  /* there is a need to split the root, only if there is not enough space
   * for a new entry.
   */
  if (max_entry > max_free)
    {
      /* consider BTREE_MAX_KEYLEN_INPAGE + BTREE_MAX_OIDLEN_INPAGE */
      assert_release (key_cnt >= 3);

      /* start system top operation */
      log_start_system_op (thread_p);
      top_op_active = 1;

      /* get two new pages */
      Q = btree_get_new_page (thread_p, &btid_int, &Q_vpid, &P_vpid);
      if (Q == NULL)
	{
	  goto error;
	}

      /* log the newly allocated pageid for deallocation for undo purposes */
      if (btree_is_new_file (&btid_int) != true)
	{
	  /* we don't do undo logging for new files */
	  pageid_struct.vpid = Q_vpid;
	  pageid_struct.vfid.fileid = btid->vfid.fileid;
	  pageid_struct.vfid.volid = btid->vfid.volid;
	  log_append_undo_data2 (thread_p, RVBT_NEW_PGALLOC, &btid->vfid,
				 NULL, -1, sizeof (PAGEID_STRUCT),
				 &pageid_struct);
	}

      R = btree_get_new_page (thread_p, &btid_int, &R_vpid, &P_vpid);
      if (R == NULL)
	{
	  goto error;
	}

      /* log the newly allocated pageid for deallocation for undo purposes */
      if (btree_is_new_file (&btid_int) != true)
	{
	  /* we don't do undo logging for new files */
	  pageid_struct.vpid = R_vpid;
	  assert (pageid_struct.vfid.fileid == btid->vfid.fileid);
	  assert (pageid_struct.vfid.volid == btid->vfid.volid);
	  log_append_undo_data2 (thread_p, RVBT_NEW_PGALLOC, &btid->vfid,
				 NULL, -1, sizeof (PAGEID_STRUCT),
				 &pageid_struct);
	}

      /* split the root P into two pages Q and R */
      if (btree_split_root (thread_p, &btid_int, P, Q, R, &P_vpid, &Q_vpid,
			    &R_vpid, node_type, key, &child_vpid) != NO_ERROR)
	{
	  goto error;
	}

#if !defined(NDEBUG)
      (void) spage_check_num_slots (thread_p, P);
      (void) spage_check_num_slots (thread_p, Q);
      (void) spage_check_num_slots (thread_p, R);
#endif

      pgbuf_unfix_and_init (thread_p, P);

      if (VPID_EQ (&child_vpid, &Q_vpid))
	{
	  /* child page to be followed is page Q */
	  pgbuf_unfix_and_init (thread_p, R);

	  if (btree_is_new_file (&btid_int))
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
	    }
	  else
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	    }

	  top_op_active = 0;

	  P = Q;
	  Q = NULL;
	  P_vpid = Q_vpid;
	}
      else if (VPID_EQ (&child_vpid, &R_vpid))
	{
	  /* child page to be followed is page R */
	  pgbuf_unfix_and_init (thread_p, Q);

	  if (btree_is_new_file (&btid_int))
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
	    }
	  else
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	    }

	  top_op_active = 0;

	  P = R;
	  R = NULL;
	  P_vpid = R_vpid;
	}
      else
	{
	  assert (false);	/* is error ? */

	  pgbuf_unfix_and_init (thread_p, R);
	  pgbuf_unfix_and_init (thread_p, Q);

	  if (btree_is_new_file (&btid_int))
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
	    }
	  else
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	    }

	  top_op_active = 0;

	  P_vpid = child_vpid;
	  P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			 PGBUF_UNCONDITIONAL_LATCH);
	  if (P == NULL)
	    {
	      goto error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);
	}
    }

  key_cnt = btree_node_number_of_keys (P);

  /* get the header record */
  header = btree_get_node_header (P);
  if (header == NULL)
    {
      goto error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  pnt_max_key_len = header->max_key_len;
  pnt_node_level = header->node_level;

  while (node_type == BTREE_NON_LEAF_NODE)
    {
      /* find and get the child page to be followed */
      if (btree_search_nonleaf_page
	  (thread_p, &btid_int, P, key, &p_slot_id, &Q_vpid) != NO_ERROR)
	{
	  goto error;
	}
      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (Q == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);

      max_free = spage_max_space_for_new_record (thread_p, Q);

      key_cnt = btree_node_number_of_keys (Q);

      /* read the header record */
      header = btree_get_node_header (Q);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      assert (header->max_key_len <= pnt_max_key_len);
      assert (header->node_level == pnt_node_level - 1);

      key_len_in_page = BTREE_GET_KEY_LEN_IN_PAGE (key_len);
      /* is new key longer than all in the subtree of child page Q ? */
      if (key_cnt > 0 && key_len_in_page > header->max_key_len)
	{
	  header->max_key_len = key_len_in_page;
	  if (btree_node_header_redo_log (thread_p, &btid->vfid, Q) !=
	      NO_ERROR)
	    {
	      goto error;
	    }

	  pgbuf_set_dirty (thread_p, Q, DONT_FREE);
	}

      /* find the maximum entry size that may need to be inserted to Q */
      if (node_type == BTREE_LEAF_NODE)
	{
	  if (mvcc_Enabled)
	    {
	      max_entry = 2 * MVCC_LEAFENTSZ (max_key_len);
	    }
	  else
	    {
	      max_entry = 2 * LEAFENTSZ (max_key_len);
	    }
	}
      else
	{
	  max_entry = NLEAFENTSZ (max_key_len);
	}

      /* slotted page overhead */
      max_entry += (alignment +	/* sphdr->alignment */
		    spage_slot_size ());	/* slot size */

      /* there is a need to split Q, only if there is not enough space
       * for a new entry.
       */
      if (max_entry > max_free)
	{
	  /* consider BTREE_MAX_KEYLEN_INPAGE + BTREE_MAX_OIDLEN_INPAGE */
	  assert_release (key_cnt >= 3);

	  /* start system top operation */
	  if (log_start_system_op (thread_p) == NULL)
	    {
	      goto error;
	    }
	  top_op_active = 1;

	  /* split the page Q into two pages Q and R, and update parent page P */

	  R = btree_get_new_page (thread_p, &btid_int, &R_vpid, &Q_vpid);
	  if (R == NULL)
	    {
	      goto error;
	    }

	  /* Log the newly allocated pageid for deallocation for undo purposes */
	  if (btree_is_new_file (&btid_int) != true)
	    {
	      /* we don't do undo logging for new files */
	      pageid_struct.vpid = R_vpid;
	      pageid_struct.vfid.fileid = btid->vfid.fileid;
	      pageid_struct.vfid.volid = btid->vfid.volid;
	      log_append_undo_data2 (thread_p, RVBT_NEW_PGALLOC, &btid->vfid,
				     NULL, -1, sizeof (PAGEID_STRUCT),
				     &pageid_struct);
	    }

	  if (btree_split_node (thread_p, &btid_int, P, Q, R,
				&P_vpid, &Q_vpid, &R_vpid, p_slot_id,
				node_type, key, &child_vpid) != NO_ERROR)
	    {
	      goto error;
	    }

#if !defined(NDEBUG)
	  (void) spage_check_num_slots (thread_p, P);
	  (void) spage_check_num_slots (thread_p, Q);
	  (void) spage_check_num_slots (thread_p, R);
#endif

	  if (node_type == BTREE_LEAF_NODE)
	    {
	      assert (next_page == NULL);

	      next_page = btree_get_next_page (thread_p, R);
	      if (next_page != NULL)
		{
		  (void) pgbuf_check_page_ptype (thread_p, next_page,
						 PAGE_BTREE);

		  if (btree_set_vpid_previous_vpid
		      (thread_p, &btid_int, next_page, &R_vpid) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	    }

	  if (VPID_EQ (&child_vpid, &Q_vpid))
	    {
	      /* child page to be followed is Q */
	      pgbuf_unfix_and_init (thread_p, R);

	      if (btree_is_new_file (&btid_int))
		{
		  log_end_system_op (thread_p,
				     LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
		}
	      else
		{
		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
		}

	      top_op_active = 0;

	    }
	  else if (VPID_EQ (&child_vpid, &R_vpid))
	    {
	      /* child page to be followed is R */
	      pgbuf_unfix_and_init (thread_p, Q);

	      if (btree_is_new_file (&btid_int))
		{
		  log_end_system_op (thread_p,
				     LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
		}
	      else
		{
		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
		}

	      top_op_active = 0;

	      Q = R;
	      R = NULL;
	      Q_vpid = R_vpid;
	    }
	  else
	    {
	      assert (false);	/* is error ? */

	      pgbuf_unfix_and_init (thread_p, Q);
	      pgbuf_unfix_and_init (thread_p, R);

	      if (btree_is_new_file (&btid_int))
		{
		  log_end_system_op (thread_p,
				     LOG_RESULT_TOPOP_ATTACH_TO_OUTER);
		}
	      else
		{
		  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
		}

	      top_op_active = 0;

	      Q_vpid = child_vpid;
	      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			     PGBUF_UNCONDITIONAL_LATCH);
	      if (Q == NULL)
		{
		  goto error;
		}

	      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);
	    }

	  if (next_page)
	    {
	      pgbuf_unfix_and_init (thread_p, next_page);
	    }
	}

      /* release parent page P, and repeat the same operations from child
       * page Q on
       */
      pgbuf_unfix_and_init (thread_p, P);
      P = Q;
      Q = NULL;
      P_vpid = Q_vpid;

      key_cnt = btree_node_number_of_keys (P);

      /* node_type must be recalculated */
      header = btree_get_node_header (P);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      pnt_max_key_len = header->max_key_len;
      pnt_node_level = header->node_level;
    }				/* while */

  p_slot_id = NULL_SLOTID;

  /* find next OID for range locking */
  if (mvcc_lock_request == false && nextkey_lock_request == false)
    {
      goto key_insertion;
    }

  /* find next key */
  if (btree_search_leaf_page (thread_p, &btid_int, P, key, &p_slot_id))
    {
      /* key has been found */
      key_found = true;
      assert (p_slot_id > 0);
      if (!mvcc_Enabled && !BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));

	  /* key already exists, skip next key locking in non-unique
	     indexes */
	  if (next_lock_flag == true)
	    {
	      assert (!OID_ISNULL (&saved_N_oid));
	      assert (!OID_ISNULL (&saved_N_class_oid));
	      lock_unlock_object_donot_move_to_non2pl (thread_p,
						       &saved_N_oid,
						       &saved_N_class_oid,
						       NS_LOCK);
	      next_lock_flag = false;
	      OID_SET_NULL (&saved_N_oid);
	      OID_SET_NULL (&saved_N_class_oid);
	    }
	  /* no need to lock next key during this call */
	  next_key_granted_mode = NULL_LOCK;
	  OID_SET_NULL (&N_oid);
	  goto curr_key_locking;
	}

      slot_id = p_slot_id;
    }
  else
    {
      /* key has not been found */
      key_found = false;
      if (p_slot_id == NULL_SLOTID)
	{
	  goto error;
	}

      slot_id = p_slot_id - 1;	/* if not found fetch current position */
    }

  if (mvcc_Enabled)
    {
      goto curr_key_locking;
    }

  memset (&tmp_bts, 0, sizeof (BTREE_SCAN));
  BTREE_INIT_SCAN (&tmp_bts);
  tmp_bts.C_page = P;
  tmp_bts.slot_id = slot_id;

  ret_val =
    btree_find_next_index_record_holding_current (thread_p, &tmp_bts,
						  &peek_rec);
  if (ret_val != NO_ERROR)
    {
      goto error;
    }

  is_last_key = false;		/* init */
  if (tmp_bts.C_page != NULL && tmp_bts.C_page != P)
    {
      next_page_flag = true;
      N_vpid = tmp_bts.C_vpid;
      N = tmp_bts.C_page;
    }
  else
    {
      next_page_flag = false;
      /* tmp_bts.C_page is NULL if next record is not exists */
      if (tmp_bts.C_page == NULL)
	{
	  is_last_key = true;
	}
    }

  tmp_bts.C_page = NULL;	/* this page is pointed by P (or N) */

  if (is_last_key)
    {
      assert (next_page_flag == false);
      /* The first entry of the root page is used as the next OID */
      N_oid.volid = btid->vfid.volid;
      N_oid.pageid = btid->root_pageid;
      N_oid.slotid = 0;
      if (BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  COPY_OID (&N_class_oid, &btid_int.topclass_oid);
	}
      else
	{
	  COPY_OID (&N_class_oid, &class_oid);
	}
    }
  else
    {
      btree_read_record (thread_p, &btid_int, P, &peek_rec, NULL,
			 &leaf_pnt, BTREE_LEAF_NODE, &dummy, &offset,
			 PEEK_KEY_VALUE, NULL);
      (void) btree_leaf_get_first_oid (&btid_int, &peek_rec, &N_oid,
				       &N_class_oid, NULL);
      btree_make_pseudo_oid (N_oid.pageid, N_oid.slotid, N_oid.volid,
			     btid_int.sys_btid, &N_oid);

      if (BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  assert (!OID_ISNULL (&N_class_oid));

	  if (OID_EQ (&N_class_oid, &class_oid)
	      && IS_WRITE_EXCLUSIVE_LOCK (class_lock))
	    {
	      if (next_lock_flag == true)
		{
		  assert (!OID_ISNULL (&saved_N_oid));
		  assert (!OID_ISNULL (&saved_N_class_oid));
		  lock_unlock_object_donot_move_to_non2pl (thread_p,
							   &saved_N_oid,
							   &saved_N_class_oid,
							   NS_LOCK);
		  next_lock_flag = false;
		  OID_SET_NULL (&saved_N_oid);
		  OID_SET_NULL (&saved_N_class_oid);
		}

	      if (N != NULL)
		{
		  pgbuf_unfix_and_init (thread_p, N);
		}

	      goto curr_key_locking;
	    }
	}
      else
	{
	  COPY_OID (&N_class_oid, &class_oid);
	}
    }

  if (next_lock_flag == true)
    {
      if (OID_EQ (&saved_N_oid, &N_oid))
	{
	  if (next_page_flag == true)
	    {
	      pgbuf_unfix_and_init (thread_p, N);
	      next_page_flag = false;
	    }
	  /* keep the old value of next_key_granted_mode, needed at
	     curr key locking */
	  goto curr_key_locking;
	}

      assert (!OID_ISNULL (&saved_N_oid));
      assert (!OID_ISNULL (&saved_N_class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p, &saved_N_oid,
					       &saved_N_class_oid, NS_LOCK);
      next_lock_flag = false;
      OID_SET_NULL (&saved_N_oid);
      OID_SET_NULL (&saved_N_class_oid);
    }

  /* CONDITIONAL lock request */
  ret_val =
    lock_hold_object_instant_get_granted_mode (thread_p, &N_oid, &N_class_oid,
					       NS_LOCK,
					       &next_key_granted_mode);
  if (ret_val == LK_GRANTED)
    {
      if (next_page_flag == true)
	{
	  pgbuf_unfix_and_init (thread_p, N);
	  next_page_flag = false;
	}
    }
  else if (ret_val == LK_NOTGRANTED)
    {
      /* save some information for validation checking
       * after UNCONDITIONAL lock request
       */
      temp_lsa = pgbuf_get_lsa (P);
      LSA_COPY (&saved_plsa, temp_lsa);
      pgbuf_unfix_and_init (thread_p, P);
      if (next_page_flag == true)
	{
	  temp_lsa = pgbuf_get_lsa (N);
	  LSA_COPY (&saved_nlsa, temp_lsa);
	  pgbuf_unfix_and_init (thread_p, N);
	}
      COPY_OID (&saved_N_oid, &N_oid);
      COPY_OID (&saved_N_class_oid, &N_class_oid);

      assert (P == NULL);
      assert (Q == NULL);
      assert (R == NULL);
      assert (N == NULL);

      /* UNCONDITIONAL lock request */
      ret_val =
	lock_object_with_btid_get_granted_mode (thread_p, &N_oid,
						&N_class_oid, btid, NS_LOCK,
						LK_UNCOND_LOCK,
						&next_key_granted_mode);
      if (ret_val != LK_GRANTED)
	{
	  goto error;
	}

      next_lock_flag = true;

      /* validation checking after the unconditional lock acquisition
       * in this implementation, only PageLSA of the page is checked.
       * it means that if the PageLSA has not been changed,
       * the page image does not changed
       * during the unconditional next key lock acquisition.
       * so, the next lock that is acquired is valid.
       * if we give more accurate and precise checking condition,
       * the operation that traverse the tree can be reduced.
       */
      P = pgbuf_fix_without_validation (thread_p, &P_vpid, OLD_PAGE,
					PGBUF_LATCH_WRITE,
					PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      temp_lsa = pgbuf_get_lsa (P);
      if (!btree_leaf_lsa_eq (thread_p, &saved_plsa, temp_lsa))
	{
	  pgbuf_unfix_and_init (thread_p, P);
	  next_page_flag = false;

	  assert (next_lock_flag == true || curr_lock_flag == true);
	  goto start_point;
	}

      /* The first leaf page is valid */

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      if (next_page_flag == true)
	{
	  N = pgbuf_fix_without_validation (thread_p, &N_vpid, OLD_PAGE,
					    PGBUF_LATCH_READ,
					    PGBUF_UNCONDITIONAL_LATCH);
	  if (N == NULL)
	    {
	      goto error;
	    }

	  temp_lsa = pgbuf_get_lsa (N);
	  if (!btree_leaf_lsa_eq (thread_p, &saved_nlsa, temp_lsa))
	    {
	      pgbuf_unfix_and_init (thread_p, P);
	      pgbuf_unfix_and_init (thread_p, N);
	      next_page_flag = false;

	      assert (next_lock_flag == true || curr_lock_flag == true);
	      goto start_point;
	    }

	  /* The next leaf page is valid */

	  (void) pgbuf_check_page_ptype (thread_p, N, PAGE_BTREE);

	  pgbuf_unfix_and_init (thread_p, N);
	  next_page_flag = false;
	}

      /* valid point for key insertion
       * only the page P is currently locked and fetched
       */
    }
  else
    {
      goto error;
    }

curr_key_locking:
  assert (!mvcc_Enabled || BTREE_IS_UNIQUE (btid_int.unique_pk));

  if (mvcc_Enabled)
    {
      if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
	{
	  goto key_insertion;
	}

      result = BTREE_KEY_NOTFOUND;
      if (key_found == true)
	{
	  assert (p_slot_id > 0);
	  /* check whether the key is visible */
	  if (spage_get_record (P, p_slot_id, &peek_rec, PEEK) != S_SUCCESS)
	    {
	      goto error;
	    }
	  btree_read_record (thread_p, &btid_int, P, &peek_rec, NULL,
			     &leaf_pnt, BTREE_LEAF_NODE, &dummy, &offset,
			     PEEK_KEY_VALUE, NULL);
	  result =
	    btree_key_find_first_visible_row (thread_p, &btid_int, &peek_rec,
					      offset, BTREE_LEAF_NODE,
					      &C_oid, &C_class_oid, 1);
	  switch (result)
	    {
	    case BTREE_KEY_NOTFOUND:
	      /* there is no active key, must lock the new oid */
	      COPY_OID (&C_class_oid, &class_oid);
	      COPY_OID (&C_oid, oid);
	      break;

	    case BTREE_KEY_FOUND:
	    case BTREE_ACTIVE_KEY_FOUND:
	      /* the row is updated by concurrent transaction, need to lock
	       * C_oid, unique constraint violation is checked at insert
	       */
	      break;

	    case BTREE_ERROR_OCCURRED:
	      goto error;
	    }
	}
      else
	{
	  /* must lock the new oid */
	  COPY_OID (&C_class_oid, &class_oid);
	  COPY_OID (&C_oid, oid);
	}

      if (BTREE_IS_UNIQUE (btid_int.unique_pk))
	{
	  assert_release (!OID_ISNULL (&C_class_oid));

	  if (OID_EQ (&C_class_oid, &class_oid) && class_lock == X_LOCK)
	    {
	      goto key_insertion;
	    }
	}

      /* preserve in MVCC */
      if (curr_lock_flag == true)
	{
	  assert (BTREE_IS_UNIQUE (btid_int.unique_pk));
	  if (OID_EQ (&saved_C_oid, &C_oid))
	    {
	      goto key_insertion;
	    }

	  lock_unlock_object (thread_p, &saved_C_oid, &saved_C_class_oid,
			      current_lock, true);

	  if (result == BTREE_KEY_NOTFOUND)
	    {
	      /* other previous insert has rollback or has delete and commit */
	      if ((logtb_find_current_isolation (thread_p)
		   >= TRAN_REPEATABLE_READ)
		  && (btree_find_oid_from_leaf (&btid_int, &peek_rec, offset,
						&saved_C_oid, NULL,
						&mvcc_local_rec_header)
		      != NOT_FOUND))
		{
		  if (mvcc_snapshot == NULL)
		    {
		      mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
		    }

		  if ((mvcc_snapshot)->snapshot_fnc (thread_p,
						     &mvcc_local_rec_header,
						     mvcc_snapshot))
		    {
		      /* can't have 2 OID visible objects in unique, as follow:
		       *  1. the previous locked object - modified by other
		       *  transaction
		       *  2. the current inserted OID
		       */
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			      ER_MVCC_SERIALIZABLE_CONFLICT, 0);

		      goto error;
		    }
		}
	    }
	  curr_lock_flag = false;
	  OID_SET_NULL (&saved_C_oid);
	  OID_SET_NULL (&saved_C_class_oid);
	}
      else
	{
	  if ((key_found == true) && (result == BTREE_KEY_NOTFOUND)
	      && (logtb_find_current_isolation (thread_p)
		  >= TRAN_REPEATABLE_READ))
	    {
	      /* check MVCC snapshot to be sure that can't have 2 OID visible
	       * objects in unique
	       */
	      int max_visible_oids = 1;
	      if (mvcc_snapshot == NULL)
		{
		  mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
		}

	      if (btree_leaf_mvcc_get_num_visible_oids
		  (thread_p, &btid_int, &peek_rec, offset, BTREE_LEAF_NODE,
		   &max_visible_oids, mvcc_snapshot) > 0)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_MVCC_SERIALIZABLE_CONFLICT, 0);

		  goto error;
		}
	      else if (!VPID_ISNULL (&leaf_pnt.ovfl))
		{
		  int num_visible_oids;

		  if (btree_mvcc_get_num_visible_oids_from_all_ovf
		      (thread_p, &btid_int, &leaf_pnt.ovfl,
		       &num_visible_oids,
		       &max_visible_oids, mvcc_snapshot) != NO_ERROR)
		    {
		      goto error;
		    }

		  if (num_visible_oids > 0)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			      ER_MVCC_SERIALIZABLE_CONFLICT, 0);
		      goto error;
		    }
		}
	    }
	}

      current_lock = X_LOCK;
      ret_val =
	lock_object_with_btid (thread_p, &C_oid, &C_class_oid, btid,
			       X_LOCK, LK_COND_LOCK);

      if (ret_val == LK_GRANTED)
	{
	  curr_lock_flag = true;
	}
      else if (ret_val == LK_NOTGRANTED_DUE_TIMEOUT)
	{
	  temp_lsa = pgbuf_get_lsa (P);
	  LSA_COPY (&saved_plsa, temp_lsa);
	  pgbuf_unfix_and_init (thread_p, P);

	  COPY_OID (&saved_C_oid, &C_oid);
	  COPY_OID (&saved_C_class_oid, &C_class_oid);

	  assert (P == NULL && Q == NULL && R == NULL);

	  /* UNCONDITIONAL lock request */
	  ret_val = lock_object_with_btid (thread_p, &C_oid, &C_class_oid,
					   btid, X_LOCK, LK_UNCOND_LOCK);
	  if (ret_val != LK_GRANTED)
	    {
	      goto error;
	    }
	  curr_lock_flag = true;

	  P = pgbuf_fix_without_validation (thread_p, &P_vpid, OLD_PAGE,
					    PGBUF_LATCH_WRITE,
					    PGBUF_UNCONDITIONAL_LATCH);
	  if (P == NULL)
	    {
	      goto error;
	    }

	  temp_lsa = pgbuf_get_lsa (P);
	  if (!LSA_EQ (&saved_plsa, temp_lsa))
	    {
	      pgbuf_unfix_and_init (thread_p, P);

	      assert (curr_lock_flag == true);
	      goto start_point;
	    }

	  if (result == BTREE_ACTIVE_KEY_FOUND)
	    {
	      /* check whether the other transaction key insertion has been
	       * committed/aborted
	       */
	      goto curr_key_locking;
	    }
	}
      else
	{
	  goto error;
	}

      /* Object is locked, may insert into key */
      goto key_insertion;
    }

  if (key_found == true)
    {
      if (spage_get_record (P, p_slot_id, &peek_rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}
      btree_read_record (thread_p, &btid_int, P, &peek_rec, NULL,
			 &leaf_pnt, BTREE_LEAF_NODE, &dummy, &offset,
			 PEEK_KEY_VALUE, NULL);
      (void) btree_leaf_get_first_oid (&btid_int, &peek_rec, &C_oid,
				       &C_class_oid, NULL);
      if (!BTREE_IS_UNIQUE (btid_int.unique_pk))	/* non-unique index */
	{
	  assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	  COPY_OID (&C_class_oid, &class_oid);
	}
    }
  else
    {
      COPY_OID (&C_class_oid, &class_oid);
      COPY_OID (&C_oid, oid);
      assert (curr_key_lock_escalation == NO_KEY_LOCK_ESCALATION);
    }

  btree_make_pseudo_oid (C_oid.pageid, C_oid.slotid, C_oid.volid,
			 btid_int.sys_btid, &C_oid);

  if (BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      assert_release (!OID_ISNULL (&C_class_oid));

      if (OID_EQ (&C_class_oid, &class_oid)
	  && IS_WRITE_EXCLUSIVE_LOCK (class_lock))
	{
	  goto key_insertion;
	}
    }

  if (curr_lock_flag == true)
    {
      if (OID_EQ (&saved_C_oid, &C_oid))
	{
	  /* current key already locked, key_found = true */
	  if (!BTREE_IS_UNIQUE (btid_int.unique_pk)
	      && current_lock == NS_LOCK)
	    {
	      assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	      if (prev_tot_hold_mode == NULL_LOCK)
		{
		  /* compute prev_tot_hold_mode and curr_key_many_locks_needed
		     again, since the current values could be inconsistent.
		     keep the old current_lock value */
		  tran_index = LOG_FIND_THREAD_TRAN_INDEX (thread_p);
		  prev_tot_hold_mode =
		    lock_get_all_except_transaction (&C_oid, &C_class_oid,
						     tran_index);
		  if (prev_tot_hold_mode == NS_LOCK)
		    {
		      curr_key_many_locks_needed = true;
		    }
		  else
		    {
		      curr_key_many_locks_needed = false;
		    }
		}
	      else if (prev_tot_hold_mode == NS_LOCK)
		{
		  /* if the others transaction lock mode was NS_LOCK before
		     unconditional lock request and now is NULL_LOCK,
		     we can acquire a NS-lock on one next pseudo-OID,
		     even if is not neccessary. */
		}
	    }

	  goto curr_key_lock_consistency;
	}

      assert (!OID_ISNULL (&saved_C_oid));
      assert (!OID_ISNULL (&saved_C_class_oid));
      lock_unlock_object (thread_p, &saved_C_oid, &saved_C_class_oid,
			  current_lock, true);
      if (curr_key_lock_escalation != NO_KEY_LOCK_ESCALATION)
	{
	  curr_key_lock_escalation = NO_KEY_LOCK_ESCALATION;
	}

      curr_lock_flag = false;
      OID_SET_NULL (&saved_C_oid);
      OID_SET_NULL (&saved_C_class_oid);
    }

  current_lock = ((!BTREE_IS_UNIQUE (btid_int.unique_pk) && key_found) ||
		  (next_key_granted_mode != S_LOCK &&
		   next_key_granted_mode != NX_LOCK)) ? NS_LOCK : NX_LOCK;

curr_key_lock_promote:

  if (current_lock == NX_LOCK)
    {
      /* lock state replication via next key locking */
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));
      ret_val =
	lock_object_with_btid (thread_p, &C_oid, &C_class_oid, btid,
			       current_lock, LK_COND_LOCK);
      prev_tot_hold_mode = NULL_LOCK;
      curr_key_many_locks_needed = false;
    }
  else
    {
      /* lock and get the other transactions lock mode */
      ret_val =
	lock_btid_object_get_prev_total_hold_mode (thread_p, &C_oid,
						   &C_class_oid,
						   btid, current_lock,
						   LK_COND_LOCK,
						   &prev_tot_hold_mode,
						   &curr_key_lock_escalation);

      curr_key_many_locks_needed = false;
      if (curr_key_lock_escalation == NO_KEY_LOCK_ESCALATION)
	{
	  if (!BTREE_IS_UNIQUE (btid_int.unique_pk)
	      && key_found && prev_tot_hold_mode == NS_LOCK)
	    {
	      assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
	      curr_key_many_locks_needed = true;
	    }
	}
      else
	{
	  /* key lock already escalated or key lock escalation needed,
	   * key lock escalation make sense only for multiple oids key
	   * key lock is escalated when the lock on PSEUDO-OID attached
	   * to the first OID from key buffer is escalated from NS to NX
	   */
	  if (ret_val == LK_GRANTED)
	    {
	      current_lock = NX_LOCK;
	      assert (lock_has_lock_on_object (&C_oid, &C_class_oid,
					       LOG_FIND_THREAD_TRAN_INDEX
					       (thread_p), NX_LOCK) == 1);
	    }
	}
    }

  if (ret_val == LK_GRANTED)
    {
      curr_lock_flag = true;
    }
  else if (ret_val == LK_NOTGRANTED_DUE_TIMEOUT)
    {
      temp_lsa = pgbuf_get_lsa (P);
      LSA_COPY (&saved_plsa, temp_lsa);
      pgbuf_unfix_and_init (thread_p, P);

      COPY_OID (&saved_C_oid, &C_oid);
      COPY_OID (&saved_C_class_oid, &C_class_oid);

      if (OID_ISNULL (&saved_N_oid))
	{
	  COPY_OID (&saved_N_oid, &N_oid);
	  COPY_OID (&saved_N_class_oid, &N_class_oid);
	}

      assert (P == NULL);
      assert (Q == NULL);
      assert (R == NULL);
      assert (N == NULL);

      /* UNCONDITIONAL lock request */
      if (current_lock == NX_LOCK
	  && curr_key_lock_escalation == NO_KEY_LOCK_ESCALATION)
	{
	  /* prev_tot_hold_mode set to NULL_LOCK */
	  assert (!OID_ISNULL (&C_oid));
	  assert (!OID_ISNULL (&C_class_oid));
	  ret_val = lock_object_with_btid (thread_p, &C_oid, &C_class_oid,
					   btid, current_lock,
					   LK_UNCOND_LOCK);
	}
      else
	{
	  /* current_lock set to NS_LOCK */
	  ret_val = lock_btid_object_get_prev_total_hold_mode
	    (thread_p, &C_oid, &C_class_oid, btid, current_lock,
	     LK_UNCOND_LOCK, &prev_tot_hold_mode, &curr_key_lock_escalation);

	  curr_key_many_locks_needed = false;
	  if (curr_key_lock_escalation == NO_KEY_LOCK_ESCALATION)
	    {
	      if (!BTREE_IS_UNIQUE (btid_int.unique_pk)
		  && key_found && prev_tot_hold_mode == NS_LOCK)
		{
		  assert (!BTREE_IS_PRIMARY_KEY (btid_int.unique_pk));
		  curr_key_many_locks_needed = true;
		}
	    }
	  else if (ret_val == LK_GRANTED)
	    {
	      assert (lock_has_lock_on_object (&C_oid, &C_class_oid,
					       LOG_FIND_THREAD_TRAN_INDEX
					       (thread_p), NX_LOCK) == 1);
	      /* either NX_LOCK on the current key or X_LOCK on the table
	       * is held.
	       */
	      current_lock = NX_LOCK;
	    }
	}

      if (ret_val != LK_GRANTED)
	{
	  goto error;
	}
      curr_lock_flag = true;

      P = pgbuf_fix_without_validation (thread_p, &P_vpid, OLD_PAGE,
					PGBUF_LATCH_WRITE,
					PGBUF_UNCONDITIONAL_LATCH);
      if (P == NULL)
	{
	  goto error;
	}

      temp_lsa = pgbuf_get_lsa (P);
      if (!btree_leaf_lsa_eq (thread_p, &saved_plsa, temp_lsa))
	{
	  pgbuf_unfix_and_init (thread_p, P);

	  assert (next_lock_flag == true || curr_lock_flag == true);
	  goto start_point;
	}

      (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

      if (curr_key_many_locks_needed ||
	  curr_key_lock_escalation != NO_KEY_LOCK_ESCALATION)
	{
	  /* still have to lock pseudo OIDS, peek the record again */
	  assert (p_slot_id > 0);
	  if (spage_get_record (P, p_slot_id, &peek_rec, PEEK) != S_SUCCESS)
	    {
	      goto error;
	    }

	  btree_read_record (thread_p, &btid_int, P, &peek_rec, NULL,
			     &leaf_pnt, BTREE_LEAF_NODE, &dummy,
			     &offset, PEEK_KEY_VALUE, NULL);
	}
    }
  else
    {
      goto error;
    }

curr_key_lock_consistency:
  if (curr_key_many_locks_needed)
    {
      assert (peek_rec.data != NULL);
      assert (offset != -1);
      if (peek_rec.data != NULL)
	{
	  if (btree_insert_lock_curr_key_remaining_pseudo_oid
	      (thread_p, &btid_int, &peek_rec, offset, &leaf_pnt.ovfl,
	       oid, &C_class_oid) != NO_ERROR)
	    {
	      current_lock = NX_LOCK;
	      goto curr_key_lock_promote;
	    }
	}
    }

  if (curr_key_lock_escalation != NO_KEY_LOCK_ESCALATION)
    {
      /* key lock escalated
       * unlock remaining PSEUDO-OIDs of current key, previously NS-locked
       */
      assert (peek_rec.data != NULL);
      assert (curr_key_many_locks_needed == false);
      if (peek_rec.data != NULL)
	{
	  if (key_found == true)
	    {
	      assert (offset != -1);
	      if (btree_insert_unlock_curr_key_remaining_pseudo_oid
		  (thread_p, &btid_int, &peek_rec, offset, &leaf_pnt.ovfl,
		   &C_class_oid) != NO_ERROR)
		{
		  goto error;
		}
	    }
	}
    }

  /* valid point for key insertion
   * only the page P is currently locked and fetched
   */

key_insertion:

  /* a leaf page is reached, make the actual insertion in this page.
   * Because of the specific top-down splitting algorithm, there will be
   * no need to go up to parent pages, and it will always be possible to
   * make the insertion in this leaf page.
   */
  key_added_deleted = 0;

  if (prm_get_bool_value (PRM_ID_LOG_BTREE_OPS))
    {
      if (p_mvcc_rec_header)
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: insert %s "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d)) with "
			 "mvcc_info=%lld | %lld",
			 MVCC_IS_HEADER_DELID_VALID (p_mvcc_rec_header) ?
			 "delid" : "object",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid,
			 MVCC_GET_INSID (p_mvcc_rec_header),
			 MVCC_GET_DELID (p_mvcc_rec_header));
	}
      else
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: non-mvcc insert object "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d))",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid);
	}
    }
  ret_val = btree_insert_into_leaf (thread_p, &key_added_deleted, &btid_int,
				    P, key, &class_oid, oid, &P_vpid,
				    op_type, key_found, p_slot_id,
				    p_mvcc_rec_header);
  if (prm_get_bool_value (PRM_ID_LOG_BTREE_OPS))
    {
      if (p_mvcc_rec_header)
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: %s insert %s "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d)) with "
			 "mvcc_info=%lld | %lld",
			 ret_val == NO_ERROR ? "Successful" : "Failed",
			 MVCC_IS_HEADER_DELID_VALID (p_mvcc_rec_header) ?
			 "delid" : "object",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid,
			 MVCC_GET_INSID (p_mvcc_rec_header),
			 MVCC_GET_DELID (p_mvcc_rec_header));
	}
      else
	{
	  _er_log_debug (ARG_FILE_LINE, "DEBUG_BTREE: %s non-mvcc insert object "
			 "oid(%d, %d, %d) "
			 "class_oid(%d, %d, %d) and btid(%d, (%d, %d))",
			 ret_val == NO_ERROR ? "Successful" : "Failed",
			 oid->volid, oid->pageid, oid->slotid,
			 cls_oid->volid, cls_oid->pageid, cls_oid->slotid,
			 btid->root_pageid, btid->vfid.volid,
			 btid->vfid.fileid);
	}
    }

  if (ret_val != NO_ERROR)
    {
      /* defence code */
      if (ret_val == ER_BTREE_NO_SPACE)
	{
	  char *ptr = NULL;
	  FILE *fp = NULL;
	  size_t sizeloc;

	  assert (false);

	  fp = port_open_memstream (&ptr, &sizeloc);
	  if (fp)
	    {
	      btree_dump_page (thread_p, fp, &class_oid, &btid_int,
			       NULL, P, &P_vpid, 2, 2);
	      spage_dump (thread_p, fp, P, true);
	      port_close_memstream (fp, &ptr, &sizeloc);
	    }

	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_NO_SPACE, 2,
		  key_len, ptr);

	  if (ptr)
	    {
	      free (ptr);
	    }

	  if (retry_btree_no_space < 1)
	    {
	      /* ER_BTREE_NO_SPACE can be made by split node algorithm
	       * In this case, release resource and retry it one time.
	       */
	      assert (top_op_active == 0);
	      assert (Q == NULL);
	      assert (R == NULL);
	      assert (N == NULL);

	      pgbuf_unfix_and_init (thread_p, P);

	      retry_btree_no_space++;
	      goto start_point;
	    }
	}

      goto error;
    }

#if !defined(NDEBUG)
  (void) spage_check_num_slots (thread_p, P);
#endif

  /* if success, update max_key_len in page P */
  header = btree_get_node_header (P);
  if (header == NULL)
    {
      goto error;
    }

  key_len_in_page = BTREE_GET_KEY_LEN_IN_PAGE (key_len);
  if (key_len_in_page > header->max_key_len)
    {
      header->max_key_len = key_len_in_page;
      btree_node_header_redo_log (thread_p, &btid->vfid, P);

      pgbuf_set_dirty (thread_p, P, DONT_FREE);
    }

  assert (top_op_active == 0);
  assert (Q == NULL);
  assert (R == NULL);
  assert (N == NULL);

  pgbuf_unfix_and_init (thread_p, P);

  if (is_active && BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      if (op_type == SINGLE_ROW_INSERT || op_type == SINGLE_ROW_UPDATE
	  || op_type == SINGLE_ROW_MODIFY || op_type == SINGLE_ROW_DELETE)
	{
	  assert (op_type != SINGLE_ROW_DELETE || mvcc_Enabled == true);
	}
      else
	{
	  if (unique_stat_info == NULL)
	    {
	      goto error;
	    }
	  else
	    {
	      /* revert local statistical information */
	      if (key_added_deleted == 0)
		{
		  if (BTREE_INSERT_IS_LOGICAL_DELETE (p_mvcc_rec_header))
		    {
		      unique_stat_info->num_keys++;
		    }
		  else
		    {
		      unique_stat_info->num_keys--;
		    }
		}
	    }
	}
    }

  if (next_lock_flag == true)
    {
      assert (!mvcc_Enabled);
      assert (!OID_ISNULL (&N_oid));
      assert (!OID_ISNULL (&N_class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p, &N_oid,
					       &N_class_oid, NS_LOCK);
    }

  mnt_bt_inserts (thread_p);

  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

#if !defined(NDEBUG)
  if (prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) & BTREE_DEBUG_DUMP_FULL)
    {
      btree_dump (thread_p, stdout, btid, 2);
    }
#endif

  return key;

error:
  /* do not unfix P, Q, R before to-pop rollback */
  if (top_op_active)
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
    }

  if (P)
    {
      pgbuf_unfix_and_init (thread_p, P);
    }
  if (Q)
    {
      pgbuf_unfix_and_init (thread_p, Q);
    }
  if (R)
    {
      pgbuf_unfix_and_init (thread_p, R);
    }
  if (N)
    {
      pgbuf_unfix_and_init (thread_p, N);
    }
  if (next_page)
    {
      pgbuf_unfix_and_init (thread_p, next_page);
    }

  if (next_lock_flag)
    {
      assert (!OID_ISNULL (&N_oid));
      assert (!OID_ISNULL (&N_class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p, &N_oid,
					       &N_class_oid, NS_LOCK);
    }
  if (curr_lock_flag)
    {
      assert (!OID_ISNULL (&C_oid));
      assert (!OID_ISNULL (&C_class_oid));

      lock_unlock_object_donot_move_to_non2pl (thread_p, &C_oid,
					       &C_class_oid, current_lock);
    }

  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

  return NULL;
}

/*
 * btree_update () -
 *   return: NO_ERROR
 *   btid(in): B+tree index identifier
 *   old_key(in): Old key value
 *   new_key(in): New key value
 *   locked_keys(in): keys already locked by the current transaction
 *		      when search
 *   cls_oid(in):
 *   oid(in): Object identifier to be updated
 *   new_oid(in): Object identifier after it was updated
 *   op_type(in):
 *   unique_stat_info(in):
 *   unique(in):
 *   p_mvcc_rec_header(in/out): array of MVCC_REC_HEADER of size 2 or NULL
 *
 * Note: Deletes the <old_key, oid> key-value pair from the B+tree
 * index and inserts the <new_key, oid> key-value pair to the
 * B+tree index which results in the update of the specified
 * index entry for the given object identifier.
 */
int
btree_update (THREAD_ENTRY * thread_p, BTID * btid, DB_VALUE * old_key,
	      DB_VALUE * new_key, BTREE_LOCKED_KEYS locked_keys,
	      OID * cls_oid, OID * oid, OID * new_oid, int op_type,
	      BTREE_UNIQUE_STATS * unique_stat_info, int *unique,
	      MVCC_REC_HEADER * p_mvcc_rec_header)
{
  int ret = NO_ERROR;

  assert (old_key != NULL);
  assert (new_key != NULL);
  assert (unique != NULL);

  if (p_mvcc_rec_header != NULL && !OID_EQ (oid, new_oid))
    {
      /* in MVCC, logical deletion means DEL_ID insertion */
      /* Note that it is possible that update "in-place" is done instead of
       * standard MVCC update, in which case the "logical" deletion is no
       * longer required.
       */
      if (btree_insert (thread_p, btid, old_key, cls_oid, oid, op_type,
			unique_stat_info, unique, &p_mvcc_rec_header[0])
	  == NULL)
	{
	  goto exit_on_error;
	}
    }
  else
    {
      /* TODO: We should pass insert MVCCID here...? */
      /* TODO: MVCC_BTREE_DELETE_OBJECT is removed due to recovery issue
       *       regarding MVCCID. Must find a solution to recover MVCC info on
       *       rollback (otherwise we will have inconsistencies regarding
       *       visibility).
       */
      /* MVCC_BTREE_OP_ARGUMENTS mvcc_args, *mvcc_args_p = NULL;
         if (mvcc_Enabled)
         {
         mvcc_args_p = &mvcc_args;
         mvcc_args_p->purpose = MVCC_BTREE_DELETE_OBJECT;
         } */
      if (btree_delete (thread_p, btid, old_key, cls_oid, oid, locked_keys,
			unique, op_type, unique_stat_info,
			NULL /* mvcc_args_p */ )
	  == NULL)
	{
	  /* if the btree we are updating is a btree for unique attributes
	   * it is possible that the btree update has already been performed
	   * via the template unique checking.
	   * In this case, we will ignore the error from btree_delete
	   */
	  if (*unique && er_errid () == ER_BTREE_UNKNOWN_KEY)
	    {
	      goto end;
	    }

	  goto exit_on_error;
	}
    }

  {
    MVCC_REC_HEADER *p_local_rec_header = NULL;
    if (p_mvcc_rec_header != NULL)
      {
	p_local_rec_header = &p_mvcc_rec_header[1];
      }

    if (btree_insert (thread_p, btid, new_key,
		      cls_oid, new_oid, op_type, unique_stat_info, unique,
		      p_local_rec_header) == NULL)
      {
	goto exit_on_error;
      }
  }

#if 0
  {
    BTREE_CHECKSCAN bt_checkscan;
    DISK_ISVALID isvalid = DISK_VALID;

    /* start a check-scan on index */
    if (btree_keyoid_checkscan_start
	(thread_p, btid, &bt_checkscan) != NO_ERROR)
      {
	goto exit_on_error;
      }

    if (!DB_IS_NULL (old_key) && !btree_multicol_key_is_null (old_key))
      {
	isvalid = btree_keyoid_checkscan_check (thread_p,
						&bt_checkscan,
						cls_oid, old_key, oid);

	if (er_errid () == ER_INTERRUPTED)
	  {
	    /* in case of user interrupt */
	    ;			/* do not check isvalid */
	  }
	else
	  {
	    assert (isvalid == DISK_INVALID);	/* not found */
	  }
      }

    if (!DB_IS_NULL (new_key) && !btree_multicol_key_is_null (new_key))
      {
	isvalid = btree_keyoid_checkscan_check (thread_p,
						&bt_checkscan,
						cls_oid, new_key, new_oid);

	if (er_errid () == ER_INTERRUPTED)
	  {
	    /* in case of user interrupt */
	    ;			/* do not check isvalid */
	  }
	else
	  {
	    assert (isvalid == DISK_VALID);	/* found */
	  }
      }

    /* close the index check-scan */
    btree_keyoid_checkscan_end (thread_p, &bt_checkscan);
  }
#endif

end:

  mnt_bt_updates (thread_p);

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_reflect_unique_statistics () -
 *   return: NO_ERROR
 *   unique_stat_info(in):
 *   only_active_tran(in): if true then reflect statistics only if transaction
 *			   is active
 *
 * Note: This function reflects the given local statistical
 * information into the global statistical information
 * saved in a root page of corresponding unique index.
 */
int
btree_reflect_unique_statistics (THREAD_ENTRY * thread_p,
				 BTREE_UNIQUE_STATS * unique_stat_info,
				 bool only_active_tran)
{
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  char *redo_data = NULL;
  int ret = NO_ERROR;

  /* check if unique_stat_info is NULL */
  if (unique_stat_info == NULL)
    {
      return ER_FAILED;
    }

  /* fix the root page */
  root_vpid.pageid = unique_stat_info->btid.root_pageid;
  root_vpid.volid = unique_stat_info->btid.vfid.volid;
  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  /* read the root information */
  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  if (root_header->num_nulls != -1)
    {
      assert_release (BTREE_IS_UNIQUE (root_header->unique_pk));

      if (!only_active_tran || logtb_is_current_active (thread_p))
	{
	  /* update header information */
	  ret = btree_change_root_header_delta (thread_p,
						&unique_stat_info->btid.vfid,
						root,
						unique_stat_info->num_nulls,
						unique_stat_info->num_oids,
						unique_stat_info->num_keys);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  /* set the root page as dirty page */
	  pgbuf_set_dirty (thread_p, root, DONT_FREE);
	}
    }

  /* free the root page */
  pgbuf_unfix_and_init (thread_p, root);

  return ret;

exit_on_error:

  if (root != NULL)
    {
      pgbuf_unfix_and_init (thread_p, root);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_locate_key () - Locate a key node and position
 *   return: int true: key_found, false: key_not found
 *               (if error, false and slot_id = NULL_SLOTID)
 *   btid_int(in): B+tree index identifier
 *   key(in): Key to locate
 *   pg_vpid(out): Set to the page identifier that contains the key or should
 *                 contain the key if the key was to be inserted.
 *   slot_id(out): Set to the number (position) of the record that contains the
 *                 key or would contain the key if the key was to be inserted.
 *   found_p(out):
 *
 * Note: Searchs the B+tree index to locate the page and record that
 * contains the key, or would contain the key if the key was to be located.
 */
static PAGE_PTR
btree_locate_key (THREAD_ENTRY * thread_p, BTID_INT * btid_int,
		  DB_VALUE * key, VPID * pg_vpid, INT16 * slot_id,
		  bool * found_p)
{
  PAGE_PTR P = NULL, Q = NULL;
  VPID P_vpid, Q_vpid;
  INT16 p_slot_id, root_level = 0, depth = 0;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  assert (btid_int != NULL);
  assert (btid_int->sys_btid != NULL);

  *found_p = false;
  *slot_id = NULL_SLOTID;

#if !defined(NDEBUG)
  if (key == NULL || DB_IS_NULL (key) || btree_multicol_key_is_null (key))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_NULL_KEY, 0);
      goto error;
    }

  if (BTREE_INVALID_INDEX_ID (btid_int->sys_btid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_INDEX_ID, 3,
	      btid_int->sys_btid->vfid.fileid, btid_int->sys_btid->vfid.volid,
	      btid_int->sys_btid->root_pageid);
      goto error;
    }
#endif

  P_vpid.volid = btid_int->sys_btid->vfid.volid;	/* read the root page */
  P_vpid.pageid = btid_int->sys_btid->root_pageid;
  P = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		 PGBUF_UNCONDITIONAL_LATCH);
  if (P == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, P, PAGE_BTREE);

  root_header = btree_get_root_header (P);
  if (root_header == NULL)
    {
      goto error;
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  while (node_type == BTREE_NON_LEAF_NODE)
    {
      depth++;

      /* get the child page to follow */
      if (btree_search_nonleaf_page (thread_p, btid_int, P, key,
				     &p_slot_id, &Q_vpid) != NO_ERROR)
	{
	  goto error;
	}

      Q = pgbuf_fix (thread_p, &Q_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		     PGBUF_UNCONDITIONAL_LATCH);
      if (Q == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, Q, PAGE_BTREE);

      pgbuf_unfix_and_init (thread_p, P);

      /* read the header record */
      header = btree_get_node_header (Q);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      P = Q;
      Q = NULL;
      P_vpid = Q_vpid;
    }

  assert_release (root_level == depth + 1);

  /* leaf page is reached */
  *found_p = btree_search_leaf_page (thread_p, btid_int, P, key, slot_id);
  *pg_vpid = P_vpid;

  /* NOTE that we do NOT release the page latch on P here */
  return P;

error:

  if (P)
    {
      pgbuf_unfix_and_init (thread_p, P);
    }
  if (Q)
    {
      pgbuf_unfix_and_init (thread_p, Q);
    }

  return NULL;
}

/*
 * btree_find_lower_bound_leaf () -
 *   return: NO_ERROR
 *   BTS(in):
 *   stat_info(in):
 *
 * Note: Find the first/last leaf page of the B+tree index.
 */
static int
btree_find_lower_bound_leaf (THREAD_ENTRY * thread_p, BTREE_SCAN * BTS,
			     BTREE_STATS * stat_info_p)
{
  int key_cnt;
  int ret = NO_ERROR;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  if (BTS->use_desc_index)
    {
      assert_release (stat_info_p == NULL);
      BTS->C_page = btree_find_rightmost_leaf (thread_p,
					       BTS->btid_int.sys_btid,
					       &BTS->C_vpid, stat_info_p);
    }
  else
    {
      BTS->C_page = btree_find_leftmost_leaf (thread_p,
					      BTS->btid_int.sys_btid,
					      &BTS->C_vpid, stat_info_p);
    }

  if (BTS->C_page == NULL)
    {
      goto exit_on_error;
    }

  /* get header information (key_cnt) */
  key_cnt = btree_node_number_of_keys (BTS->C_page);

  header = btree_get_node_header (BTS->C_page);
  if (header == NULL)
    {
      goto exit_on_error;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type != BTREE_LEAF_NODE)
    {
      assert_release (false);
      goto exit_on_error;
    }

  /* set slot id and OID position */
  if (BTS->use_desc_index)
    {
      BTS->slot_id = key_cnt;
    }
  else
    {
      BTS->slot_id = 1;
    }

  if (key_cnt == 0)
    {
      /* tree is empty; need to unfix current leaf page */
      ret = btree_find_next_index_record (thread_p, BTS);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      assert_release (BTREE_END_OF_SCAN (BTS));
    }
  else
    {
      BTS->oid_pos = 0;

      assert_release (BTS->slot_id <= key_cnt);
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_find_leftmost_leaf () -
 *   return: page pointer
 *   btid(in):
 *   pg_vpid(in):
 *   stat_info_p(in):
 *
 * Note: Find the page identifier for the first leaf page of the B+tree index.
 */
static PAGE_PTR
btree_find_leftmost_leaf (THREAD_ENTRY * thread_p, BTID * btid,
			  VPID * pg_vpid, BTREE_STATS * stat_info)
{
  return btree_find_boundary_leaf (thread_p, btid, pg_vpid, stat_info,
				   BTREE_BOUNDARY_FIRST);
}

/*
 * btree_find_rightmost_leaf () -
 *   return: page pointer
 *   btid(in):
 *   pg_vpid(in):
 *   stat_info(in):
 *
 * Note: Find the page identifier for the last leaf page of the B+tree index.
 */
static PAGE_PTR
btree_find_rightmost_leaf (THREAD_ENTRY * thread_p, BTID * btid,
			   VPID * pg_vpid, BTREE_STATS * stat_info)
{
  return btree_find_boundary_leaf (thread_p, btid, pg_vpid, stat_info,
				   BTREE_BOUNDARY_LAST);
}

/*
 * btree_find_boundary_leaf () -
 *   return: page pointer
 *   btid(in):
 *   pg_vpid(in):
 *   stat_info(in):
 *
 * Note: Find the page identifier for the first/last leaf page
 *       of the B+tree index.
 */
static PAGE_PTR
btree_find_boundary_leaf (THREAD_ENTRY * thread_p, BTID * btid,
			  VPID * pg_vpid, BTREE_STATS * stat_info,
			  BTREE_BOUNDARY where)
{
  PAGE_PTR P_page = NULL, C_page = NULL;
  VPID P_vpid, C_vpid;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  NON_LEAF_REC nleaf;
  RECDES rec;
  int key_cnt = 0, index = 0;
  int root_level = 0, depth = 0;

  VPID_SET_NULL (pg_vpid);

  /* read the root page */
  P_vpid.volid = btid->vfid.volid;
  P_vpid.pageid = btid->root_pageid;
  P_page = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		      PGBUF_UNCONDITIONAL_LATCH);
  if (P_page == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, P_page, PAGE_BTREE);

  root_header = btree_get_root_header (P_page);
  if (root_header == NULL)
    {
      goto error;
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  while (node_type == BTREE_NON_LEAF_NODE)
    {
      key_cnt = btree_node_number_of_keys (P_page);
      if (key_cnt <= 0)
	{			/* node record underflow */
	  er_log_debug (ARG_FILE_LINE,
			"btree_find_boundary_leaf: node key count"
			" underflow: %d.Operation Ignored.", key_cnt);
	  goto error;
	}

      assert (where == BTREE_BOUNDARY_FIRST || where == BTREE_BOUNDARY_LAST);
      if (where == BTREE_BOUNDARY_FIRST)
	{
	  index = 1;
	}
      else
	{
	  index = key_cnt;
	}

      depth++;

      /* get the child page to flow */
      assert (index > 0);
      if (spage_get_record (P_page, index, &rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf);
      C_vpid = nleaf.pnt;
      C_page = pgbuf_fix (thread_p, &C_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
      if (C_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, C_page, PAGE_BTREE);

      pgbuf_unfix_and_init (thread_p, P_page);

      key_cnt = btree_node_number_of_keys (C_page);

      header = btree_get_node_header (C_page);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      P_page = C_page;
      C_page = NULL;
      P_vpid = C_vpid;
    }

  if (key_cnt != 0)
    {
      goto end;			/* OK */
    }

again:

  header = btree_get_node_header (P_page);
  if (header == NULL)
    {
      goto error;
    }

  /* fix the next leaf page and set slot_id and oid_pos if it exists. */
  assert (where == BTREE_BOUNDARY_FIRST || where == BTREE_BOUNDARY_LAST);
  if (where == BTREE_BOUNDARY_FIRST)
    {
      C_vpid = header->next_vpid;	/* move backward */
    }
  else
    {
      C_vpid = header->prev_vpid;	/* move foward */
    }

  if (!VPID_ISNULL (&C_vpid))
    {
      C_page = pgbuf_fix (thread_p, &C_vpid, OLD_PAGE,
			  PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (C_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, C_page, PAGE_BTREE);

      /* unfix the previous leaf page if it is fixed. */
      if (P_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, P_page);
	  /* do not clear bts->P_vpid for UNCONDITIONAL lock request handling */
	}
    }

  /* check if the current leaf page has valid slots */
  if (C_page != NULL)
    {
      key_cnt = btree_node_number_of_keys (C_page);

      if (key_cnt <= 0)
	{			/* empty page */
	  P_page = C_page;
	  C_page = NULL;
	  goto again;
	}

      P_vpid = C_vpid;
      P_page = C_page;
    }

  /* NOTE that we do NOT release the page latch on P here */
end:

  *pg_vpid = P_vpid;

  assert_release (root_level == depth + 1);

  if (stat_info)
    {
      stat_info->height = root_level;
    }

  return P_page;

error:

  if (P_page)
    {
      pgbuf_unfix_and_init (thread_p, P_page);
    }
  if (C_page)
    {
      pgbuf_unfix_and_init (thread_p, C_page);
    }

  return NULL;
}

/*
 * btree_find_AR_sampling_leaf () -
 *   return: page pointer
 *   btid(in):
 *   pg_vpid(in):
 *   stat_info_p(in):
 *   found_p(out):
 *
 * Note: Find the page identifier via the Acceptance/Rejection Sampling
 *       leaf page of the B+tree index.
 * Note: Random Sampling from Databases
 *       (Chapter 3. Random Sampling from B+ Trees)
 */
static PAGE_PTR
btree_find_AR_sampling_leaf (THREAD_ENTRY * thread_p, BTID * btid,
			     VPID * pg_vpid, BTREE_STATS * stat_info_p,
			     bool * found_p)
{
  PAGE_PTR P_page = NULL, C_page = NULL;
  VPID P_vpid, C_vpid;
  int slot_id;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  NON_LEAF_REC nleaf;
  RECDES rec;
  int est_page_size, free_space;
  int key_cnt = 0;
  int root_level = 0, depth = 0;
  double prob = 1.0;		/* Acceptance probability */

  assert (stat_info_p != NULL);
  assert (found_p != NULL);

  *found_p = false;		/* init */

  VPID_SET_NULL (pg_vpid);

  /* read the root page */
  P_vpid.volid = btid->vfid.volid;
  P_vpid.pageid = btid->root_pageid;
  P_page = pgbuf_fix (thread_p, &P_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		      PGBUF_UNCONDITIONAL_LATCH);
  if (P_page == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, P_page, PAGE_BTREE);

  key_cnt = btree_node_number_of_keys (P_page);

  root_header = btree_get_root_header (P_page);
  if (root_header == NULL)
    {
      goto error;
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  est_page_size = (int) (DB_PAGESIZE - (spage_header_size () +
					sizeof (BTREE_NODE_HEADER) +
					spage_slot_size ()));
  assert (est_page_size > 0);

  while (node_type == BTREE_NON_LEAF_NODE)
    {
      depth++;

      /* get the randomized child page to follow */

      if (key_cnt <= 0)
	{			/* node record underflow */
	  er_log_debug (ARG_FILE_LINE, "btree_find_AR_sampling_leaf:"
			" node key count underflow: %d. Operation Ignored.",
			key_cnt);
	  goto error;
	}

      slot_id = (int) (drand48 () * key_cnt);
      slot_id = MAX (slot_id, 1);

      assert (slot_id > 0);
      if (spage_get_record (P_page, slot_id, &rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}

      btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf);
      C_vpid = nleaf.pnt;
      C_page = pgbuf_fix (thread_p, &C_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			  PGBUF_UNCONDITIONAL_LATCH);
      if (C_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, C_page, PAGE_BTREE);

      pgbuf_unfix_and_init (thread_p, P_page);

      key_cnt = btree_node_number_of_keys (C_page);

      header = btree_get_node_header (C_page);
      if (header == NULL)
	{
	  goto error;
	}

      node_type =
	(header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

      /* update Acceptance probability */

      free_space = spage_max_space_for_new_record (thread_p, C_page);
      assert (est_page_size > free_space);

      prob *=
	(((double) est_page_size) - free_space) / ((double) est_page_size);

      P_page = C_page;
      C_page = NULL;
      P_vpid = C_vpid;
    }

  if (key_cnt != 0)
    {
      goto end;			/* OK */
    }

again:

  header = btree_get_node_header (P_page);
  if (header == NULL)
    {
      goto error;
    }

  /* fix the next leaf page and set slot_id and oid_pos if it exists. */
  C_vpid = header->next_vpid;
  if (!VPID_ISNULL (&C_vpid))
    {
      C_page = pgbuf_fix (thread_p, &C_vpid, OLD_PAGE,
			  PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (C_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, C_page, PAGE_BTREE);

      /* unfix the previous leaf page if it is fixed. */
      if (P_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, P_page);
	  /* do not clear bts->P_vpid for UNCONDITIONAL lock request handling */
	}
    }

  /* check if the current leaf page has valid slots */
  if (C_page != NULL)
    {
      key_cnt = btree_node_number_of_keys (C_page);

      if (key_cnt <= 0)
	{			/* empty page */
	  P_page = C_page;
	  C_page = NULL;
	  goto again;
	}
      P_vpid = C_vpid;
      P_page = C_page;
    }

  /* NOTE that we do NOT release the page latch on P here */
end:

  *pg_vpid = P_vpid;

  assert_release (stat_info_p->height == 0
		  || stat_info_p->height == depth + 1);
  assert_release (root_level == depth + 1);

  stat_info_p->height = root_level;

  /* do Acceptance/Rejection sampling */
  if (drand48 () < prob)
    {
      /* Acceptance */
      *found_p = true;
    }
  else
    {
      /* Rejection */
      assert (*found_p == false);
    }

  return P_page;

error:

  if (P_page)
    {
      pgbuf_unfix_and_init (thread_p, P_page);
    }
  if (C_page)
    {
      pgbuf_unfix_and_init (thread_p, C_page);
    }

  return NULL;
}

/*
 * btree_keyval_search () -
 *   return: the number of object identifiers in the set pointed
 *           at by oids_ptr, or -1 if an error occurs. Since there can be
 *           many object identifiers for the given key, to avoid main
 *           memory limitations, the set of object identifiers are returned
 *           iteratively. At each call, the btree_scan is modified, to
 *           remember the old search position.
 *   btid(in):
 *      btid: B+tree index identifier
 *   scan_op_type(in):
 *   BTS(in/out): Btree range search scan structure
 *   key(in): Key to be searched for its object identifier set
 *   class_oid(in):
 *   oids_ptr(in): Points to the already allocated storage area to store oids
 *   oids_size(in): Size of allocated area for oid set storage
 *   filter(in):
 *   isidp(in):
 *   is_all_class_srch(in):
 *
 * Note: Finds the set of object identifiers for the given key.
 * if the key is not found, a 0 count is returned. Otherwise,
 * the area pointed at by oids_ptr is filled with one group of
 * object identifiers.
 *
 * Note: the btree_scan structure must first be initialized by using the macro
 * BTREE_INIT_SCAN() defined in bt.h
 *
 * Note: After the first iteration, caller can use BTREE_END_OF_SCAN() macro
 * defined in bt.h to understand the end of range.
 */
int
btree_keyval_search (THREAD_ENTRY * thread_p, BTID * btid,
		     SCAN_OPERATION_TYPE scan_op_type,
		     BTREE_SCAN * BTS,
		     KEY_VAL_RANGE * key_val_range, OID * class_oid,
		     OID * oids_ptr, int oids_size, FILTER_INFO * filter,
		     INDX_SCAN_ID * isidp, bool is_all_class_srch)
{
  /* this is just a GE_LE range search with the same key */
  int rc;
  LOCK class_lock = NULL_LOCK;
  int scanid_bit = -1;
  int num_classes;

  if (isidp->mvcc_need_locks)
    {
      rc = lock_scan (thread_p, class_oid, true, &class_lock, &scanid_bit);
      if (rc != LK_GRANTED)
	{
	  return ER_FAILED;
	}

      isidp->scan_cache.scanid_bit = scanid_bit;
    }

  /* check if the search is based on all classes contained in the class hierarchy. */
  num_classes = (is_all_class_srch) ? 0 : 1;

  rc =
    btree_range_search (thread_p, btid, scan_op_type, BTS,
			key_val_range, num_classes, class_oid, oids_ptr,
			oids_size, filter, isidp, true, false, NULL, NULL,
			false, 0);

  if (isidp->mvcc_need_locks)
    {
      lock_unlock_scan (thread_p, class_oid, scanid_bit, END_SCAN);
    }

  return rc;
}

/*
 * btree_coerce_key () -
 *   return: NO_ERROR or error code
 *   src_keyp(in/out):
 *   keysize(in): term# associated with index key range
 *   btree_domainp(in): B+tree index domain
 *   key_minmax(in): MIN_VALUE or MAX_VALUE
 *
 * Note:
 */
int
btree_coerce_key (DB_VALUE * keyp, int keysize,
		  TP_DOMAIN * btree_domainp, int key_minmax)
{
  DB_TYPE stype, dtype;
  int ssize, dsize;
  TP_DOMAIN *dp;
  DB_VALUE value;
  DB_MIDXKEY *midxkey;
  TP_DOMAIN *partial_dom;
  int minmax;
  int err = NO_ERROR;
  bool part_key_desc = false;

  /* assuming all parameters are not NULL pointer, and 'src_key' is not NULL
     value */
  stype = DB_VALUE_TYPE (keyp);
  dtype = TP_DOMAIN_TYPE (btree_domainp);

  if (stype == DB_TYPE_MIDXKEY && dtype == DB_TYPE_MIDXKEY)
    {
      /* if multi-column index */
      /* The type of B+tree key domain can be DB_TYPE_MIDXKEY only in the
         case of multi-column index. And, if it is, query optimizer makes
         the search key('src_key') as sequence type even if partial key was
         specified. One more assumption is that query optimizer make the
         search key(either complete or partial) in the same order (of
         sequence) of B+tree key domain. */

      /* get number of elements of sequence type of the 'src_key' */
      midxkey = DB_PULL_MIDXKEY (keyp);
      ssize = midxkey->ncolumns;

      /* count number of elements of sequence type of the B+tree key domain */
      for (dp = btree_domainp->setdomain, dsize = 0; dp;
	   dp = dp->next, dsize++)
	{
	  ;
	}

      if (ssize < 0 || ssize > dsize || dsize == 0 || ssize > keysize)
	{
	  /* something wrong with making search key in query optimizer */
	  err = ER_FAILED;	/* error */
	}
      else if (ssize == dsize)
	{
	  if (midxkey->domain == NULL)	/* checkdb */
	    {
	      midxkey->domain = btree_domainp;
	    }

	  return NO_ERROR;
	}
      else
	{
	  /* do coercing, append min or max value of the coressponding domain
	     type to the partial search key value */
	  DB_VALUE *dbvals = NULL;
	  int num_dbvals;

	  num_dbvals = dsize - ssize;

	  if (num_dbvals == 1)
	    {
	      dbvals = &value;
	    }
	  else if (num_dbvals > 1)
	    {
	      dbvals = (DB_VALUE *) db_private_alloc (NULL,
						      num_dbvals *
						      sizeof (DB_VALUE));
	      if (dbvals == NULL)
		{
		  return ER_OUT_OF_VIRTUAL_MEMORY;
		}
	    }
	  else
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_GENERIC_ERROR, 0);
	      fprintf (stderr, "Error: btree_coerce_key(num_dbval %d)\n",
		       num_dbvals);
	      return ER_GENERIC_ERROR;
	    }

	  /* get the last domain element of partial-key */
	  for (dp = btree_domainp->setdomain, dsize = 1;
	       dsize < keysize && dp; dsize++, dp = dp->next)
	    {
	      ;			/* nop */
	    }

	  if (dsize < keysize || dp == NULL)
	    {
	      if (dbvals != &value)
		{
		  db_private_free_and_init (NULL, dbvals);
		}
	      return ER_FAILED;
	    }

	  part_key_desc = dp->is_desc;

	  for (dp = btree_domainp->setdomain, dsize = 0; dp && dsize < ssize;
	       dp = dp->next, dsize++)
	    {
	      ;
	    }

	  num_dbvals = 0;
	  partial_dom = dp;

	  for (err = NO_ERROR; dp && err == NO_ERROR; dp = dp->next, dsize++)
	    {
	      /* server doesn't treat DB_TYPE_OBJECT, so that convert it to
	         DB_TYPE_OID */
	      DB_TYPE type;

	      type = (TP_DOMAIN_TYPE (dp) == DB_TYPE_OBJECT) ? DB_TYPE_OID
		: TP_DOMAIN_TYPE (dp);

	      minmax = key_minmax;	/* init */
	      if (minmax == BTREE_COERCE_KEY_WITH_MIN_VALUE)
		{
		  if (!part_key_desc)
		    {		/* CASE 1, 2 */
		      if (dp->is_desc != true)
			{	/* CASE 1 */
			  ;	/* nop */
			}
		      else
			{	/* CASE 2 */
			  minmax = BTREE_COERCE_KEY_WITH_MAX_VALUE;
			}
		    }
		  else
		    {		/* CASE 3, 4 */
		      if (dp->is_desc != true)
			{	/* CASE 3 */
			  minmax = BTREE_COERCE_KEY_WITH_MAX_VALUE;
			}
		      else
			{	/* CASE 4 */
			  ;	/* nop */
			}
		    }
		}
	      else if (minmax == BTREE_COERCE_KEY_WITH_MAX_VALUE)
		{
		  if (!part_key_desc)
		    {		/* CASE 1, 2 */
		      if (dp->is_desc != true)
			{	/* CASE 1 */
			  ;	/* nop */
			}
		      else
			{	/* CASE 2 */
			  minmax = BTREE_COERCE_KEY_WITH_MIN_VALUE;
			}
		    }
		  else
		    {		/* CASE 3, 4 */
		      if (dp->is_desc != true)
			{	/* CASE 3 */
			  minmax = BTREE_COERCE_KEY_WITH_MIN_VALUE;
			}
		      else
			{	/* CASE 4 */
			  ;	/* nop */
			}
		    }
		}

	      if (minmax == BTREE_COERCE_KEY_WITH_MIN_VALUE)
		{
		  if (dsize < keysize)
		    {
		      err = db_value_domain_min (&dbvals[num_dbvals], type,
						 dp->precision, dp->scale,
						 dp->codeset,
						 dp->collation_id,
						 &dp->enumeration);
		    }
		  else
		    {
		      err = db_value_domain_init (&dbvals[num_dbvals], type,
						  dp->precision, dp->scale);
		    }
		}
	      else if (minmax == BTREE_COERCE_KEY_WITH_MAX_VALUE)
		{
		  err = db_value_domain_max (&dbvals[num_dbvals], type,
					     dp->precision, dp->scale,
					     dp->codeset, dp->collation_id,
					     &dp->enumeration);
		}
	      else
		{
		  err = ER_FAILED;
		}

	      num_dbvals++;
	    }

	  if (err == NO_ERROR)
	    {
	      err = pr_midxkey_add_elements (keyp, dbvals, num_dbvals,
					     partial_dom);
	    }

	  if (dbvals != &value)
	    {
	      db_private_free_and_init (NULL, dbvals);
	    }
	}

    }
  else if (
	    /* check if they are string or bit type */
	    /* compatible if two types are same (except for sequence type) */
	    (stype == dtype)
	    /* CHAR type and VARCHAR type are compatible with each other */
	    || ((stype == DB_TYPE_CHAR || stype == DB_TYPE_VARCHAR)
		&& (dtype == DB_TYPE_CHAR || dtype == DB_TYPE_VARCHAR))
	    /* NCHAR type and VARNCHAR type are compatible with each other */
	    || ((stype == DB_TYPE_NCHAR || stype == DB_TYPE_VARNCHAR)
		&& (dtype == DB_TYPE_NCHAR || dtype == DB_TYPE_VARNCHAR))
	    /* BIT type and VARBIT type are compatible with each other */
	    || ((stype == DB_TYPE_BIT || stype == DB_TYPE_VARBIT)
		&& (dtype == DB_TYPE_BIT || dtype == DB_TYPE_VARBIT))
	    /* OID type and OBJECT type are compatible with each other */
	    /* Keys can come in with a type of DB_TYPE_OID, but the B+tree domain
	       itself will always be a DB_TYPE_OBJECT. The comparison routines
	       can handle OID and OBJECT as compatible type with each other . */
	    || (stype == DB_TYPE_OID || stype == DB_TYPE_OBJECT))
    {
      err = NO_ERROR;
    }
  else
    {
      DB_VALUE temp_val;

      DB_MAKE_NULL (&temp_val);

      if (tp_more_general_type (dtype, stype) > 0)
	{
	  /* the other case, do real coercing using 'tp_value_coerce()' */
	  if (tp_value_coerce (keyp, &temp_val,
			       btree_domainp) == DOMAIN_COMPATIBLE)
	    {
	      pr_clear_value (keyp);
	      pr_clone_value (&temp_val, keyp);
	    }

	  pr_clear_value (&temp_val);
	}
      else if (TP_IS_NUMERIC_TYPE (dtype) || TP_IS_DATE_OR_TIME_TYPE (dtype))
	{
	  /* try to strict cast keyp to dtype */
	  err = tp_value_coerce_strict (keyp, &temp_val, btree_domainp);
	  if (err == NO_ERROR)
	    {
	      pr_clear_value (keyp);
	      pr_clone_value (&temp_val, keyp);
	    }
	  else
	    {
	      /* unsuccessful,  */
	      err = NO_ERROR;
	    }

	  pr_clear_value (&temp_val);
	}
      else
	{
	  err = NO_ERROR;
	}
    }

  if (err != NO_ERROR)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
    }

  /* return result */
  return err;
}

/*
 * btree_initialize_bts () -
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *   btid(in): B+-tree identifier
 *   readonly_purpose(in):
 *   cls_oid(in): class oid (NULL_OID or valid OID)
 *   key1(in): the lower bound key value of key range
 *   key2(in): the upper bound key value of key range
 *   range(in): the range of key range
 *   filter(in): key filter
 *   need_construct_btid_int(in):
 *   copy_buf(in):
 *   copy_buf_len(in):
 *   bool for_update(in): true if FOR UPDATE clause is active
 *   bool mvcc_need_locks(in): true if need locks in MVCC during index scan
 *
 * Note: Initialize a new B+-tree scan structure for an index scan.
 */
static int
btree_initialize_bts (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
		      BTID * btid, int readonly_purpose,
		      OID * class_oid, KEY_VAL_RANGE * key_val_range,
		      FILTER_INFO * filter,
		      bool need_construct_btid_int, char *copy_buf,
		      int copy_buf_len, bool for_update, bool mvcc_need_locks)
{
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  int i;
  int ret = NO_ERROR;
  DB_MIDXKEY *midxkey;
  KEY_VAL_RANGE inf_key_val_range;

  if (key_val_range == NULL)
    {
      /* NULL key_val_range argument means a full range scan */
      DB_MAKE_NULL (&inf_key_val_range.key1);
      DB_MAKE_NULL (&inf_key_val_range.key2);
      inf_key_val_range.range = INF_INF;
      inf_key_val_range.num_index_term = 0;
      inf_key_val_range.is_truncated = false;

      key_val_range = &inf_key_val_range;
    }

  /* initialize page related fields */
  /* previous leaf page, current leaf page, overflow page */
  BTREE_INIT_SCAN (bts);

  /* initialize current key related fields */
  bts->clear_cur_key = false;
  bts->read_cur_key = false;

  /* cache transaction isolation level */
  bts->tran_isolation = logtb_find_current_isolation (thread_p);

  /* in MVCC there  is not need to acquire locks in btree_range_search
   * even if update/delete. The locks will be acquired later, outside
   * of btree_range_search, only for visible OIDs.
   */
  if (mvcc_Enabled == true && mvcc_need_locks == false)
    {
      bts->read_uncommitted = true;
    }
  else
    {
      bts->read_uncommitted = false;
    }

  assert (need_construct_btid_int == true
	  || (need_construct_btid_int == false
	      && !OID_ISNULL (&bts->btid_int.topclass_oid)));
  if (need_construct_btid_int == true)
    {
      /* construct BTID_INT structure */
      root_vpid.pageid = btid->root_pageid;
      root_vpid.volid = btid->vfid.volid;
      root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			PGBUF_UNCONDITIONAL_LATCH);
      if (root == NULL)
	{
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

      root_header = btree_get_root_header (root);
      if (root_header == NULL)
	{
	  goto exit_on_error;
	}

      bts->btid_int.sys_btid = btid;
      ret = btree_glean_root_header_info (thread_p,
					  root_header, &bts->btid_int);
      if (ret != NO_ERROR)
	{
	  pgbuf_unfix_and_init (thread_p, root);
	  goto exit_on_error;
	}

      pgbuf_unfix_and_init (thread_p, root);

      if (DB_VALUE_TYPE (&key_val_range->key1) == DB_TYPE_MIDXKEY)
	{
	  midxkey = DB_PULL_MIDXKEY (&key_val_range->key1);
	  if (midxkey->domain == NULL || LOG_CHECK_LOG_APPLIER (thread_p)
	      || LOG_CHECK_LOG_PREFETCHER (thread_p))
	    {
	      /*
	       * The asc/desc properties in midxkey from log_applier may be
	       * inaccurate. therefore, we should use btree header's domain
	       * while processing btree search request from log_applier.
	       */
	      if (midxkey->domain)
		{
		  tp_domain_free (midxkey->domain);
		}
	      midxkey->domain = bts->btid_int.key_type;
	    }
	}
      if (DB_VALUE_TYPE (&key_val_range->key2) == DB_TYPE_MIDXKEY)
	{
	  midxkey = DB_PULL_MIDXKEY (&key_val_range->key2);
	  if (midxkey->domain == NULL || LOG_CHECK_LOG_APPLIER (thread_p)
	      || LOG_CHECK_LOG_PREFETCHER (thread_p))
	    {
	      if (midxkey->domain)
		{
		  tp_domain_free (midxkey->domain);
		}
	      midxkey->domain = bts->btid_int.key_type;
	    }
	}

      /* is from keyval_search; checkdb or find_unique */
      assert_release (key_val_range->num_index_term == 0);
    }

  /*
   * set index key copy_buf info;
   * is allocated at btree_keyval_search() or scan_open_index_scan()
   */
  bts->btid_int.copy_buf = copy_buf;
  bts->btid_int.copy_buf_len = copy_buf_len;

  /* initialize the key range with given information */
  /*
   * Set up the keys and make sure that they have the proper domain
   * (by coercing, if necessary). Open-ended searches will have one or
   * both of key1 or key2 set to NULL so that we no longer have to do
   * DB_IS_NULL() tests on them.
   */
  /* to fix multi-column index NULL problem */

  /* only used for multi-column index with PRM_ORACLE_STYLE_EMPTY_STRING,
   * otherwise set as zero
   */

  bts->key_range.num_index_term = key_val_range->num_index_term;

  /* re-check for partial-key domain is desc */
  if (!BTREE_IS_PART_KEY_DESC (&(bts->btid_int)))
    {
      TP_DOMAIN *dom;

      dom = bts->btid_int.key_type;
      if (TP_DOMAIN_TYPE (dom) == DB_TYPE_MIDXKEY)
	{
	  dom = dom->setdomain;
	}

      /* get the last domain element of partial-key */
      for (i = 1; i < key_val_range->num_index_term && dom;
	   i++, dom = dom->next)
	{
	  ;			/* nop */
	}

      if (i < key_val_range->num_index_term || dom == NULL)
	{
	  assert (false);
	  goto exit_on_error;
	}

      bts->btid_int.part_key_desc = dom->is_desc;
    }

#if !defined(NDEBUG)
  if (DB_VALUE_TYPE (&key_val_range->key1) == DB_TYPE_MIDXKEY)
    {
      midxkey = DB_PULL_MIDXKEY (&key_val_range->key1);
      assert (midxkey->ncolumns == midxkey->domain->precision);
    }
  if (DB_VALUE_TYPE (&key_val_range->key2) == DB_TYPE_MIDXKEY)
    {
      midxkey = DB_PULL_MIDXKEY (&key_val_range->key2);
      assert (midxkey->ncolumns == midxkey->domain->precision);
    }
#endif

  /* lower bound key and upper bound key */
  if (DB_IS_NULL (&key_val_range->key1)
      || btree_multicol_key_is_null (&key_val_range->key1))
    {
      bts->key_range.lower_key = NULL;
    }
  else
    {
      bts->key_range.lower_key = &key_val_range->key1;
    }

  if (DB_IS_NULL (&key_val_range->key2)
      || btree_multicol_key_is_null (&key_val_range->key2))
    {
      bts->key_range.upper_key = NULL;
    }
  else
    {
      bts->key_range.upper_key = &key_val_range->key2;
    }

  /* range type */
  bts->key_range.range = key_val_range->range;

  if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
    {
      int j, ids_size;

      if (filter)
	{
	  ids_size = 0;		/* init */
	  for (i = 0; i < key_val_range->num_index_term; i++)
	    {
	      filter->vstr_ids[i] = -1;	/* init to false */
	      for (j = 0; j < filter->scan_attrs->num_attrs; j++)
		{
		  if (filter->btree_attr_ids[i] ==
		      filter->scan_attrs->attr_ids[j])
		    {
		      filter->vstr_ids[i] = filter->btree_attr_ids[i];
		      ids_size = i + 1;
		      break;
		    }
		}
	    }

	  /* reset num of variable string attr in key range */
	  *(filter->num_vstr_ptr) = ids_size;
	}
    }

  /* initialize key fileter */
  bts->key_filter = filter;	/* valid pointer or NULL */

  bts->key_range_max_value_equal = false;

  /* cache class OID and memory address to class lock mode */
  if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      OID_SET_NULL (&bts->cls_oid);
    }
  else
    {				/* non-unique index */
      /*
       * The non-unique index is always a single class index
       * 'db_user' class has non-unique index, but btree_find_unique request
       * can be called. In such case class_oid is NULL
       */
      if (class_oid == NULL)
	{
	  OID_SET_NULL (&bts->cls_oid);
	}
      else
	{
	  COPY_OID (&bts->cls_oid, class_oid);
	}
    }


  if (for_update)
    {
      bts->lock_mode = U_LOCK;
      bts->key_lock_mode = NX_LOCK;
      bts->escalated_mode = X_LOCK;
    }
  else if (readonly_purpose)
    {
      bts->lock_mode = S_LOCK;
      bts->key_lock_mode = S_LOCK;
      bts->escalated_mode = S_LOCK;
    }
  else
    {
      bts->lock_mode = U_LOCK;
      bts->key_lock_mode = NX_LOCK;
      bts->escalated_mode = X_LOCK;
    }

  VPID_SET_NULL (&(bts->prev_ovfl_vpid));
  bts->prev_KF_satisfied = false;

  bts->read_keys = 0;
  bts->qualified_keys = 0;

  return ret;

exit_on_error:

  if (root != NULL)
    {
      pgbuf_unfix_and_init (thread_p, root);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_find_next_index_record () -
 *   return: NO_ERROR
 *   bts(in):
 *
 * Note: This functions finds the next index record(or slot).
 * Then, it adjusts the slot_id and oid_pos information
 * about the oid-set contained in the found index slot.
 * If next records is located in next page, unfix current page
 * and change C_page as it.
 */
static int
btree_find_next_index_record (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  PAGE_PTR first_page;
  int ret_val = NO_ERROR;

  first_page = bts->C_page;	/* init */

  ret_val =
    btree_find_next_index_record_holding_current (thread_p, bts, NULL);
#if 0				/* TODO - need to check return value */
  if (ret_val != NO_ERROR)
    {
      goto error;
    }
#endif

  if (first_page != bts->C_page)
    {
      /* reset common_prefix to recalculate */
      bts->common_prefix = COMMON_PREFIX_UNKNOWN;
    }

  /*
   * unfix first page if fix next page and move to it
   *
   *  case 1: P_page == NULL, C_page == first_page       x do not fix 1 next page
   *  case 2: P_page == first_page, C_page == NULL       x can't fix 1 next page
   *  case 3: P_page == first_page, C_page != first_pag  o fix 1 next
   *  case 4: P_page == NULL, C_page == NULL             o can't fix N next, unfix N-1 prev
   *  case 5: P_page == NULL, C_page != first_page       o fix N next, unfix N-1 prev
   *  other case: imppossible (assert)
   *
   *  in case of 3, 4, 5, unfix first_page
   */

#if !defined(NDEBUG)
  if ((bts->P_page == NULL && bts->C_page == first_page) ||
      (bts->P_page == first_page && bts->C_page == NULL) ||
      (bts->P_page == first_page && bts->C_page && bts->C_page != first_page)
      || (bts->P_page == NULL && bts->C_page == NULL) ||
      (bts->P_page == NULL && bts->C_page && bts->C_page != first_page))
    {
      /* case 1, 2, 3, 4, 5 */
    }
  else
    {
      assert (false);
    }
#endif

  if ((bts->C_page == NULL && bts->P_page == NULL)	/* case 4 */
      || (bts->C_page != NULL && bts->C_page != first_page))	/* case 3, 5 */
    {
      if (first_page == bts->P_page)
	{
	  /* prevent double unfix by caller */
	  bts->P_page = NULL;
	}

      if (first_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, first_page);
	}
    }

  return ret_val;
}

/*
 * btree_find_next_index_record_holding_current () -
 *   return: NO_ERROR
 *   bts(in):
 *
 * Note: This functions finds & peek next index record
 * this function does not unfix first page
 */
static int
btree_find_next_index_record_holding_current (THREAD_ENTRY * thread_p,
					      BTREE_SCAN * bts,
					      RECDES * peek_rec)
{
  RECDES rec;
  int ret = NO_ERROR;
  PAGE_PTR first_page = bts->C_page;

  rec.data = NULL;

  /*
   * Assumptions : last accessed leaf page is fixed.
   *    - bts->C_page != NULL
   *    - bts->O_page : NULL or NOT NULL
   *    - bts->P_page == NULL
   */

  /* unfix the overflow page if it is fixed. */
  if (bts->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->O_page);
      VPID_SET_NULL (&(bts->O_vpid));
    }

  /* unfix the previous leaf page if it is fixed. */
  if (bts->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->P_page);
      VPID_SET_NULL (&(bts->P_vpid));
    }

  if (bts->C_page == NULL)
    {
      return ER_FAILED;
    }

  bts->P_vpid = bts->C_vpid;	/* save started leaf vpid */

  while (bts->C_page != NULL)
    {
      ret = btree_find_next_index_record_holding_current_helper
	(thread_p, bts, first_page);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* filter out fence_key record */
      if (bts->C_page != NULL)
	{
	  assert (bts->slot_id > 0);
	  if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK) !=
	      S_SUCCESS)
	    {
	      assert (false);
	      goto exit_on_error;
	    }

	  assert (rec.length % 4 == 0);

	  if (!btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	    {
	      break;		/* found */
	    }
	}
    }

  if (VPID_EQ (&bts->P_vpid, &bts->C_vpid))
    {
      /* set bts->P_vpid to null for unconditional lock request handling */
      VPID_SET_NULL (&bts->P_vpid);
    }

#if !defined(NDEBUG)
  if (bts->C_page != NULL)
    {
      /* is must be normal record */
      assert (bts->slot_id > 0);
      if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK) !=
	  S_SUCCESS)
	{
	  goto exit_on_error;
	}
      assert (!btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE));
    }
#endif
  if (bts->C_page != NULL && peek_rec != NULL)
    {
      *peek_rec = rec;
    }

  return ret;

exit_on_error:

  assert (ret != NO_ERROR);

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_find_next_index_record_holding_current_helper () -
 *   return: NO_ERROR
 *   bts(in):
 *
 * Note: This functions finds the next index record(or slot).
 * Then, it adjusts the slot_id and oid_pos information
 * about the oid-set contained in the found index slot.
 */
static int
btree_find_next_index_record_holding_current_helper (THREAD_ENTRY * thread_p,
						     BTREE_SCAN * bts,
						     PAGE_PTR first_page)
{
  int key_cnt;
  int ret = NO_ERROR;
  PGBUF_LATCH_CONDITION latch_condition;
  BTREE_NODE_HEADER *header = NULL;

  /* get header information (key_cnt) from the current leaf page */
  key_cnt = btree_node_number_of_keys (bts->C_page);

#if !defined(NDEBUG)
  header = btree_get_node_header (bts->C_page);

  assert (header != NULL);
  assert (header->node_level == 1);	/* BTREE_LEAF_NODE */
#endif

  /*
   * If the next index record exists in the current leaf page,
   * the next index record(slot) and OID position can be identified easily.
   */
  if (key_cnt > 0)
    {
      if (bts->use_desc_index)
	{
	  if (bts->slot_id > 1)
	    {
	      bts->slot_id--;
	      bts->oid_pos = 0;
	      goto end;		/* OK */
	    }
	}
      else
	{
	  if (bts->slot_id < key_cnt)
	    {

	      bts->slot_id++;
	      bts->oid_pos = 0;
	      goto end;		/* OK */
	    }
	}
    }

  while (bts->C_page != NULL)
    {
      header = btree_get_node_header (bts->C_page);
      if (header == NULL)
	{
	  if (first_page != bts->P_page)
	    {
	      pgbuf_unfix_and_init (thread_p, bts->P_page);
	    }

	  goto exit_on_error;
	}

      if (bts->use_desc_index)
	{
	  bts->C_vpid = header->prev_vpid;
	  latch_condition = PGBUF_CONDITIONAL_LATCH;
	}
      else
	{
	  bts->C_vpid = header->next_vpid;
	  latch_condition = PGBUF_UNCONDITIONAL_LATCH;
	}

      bts->P_page = bts->C_page;
      bts->C_page = NULL;

      if (!VPID_ISNULL (&(bts->C_vpid)))
	{
	  bts->C_page = pgbuf_fix (thread_p, &bts->C_vpid, OLD_PAGE,
				   PGBUF_LATCH_READ, latch_condition);
	  if (bts->C_page == NULL)
	    {
	      if (bts->use_desc_index)
		{
		  assert (latch_condition == PGBUF_CONDITIONAL_LATCH);
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_DESC_ISCAN_ABORTED, 3,
			  bts->btid_int.sys_btid->vfid.volid,
			  bts->btid_int.sys_btid->vfid.fileid,
			  bts->btid_int.sys_btid->root_pageid);
		}

	      if (first_page != bts->P_page)
		{
		  pgbuf_unfix_and_init (thread_p, bts->P_page);
		}

	      goto exit_on_error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, bts->C_page, PAGE_BTREE);

	  /* unfix the previous leaf page */
	  assert (bts->P_page != NULL);

	  if (first_page != bts->P_page)
	    {
	      pgbuf_unfix_and_init (thread_p, bts->P_page);
	    }

	  /* do not clear bts->P_vpid for UNCONDITIONAL lock request handling */

	  key_cnt = btree_node_number_of_keys (bts->C_page);

	  if (key_cnt > 0)
	    {
	      if (bts->use_desc_index)
		{
		  bts->slot_id = key_cnt;
		  bts->oid_pos = 0;
		}
	      else
		{
		  bts->slot_id = 1;
		  bts->oid_pos = 0;
		}

	      goto end;		/* OK */
	    }
	}
      else
	{
	  if (first_page != bts->P_page)
	    {
	      pgbuf_unfix_and_init (thread_p, bts->P_page);
	    }
	}
    }

end:
  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_get_next_oidset_pos () - Get the next oid-set position info.
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *   first_ovfl_vpid(in): the pageid of the first OID overflow page
 *                        of the current index slot.
 *
 * Note: This function finds the next oid-set to be scanned.
 * It fixes the needed index pages, and
 * sets the slot_id and oid_pos information of the next oid-set.
 */
static int
btree_get_next_oidset_pos (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
			   VPID * first_ovfl_vpid)
{
  int ret = NO_ERROR;

  /*
   * Assumptions : last accessed leaf page is fixed.
   *    - bts->C_page != NULL
   *    - bts->O_page : NULL or NOT NULL
   *    - bts->P_page == NULL
   */

  if (bts->O_page != NULL)
    {
      /*
       * Assumption :
       * bts->oid_pos >= # of OIDs contained in the overflow page
       */

      (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);

      /* get the pageid of the next overflow page */
      btree_get_next_overflow_vpid (bts->O_page, &bts->O_vpid);

      /* unfix current overflow page */
      pgbuf_unfix_and_init (thread_p, bts->O_page);
    }
  else
    {
      /*
       * Assumption :
       * bts->oid_pos >= # of OIDs contained in the index entry
       */
      /*
       * get the pageid of the first overflow page
       * requirements : first_ovfl_vpid != NULL
       * first_ovfl_vpid : either NULL_VPID or valid VPID
       */
      bts->O_vpid = *first_ovfl_vpid;
    }

  /* fix the next overflow page or the next leaf page */
  if (!VPID_ISNULL (&(bts->O_vpid)))
    {
      /* fix the next overflow page */
      bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
			       PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (bts->O_page == NULL)
	{
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);

      bts->oid_pos = 0;
      /* bts->slot_id is not changed */
    }
  else
    {
      /* find the next index record */
      ret = btree_find_next_index_record (thread_p, bts);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_prepare_first_search () - Prepare for the first index scan
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *
 * Note: This function finds the first oid-set to be scanned.
 * This function is invoked in the first index scan.
 * Then, it searches down the B+-tree, fixes the needed index pages,
 * and sets the slot_id and oid_pos information of the first index scan.
 */
static int
btree_prepare_first_search (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  bool found;
  int key_cnt;
  RECDES rec;
  int ret = NO_ERROR;

  /* search down the tree to find the first oidset */
  /*
   * Following information must be gotten.
   * bts->C_vpid, bts->C_page, bts->slot_id, bts->oid_pos
   */

  /*
   * If the key range does not have a lower bound key value,
   * the first key of the index is used as the lower bound key value.
   */
  if (bts->key_range.lower_key == NULL)
    {				/* The key range has no bottom */
      ret = btree_find_lower_bound_leaf (thread_p, bts, NULL);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      return NO_ERROR;		/* OK */
    }

  /*
   * bts->key_range.lower_key != NULL
   * Find out and fix the first leaf page
   * that contains the given lower bound key value.
   */
  bts->C_page = btree_locate_key (thread_p, &bts->btid_int,
				  bts->key_range.lower_key,
				  &bts->C_vpid, &bts->slot_id, &found);

  if (!found)
    {
      bool flag_next_key = false;

      if (bts->slot_id == NULL_SLOTID)
	{
	  goto exit_on_error;
	}

      assert (bts->slot_id > 0);

      if (bts->use_desc_index)
	{
	  bts->slot_id--;
	}

      if (bts->C_page == NULL)
	{
	  return ret;
	}

      key_cnt = btree_node_number_of_keys (bts->C_page);

      if (bts->slot_id > key_cnt || bts->slot_id == 0)
	{
	  /*
	   * The lower bound key does not exist in the current leaf page.
	   * Therefore, get the first slot of the next leaf page.
	   *       (or, get the last slot of the prev leaf page.)
	   */
	  flag_next_key = true;
	}
      else if (bts->slot_id == key_cnt || bts->slot_id == 1)
	{
	  /* key is exists but it could be a fence key */
	  assert (bts->slot_id > 0);
	  if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK) !=
	      S_SUCCESS)
	    {
	      assert (false);
	      goto exit_on_error;
	    }
	  assert (rec.length % 4 == 0);

	  /* filter out fence_key */
	  if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	    {
	      flag_next_key = true;
	    }
	}

      if (flag_next_key)
	{
	  ret = btree_find_next_index_record (thread_p, bts);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
      else
	{
	  bts->oid_pos = 0;
	}
    }
  else
    {
      assert (bts->C_page != NULL);
      assert (bts->slot_id > 0);
      if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK) !=
	  S_SUCCESS)
	{
	  assert (false);
	  goto exit_on_error;
	}
      assert (rec.length % 4 == 0);

      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{			/* is impossible case */
	  assert (false);
	  goto exit_on_error;
	}

      if (bts->key_range.range == GT_LT
	  || bts->key_range.range == GT_LE || bts->key_range.range == GT_INF)
	{
	  /* get the next index record */
	  ret = btree_find_next_index_record (thread_p, bts);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
      else
	{
	  bts->oid_pos = 0;
	}
    }

  assert (bts->oid_pos == 0);

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_prepare_next_search () - Prepare for the next index scan
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *
 * Note: This function finds the next oid-set to be scaned.
 * This function is invoked by the next index scan.
 * Then it fixes the needed index pages, and sets
 * the slot_id and oid_pos information of the next index scan.
 */
static int
btree_prepare_next_search (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  bool found;
  int key_cnt;
  RECDES rec;
  int ret = NO_ERROR;
  bool skip_remaining_oids = false;
#if !defined(NDEBUG)
  BTREE_NODE_HEADER *header = NULL;
#endif

  if (mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      skip_remaining_oids = true;
    }

  /*
   * Assumptions :
   * 1. bts->C_vpid.pageid != NULL_PAGEID
   * 2. bts->O_vpid.pageid is NULL_PAGEID or not NULL_PAGEID.
   * 3. bts->P_vpid.pageid == NULL_PAGEID
   * 4. bts->slot_id indicates the last accessed slot
   * 5. 1 < bts->oid_pos <= (last oid position + 1)
   */

  /* fix the current leaf page */
  bts->C_page = pgbuf_fix_without_validation (thread_p, &bts->C_vpid,
					      OLD_PAGE, PGBUF_LATCH_READ,
					      PGBUF_UNCONDITIONAL_LATCH);
  if (bts->C_page == NULL)
    {
      goto exit_on_error;
    }

  /* check if the current leaf page has been changed */
  if (!btree_leaf_lsa_eq (thread_p,
			  &bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page)))
    {
      /*
       * The current leaf page has been changed.
       * unfix the current leaf page
       */
      pgbuf_unfix_and_init (thread_p, bts->C_page);

      /* find out the last accessed index record */
      bts->C_page = btree_locate_key (thread_p, &bts->btid_int, &bts->cur_key,
				      &bts->C_vpid, &bts->slot_id, &found);

      if (!found)
	{
	  bool flag_next_key = false;

	  if (bts->slot_id == NULL_SLOTID)
	    {
	      goto exit_on_error;
	    }

	  assert (bts->slot_id > 0);

#if 0				/* TODO */
	  if (bts->use_desc_index)
	    {
	      bts->slot_id--;
	    }
#endif

	  if (bts->C_page == NULL)
	    {
	      goto exit_on_error;
	    }

	  if (bts->read_uncommitted == false)
	    {
	      /*
	       * transaction isolation level >= Committed Read
	       * Since one or more OIDs associated with bts->cur_key
	       * have been locked, bts->cur_key must be found in the index.
	       */
	      goto exit_on_error;
	    }

	  /*
	   * Uncommitted Read
	   * bts->cur_key might be deleted.
	   * get header information (key_cnt)
	   */
	  key_cnt = btree_node_number_of_keys (bts->C_page);

#if !defined(NDEBUG)
	  header = btree_get_node_header (bts->C_page);

	  assert (header != NULL);
	  assert (header->node_level == 1);	/* BTREE_LEAF_NODE */
#endif

	  if (bts->slot_id > key_cnt || bts->slot_id == 0)
	    {
	      /*
	       * cur_key does not exist in the current leaf page.
	       * Therefore, get the first slot of the next leaf page.
	       *       (or, get the last slot of the prev leaf page.)
	       */
	      flag_next_key = true;
	    }
	  else if (bts->slot_id == key_cnt || bts->slot_id == 1)
	    {
	      /* cur_key is exists but it could be a fence key */
	      assert (bts->slot_id > 0);
	      if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK)
		  != S_SUCCESS)
		{
		  assert (false);
		  goto exit_on_error;
		}
	      assert (rec.length % 4 == 0);

	      /* filter out fence_key */
	      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
		{
		  flag_next_key = true;
		}
	    }

	  if (flag_next_key)
	    {
	      ret = btree_find_next_index_record (thread_p, bts);
	      if (ret != NO_ERROR)
		{
		  goto exit_on_error;
		}

	      /* set skip_remaining_oids to false since already advanced
	       * to the next record
	       */
	      skip_remaining_oids = false;
	    }
	}
      /* if found, everything is OK. */
    }

  assert (bts->slot_id > 0);
  assert (bts->C_page != NULL);

  (void) pgbuf_check_page_ptype (thread_p, bts->C_page, PAGE_BTREE);

  /*
   * If the current leaf page has not been changed,
   * bts->slot_id and bts->oid_pos are still valid.
   */

  /*
   * If bts->O_vpid.pageid != NULL_PAGEID, fix the overflow page.
   * When bts->O_vpid.pageid != NULL_PAGEID, bts->oid_pos indicates
   * any one OID among OIDs contained in the overflow page. And,
   * The OIDs positioned before bts->oid_pos cannot be removed.
   * Because, instance locks on the OIDs are held.
   * Therefore, the overflow page is still existent in the index.
   * bts->slot_id and bts->oid_pos are still valid.
   */
  if (skip_remaining_oids == true)
    {
      /* in MVCC, even if there are multiple OIDs in unique BTREE,
       * only one is visible and it was already locked & fetched during previous
       * index scan. So, in this case, advance to the next record.
       */
      ret = btree_find_next_index_record (thread_p, bts);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
      VPID_SET_NULL (&(bts->O_vpid));
    }
  else if (!VPID_ISNULL (&(bts->O_vpid)))
    {
      /* fix the current overflow page */
      bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
			       PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (bts->O_page == NULL)
	{
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_apply_key_range_and_filter () - Apply key range and key filter condition
 *   return: NO_ERROR
 *   bts(in)	: pointer to B+-tree scan structure
 *   is_iss(in) : true if this is an index skip scan
 *   is_key_range_satisfied(out): true, or false
 *   is_key_filter_satisfied(out): true, or false
 *
 * Note: This function applies key range condition and key filter condition
 * to the current key value saved in B+-tree scan structure.
 * The results of the evaluation of the given conditions are
 * returned throught key_range_satisfied and key_filter_satisfied.
 */
static int
btree_apply_key_range_and_filter (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
				  bool is_iss,
				  bool * is_key_range_satisfied,
				  bool * is_key_filter_satisfied,
				  bool need_to_check_null)
{
  int c;			/* comparison result */
  DB_LOGICAL ev_res;		/* evaluation result */
  DB_MIDXKEY *mkey;		/* midxkey ptr */
  DB_VALUE ep;			/* element ptr */
  bool allow_null_in_midxkey = false;
  DB_TYPE type;
  int ret = NO_ERROR;

  *is_key_range_satisfied = *is_key_filter_satisfied = false;
  bts->key_range_max_value_equal = false;	/* init as false */

  /* Key Range Checking */
  if (bts->key_range.upper_key == NULL)
    {
      c = DB_GT;
    }
  else
    {
      c = btree_compare_key (bts->key_range.upper_key, &bts->cur_key,
			     bts->btid_int.key_type, 1, 1, NULL);
      assert (c == DB_LT || c == DB_EQ || c == DB_GT);

      if (c == DB_UNK)
	{
	  /* error should have been set */
	  goto exit_on_error;
	}

      /* when using descending index the comparison should be changed again */
      if (bts->use_desc_index)
	{
	  c = -c;
	}
    }

  if (c < 0)
    {
      *is_key_range_satisfied = false;
    }
  else if (c == 0)
    {
      if (bts->key_range.range == GT_LE
	  || bts->key_range.range == GE_LE || bts->key_range.range == INF_LE)
	{
	  *is_key_range_satisfied = true;
	  bts->key_range_max_value_equal = true;
	}
      else
	{
	  *is_key_range_satisfied = false;
	}
    }
  else
    {
      *is_key_range_satisfied = true;
    }

  if (*is_key_range_satisfied)
    {
      if (need_to_check_null
	  && DB_VALUE_DOMAIN_TYPE (&bts->cur_key) == DB_TYPE_MIDXKEY
	  && bts->key_range.num_index_term > 0)
	{
	  mkey = DB_PULL_MIDXKEY (&(bts->cur_key));
	  /* get the last element from key range elements */
	  ret = pr_midxkey_get_element_nocopy (mkey,
					       bts->key_range.num_index_term -
					       1, &ep, NULL, NULL);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }

	  if (DB_IS_NULL (&ep))
	    {
	      bool is_desc = false;

	      allow_null_in_midxkey = false;	/* init */

	      assert_release (bts->key_range.num_index_term == 1);

	      if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
		{
		  if (ep.need_clear)
		    {		/* need to check */
		      type = DB_VALUE_DOMAIN_TYPE (&ep);
		      if (QSTR_IS_ANY_CHAR_OR_BIT (type)
			  && ep.data.ch.medium.buf != NULL)
			{
			  allow_null_in_midxkey = true;	/* is Empty-string */
			}
		    }
		}

	      is_desc = (bts->use_desc_index ? true : false);
	      if (bts->btid_int.key_type && bts->btid_int.key_type->setdomain
		  && bts->btid_int.key_type->setdomain->is_desc)
		{
		  is_desc = !is_desc;
		}

	      if (is_iss && is_desc && bts->key_range.num_index_term == 1)
		{
		  /* We're inside an INDEX SKIP SCAN doing a descending scan. We
		   * allow the first term of a MIDXKEY to be NULL since ISS has
		   * to return the results for which the first column of
		   * the index is NULL.
		   */
		  allow_null_in_midxkey = true;
		}
	      if (!allow_null_in_midxkey)
		{
		  *is_key_filter_satisfied = false;
		  goto end;	/* give up */
		}
	    }
	}

      /*
       * Only in case that key_range_satisfied is true,
       * the key filter can be applied to the current key value.
       */
      *is_key_filter_satisfied = true;
      if (bts->key_filter && bts->key_filter->scan_pred->regu_list)
	{
	  ev_res = eval_key_filter (thread_p, &bts->cur_key, bts->key_filter);
	  if (ev_res != V_TRUE)
	    {
	      *is_key_filter_satisfied = false;
	    }

	  if (ev_res == V_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }

end:
  assert ((*is_key_range_satisfied == false
	   && *is_key_filter_satisfied == false)
	  || (*is_key_range_satisfied == true
	      && *is_key_filter_satisfied == false)
	  || (*is_key_range_satisfied == true
	      && *is_key_filter_satisfied == true));

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_attrinfo_read_dbvalues () -
 *      Find db_values of desired attributes of given key
 *
 *   curr_key(in): the current key
 *   btree_att_ids(in): the btree attributes ids
 *   btree_num_att(in): the btree attributes count
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Find DB_VALUES of desired attributes of given key.
 * The attr_info structure must have already been initialized
 * with the desired attributes.
 */
int
btree_attrinfo_read_dbvalues (THREAD_ENTRY * thread_p,
			      DB_VALUE * curr_key,
			      int *btree_att_ids, int btree_num_att,
			      HEAP_CACHE_ATTRINFO * attr_info,
			      int func_index_col_id)
{
  int i, j, error = NO_ERROR;
  HEAP_ATTRVALUE *attr_value;
  bool found;

  if (curr_key == NULL || btree_att_ids == NULL || btree_num_att < 0
      || attr_info == NULL)
    {
      return ER_FAILED;
    }

  if (DB_VALUE_TYPE (curr_key) != DB_TYPE_MIDXKEY)
    {
      if (attr_info->num_values != 1 || btree_num_att != 1
	  || attr_info->values->attrid != btree_att_ids[0])
	{
	  return ER_FAILED;
	}

      if (pr_clear_value (&(attr_info->values->dbvalue)) != NO_ERROR)
	{
	  attr_info->values->state = HEAP_UNINIT_ATTRVALUE;
	  return ER_FAILED;
	}

      if (pr_clone_value (curr_key, &(attr_info->values->dbvalue)) !=
	  NO_ERROR)
	{
	  attr_info->values->state = HEAP_UNINIT_ATTRVALUE;
	  return ER_FAILED;
	}

      attr_info->values->state = HEAP_WRITTEN_ATTRVALUE;
    }
  else
    {
      attr_value = attr_info->values;
      for (i = 0; i < attr_info->num_values; i++)
	{
	  found = false;
	  for (j = 0; j < btree_num_att; j++)
	    {
	      if (attr_value->attrid == btree_att_ids[j])
		{
		  found = true;
		  break;
		}
	    }

	  if (found == false)
	    {
	      error = ER_FAILED;
	      goto error;
	    }

	  if (pr_clear_value (&(attr_value->dbvalue)) != NO_ERROR)
	    {
	      error = ER_FAILED;
	      goto error;
	    }

	  if (func_index_col_id != -1)
	    {
	      /* consider that in the midxkey resides the function result,
	       * which must be skipped if we are interested in attributes
	       */
	      if (j >= func_index_col_id)
		{
		  j++;
		}
	    }
	  if (pr_midxkey_get_element_nocopy (DB_GET_MIDXKEY (curr_key), j,
					     &(attr_value->dbvalue),
					     NULL, NULL) != NO_ERROR)
	    {
	      error = ER_FAILED;
	      goto error;
	    }

	  attr_value->state = HEAP_WRITTEN_ATTRVALUE;
	  attr_value++;
	}
    }

  return NO_ERROR;

error:

  attr_value = attr_info->values;
  for (i = 0; i < attr_info->num_values; i++)
    {
      attr_value->state = HEAP_UNINIT_ATTRVALUE;
    }

  return error;
}

/*
 * btree_dump_curr_key () -
 *      Dump the current key
 *
 *   bts(in): pointer to B+-tree scan structure
 *   filter(in): key filter
 *   oid(in): the current oid
 *   iscan_id(in): index scan id
 */
static int
btree_dump_curr_key (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
		     FILTER_INFO * filter, OID * oid, INDX_SCAN_ID * iscan_id)
{
  HEAP_CACHE_ATTRINFO *attr_info;
  REGU_VARIABLE_LIST regu_list;
  int error;

  if (bts == NULL || iscan_id == NULL
      || iscan_id->indx_cov.list_id == NULL
      || iscan_id->indx_cov.val_descr == NULL
      || iscan_id->indx_cov.output_val_list == NULL
      || iscan_id->indx_cov.tplrec == NULL)
    {
      return ER_FAILED;
    }

  if (iscan_id->rest_attrs.num_attrs > 0)
    {
      /* normal index scan or join index scan */
      attr_info = iscan_id->rest_attrs.attr_cache;
      regu_list = iscan_id->rest_regu_list;
    }
  else if (iscan_id->pred_attrs.num_attrs > 0)
    {
      /* rest_attrs.num_attrs == 0 if index scan term is
       * join index scan with always-true condition.
       * example: SELECT ... FROM X inner join Y on 1 = 1;
       */
      attr_info = iscan_id->pred_attrs.attr_cache;
      regu_list = iscan_id->scan_pred.regu_list;
    }
  else
    {
      assert_release (false);
      attr_info = NULL;
      regu_list = NULL;
    }

  error = btree_attrinfo_read_dbvalues (thread_p, &(bts->cur_key),
					filter->btree_attr_ids,
					filter->btree_num_attrs, attr_info,
					iscan_id->indx_cov.func_index_col_id);
  if (error != NO_ERROR)
    {
      return error;
    }

  error = fetch_val_list (thread_p, regu_list,
			  iscan_id->indx_cov.val_descr, NULL, oid, NULL,
			  PEEK);
  if (error != NO_ERROR)
    {
      return error;
    }

  error = qexec_insert_tuple_into_list (thread_p, iscan_id->indx_cov.list_id,
					iscan_id->indx_cov.output_val_list,
					iscan_id->indx_cov.val_descr,
					iscan_id->indx_cov.tplrec);
  if (error != NO_ERROR)
    {
      return error;
    }

  return NO_ERROR;
}

/*
 * btree_handle_prev_leaf_after_locking () -
 *      The handling after unconditional instance locking
 *      in case that the previous leaf page exists.
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *   oid_idx(in): current OID position in OID-set to be scanned
 *   prev_leaf_lsa(in): the page LSA of the previous leaf page
 *   prev_key(in): pointer to previous key value
 *   which_action(out): BTREE_CONTINUE
 *                      BTREE_GETOID_AGAIN_WITH_CHECK
 *                      BTREE_SEARCH_AGAIN_WITH_CHECK
 *
 * Note: This function is invoked after the unconditional instance locking
 * in case that the previous leaf page was existent.
 * The purpose of this function is to check the validation
 * of the unconditionally acquired lock and then to adjust
 * the next processing based on the validation result.
 */
static int
btree_handle_prev_leaf_after_locking (THREAD_ENTRY * thread_p,
				      BTREE_SCAN * bts, int oid_idx,
				      LOG_LSA * prev_leaf_lsa,
				      DB_VALUE * prev_key, int *which_action)
{
  int key_cnt;
  bool found;
  int ret = NO_ERROR;
  bool old_check_page_validation;
#if !defined(NDEBUG)
  BTREE_NODE_HEADER *header = NULL;
#endif

  /*
   * Following conditions are satisfied.
   * 1. The second argument, oid_idx, is always 0(zero).
   * 2. VPID_ISNULL (&(bts->O_vpid))
   */

  old_check_page_validation =
    thread_set_check_page_validation (thread_p, false);

  /* fix the previous leaf page */
  bts->P_page = pgbuf_fix (thread_p, &bts->P_vpid, OLD_PAGE,
			   PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);

  thread_set_check_page_validation (thread_p, old_check_page_validation);

  if (bts->P_page == NULL)
    {
      goto exit_on_error;
    }

  /* check if the previous leaf page has been changed */
  if (btree_leaf_lsa_eq (thread_p,
			 prev_leaf_lsa, pgbuf_get_lsa (bts->P_page)))
    {
      /*
       * The previous leaf page has not been changed
       */

      (void) pgbuf_check_page_ptype (thread_p, bts->P_page, PAGE_BTREE);

      if (!VPID_ISNULL (&(bts->prev_ovfl_vpid)))
	{
	  /*
	   * The previous key value has its associated OID overflow pages.
	   * It also means prev_KF_satisfied == true
	   * find the last locked OID and then the next OID.
	   * Because, some updates such as the insertion of new OID
	   * might be occurred in the OID overflow page.
	   */

	  /* fix the current (last) leaf page */
	  bts->C_page = bts->P_page;
	  bts->P_page = NULL;
	  bts->C_vpid = bts->P_vpid;
	  VPID_SET_NULL (&(bts->P_vpid));

	  /* get bts->slot_id */
	  key_cnt = btree_node_number_of_keys (bts->C_page);

#if !defined(NDEBUG)
	  header = btree_get_node_header (bts->C_page);

	  assert (header != NULL);
	  assert (header->node_level == 1);	/* BTREE_LEAF_NODE */
#endif

	  bts->slot_id = key_cnt;

	  /* fix the overflow page */
	  bts->O_vpid = bts->prev_ovfl_vpid;
	  bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
				   PGBUF_LATCH_READ,
				   PGBUF_UNCONDITIONAL_LATCH);
	  if (bts->O_page == NULL)
	    {
	      goto exit_on_error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);

	  bts->oid_pos = bts->prev_oid_pos + 1;

	  /*
	   * prev_ovfl_vpid, prev_oid_pos has garbage data, now.
	   * Their values must be set when bts->oid_pos becomes 0.
	   */

	  /* The locked OID must be checked again. */
	  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

	  return ret;		/* NO_ERROR */
	}

      /*
       * VPID_ISNULL (&(bts->prev_ovfl_vpid))
       * It has one meaning of the following two.
       * (1) The previous key value does not have
       *     its associated OID overflow pages.
       * (2) The previous key value does not satisfy
       *     the key filter condition.
       */
      if (VPID_ISNULL (&(bts->C_vpid)))
	{
	  /*
	   * The last pseudo oid has been locked correctly.
	   * unfix the previous leaf page
	   */
	  pgbuf_unfix_and_init (thread_p, bts->P_page);
	  VPID_SET_NULL (&(bts->P_vpid));

	  /* Note : some special case (the last key) */
	  *which_action = BTREE_CONTINUE;
	  return ret;		/* NO_ERROR */
	}

      /*
       * !VPID_ISNULL (&(bts->C_vpid))
       * fix the current leaf page
       */
      bts->C_page = pgbuf_fix (thread_p, &bts->C_vpid, OLD_PAGE,
			       PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (bts->C_page == NULL)
	{
	  goto exit_on_error;
	}

      if (btree_leaf_lsa_eq (thread_p,
			     &bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page)))
	{
	  /*
	   * The last instance locking is performed correctly.
	   * unfix the previous leaf page
	   */

	  (void) pgbuf_check_page_ptype (thread_p, bts->C_page, PAGE_BTREE);

	  pgbuf_unfix_and_init (thread_p, bts->P_page);
	  VPID_SET_NULL (&(bts->P_vpid));

	  /* the locking is correct */
	  *which_action = BTREE_CONTINUE;
	  return ret;		/* NO_ERROR */
	}

      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

      return ret;		/* NO_ERROR */
    }

  /*
   * ! LSA_EQ(prev_leaf_lsa, pgbuf_get_lsa(bts->P_page))
   * The previous leaf page has been changed.
   * At worst case, the previous leaf page might have been deallocated
   * by the merge operation of other transactions.
   */

#if !defined (SERVER_MODE)
  assert_release (false);
#endif

  /* unfix the previous leaf page */
  pgbuf_unfix_and_init (thread_p, bts->P_page);
  VPID_SET_NULL (&(bts->P_vpid));
  VPID_SET_NULL (&(bts->C_vpid));

  if (bts->prev_oid_pos == -1)
    {
      /*
       * This is the first request.
       * All index pages has been unfixed, now.
       */
      *which_action = BTREE_SEARCH_AGAIN_WITH_CHECK;

      return ret;		/* NO_ERROR */
    }

  /* search the previous index entry */
  bts->C_page = btree_locate_key (thread_p, &bts->btid_int, prev_key,
				  &bts->C_vpid, &bts->slot_id, &found);

  if (!found)
    {
      if (bts->C_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, bts->C_page);
	}
      *which_action = BTREE_SEARCH_AGAIN_WITH_CHECK;
      return NO_ERROR;
    }

  /* found */
  assert (bts->slot_id > 0);
  assert (bts->C_page != NULL);

  if (bts->prev_KF_satisfied == true
      || bts->tran_isolation == TRAN_SERIALIZABLE)
    {
      if (!VPID_ISNULL (&(bts->prev_ovfl_vpid)))
	{
	  bts->O_vpid = bts->prev_ovfl_vpid;
	  bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
				   PGBUF_LATCH_READ,
				   PGBUF_UNCONDITIONAL_LATCH);
	  if (bts->O_page == NULL)
	    {
	      goto exit_on_error;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);
	}

      bts->oid_pos = bts->prev_oid_pos + 1;
      /*
       * bts->oid_pos is the OID position information.
       * If the previous key has the OID overflow page, it indicates
       * the position after the last OID position on the OID overflow page.
       * Otherwise, it indicates the position after the last OID position
       * on the oidset of the previous index entry.
       */
    }
  else
    {
      /* bts->prev_KF_satisfied == false */
      ret = btree_find_next_index_record (thread_p, bts);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
      /* bts->C_vpid.pageid can be NULL_PAGEID. */
    }

  /* The locked OID must be checked again. */
  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * btree_handle_curr_leaf_after_locking () -
 *      The handling after unconditional instance locking
 *      in case that the previous leaf page does not exist.
 *   return: NO_ERROR
 *   bts(in): pointer to B+-tree scan structure
 *   oid_idx(in): current OID position in OID-set to be scanned
 *   ovfl_page_lsa(in):
 *   prev_key(in): pointer to previous key value
 *   prev_oid_ptr(in): pointer to previously locked inst OID
 *   which_action(out): BTREE_CONTINUE
 *                      BTREE_GETOID_AGAIN
 *                      BTREE_GETOID_AGAIN_WITH_CHECK
 *                      BTREE_SEARCH_AGAIN_WITH_CHECK
 *
 * Note: This function is invoked after the unconditional instance locking
 * in case that the previous leaf page is not existent.
 * The purpose of this function is to check the validation
 * of the unconditionally acquired lock and then to adjust
 * the next processing based on the validation result.
 */
static int
btree_handle_curr_leaf_after_locking (THREAD_ENTRY * thread_p,
				      BTREE_SCAN * bts, int oid_idx,
				      LOG_LSA * ovfl_page_lsa,
				      DB_VALUE * prev_key,
				      OID * prev_oid_ptr, int *which_action)
{
  bool found;
  int leaf_not_change;
  int ret = NO_ERROR;
  bool old_check_page_validation;

  /*
   * Following conditions are satisfied.
   * 1. VPID_ISNULL (&(bts->P_vpid))
   */

  old_check_page_validation =
    thread_set_check_page_validation (thread_p, false);

  /* fix the current leaf page again */
  bts->C_page = pgbuf_fix (thread_p, &bts->C_vpid, OLD_PAGE,
			   PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);

  thread_set_check_page_validation (thread_p, old_check_page_validation);

  if (bts->C_page == NULL)
    {
      goto exit_on_error;
    }

  /* check if the current leaf page has been changed */
  if (btree_leaf_lsa_eq (thread_p,
			 &bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page)))
    {
      /* The current leaf page has not been changed. */

      (void) pgbuf_check_page_ptype (thread_p, bts->C_page, PAGE_BTREE);

      if (VPID_ISNULL (&(bts->O_vpid)))
	{
	  /* I'm in leaf node */
	  if (!VPID_ISNULL (&(bts->prev_ovfl_vpid))
	      && bts->oid_pos + oid_idx == 0)
	    {
	      /*
	       * Since I'm in position 0 in leaf node, previous overflow 
	       * belongs to previous key value.
	       * The previous key value has its associated OID overflow pages.
	       * It also means prev_KF_satisfied == true.
	       */
	      bts->slot_id -= 1;

	      bts->O_vpid = bts->prev_ovfl_vpid;
	      bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
				       PGBUF_LATCH_READ,
				       PGBUF_UNCONDITIONAL_LATCH);
	      if (bts->O_page == NULL)
		{
		  goto exit_on_error;
		}

	      (void) pgbuf_check_page_ptype (thread_p, bts->O_page,
					     PAGE_BTREE);

	      bts->oid_pos = bts->prev_oid_pos + 1;

	      /* The locked OID must be checked again. */
	      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

	      return ret;	/* NO_ERROR */
	    }

	  /*
	   * VPID_ISNULL (&(bts->prev_ovfl_vpid))
	   * || (bts->oid_pos + oid_idx) > 0
	   */
	  /* The current OID has not been changed. */
	  *which_action = BTREE_CONTINUE;

	  return ret;		/* NO_ERROR */
	}

      leaf_not_change = true;
    }
  else
    {
      /*
       * ! LSA_EQ(&bts->cur_page_lsa, pgbuf_get_lsa(bts->C_page))
       * The current leaf page has been changed.
       * find the current locked <key, oid> pair
       * Be careful that the locked OID can be deleted.
       */

#if !defined (SERVER_MODE)
      assert_release (false);
#endif

      /* unfix the current leaf page */
      pgbuf_unfix_and_init (thread_p, bts->C_page);
      VPID_SET_NULL (&(bts->C_vpid));

      if ((bts->oid_pos + oid_idx) == 0 && VPID_ISNULL (&(bts->O_vpid)))
	{
	  /* When the first OID of each key is locked */
	  if (bts->prev_oid_pos == -1)
	    {			/* the first request */
	      /* All index pages has been unfixed, now. */
	      *which_action = BTREE_SEARCH_AGAIN_WITH_CHECK;
	      return ret;	/* NO_ERROR */
	    }

	  /*
	   * bts->prev_oid_pos != -1
	   * find the previous key value again
	   */
	  bts->C_page = btree_locate_key (thread_p, &bts->btid_int, prev_key,
					  &bts->C_vpid, &bts->slot_id,
					  &found);

	  if (!found)
	    {
	      if (prev_oid_ptr->pageid == NULL_PAGEID)
		{
		  if (VPID_ISNULL (&(bts->O_vpid)))
		    {
		      if (bts->C_page != NULL)
			{
			  pgbuf_unfix_and_init (thread_p, bts->C_page);
			}
		      *which_action = BTREE_SEARCH_AGAIN_WITH_CHECK;
		      return NO_ERROR;
		    }
		  else
		    {
		      goto search_overflow_page;
		    }
		}

	      /*
	       * Since one or more OIDs associated with
	       * the previous key value have already been locked,
	       * the previous key value must be found in the index.
	       */
	      goto exit_on_error;
	    }

	  /* found */
	  assert (bts->slot_id > 0);
	  assert (bts->C_page != NULL);

	  if (bts->prev_KF_satisfied == true
	      || bts->tran_isolation == TRAN_SERIALIZABLE)
	    {
	      if (!VPID_ISNULL (&(bts->prev_ovfl_vpid)))
		{
		  bts->O_vpid = bts->prev_ovfl_vpid;
		  bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
					   PGBUF_LATCH_READ,
					   PGBUF_UNCONDITIONAL_LATCH);
		  if (bts->O_page == NULL)
		    {
		      goto exit_on_error;
		    }

		  (void) pgbuf_check_page_ptype (thread_p, bts->O_page,
						 PAGE_BTREE);
		}

	      bts->oid_pos = bts->prev_oid_pos + 1;
	      /*
	       * bts->oid_pos is the OID position information.
	       * If the previous key has the OID overflow page,
	       * it indicates the position after
	       * the last OID position on the OID overflow page.
	       * Otherwise, it indicates the position after
	       * the last OID position
	       * on the oidset of the previous index entry.
	       */
	    }
	  else
	    {
	      /*
	       * bts->prev_KF_satisfied == false
	       * bts->O_vpid.pageid must be NULL_PAGEID.
	       */
	      ret = btree_find_next_index_record (thread_p, bts);
	      if (ret != NO_ERROR)
		{
		  goto exit_on_error;
		}
	      /* bts->C_vpid.pageid can be NULL_PAGEID */
	    }

	  /* The locked OID must be checked again. */
	  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

	  return ret;		/* NO_ERROR */
	}

      /* (bts->oid_pos+oid_idx) > 0 || !VPID_ISNULL (&(bts->O_vpid)) */
      /* The locked OID is not the first OID in the index entry */
      bts->C_page = btree_locate_key (thread_p, &bts->btid_int, &bts->cur_key,
				      &bts->C_vpid, &bts->slot_id, &found);

      if (!found)
	{
	  /*
	   * Since one or more OIDs associated with
	   * the current key value have already been locked,
	   * the current key value must be found in the index.
	   */
	  goto exit_on_error;
	}

      /* found */
      assert (bts->slot_id > 0);
      assert (bts->C_page != NULL);

      if (VPID_ISNULL (&(bts->O_vpid)))
	{
	  /* The locked OID must be checked again. */
	  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

	  return ret;		/* NO_ERROR */
	}

    search_overflow_page:

      /* !VPID_ISNULL (&(bts->O_vpid)) : go through */
      leaf_not_change = false;
    }

  /*
   * !VPID_ISNULL (&(bts->O_vpid))
   * fix the overflow page again
   */
  bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			   PGBUF_UNCONDITIONAL_LATCH);
  if (bts->O_page == NULL)
    {
      goto exit_on_error;
    }

  /* check if the overflow page has been changed */
  if (btree_leaf_lsa_eq (thread_p,
			 ovfl_page_lsa, pgbuf_get_lsa (bts->O_page)))
    {
      /*
       * The current overflow page has not been changed
       * the locking is correct
       */

      (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);

      *which_action = BTREE_CONTINUE;

      return ret;		/* NO_ERROR */
    }

  /*
   * !LSA_EQ(ovfl_page_lsa, pgbuf_get_lsa(bts->O_page))
   * The image of the overflow page has been changed.
   */
  if ((bts->oid_pos + oid_idx) > 0)
    {
      /* The locked OID must be checked again. */
      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

      return ret;		/* NO_ERROR */
    }

  /*
   * (bts->oid_pos+idx) == 0 : idx == 0 && bts->oid_pos == 0
   */
  if (leaf_not_change == true && VPID_ISNULL (&(bts->prev_ovfl_vpid)))
    {
      /* In this case, the overflow page is the first OID overflow page
       * that is connected from the current index entry.
       * If the corresponding leaf page has not been changed,
       * it is guaranteed that the OID overflow page is not deallocated.
       * The OID overflow page is still the first OID overflow page.
       */
      /* The locked OID must be checked again. */
      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

      return ret;		/* NO_ERROR */
    }

  /* unfix the overflow page */
  pgbuf_unfix_and_init (thread_p, bts->O_page);
  VPID_SET_NULL (&(bts->O_vpid));

  /* find the next OID again */
  if (!VPID_ISNULL (&(bts->prev_ovfl_vpid)))
    {
      bts->O_vpid = bts->prev_ovfl_vpid;
      bts->O_page = pgbuf_fix (thread_p, &bts->O_vpid, OLD_PAGE,
			       PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (bts->O_page == NULL)
	{
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, bts->O_page, PAGE_BTREE);
    }

  bts->oid_pos = bts->prev_oid_pos + 1;

  /* bts->oid_pos is the OID position information.
   * If the previous OID overflow page exists, it indicates
   * the postion after the last OID position on the OID overflow page.
   * Otherwise, it indicates the position after the last OID position
   * on the oidset of the current index entry.
   */

  /* The locked OID must be checked again. */
  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}


/*
 * btree_lock_current_key () - Locks the current key
 *   return: error code
 *   thread_p(in):
 *   bts(in): the btree scan
 *   prev_oid_locked_ptr(in): pointer to previously locked inst OID
 *   first_key_oid (in): the first oid of the key
 *   class_oid (in): the class oid
 *   scanid_bit(in): scanid bit for scan
 *   prev_key(in): pointer to previous key value
 *   ck_pseudo_oid(out): pseudo OID of the current key
 *   which_action(out): BTREE_CONTINUE
 *                      BTREE_GETOID_AGAIN
 *                      BTREE_GETOID_AGAIN_WITH_CHECK
 *                      BTREE_SEARCH_AGAIN_WITH_CHECK
 *
 * Note:
 *  This function must be called in the following situations:
 *     1. The first OID of the bts->currKey has been previously locked
 *     2. The first OID of the bts->currKey has not been previously locked,
 *     and the last key from tree is not reached
 *     3. The last key must be locked
 *
 *  In cases 1,2: bts->C_page != NULL && !VPID_ISNULL (&(bts->C_vpid))
 *  In    case 3: bts->P_page != NULL && !VPID_ISNULL (&(bts->P_vpid))
 *
 */
static int
btree_lock_current_key (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
			OID * prev_oid_locked_ptr, OID * first_key_oid,
			OID * class_oid, int scanid_bit, DB_VALUE * prev_key,
			OID * ck_pseudo_oid, int *which_action)
{
  int ret_val = NO_ERROR, lock_ret;
  OID oid;
  LOG_LSA prev_leaf_lsa, ovfl_page_lsa;

  if (first_key_oid == NULL || OID_ISNULL (first_key_oid)
      || prev_oid_locked_ptr == NULL || prev_key == NULL
      || ck_pseudo_oid == NULL || which_action == NULL)
    {
      return ER_FAILED;
    }

  *which_action = BTREE_CONTINUE;

  if (bts->C_page != NULL)
    {
      assert (!VPID_ISNULL (&(bts->C_vpid)));
      btree_make_pseudo_oid (first_key_oid->pageid, first_key_oid->slotid,
			     first_key_oid->volid, bts->btid_int.sys_btid,
			     &oid);
    }
  else
    {
      /* lock the last key from tree */
      COPY_OID (&oid, first_key_oid);
    }

  if (OID_EQ (ck_pseudo_oid, &oid))
    {
      /* the current key has already been locked */
      assert (lock_get_object_lock (&oid, class_oid,
				    LOG_FIND_THREAD_TRAN_INDEX (thread_p))
	      >= bts->key_lock_mode);

      goto end;
    }


  if (!OID_ISNULL (ck_pseudo_oid))
    {
      assert (!OID_ISNULL (ck_pseudo_oid));
      assert (!OID_ISNULL (class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p, ck_pseudo_oid,
					       class_oid, bts->key_lock_mode);
      OID_SET_NULL (ck_pseudo_oid);
    }

  assert (!OID_ISNULL (&oid));
  assert (!OID_ISNULL (class_oid));
  lock_ret =
    lock_object_on_iscan (thread_p, &oid, class_oid, bts->btid_int.sys_btid,
			  bts->key_lock_mode, LK_COND_LOCK, scanid_bit);
  if (lock_ret == LK_GRANTED)
    {
      COPY_OID (ck_pseudo_oid, &oid);
      goto end;
    }
  else if (lock_ret == LK_NOTGRANTED_DUE_ABORTED
	   || lock_ret == LK_NOTGRANTED_DUE_ERROR)
    {
      goto error;
    }

  /* Since the current key was not already locked,
   * his OID list can be changed during unconditional key locking.
   * So the current key must be searched again in tree and the OID list
   * must be re-read after unconditional key locking. Save LSA.
   */

  if (bts->P_page != NULL)
    {
      LSA_COPY (&prev_leaf_lsa, pgbuf_get_lsa (bts->P_page));
      pgbuf_unfix_and_init (thread_p, bts->P_page);
    }

  if (bts->O_page != NULL)
    {
      LSA_COPY (&ovfl_page_lsa, pgbuf_get_lsa (bts->O_page));
      pgbuf_unfix_and_init (thread_p, bts->O_page);
    }

  if (bts->C_page)
    {
      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  if (bts->use_desc_index)
    {
      /* permission not granted and descending index scan -> abort
       * to avoid a deadlock
       */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DESC_ISCAN_ABORTED,
	      3, bts->btid_int.sys_btid->vfid.volid,
	      bts->btid_int.sys_btid->vfid.fileid,
	      bts->btid_int.sys_btid->root_pageid);
      goto error;
    }

  /* UNCONDITIONAL lock request */
  assert (!OID_ISNULL (&oid));
  assert (!OID_ISNULL (class_oid));
  lock_ret = lock_object_on_iscan (thread_p, &oid, class_oid,
				   bts->btid_int.sys_btid,
				   bts->key_lock_mode,
				   LK_UNCOND_LOCK, scanid_bit);
  if (lock_ret != LK_GRANTED)
    {
      goto error;
    }

  /* check if the current/previous page has been changed during
     unconditional next key locking */
  if (!VPID_ISNULL (&(bts->P_vpid)))
    {
      /* The previous leaf page does exist. */
      if (btree_handle_prev_leaf_after_locking (thread_p, bts, 0,
						&prev_leaf_lsa, prev_key,
						which_action) != NO_ERROR)
	{
	  goto error;
	}
    }
  else
    {
      /* The previous leaf page does not exist. */
      if (btree_handle_curr_leaf_after_locking (thread_p, bts, 0,
						&ovfl_page_lsa, prev_key,
						prev_oid_locked_ptr,
						which_action) != NO_ERROR)
	{
	  goto error;
	}
    }

  COPY_OID (ck_pseudo_oid, &oid);
end:
  return ret_val;

error:
  return (ret_val == NO_ERROR
	  && (ret_val = er_errid ()) == NO_ERROR) ? ER_FAILED : ret_val;
}

/*
 * btree_lock_next_key () - Locks the next key
 *   return: error code
 *   thread_p(in):
 *   bts(in): the btree scan
 *   prev_oid_locked_ptr(in): pointer to previously locked inst OID
 *   scanid_bit(in): scanid bit for scan
 *   prev_key(in): the previous key
 *   nk_pseudo_oid(in/out): pseudo OID of locked next key
 *   nk_class_oid(in/out):  class OID of locked next key
 *   which_action(in/out): BTREE_CONTINUE
 *                         BTREE_GETOID_AGAIN
 *                         BTREE_GETOID_AGAIN_WITH_CHECK
 *                         BTREE_SEARCH_AGAIN_WITH_CHECK
 *
 * Note:
 *  This function must be called only after acquiring the lock on the
 *  first OID of the bts->currKey
 *
 */
static int
btree_lock_next_key (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
		     OID * prev_oid_locked_ptr, int scanid_bit,
		     DB_VALUE * prev_key, OID * nk_pseudo_oid,
		     OID * nk_class_oid, int *which_action)
{
  VPID next_vpid;
  PAGE_PTR next_page = NULL, temp_page = NULL;
  RECDES peek_rec;
  OID N_class_oid, N_oid;
  LOG_LSA saved_nlsa, *temp_lsa = NULL;
  bool next_page_flag = false, saved_next_page_flag = false;
  LOG_LSA prev_leaf_lsa;
  LOG_LSA ovfl_page_lsa;
  int ret_val = NO_ERROR, lock_ret;
  BTREE_SCAN tmp_bts;
  bool is_last_key;

  /*
   * Assumptions : last accessed leaf page is fixed.
   *    - bts->C_vpid.pageid != NULL
   *    - bts->C_page != NULL
   *    - the first OID associated with bts->currKey is locked by the current
   *      transaction
   *    - the page where the first OID associated with bts->currKey reside
   *      must be unchanged
   */

  if (bts->C_page == NULL || VPID_ISNULL (&(bts->C_vpid))
      || prev_oid_locked_ptr == NULL || prev_key == NULL
      || nk_pseudo_oid == NULL || nk_class_oid == NULL
      || which_action == NULL)
    {
      return ER_FAILED;
    }

  *which_action = BTREE_CONTINUE;

start_point:
  memset (&tmp_bts, 0, sizeof (BTREE_SCAN));
  BTREE_INIT_SCAN (&tmp_bts);
  tmp_bts.C_page = bts->C_page;
  tmp_bts.slot_id = bts->slot_id;

  ret_val =
    btree_find_next_index_record_holding_current (thread_p, &tmp_bts,
						  &peek_rec);
#if 0				/* TODO - need to check return value */
  if (ret_val != NO_ERROR)
    {
      goto error;
    }
#endif

  if (tmp_bts.C_page != NULL && tmp_bts.C_page != bts->C_page)
    {
      next_page_flag = true;
      next_page = tmp_bts.C_page;
      next_vpid = tmp_bts.C_vpid;
    }
  else
    {
      next_page_flag = false;
    }

  /* tmp_bts.C_page is NULL if next record is not exists */
  is_last_key = (tmp_bts.C_page == NULL);
  tmp_bts.C_page = NULL;	/* this page is pointed by bts.C_page(or next_page) */

  if (is_last_key)
    {
      /* the first entry of the root page is used as the next OID */
      N_oid.volid = bts->btid_int.sys_btid->vfid.volid;
      N_oid.pageid = bts->btid_int.sys_btid->root_pageid;
      N_oid.slotid = 0;

      if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
	{
	  COPY_OID (&N_class_oid, &bts->btid_int.topclass_oid);
	}
      else
	{
	  COPY_OID (&N_class_oid, &bts->cls_oid);
	}
    }
  else
    {
      btree_leaf_get_first_oid (&bts->btid_int, &peek_rec, &N_oid,
				&N_class_oid, NULL);
      if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))	/* unique index */
	{
	  assert (!OID_ISNULL (&N_class_oid));
	}
      else
	{
	  COPY_OID (&N_class_oid, &bts->cls_oid);
	}

      btree_make_pseudo_oid (N_oid.pageid, N_oid.slotid, N_oid.volid,
			     bts->btid_int.sys_btid, &N_oid);
    }

  if (OID_EQ (nk_pseudo_oid, &N_oid))
    {
      /* the next key has already been locked */
      assert (lock_get_object_lock (&N_oid, &N_class_oid,
				    LOG_FIND_THREAD_TRAN_INDEX (thread_p))
	      >= bts->key_lock_mode);
      goto end;
    }
  if (!OID_ISNULL (nk_pseudo_oid))
    {
      assert (!OID_ISNULL (nk_pseudo_oid));
      assert (!OID_ISNULL (nk_class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p, nk_pseudo_oid,
					       nk_class_oid,
					       bts->key_lock_mode);
      OID_SET_NULL (nk_pseudo_oid);
      OID_SET_NULL (nk_class_oid);
    }

  assert (!OID_ISNULL (&N_oid));
  assert (!OID_ISNULL (&N_class_oid));
  lock_ret =
    lock_object_on_iscan (thread_p, &N_oid, &N_class_oid,
			  bts->btid_int.sys_btid,
			  bts->key_lock_mode, LK_COND_LOCK, scanid_bit);
  if (lock_ret == LK_GRANTED)
    {
      COPY_OID (nk_pseudo_oid, &N_oid);
      COPY_OID (nk_class_oid, &N_class_oid);
      goto end;
    }
  else if (lock_ret == LK_NOTGRANTED_DUE_ABORTED
	   || lock_ret == LK_NOTGRANTED_DUE_ERROR)
    {
      goto error;
    }

  /* Since the current key was not already locked,
   * its OID list can be changed during unconditional next key locking.
   * So the current key must be searched again in tree and the OID list
   * must be reread after unconditional next key locking. Save LSA.
   */
  if (bts->P_page != NULL)
    {
      LSA_COPY (&prev_leaf_lsa, pgbuf_get_lsa (bts->P_page));
      pgbuf_unfix_and_init (thread_p, bts->P_page);
    }

  if (bts->O_page != NULL)
    {
      LSA_COPY (&ovfl_page_lsa, pgbuf_get_lsa (bts->O_page));
      pgbuf_unfix_and_init (thread_p, bts->O_page);
    }

  if (bts->C_page)
    {
      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  saved_next_page_flag = next_page_flag;
  if (next_page_flag == true)
    {
      temp_lsa = pgbuf_get_lsa (next_page);
      LSA_COPY (&saved_nlsa, temp_lsa);
      pgbuf_unfix_and_init (thread_p, next_page);
      next_page_flag = false;
    }
  if (bts->use_desc_index)
    {
      /* permission not granted and descending index scan -> abort
       * to avoid a deadlock
       */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DESC_ISCAN_ABORTED,
	      3, bts->btid_int.sys_btid->vfid.volid,
	      bts->btid_int.sys_btid->vfid.fileid,
	      bts->btid_int.sys_btid->root_pageid);
      goto error;
    }

  assert (next_page == NULL);
  assert (temp_page == NULL);

  /* UNCONDITIONAL lock request */
  assert (!OID_ISNULL (&N_oid));
  assert (!OID_ISNULL (&N_class_oid));
  lock_ret = lock_object_on_iscan (thread_p, &N_oid, &N_class_oid,
				   bts->btid_int.sys_btid,
				   bts->key_lock_mode,
				   LK_UNCOND_LOCK, scanid_bit);
  if (lock_ret != LK_GRANTED)
    {
      goto error;
    }

  /* check if the current/previous page has been changed during
     unconditional next key locking */
  if (!VPID_ISNULL (&(bts->P_vpid)))
    {
      /* The previous leaf page does exist. */
      if (btree_handle_prev_leaf_after_locking (thread_p, bts, 0,
						&prev_leaf_lsa, prev_key,
						which_action) != NO_ERROR)
	{
	  goto error;
	}
    }
  else
    {
      /* The previous leaf page does not exist. */
      if (btree_handle_curr_leaf_after_locking (thread_p, bts, 0,
						&ovfl_page_lsa, prev_key,
						prev_oid_locked_ptr,
						which_action) != NO_ERROR)
	{
	  goto error;
	}
    }

  if (*which_action != BTREE_CONTINUE)
    {
      /* save the next key locked */
      COPY_OID (nk_pseudo_oid, &N_oid);
      COPY_OID (nk_class_oid, &N_class_oid);
      goto end;
    }

  /* the current page is unchanged and locked;
     we have to check if the next_page has been changed
     during unconditional locking */

  /*restore next_page_flag */
  next_page_flag = saved_next_page_flag;
  /* The first leaf page is valid */
  if (next_page_flag == true)
    {
      next_page = pgbuf_fix_without_validation (thread_p, &next_vpid,
						OLD_PAGE,
						PGBUF_LATCH_READ,
						PGBUF_UNCONDITIONAL_LATCH);
      if (next_page == NULL)
	{
	  next_page_flag = false;
	  goto error;
	}

      temp_lsa = pgbuf_get_lsa (next_page);
      if (!btree_leaf_lsa_eq (thread_p, &saved_nlsa, temp_lsa))
	{
	  pgbuf_unfix_and_init (thread_p, next_page);
	  next_page_flag = false;
	  goto start_point;
	}

      (void) pgbuf_check_page_ptype (thread_p, next_page, PAGE_BTREE);
    }

  COPY_OID (nk_pseudo_oid, &N_oid);
  COPY_OID (nk_class_oid, &N_class_oid);

end:

  if (next_page)
    {
      pgbuf_unfix_and_init (thread_p, next_page);
    }
  if (temp_page)
    {
      pgbuf_unfix_and_init (thread_p, temp_page);
    }

  return ret_val;

error:

  ret_val = (ret_val == NO_ERROR
	     && (ret_val = er_errid ()) == NO_ERROR) ? ER_FAILED : ret_val;

  goto end;
}

/*
 * btree_make_pseudo_oid () - generates a pseudo-OID
 *   return: error code
 *   p(in): page id
 *   s(in): slot id
 *   v(in): volume id
 *   btid(in): btree id
 *   oid(out): the pseudo oid
 */
static void
btree_make_pseudo_oid (int p, short s, short v, BTID * btid, OID * oid)
{
  short catp, pos, cb;

  assert (oid != NULL && btid != NULL);

  pos = (btid->root_pageid % DISK_PAGE_BITS) % (SHRT_MAX - 2);
  catp = (btid->root_pageid / SHRT_MAX) & 0x1;
  cb = pos & 0x7;

  oid->volid = -(pos + 2);
  oid->pageid = p;
  oid->slotid = s + ((v & 0x0f) << 8) + (catp << 12) + (cb << 13);

  assert (oid->volid < NULL_VOLID);
}

/*
 * btree_get_next_key_info () - Advance to next key in b-tree and obtain
 *				information.
 *
 * return		: Scan code.
 * thread_p (in)	: Thread entry.
 * btid (in)		: B-tree identifier.
 * bts (in)		: B-tree scan.
 * num_classes (in)	: Number of class in class_oid_ptr.
 * class_oids_ptr (in)	: Class Object identifiers.
 * index_scan_id_p (in) : Index scan data.
 * key_info (out)	: Array of value pointers to store key information.
 *
 * TODO: Handle unique on hierarchy indexes.
 */
SCAN_CODE
btree_get_next_key_info (THREAD_ENTRY * thread_p, BTID * btid,
			 BTREE_SCAN * bts, int num_classes,
			 OID * class_oids_ptr, INDX_SCAN_ID * index_scan_id_p,
			 DB_VALUE ** key_info)
{
  int error_code = NO_ERROR;
  SCAN_CODE result = S_SUCCESS;
  int oid_size;
  RECDES rec;
  LEAF_REC leaf_pnt;
  OID class_oid, oid;
  int offset;

#if defined(BTREE_DEBUG)
  if (BTREE_INVALID_INDEX_ID (btid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_INDEX_ID,
	      3, btid->vfid.fileid, btid->vfid.volid, btid->root_pageid);
      return -1;
    }
#endif /* BTREE_DEBUG */

  OID_SET_NULL (&class_oid);
  rec.data = NULL;

  /* initialize key filter */
  bts->key_filter = NULL;

  /* copy use desc index information in the BTS to have it available in
   * the b-tree functions.
   */
  if (index_scan_id_p->indx_info)
    {
      bts->use_desc_index = index_scan_id_p->indx_info->use_desc_index;
    }
  else
    {
      bts->use_desc_index = 0;
    }

  if (bts->C_vpid.pageid == NULL_PAGEID)
    {
      /* first btree_get_next_key_info call, initialize bts */
      error_code =
	btree_initialize_bts (thread_p, bts, btid, 1,
			      class_oids_ptr, NULL, NULL, false,
			      index_scan_id_p->copy_buf,
			      index_scan_id_p->copy_buf_len, false,
			      index_scan_id_p->mvcc_need_locks);
      if (error_code != NO_ERROR)
	{
	  goto error;
	}
      error_code = btree_prepare_first_search (thread_p, bts);
      if (error_code != NO_ERROR)
	{
	  goto error;
	}
      /* search is positioned on the first key */
    }
  else
    {
      /* resume search */
      mnt_bt_resumes (thread_p);

      error_code = btree_prepare_next_search (thread_p, bts);
      /* go to next key */
      btree_find_next_index_record (thread_p, bts);
      if (error_code != NO_ERROR)
	{
	  goto error;
	}
    }

  oid_size =
    BTREE_IS_UNIQUE (bts->btid_int.unique_pk) ? 2 * OR_OID_SIZE : OR_OID_SIZE;

  if (bts->C_vpid.pageid == NULL_PAGEID)
    {
      /* Reached the end of leaf level */
      result = S_END;
      goto end;
    }

  /* C_page should be already loaded */
  assert (bts->C_page != NULL);
  if (spage_get_record (bts->C_page, bts->slot_id, &rec, PEEK) != S_SUCCESS)
    {
      goto error;
    }

  btree_clear_key_value (&bts->clear_cur_key, &bts->cur_key);

  /* Read key record */
  btree_read_record (thread_p, &bts->btid_int, bts->C_page, &rec,
		     &bts->cur_key, (void *) &leaf_pnt, BTREE_LEAF_NODE,
		     &bts->clear_cur_key, &offset, COPY_KEY_VALUE, NULL);

  /* TODO: Fill the rest of key information here */
  /* TODO: Do we have to get all oids or should we just count them ?
   * Or maybe select only the first OID? Or a maximum number of OIDs...
   */

  DB_MAKE_INT (key_info[BTREE_KEY_INFO_VOLUMEID], bts->C_vpid.volid);
  DB_MAKE_INT (key_info[BTREE_KEY_INFO_PAGEID], bts->C_vpid.pageid);
  DB_MAKE_INT (key_info[BTREE_KEY_INFO_SLOTID], bts->slot_id);

  /* Get key */
  db_value_clear (key_info[BTREE_KEY_INFO_KEY]);
  db_value_clone (&bts->cur_key, key_info[BTREE_KEY_INFO_KEY]);

  /* Get overflow key and overflow oids */
  db_value_clear (key_info[BTREE_KEY_INFO_OVERFLOW_KEY]);
  DB_MAKE_STRING (key_info[BTREE_KEY_INFO_OVERFLOW_KEY],
		  btree_leaf_is_flaged (&rec,
					BTREE_LEAF_RECORD_OVERFLOW_KEY) ?
		  "true" : "false");
  db_value_clear (key_info[BTREE_KEY_INFO_OVERFLOW_OIDS]);
  DB_MAKE_STRING (key_info[BTREE_KEY_INFO_OVERFLOW_OIDS],
		  btree_leaf_is_flaged (&rec,
					BTREE_LEAF_RECORD_OVERFLOW_OIDS) ?
		  "true" : "false");

  /* Get OIDs count -> For now ignore the overflow OIDs */
  DB_MAKE_INT (key_info[BTREE_KEY_INFO_OID_COUNT],
	       btree_leaf_get_num_oids (&rec, offset, BTREE_LEAF_NODE,
					oid_size));

  /* Get OIDs -> For now just the first OID */
  if (mvcc_Enabled)
    {
      BTREE_SEARCH result =
	btree_key_find_first_visible_row (thread_p, &bts->btid_int, &rec,
					  offset, BTREE_LEAF_NODE,
					  &oid, &class_oid, -1);
      if (result == BTREE_KEY_NOTFOUND)
	{
	  if (!VPID_ISNULL (&(leaf_pnt.ovfl)))
	    {
	      /* search for visible OID into OID overflow page */
	      result =
		btree_key_find_first_visible_row_from_all_ovf (thread_p,
							       &bts->btid_int,
							       &(leaf_pnt.
								 ovfl), &oid,
							       &class_oid);
	      if (result == BTREE_KEY_NOTFOUND)
		{
		  OID_SET_NULL (&oid);
		}
	    }
	  else
	    {
	      OID_SET_NULL (&oid);
	    }
	}
    }
  else
    {
      btree_leaf_get_oid_from_oidptr (bts, rec.data, BTREE_LEAF_NODE, &oid,
				      &class_oid, NULL);
    }
  DB_MAKE_OID (key_info[BTREE_KEY_INFO_FIRST_OID], &oid);

end:
  if (bts->C_page != NULL)
    {
      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  if (bts->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  if (bts->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->P_page);
    }

  return result;

error:
  result = S_ERROR;
  goto end;
}

/*
 * btree_find_min_or_max_key () -
 *   return: NO_ERROR
 *   btid(in):
 *   key(in):
 *   find_min_key(in):
 */
int
btree_find_min_or_max_key (THREAD_ENTRY * thread_p, BTID * btid,
			   DB_VALUE * key, int find_min_key)
{
  VPID root_vpid;
  PAGE_PTR root_page_ptr = NULL;
  int offset;
  bool clear_key = false;
  DB_VALUE key_value;
  BTREE_ROOT_HEADER *root_header = NULL;
  RECDES rec;
  LEAF_REC leaf_pnt;
  BTREE_SCAN btree_scan, *BTS;
  int ret = NO_ERROR;

  if (key == NULL)
    {
      return NO_ERROR;
    }

  DB_MAKE_NULL (key);

  BTS = &btree_scan;
  BTREE_INIT_SCAN (BTS);

  BTS->btid_int.sys_btid = btid;

  root_vpid.pageid = btid->root_pageid;
  root_vpid.volid = btid->vfid.volid;

  root_page_ptr = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			     PGBUF_UNCONDITIONAL_LATCH);
  if (root_page_ptr == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root_page_ptr, PAGE_BTREE);

  root_header = btree_get_root_header (root_page_ptr);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  ret = btree_glean_root_header_info (thread_p, root_header, &BTS->btid_int);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  pgbuf_unfix_and_init (thread_p, root_page_ptr);

  assert (tp_valid_indextype (TP_DOMAIN_TYPE (BTS->btid_int.key_type)));

  /*
   * in case of desc domain index,
   * we have to find the min/max key in opposite order.
   */
  if (BTS->btid_int.key_type->is_desc)
    {
      find_min_key = !find_min_key;
    }

  if (find_min_key)
    {
      BTS->use_desc_index = 0;
    }
  else
    {
      BTS->use_desc_index = 1;
    }

  ret = btree_find_lower_bound_leaf (thread_p, BTS, NULL);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (!BTREE_END_OF_SCAN (BTS))
    {
      assert (BTS->slot_id > 0);
      if (spage_get_record (BTS->C_page, BTS->slot_id, &rec, PEEK) !=
	  S_SUCCESS)
	{
	  goto exit_on_error;
	}

      (void) btree_read_record (thread_p, &BTS->btid_int, BTS->C_page, &rec,
				&key_value, (void *) &leaf_pnt,
				BTREE_LEAF_NODE, &clear_key, &offset,
				PEEK_KEY_VALUE, NULL);
      if (DB_IS_NULL (&key_value))
	{
	  goto exit_on_error;
	}

      (void) pr_clone_value (&key_value, key);

      if (clear_key)
	{
	  pr_clear_value (&key_value);
	  clear_key = false;
	}
    }

end:

  if (BTS->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->P_page);
    }

  if (BTS->C_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->C_page);
    }

  if (BTS->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, BTS->O_page);
    }

  if (root_page_ptr)
    {
      pgbuf_unfix_and_init (thread_p, root_page_ptr);
    }

  if (clear_key)
    {
      pr_clear_value (&key_value);
      clear_key = false;
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * Recovery functions
 */

/*
 * btree_rv_util_save_page_records () - Save a set of page records
 *   return: int
 *   page_ptr(in): Page Pointer
 *   first_slotid(in): First Slot identifier to be saved
 *   rec_cnt(in): Number of slots to be saved
 *   ins_slotid(in): First Slot identifier to reinsert set of records
 *   data(in): Data area where the records will be stored
 *             (Enough space(DB_PAGESIZE) must have been allocated by caller
 *   length(in): Effective length of the data area after save is completed
 *
 * Note: Copy the set of records to designated data area.
 *
 * Note: This is a UTILITY routine, but not an actual recovery routine
 */
int
btree_rv_util_save_page_records (PAGE_PTR page_ptr,
				 INT16 first_slotid,
				 int rec_cnt, INT16 ins_slotid,
				 char *data, int *length)
{
  RECDES rec;
  int i, offset, wasted;
  char *datap;
  int ret = NO_ERROR;

  *length = 0;
  datap = (char *) data + sizeof (RECSET_HEADER);
  offset = sizeof (RECSET_HEADER);
  wasted = DB_WASTED_ALIGN (offset, BTREE_MAX_ALIGN);
  datap += wasted;
  offset += wasted;

  for (i = 0; i < rec_cnt; i++)
    {
      assert (first_slotid + i > 0);
      if (spage_get_record (page_ptr, first_slotid + i, &rec, PEEK)
	  != S_SUCCESS)
	{
	  return ((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
	}

      *(INT16 *) datap = rec.length;
      datap += 2;
      offset += 2;

      *(INT16 *) datap = rec.type;
      datap += 2;
      offset += 2;

      memcpy (datap, rec.data, rec.length);
      datap += rec.length;
      offset += rec.length;
      wasted = DB_WASTED_ALIGN (offset, BTREE_MAX_ALIGN);
      datap += wasted;
      offset += wasted;
    }

  datap = data;
  ((RECSET_HEADER *) datap)->rec_cnt = rec_cnt;
  ((RECSET_HEADER *) datap)->first_slotid = ins_slotid;
  *length = offset;

  return NO_ERROR;
}

/*
 * btree_rv_save_keyval () - Save a < key, value > pair and other information
 *			     for logical log purposes.
 *
 * return	: Error code.
 * btid (in)	: B+tree index identifier.
 * key (in)	: Key to be saved.
 * cls_oid (in) : Class identifier.
 * oid (in)	: Object identifier.
 * mvcc_id (in) : MVCCID for operation (NULL if it is not an MVCC operation).
 * data (out)	: Data area where the above fields will be stored
 *		  (Note: The caller should FREE the allocated area.)
 * length (out) : Length of the data area after save is completed.
 *
 * Note: Copy the adequate key-value information to the data area and return
 *	 this data area.
 *	 The MVCCID is stored in buffer only if is not null. In this case, an
 *	 area at the beginning of recovery data si reserved for the log lsa
 *	 of previous MVCC operation (used by vacuum).
 *
 * Note: This is a UTILITY routine, but not an actual recovery routine
 *
 * Warning: This routine assumes that the keyval is from a leaf page and not a
 *	    non-leaf page.  Because of this assumption, we use the index
 *	    domain and not the non-leaf domain to write out the key value.
 *	    Currently all calls to this routine are from leaf pages. Be
 *	    careful if you add a call to this routine.
 */
int
btree_rv_save_keyval (BTID_INT * btid, DB_VALUE * key, OID * cls_oid,
		      OID * oid, MVCC_BTREE_OP_ARGUMENTS * mvcc_args,
		      char **data, int *length)
{
  char *datap;
  int key_len;
  OR_BUF buf;
  PR_TYPE *pr_type;
  int ret = NO_ERROR;
  size_t size;

  assert (key != NULL);
  assert (cls_oid != NULL);
  assert (oid != NULL);

  *length = 0;

  key_len = (int) btree_get_key_length (key);

  size = OR_BTID_ALIGNED_SIZE +	/* btid */
    (2 * OR_OID_SIZE) +		/* object OID + class OID */
    key_len +			/* key length */
    (2 * INT_ALIGNMENT);	/* extra space for alignment */

  if (mvcc_Enabled)
    {
      /* Extra MVCC information is added */
      size += OR_INT_SIZE;	/* mvcc op purpose */
      if (mvcc_args != NULL)
	{
	  /* MVCCID's may be stored */
	  switch (mvcc_args->purpose)
	    {
	    case MVCC_BTREE_VACUUM_OBJECT:
	    case MVCC_BTREE_VACUUM_INSID:
	      break;
	    case MVCC_BTREE_DELETE_DELID:
	    case MVCC_BTREE_INSERT_DELID:
	    case MVCC_BTREE_INSERT_OBJECT:
	    case MVCC_BTREE_DELETE_OBJECT:
	      size += OR_MVCCID_SIZE;
	      break;
	    case MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO:
	      size += 2 * OR_MVCCID_SIZE;
	      break;
	    default:
	      break;
	    }
	}
    }

  *data = (char *) db_private_alloc (NULL, size);
  if (*data == NULL)
    {
      goto exit_on_error;
    }

  datap = (char *) (*data);

  if (mvcc_Enabled)
    {
      if (mvcc_args != NULL)
	{
	  /* Add purpose */
	  datap = or_pack_int (datap, (int) mvcc_args->purpose);
	  /* Add MVCCID's */
	  switch (mvcc_args->purpose)
	    {
	    case MVCC_BTREE_VACUUM_OBJECT:
	    case MVCC_BTREE_VACUUM_INSID:
	      /* Do we need anything here? */
	      break;
	    case MVCC_BTREE_DELETE_DELID:
	      /* Do we really need this? */
	    case MVCC_BTREE_INSERT_DELID:
	      /* Can we use rcv->mvcc_id? */
	      datap = or_pack_mvccid (datap, mvcc_args->delete_mvccid);
	      break;
	    case MVCC_BTREE_INSERT_OBJECT:
	      /* Can we use rcv->mvcc_id? */
	    case MVCC_BTREE_DELETE_OBJECT:
	      /* We need to log insert MVCCID in order to re-add it
	       * at UNDO. Delete MVCCID should be NULL, otherwise we would
	       * delete an invisible object.
	       */
	      datap = or_pack_mvccid (datap, mvcc_args->insert_mvccid);
	      break;
	    case MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO:
	      datap = or_pack_mvccid (datap, mvcc_args->insert_mvccid);
	      datap = or_pack_mvccid (datap, mvcc_args->delete_mvccid);
	      break;
	    default:
	      break;
	    }
	}
      else
	{
	  /* Add purpose */
	  datap = or_pack_int (datap, (int) MVCC_BTREE_NON_MVCC_OP);
	}
    }

  datap = or_pack_btid (datap, btid->sys_btid);

  OR_PUT_OID (datap, oid);
  datap += OR_OID_SIZE;
  OR_PUT_OID (datap, cls_oid);
  datap += OR_OID_SIZE;
  datap = PTR_ALIGN (datap, INT_ALIGNMENT);
  or_init (&buf, datap, key_len);
  pr_type = btid->key_type->type;
  if ((*(pr_type->index_writeval)) (&buf, key) != NO_ERROR)
    {
      db_private_free_and_init (NULL, *data);
      goto exit_on_error;
    }
  datap += key_len;

  *length = CAST_STRLEN (datap - *data);

  assert (0 < *length);
  assert (*length <= (int) size);

  return ret;

exit_on_error:

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * btree_rv_util_dump_leafrec () -
 *   return: nothing
 *   btid(in):
 *   rec(in): Leaf Record
 *
 * Note: Dump a Tree Leaf Node Record
 *
 * Note: This is a UTILITY routine, but not an actual recovery routine
 */
void
btree_rv_util_dump_leafrec (THREAD_ENTRY * thread_p, FILE * fp,
			    BTID_INT * btid, RECDES * rec)
{
  btree_dump_leaf_record (thread_p, fp, btid, rec, 2);
}

/*
 * btree_rv_util_dump_nleafrec () -
 *   return: nothing
 *   btid(in):
 *   rec(in): NonLeaf Record
 *
 * Note: Dump a Tree NonLeaf Node Record
 *
 * Note: This is a UTILITY routine, but not an actual recovery routine
 */
void
btree_rv_util_dump_nleafrec (THREAD_ENTRY * thread_p, FILE * fp,
			     BTID_INT * btid, RECDES * rec)
{
  btree_dump_non_leaf_record (thread_p, fp, btid, rec, 2, 1);
}
#endif


/*
 * btree_rv_mvcc_undo_redo_increments_update () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover the in-memory unique statistics.
 */
int
btree_rv_mvcc_undo_redo_increments_update (THREAD_ENTRY * thread_p,
					   LOG_RCV * recv)
{
  char *datap;
  int num_nulls, num_oids, num_keys;
  OID class_oid;
  BTID btid;

  assert (recv->length >=
	  (3 * OR_INT_SIZE) + OR_OID_SIZE + OR_BTID_ALIGNED_SIZE);

  /* unpack the root statistics */
  datap = (char *) recv->data;

  OR_GET_OID (datap, &class_oid);
  datap += OR_OID_SIZE;

  OR_GET_BTID (datap, &btid);
  datap += OR_BTID_ALIGNED_SIZE;

  num_keys = OR_GET_INT (datap);
  datap += OR_INT_SIZE;

  num_oids = OR_GET_INT (datap);
  datap += OR_INT_SIZE;

  num_nulls = OR_GET_INT (datap);
  datap += OR_INT_SIZE;

  if (logtb_mvcc_update_class_unique_stats
      (thread_p, &class_oid, &btid, num_keys, num_oids, num_nulls,
       false) != NO_ERROR)
    {
      goto error;
    }

  return NO_ERROR;

error:
  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);

  return ER_GENERIC_ERROR;
}

/*
 * btree_rv_roothdr_undo_update () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover the root header statistics for undo purposes.
 */
int
btree_rv_roothdr_undo_update (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  char *datap;
  BTREE_ROOT_HEADER *root_header = NULL;

  if (recv->length < 3 * OR_INT_SIZE)
    {
      assert (false);
      goto error;
    }

  root_header = btree_get_root_header (recv->pgptr);
  assert (root_header != NULL);

  if (root_header != NULL)
    {
      /* unpack the root statistics */
      datap = (char *) recv->data;
      root_header->num_nulls += OR_GET_INT (datap);
      datap += OR_INT_SIZE;
      root_header->num_oids += OR_GET_INT (datap);
      datap += OR_INT_SIZE;
      root_header->num_keys += OR_GET_INT (datap);
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);

  return ER_GENERIC_ERROR;
}

/*
 * btree_rv_roothdr_dump () -
 *   return:
 *   length(in):
 *   data(in):
 *
 * Note: Dump the root header statistics recovery information.
 */
void
btree_rv_roothdr_dump (FILE * fp, int length, void *data)
{
  char *datap;
  int max_key_len, null_delta, oid_delta, key_delta;

  /* unpack the root statistics */
  datap = (char *) data;
  max_key_len = OR_GET_INT (datap);
  datap += OR_INT_SIZE;
  null_delta = OR_GET_INT (datap);
  datap += OR_INT_SIZE;
  oid_delta = OR_GET_INT (datap);
  datap += OR_INT_SIZE;
  key_delta = OR_GET_INT (datap);
  datap += OR_INT_SIZE;

  fprintf (fp,
	   "\nMAX_KEY_LEN: %d NUM NULLS DELTA: %d"
	   " NUM OIDS DELTA: %d NUM KEYS DELTA: %d\n\n",
	   max_key_len, null_delta, oid_delta, key_delta);
}

/*
 * btree_rv_ovfid_undoredo_update () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover the overflow VFID in the root header
 */
int
btree_rv_ovfid_undoredo_update (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  VFID ovfid;
  BTREE_ROOT_HEADER *root_header = NULL;

  if (recv->length < (int) sizeof (VFID))
    {
      assert (false);
      goto error;
    }

  root_header = btree_get_root_header (recv->pgptr);
  assert (root_header != NULL);

  if (root_header != NULL)
    {
      ovfid = *((VFID *) recv->data);	/* structure copy */
      root_header->ovfid = ovfid;
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);

  return ER_GENERIC_ERROR;
}

/*
 * btree_rv_ovfid_dump () -
 *   return:
 *   length(in):
 *   data(in):
 *
 * Note: Dump the overflow VFID for the root header.
 */
void
btree_rv_ovfid_dump (FILE * fp, int length, void *data)
{
  VFID ovfid;

  ovfid = *((VFID *) data);	/* structure copy */

  fprintf (fp,
	   "\nOverflow key file VFID: %d|%d\n\n", ovfid.fileid, ovfid.volid);
}

/*
 * btree_rv_nodehdr_undoredo_update () - Recover an update to a node header. used either for
 *                         undo or redo
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover the update to a node header
 */
int
btree_rv_nodehdr_undoredo_update (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
#if !defined(NDEBUG)
  RECDES peek_rec;
#endif
  int sp_success;

  rec.area_size = rec.length = recv->length;
  rec.type = REC_HOME;
  rec.data = (char *) recv->data;

#if !defined(NDEBUG)
  if (spage_get_record (recv->pgptr, HEADER, &peek_rec, PEEK) != S_SUCCESS)
    {
      return ER_FAILED;
    }

  assert (rec.length == peek_rec.length);
#endif

  sp_success = spage_update (thread_p, recv->pgptr, HEADER, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      assert (false);
      return er_errid ();
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_nodehdr_redo_insert () - Recover a node header insertion. used for redo
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a node header insertion by reinserting the node header
 * for redo purposes.
 */
int
btree_rv_nodehdr_redo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  int sp_success;

  rec.area_size = rec.length = recv->length;
  rec.type = REC_HOME;
  rec.data = (char *) recv->data;
  sp_success = spage_insert_at (thread_p, recv->pgptr, HEADER, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      assert (false);
      return er_errid ();
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_nodehdr_undo_insert () - Recover a node header insertion. used for undo
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a node header insertion by deletion  the node header
 * for undo purposes.
 */
int
btree_rv_nodehdr_undo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  PGSLOTID pg_slotid;

  pg_slotid = spage_delete (thread_p, recv->pgptr, HEADER);

  assert (pg_slotid != NULL_SLOTID);

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_noderec_undoredo_update () - Recover an update to a node record. used either
 *                         for undo or redo
 *   return:
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover the update to a node record
 */
int
btree_rv_noderec_undoredo_update (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  INT16 slotid;
  int sp_success;

  slotid = recv->offset;
  rec.type = *(INT16 *) ((char *) recv->data + OFFS2);
  rec.area_size = rec.length = recv->length - OFFS3;
  rec.data = (char *) (recv->data) + OFFS3;

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_noderec_undoredo_update - "
		 "page %d|%d, offset=%d, mvccid=%d, rec_type=%d, "
		 "rec_length=%d",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 rec.type, rec.length);

  assert (slotid > 0);
  sp_success = spage_update (thread_p, recv->pgptr, slotid, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      assert (false);
      return er_errid ();
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_noderec_redo_insert () - Recover a node record insertion. used for redo
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a node record insertion by reinserting the record for
 * redo purposes
 */
int
btree_rv_noderec_redo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  INT16 slotid;
  int sp_success;

  slotid = recv->offset;
  rec.type = *(INT16 *) ((char *) recv->data + OFFS2);
  rec.area_size = rec.length = recv->length - OFFS3;
  rec.data = (char *) (recv->data) + OFFS3;

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_noderec_redo_insert - "
		 "page %d|%d, offset=%d, mvccid=%d, rec_type=%d, "
		 "rec_length=%d",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 rec.type, rec.length);

  assert (slotid > 0);
  sp_success = spage_insert_at (thread_p, recv->pgptr, slotid, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      assert (false);
      return er_errid ();
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_noderec_undo_insert () - Recover a node record insertion. used for undo
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a node record insertion by deleting the record for
 * undo purposes
 */
int
btree_rv_noderec_undo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  INT16 slotid;
  PGSLOTID pg_slotid;

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_noderec_undo_insert - "
		 "page %d|%d, offset=%d, mvccid=%d",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset,
		 recv->mvcc_id);

  slotid = recv->offset;
  assert (slotid > 0);
  pg_slotid = spage_delete_for_recovery (thread_p, recv->pgptr, slotid);

  assert (pg_slotid != NULL_SLOTID);

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_noderec_dump () - Dump node record recovery information
 *   return: int
 *   length(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump node record recovery information
 */
void
btree_rv_noderec_dump (FILE * fp, int length, void *data)
{
#if 0
  /* This needs to be fixed.  The easiest way is for the btid to be packed and
   * sent, but this increases the log record.  We may want to allow this
   * routine to know the layout of a node record.  TODO: ???
   */

  int Node_Type;
  RECDES rec;

  Node_Type = *(INT16 *) ((char *) data + OFFS1);
  rec.type = *(INT16 *) ((char *) data + OFFS2);
  rec.area_size = DB_PAGESIZE;
  rec.data = (char *) malloc (DB_PAGESIZE);
  memcpy (rec.data, (char *) data + OFFS3, rec.length);

  if (Node_Type == 0)
    {
      btree_rv_util_dump_leafrec (fp, btid, &rec);
    }
  else
    {
      btree_rv_util_dump_nleafrec (fp, btid, &rec);
    }

  free_and_init (rec.data);
#endif
}

/*
 * btree_rv_noderec_dump_slot_id () -
 *   return: int
 *   length(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump the slot id for the slot to be deleted for undo purposes
 */

void
btree_rv_noderec_dump_slot_id (FILE * fp, int length, void *data)
{
  fprintf (fp, " Slot_id: %d \n", *(INT16 *) data);
}

/*
 * btree_rv_pagerec_insert () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Put a set of records to the page
 */
int
btree_rv_pagerec_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  RECSET_HEADER *recset_header;
  char *datap;
  int i, offset, wasted;
  int sp_success;

  /* initialization */
  recset_header = (RECSET_HEADER *) recv->data;

  /* insert back saved records */
  datap = (char *) recv->data + sizeof (RECSET_HEADER);
  offset = sizeof (RECSET_HEADER);
  wasted = DB_WASTED_ALIGN (offset, BTREE_MAX_ALIGN);
  datap += wasted;
  offset += wasted;
  for (i = 0; i < recset_header->rec_cnt; i++)
    {
      rec.area_size = rec.length = *(INT16 *) datap;
      datap += 2;
      offset += 2;
      rec.type = *(INT16 *) datap;
      datap += 2;
      offset += 2;
      rec.data = datap;
      datap += rec.length;
      offset += rec.length;
      wasted = DB_WASTED_ALIGN (offset, BTREE_MAX_ALIGN);
      datap += wasted;
      offset += wasted;

      assert (recset_header->first_slotid + i > 0);
      sp_success = spage_insert_at (thread_p, recv->pgptr,
				    recset_header->first_slotid + i, &rec);
      if (sp_success != SP_SUCCESS)
	{
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_GENERIC_ERROR, 0);
	    }
	  assert (false);
	  goto error;
	}			/* if */
    }				/* for */

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  assert (er_errid () != NO_ERROR);
  return er_errid ();
}

/*
 * btree_rv_pagerec_delete () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Delete a set of records from the page for undo or redo purpose
 */
int
btree_rv_pagerec_delete (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECSET_HEADER *recset_header;
  int i;

  recset_header = (RECSET_HEADER *) recv->data;

  /* delete all specified records from the page */
  for (i = 0; i < recset_header->rec_cnt; i++)
    {
      assert (recset_header->first_slotid > 0);
      if (spage_delete (thread_p, recv->pgptr,
			recset_header->first_slotid)
	  != recset_header->first_slotid)
	{
	  assert (er_errid () != NO_ERROR);
	  assert (false);
	  return er_errid ();
	}
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_redo_truncate_oid () -
 *   return: int
 *   recv(in):
 *
 * Note: Truncates the last OID off of a node record.
 */
int
btree_rv_redo_truncate_oid (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES copy_rec;
  int oid_size;
  char copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];

  /* initialization */
  oid_size = *(int *) recv->data;

  copy_rec.area_size = DB_PAGESIZE;
  copy_rec.data = PTR_ALIGN (copy_rec_buf, BTREE_MAX_ALIGN);

  assert (recv->offset > 0);
  if (spage_get_record (recv->pgptr, recv->offset, &copy_rec, COPY)
      != S_SUCCESS)
    {
      assert (false);
      goto error;
    }

  /* truncate the last OID */
  copy_rec.length -= oid_size;

  /* write it out */
  assert (recv->offset > 0);
  if (spage_update (thread_p, recv->pgptr, recv->offset, &copy_rec)
      != SP_SUCCESS)
    {
      assert (false);
      goto error;
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  assert (er_errid () != NO_ERROR);
  return er_errid ();
}

/*
 * btree_rv_newpage_redo_init () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Initialize a B+tree page.
 */
int
btree_rv_newpage_redo_init (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  unsigned short alignment;

  (void) pgbuf_set_page_ptype (thread_p, recv->pgptr, PAGE_BTREE);

  alignment = *(unsigned short *) recv->data;
  spage_initialize (thread_p, recv->pgptr,
		    UNANCHORED_KEEP_SEQUENCE, alignment,
		    DONT_SAFEGUARD_RVSPACE);

  return NO_ERROR;
}

/*
 * btree_rv_newpage_undo_alloc () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Undo a new page allocation
 */
int
btree_rv_newpage_undo_alloc (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  PAGEID_STRUCT *pageid_struct;
  int ret = NO_ERROR;

  pageid_struct = (PAGEID_STRUCT *) recv->data;

  ret =
    file_dealloc_page (thread_p, &pageid_struct->vfid, &pageid_struct->vpid);

  assert (ret == NO_ERROR);

  return NO_ERROR;
}

/*
 * btree_rv_newpage_dump_undo_alloc () -
 *   return: int
 *   length(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump undo information of new page creation
 */
void
btree_rv_newpage_dump_undo_alloc (FILE * fp, int length, void *data)
{
  PAGEID_STRUCT *pageid_struct = (PAGEID_STRUCT *) data;

  fprintf (fp,
	   "Deallocating page from Volid = %d, Fileid = %d\n",
	   pageid_struct->vfid.volid, pageid_struct->vfid.fileid);
}

/*
 * btree_rv_read_keyval_info_nocopy () - Recover key value and other
 *					 information on b-tree operation.
 *
 * return	    : Void.
 * thread_p (in)    : Thread entry.
 * datap (in)	    : Buffer containing recovery data.
 * data_size (in)   : Size of recovery data.
 * btid (out)	    : B-tree identifier.
 * cls_oid (out)    : Class identifier.
 * oid (out)	    : Object identifier.
 * mvcc_id (in/out) : Operation MVCCID. It must be NULL for non-MVCC
 *		      operations and not NULL for MVCC operations.
 * key (out)	    : Key value.
 *
 * Note: If it is an MVCC operation recovery (mvcc_id is not NULL), data will
 *	 start with a log lsa (of a previous MVCC operation in log), which
 *	 is used my vacuum only and must be skipped.
 *
 * Warning: This assumes that the key value has the index's domain and not the
 *	    non-leaf domain. This should be the case since this is a logical
 *	    operation and not a physical one.
 */
void
btree_rv_read_keyval_info_nocopy (THREAD_ENTRY * thread_p, char *datap,
				  int data_size, BTID_INT * btid,
				  OID * cls_oid, OID * oid,
				  MVCC_REC_HEADER * p_mvcc_header,
				  DB_VALUE * key)
{
  OR_BUF buf;
  PR_TYPE *pr_type;
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  int key_size = -1;
  DB_TYPE key_type = DB_TYPE_UNKNOWN;
  char *start = datap;
  DISK_ISVALID valid = DISK_ERROR;
  int purpose;
  MVCCID mvccid;

  if (mvcc_Enabled)
    {
      if (p_mvcc_header != NULL)
	{
	  BTREE_INIT_MVCC_HEADER (p_mvcc_header);
	}
      datap = or_unpack_int (datap, &purpose);
      switch (purpose)
	{
	case MVCC_BTREE_NON_MVCC_OP:
	case MVCC_BTREE_VACUUM_INSID:
	case MVCC_BTREE_VACUUM_OBJECT:
	  break;
	case MVCC_BTREE_DELETE_DELID:
	case MVCC_BTREE_INSERT_DELID:
	  datap = or_unpack_mvccid (datap, &mvccid);
	  if (p_mvcc_header)
	    {
	      MVCC_SET_DELID (p_mvcc_header, mvccid);
	      MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID);
	    }
	  break;
	case MVCC_BTREE_DELETE_OBJECT:
	case MVCC_BTREE_INSERT_OBJECT:
	  datap = or_unpack_mvccid (datap, &mvccid);
	  if (p_mvcc_header)
	    {
	      MVCC_SET_INSID (p_mvcc_header, mvccid);
	      MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID);
	    }
	  break;
	case MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO:
	  datap = or_unpack_mvccid (datap, &mvccid);
	  if (p_mvcc_header)
	    {
	      MVCC_SET_INSID (p_mvcc_header, mvccid);
	      MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID);
	    }
	  datap = or_unpack_mvccid (datap, &mvccid);
	  if (p_mvcc_header)
	    {
	      MVCC_SET_DELID (p_mvcc_header, mvccid);
	      MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID);
	    }
	  break;
	}
    }

  /* extract the stored btid, key, oid data */
  datap = or_unpack_btid (datap, btid->sys_btid);

  OR_GET_OID (datap, oid);
  datap += OR_OID_SIZE;
  OR_GET_OID (datap, cls_oid);
  datap += OR_OID_SIZE;

  root_vpid.pageid = btid->sys_btid->root_pageid;	/* read root page */
  root_vpid.volid = btid->sys_btid->vfid.volid;

  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE,
		    PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      goto error;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      goto error;
    }

  if (btree_glean_root_header_info (thread_p, root_header, btid) != NO_ERROR)
    {
      goto error;
    }

  pgbuf_unfix_and_init (thread_p, root);

  datap = PTR_ALIGN (datap, INT_ALIGNMENT);
  or_init (&buf, datap, CAST_STRLEN (data_size - (datap - start)));
  pr_type = btid->key_type->type;

  /* Do not copy the string--just use the pointer.  The pr_ routines
   * for strings and sets have different semantics for length.
   */
  if (pr_type->id == DB_TYPE_MIDXKEY)
    {
      key_size = CAST_STRLEN (buf.endptr - buf.ptr);
    }

  (*(pr_type->index_readval)) (&buf, key, btid->key_type, key_size,
			       false /* not copy */ ,
			       NULL, 0);

  return;

error:

  if (root)
    {
      pgbuf_unfix_and_init (thread_p, root);
    }
}

/*
 * btree_rv_keyval_mvcc_undo_insert () - Undo an MVCC operation on b-tree.
 *
 * return	 : Error code.
 * thread_p (in) : Thread entry.
 * recv (in)	 : Recovery data.
 */
int
btree_rv_keyval_mvcc_undo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  return btree_rv_keyval_undo_insert_internal (thread_p, recv, true);
}

/*
 * btree_rv_keyval_non_mvcc_undo_insert () - Undo a non-MVCC operation on
 *					     b-tree.
 *
 * return	 : Error code.
 * thread_p (in) : Thread entry.
 * recv (in)	 : Recovery data.
 */
int
btree_rv_keyval_non_mvcc_undo_insert (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  return btree_rv_keyval_undo_insert_internal (thread_p, recv, false);
}

/*
 * btree_rv_keyval_undo_insert_internal () - Undo insert operation.
 *
 * return		  : Error code.
 * thread_p (in)	  : Thread entry.
 * recv (in)		  : Recovery data.
 * is_mvcc_operation (in) : True if the data belong to an MVCC operation.
 */
static int
btree_rv_keyval_undo_insert_internal (THREAD_ENTRY * thread_p, LOG_RCV * recv,
				      bool is_mvcc_operation)
{
  BTID_INT btid;
  BTID sys_btid;
  DB_VALUE key;
  OID cls_oid;
  OID oid;
  char *datap;
  int dummy;
  int datasize;
  MVCC_REC_HEADER mvcc_header, *mvcc_header_p = NULL;
  MVCC_BTREE_OP_ARGUMENTS mvcc_args, *mvcc_args_p = NULL;

  if (is_mvcc_operation)
    {
      mvcc_header_p = &mvcc_header;
      mvcc_args_p = &mvcc_args;
    }

  /* btid needs a place to unpack the sys_btid into.  We'll use stack space. */
  btid.sys_btid = &sys_btid;

  /* extract the stored btid, key, oid data */
  datap = (char *) recv->data;
  datasize = recv->length;
  btree_rv_read_keyval_info_nocopy (thread_p, datap, datasize,
				    &btid, &cls_oid, &oid,
				    mvcc_header_p, &key);
  assert (!OID_ISNULL (&cls_oid));
  assert (!OID_ISNULL (&oid));

  if (is_mvcc_operation)
    {
      /* TODO: We should pass insert MVCCID here...? */
      /* TODO: MVCC_BTREE_DELETE_OBJECT is removed due to recovery issue
       *       regarding MVCCID. Must find a solution to recover MVCC info on
       *       rollback (otherwise we will have inconsistencies regarding
       *       visibility).
       */
      /* mvcc_args_p->purpose = MVCC_BTREE_DELETE_OBJECT; */
    }
  if (btree_delete (thread_p, btid.sys_btid, &key, &cls_oid, &oid,
		    BTREE_NO_KEY_LOCKED, &dummy, SINGLE_ROW_MODIFY,
		    (BTREE_UNIQUE_STATS *) NULL,
		    NULL /* mvcc_args_p */ ) == NULL)
    {
      int err;

      err = er_errid ();
      assert (err == ER_BTREE_UNKNOWN_KEY || err == NO_ERROR);
      return err;
    }

  return NO_ERROR;
}

/*
 * btree_rv_keyval_undo_insert_mvcc_delid () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Undo the insertion of a <key, val> pair to the B+tree,
 * by deleting the <key, val> pair from the tree.
 */
int
btree_rv_keyval_undo_insert_mvcc_delid (THREAD_ENTRY * thread_p,
					LOG_RCV * recv)
{
  BTID_INT btid;
  BTID sys_btid;
  DB_VALUE key;
  OID cls_oid;
  OID oid;
  char *datap;
  int dummy;
  int datasize;
  MVCC_BTREE_OP_ARGUMENTS mvcc_args;
  MVCC_REC_HEADER mvcc_header;

  assert (mvcc_Enabled == true);
  /* btid needs a place to unpack the sys_btid into.  We'll use stack space. */
  btid.sys_btid = &sys_btid;

  /* extract the stored btid, key, oid data */
  datap = (char *) recv->data;
  datasize = recv->length;
  btree_rv_read_keyval_info_nocopy (thread_p, datap, datasize,
				    &btid, &cls_oid, &oid, &mvcc_header,
				    &key);
  assert (!OID_ISNULL (&cls_oid));
  assert (!OID_ISNULL (&oid));


  /* Set purpose of delete to remove DELID */
  mvcc_args.purpose = MVCC_BTREE_DELETE_DELID;
  mvcc_args.delete_mvccid = MVCC_GET_DELID (&mvcc_header);
  if (btree_delete (thread_p, btid.sys_btid, &key, &cls_oid, &oid,
		    BTREE_NO_KEY_LOCKED, &dummy, SINGLE_ROW_MODIFY,
		    (BTREE_UNIQUE_STATS *) NULL, &mvcc_args) == NULL)
    {
      int err;

      err = er_errid ();
      assert (err == ER_BTREE_UNKNOWN_KEY || err == NO_ERROR);
      return err;
    }

  return NO_ERROR;
}

/*
  * btree_rv_keyval_undo_delete_mvccid () - undo mvccid deletion
  *   return: int
  *   recv(in): Recovery structure
  */
int
btree_rv_keyval_undo_delete_mvccid (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  BTID_INT btid;
  BTID sys_btid;
  DB_VALUE key;
  OID cls_oid;
  OID oid;
  char *datap;
  int datasize;
  MVCC_REC_HEADER mvcc_rec_header;

  assert (mvcc_Enabled);

  /* btid needs a place to unpack the sys_btid into.  We'll use stack space. */
  btid.sys_btid = &sys_btid;

  /* extract the stored btid, key, oid data */
  datap = (char *) recv->data;
  datasize = recv->length;
  btree_rv_read_keyval_info_nocopy (thread_p, datap, datasize,
				    &btid, &cls_oid, &oid, &mvcc_rec_header,
				    &key);

  assert (MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID)
	  && MVCC_GET_DELID (&mvcc_rec_header) != MVCCID_NULL);

  if (btree_insert (thread_p, btid.sys_btid, &key, &cls_oid, &oid,
		    SINGLE_ROW_MODIFY, (BTREE_UNIQUE_STATS *) NULL, NULL,
		    &mvcc_rec_header) == NULL)
    {
      int err;

      err = er_errid ();
      assert (err == ER_BTREE_UNKNOWN_KEY || err == NO_ERROR);
      return err;
    }

  return NO_ERROR;
}

/*
 * btree_rv_keyval_undo_delete () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: undo the deletion of a <key, val> pair to the B+tree,
 * by inserting the <key, val> pair to the tree.
 */
int
btree_rv_keyval_undo_delete (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  BTID_INT btid;
  BTID sys_btid;
  DB_VALUE key;
  OID cls_oid;
  OID oid;
  char *datap;
  int datasize;
  MVCC_REC_HEADER mvcc_header, *p_mvcc_header = NULL;

  if (mvcc_Enabled)
    {
      p_mvcc_header = &mvcc_header;
    }

  /* btid needs a place to unpack the sys_btid into.  We'll use stack space. */
  btid.sys_btid = &sys_btid;

  /* extract the stored btid, key, oid data */
  datap = (char *) recv->data;
  datasize = recv->length;
  btree_rv_read_keyval_info_nocopy (thread_p, datap, datasize,
				    &btid, &cls_oid, &oid, p_mvcc_header,
				    &key);
  /* TODO: The MVCC relocate case won't work. btree_insert expect to have
   *       only one flag set: OR_MVCC_VALID_INSID or OR_MVCC_VALID_DELID.
   *       Having both set will break it!.
   */
  assert (!MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID)
	  || !MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID));

  assert (!OID_ISNULL (&cls_oid));
  assert (!OID_ISNULL (&oid));

  if (p_mvcc_header->mvcc_flag == 0)
    {
      p_mvcc_header = NULL;
    }

  if (btree_insert (thread_p, btid.sys_btid, &key, &cls_oid, &oid,
		    SINGLE_ROW_MODIFY, (BTREE_UNIQUE_STATS *) NULL,
		    NULL, p_mvcc_header) == NULL)
    {
      int err;

      err = er_errid ();
      assert (err == ER_BTREE_DUPLICATE_OID || err == NO_ERROR);
      return err;
    }

  return NO_ERROR;
}

/*
 * btree_rv_keyval_dump () - Dump undo information <key-value> insertion.
 *
 * return      : Void.
 * fp (in)     : File pointer.
 * length (in) : Data length.
 * data (in)   : Recovery data.
 */
void
btree_rv_keyval_dump (FILE * fp, int length, void *data)
{
  BTID btid;
  OID oid;
  int purpose;
  MVCCID ins_mvccid, del_mvccid;

  if (mvcc_Enabled)
    {
      data = or_unpack_int (data, &purpose);
      switch (purpose)
	{
	case MVCC_BTREE_VACUUM_OBJECT:
	  fprintf (fp, " Logged by vacuuming object \n");
	  break;
	case MVCC_BTREE_VACUUM_INSID:
	  fprintf (fp, " Logged by vacuuming insert MVCCID \n");
	  break;
	case MVCC_BTREE_DELETE_DELID:
	  data = or_unpack_mvccid (data, &del_mvccid);
	  fprintf (fp, " Logged by delete of DELID (del_mvccid=%lld) \n",
		   del_mvccid);
	  break;
	case MVCC_BTREE_INSERT_DELID:
	  data = or_unpack_mvccid (data, &del_mvccid);
	  fprintf (fp, " Logged by insert of DELID  (del_mvccid=%lld) \n",
		   del_mvccid);
	  break;
	case MVCC_BTREE_INSERT_OBJECT:
	  data = or_unpack_mvccid (data, &ins_mvccid);
	  fprintf (fp, " Logged by insert new object (ins_mvccid=%lld) \n",
		   ins_mvccid);
	  break;
	case MVCC_BTREE_DELETE_OBJECT:
	  data = or_unpack_mvccid (data, &ins_mvccid);
	  fprintf (fp, " Logged by delete object (ins_mvccid=%lld) \n",
		   ins_mvccid);
	  break;
	case MVCC_BTREE_RELOCATE_OBJ_AND_MVCC_INFO:
	  data = or_unpack_mvccid (data, &ins_mvccid);
	  data = or_unpack_mvccid (data, &del_mvccid);
	  fprintf (fp, " Logged by insert new object (ins_mvccid=%lld, "
		   "del_mvccid=%lld) \n", ins_mvccid, del_mvccid);
	  break;
	default:
	  break;
	}
    }

  data = or_unpack_btid (data, &btid);
  fprintf (fp, " BTID = { { %d , %d }, %d} \n ",
	   btid.vfid.volid, btid.vfid.fileid, btid.root_pageid);

  or_unpack_oid (data, &oid);
  fprintf (fp, " OID = { %d, %d, %d } \n", oid.volid, oid.pageid, oid.slotid);
}

/*
 * btree_rv_undoredo_copy_page () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Copy a whole page back for undo or redo purposes
 */
int
btree_rv_undoredo_copy_page (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  (void) pgbuf_set_page_ptype (thread_p, recv->pgptr, PAGE_BTREE);	/* redo */

  (void) memcpy (recv->pgptr, recv->data, DB_PAGESIZE);

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_leafrec_redo_delete () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a leaf record deletion for redo purposes
 */
int
btree_rv_leafrec_redo_delete (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  BTREE_NODE_HEADER *header = NULL;
  RECDES rec;
  INT16 slotid;
  int key_cnt;

  slotid = recv->offset;
  rec.length = recv->length;
  rec.area_size = recv->length;
  rec.data = (char *) recv->data;

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_leafrec_redo_delete - "
		 "page %d|%d, offset=%d, mvccid=%d, rec_length=%d",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 rec.length);

  header = btree_get_node_header (recv->pgptr);
  if (header == NULL)
    {
      goto error;
    }

  /* redo the deletion of the btree slot */
  assert (slotid > 0);
  if (spage_delete (thread_p, recv->pgptr, slotid) != slotid)
    {
      assert (false);
      goto error;
    }

  /* update the page header */
  key_cnt = btree_node_number_of_keys (recv->pgptr);
  if (key_cnt == 0)
    {
      header->max_key_len = 0;
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  assert (er_errid () != NO_ERROR);
  return er_errid ();
}

/*
 * btree_rv_leafrec_redo_insert_key () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a leaf record key insertion for redo purposes
 */
int
btree_rv_leafrec_redo_insert_key (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  BTREE_NODE_HEADER *header = NULL;
  RECDES rec;
  int key_cnt;
  INT16 slotid;
  int key_len;
  int sp_success;

  rec.data = NULL;

  slotid = recv->offset;
  key_len = *(INT16 *) ((char *) recv->data + LOFFS1);
  rec.type = *(INT16 *) ((char *) recv->data + LOFFS3);
  rec.area_size = rec.length = recv->length - LOFFS4;
  rec.data = (char *) (recv->data) + LOFFS4;

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_leafrec_redo_insert_key - "
		 "page %d|%d, offset=%d, mvccid=%d, rec_length=%d, "
		 "rec_type=%d",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 rec.length, rec.type);

  /* insert the new record */
  assert (slotid > 0);
  sp_success = spage_insert_at (thread_p, recv->pgptr, slotid, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	}
      assert (false);
      goto error;
    }

  key_cnt = btree_node_number_of_keys (recv->pgptr);

  header = btree_get_node_header (recv->pgptr);
  if (header == NULL)
    {
      goto error;
    }

  key_len = BTREE_GET_KEY_LEN_IN_PAGE (key_len);
  header->max_key_len = MAX (header->max_key_len, key_len);

  assert (header->split_info.pivot >= 0 && key_cnt > 0);

  btree_split_next_pivot (&header->split_info, (float) slotid / key_cnt,
			  key_cnt);

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;

error:

  assert (er_errid () != NO_ERROR);
  return er_errid ();
}

/*
 * btree_rv_leafrec_undo_insert_key () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a leaf record key insertion for undo purposes
 */
int
btree_rv_leafrec_undo_insert_key (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  INT16 slotid;
  PGSLOTID pg_slotid;

  slotid = recv->offset;

  /* delete the new record */
  assert (slotid > 0);
  pg_slotid = spage_delete_for_recovery (thread_p, recv->pgptr, slotid);

  assert (pg_slotid != NULL_SLOTID);

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * btree_rv_leafrec_redo_insert_oid () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a leaf record oid insertion for redo purposes
 * -----------------------------------------------------------------------------
 * func                            rec_type new_ovflpg ovfl_changed oid_inserted
 * -----------------------------------------------------------------------------
 * btree_insert_oid_into_leaf_rec  _REGULAR false      false        true
 * btree_append_overflow_oids_page _REGULAR true/false true         false
 * btree_start_overflow_page       OVERFLOW true       true         true
 * btree_insert_oid_overflow_page  OVERFLOW false      false        true
 * -----------------------------------------------------------------------------
 */
int
btree_rv_leafrec_redo_insert_oid (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  INT16 slotid = recv->offset;
  RECINS_STRUCT *recins = (RECINS_STRUCT *) recv->data;
  int sp_success;
  int data_offset;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  BTREE_OVERFLOW_HEADER ovf_header_info, *ovf_header = NULL;
  MVCC_REC_HEADER mvcc_rec_header, *p_mvcc_rec_header = NULL;
  bool is_unique = false;
  bool ovfl_changed = false;
  bool new_ovfl_page = false;
  bool oid_inserted = false;
  bool has_key_domain = false;
  bool check_valid = false;
  BTID_INT btid_int;

  is_unique = BTREE_INSERT_RCV_IS_UNIQUE (recins);
  ovfl_changed = BTREE_INSERT_RCV_IS_OVFL_CHANGED (recins);
  new_ovfl_page = BTREE_INSERT_RCV_IS_NEW_OVFLPG (recins);
  oid_inserted = BTREE_INSERT_RCV_IS_OID_INSERTED (recins);
  has_key_domain = BTREE_INSERT_RCV_HAS_KEY_DOMAIN (recins);

  data_offset = sizeof (*recins);

  btid_int.unique_pk = is_unique ? 1 : 0;
  btid_int.key_type = NULL;
  if (is_unique)
    {
      /* Top class OID is packed */
      COPY_OID (&btid_int.topclass_oid, (OID *) (recv->data + data_offset));
      data_offset += sizeof (btid_int.topclass_oid);
    }
  else
    {
      OID_SET_NULL (&btid_int.topclass_oid);
    }

#if !defined (NDEBUG)
  if (has_key_domain)
    {
      (void) or_unpack_domain ((char *) recv->data + data_offset,
			       &btid_int.key_type, NULL);
      check_valid = true;
    }
#endif

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_leafrec_redo_insert_oid - "
		 "page %d|%d, offset=%d, mvccid=%d, "
		 "recins: oid=%d|%d|%d, flags=%d, "
		 "ovfl_vpid=%d|%d.",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 recins->oid.volid, recins->oid.pageid, recins->oid.slotid,
		 recins->flags, recins->ovfl_vpid.volid,
		 recins->ovfl_vpid.pageid);

  if (mvcc_Enabled)
    {
      mvcc_rec_header.mvcc_flag = 0;
      if (MVCCID_IS_VALID (recv->mvcc_id))
	{
	  /* This is an MVCC operations */
	  mvcc_rec_header.mvcc_ins_id = recv->mvcc_id;
	  mvcc_rec_header.mvcc_flag = OR_MVCC_FLAG_VALID_INSID;
	}
      p_mvcc_rec_header = &mvcc_rec_header;

      if (BTREE_INSERT_RCV_IS_RECORD_OVERFLOW (recins))
	{
	  /* fixed size when insert OID in unique btree */
	  BTREE_MVCC_SET_HEADER_FIXED_SIZE (p_mvcc_rec_header);
	}
    }

  ovf_header = &ovf_header_info;

  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  if (BTREE_INSERT_RCV_IS_RECORD_REGULAR (recins))
    {
      /* read the record */
      assert (slotid > 0);
      if (spage_get_record (recv->pgptr, slotid, &rec, COPY) != S_SUCCESS)
	{
	  assert (false);
	  goto error;
	}

      if (check_valid)
	{
	  /* Check record was valid before applying redo changes */
	  (void) btree_check_valid_record (thread_p, &btid_int, &rec,
					   BTREE_LEAF_NODE, NULL);
	}

      if (oid_inserted)
	{
	  MVCC_REC_HEADER saved_first_mvcc_rec_header;
	  OID saved_first_oid, saved_first_class_oid;
	  OID *oid, *cls_oid;
	  /* redo: btree_insert_oid_into_leaf_rec () */

	  assert (new_ovfl_page == false);
	  assert (ovfl_changed == false);

	  oid = &(recins->oid);
	  cls_oid = &(recins->class_oid);

	  if (!BTREE_INSERT_RCV_IS_INSMODE_DEFAULT (recins))
	    {
	      /* in MVCC, in case of unique indexes, do not insert at end */
	      assert (mvcc_Enabled && is_unique);
	      btree_leaf_get_first_oid (&btid_int, &rec, &saved_first_oid,
					&saved_first_class_oid,
					&saved_first_mvcc_rec_header);
	      BTREE_MVCC_SET_HEADER_FIXED_SIZE (&saved_first_mvcc_rec_header);

	      btree_leaf_change_first_oid (&rec, &btid_int, &recins->oid,
					   &recins->class_oid,
					   p_mvcc_rec_header, NULL);

	      /* prepare for insertion of saved first OID at the end of the
	       * buffer.
	       */
	      if (BTREE_INSERT_RCV_IS_INSMODE_AT_BEGINNING (recins))
		{
		  oid = &saved_first_oid;
		  cls_oid = &saved_first_class_oid;
		  p_mvcc_rec_header = &saved_first_mvcc_rec_header;
		}
	    }

	  if (BTREE_INSERT_RCV_IS_INSMODE_REPLACE_FIRST (recins))
	    {
	      /* nothing to do */
	    }
	  else if (VPID_ISNULL (&recins->ovfl_vpid))
	    {
	      int oid_size = OR_OID_SIZE;
	      btree_append_oid (&rec, oid);
	      if (is_unique == true)
		{
		  btree_append_oid (&rec, cls_oid);
		  oid_size += OR_OID_SIZE;
		}

	      if (mvcc_Enabled)
		{
		  btree_append_mvcc_info (&rec, p_mvcc_rec_header, oid_size);
		}
	    }
	  else
	    {
	      assert (oid_inserted);
	      btree_insert_oid_in_front_of_ovfl_vpid (&rec, oid, cls_oid,
						      is_unique,
						      &recins->ovfl_vpid,
						      p_mvcc_rec_header);
	    }
	}
      else
	{
	  /* redo: btree_append_overflow_oids_page () */
	  assert (ovfl_changed);
	}

      if (ovfl_changed)
	{
	  if (new_ovfl_page)
	    {
	      btree_leaf_new_overflow_oids_vpid (&rec, &recins->ovfl_vpid,
						 is_unique,
						 &recins->class_oid);
	    }
	  else
	    {
	      btree_leaf_update_overflow_oids_vpid (&rec, &recins->ovfl_vpid);
	    }
	}

      if (check_valid)
	{
	  (void) btree_check_valid_record (thread_p, &btid_int, &rec,
					   BTREE_LEAF_NODE, NULL);
	}

      assert (slotid > 0);
      sp_success = spage_update (thread_p, recv->pgptr, slotid, &rec);
      if (sp_success != SP_SUCCESS)
	{
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_GENERIC_ERROR, 0);
	    }
	  assert (false);
	  goto error;
	}
    }
  else
    {
#if !defined(NDEBUG)
      if (mvcc_Enabled == false)
	{
	  assert (OID_ISNULL (&recins->class_oid));
	}
#endif

      assert (oid_inserted);

      if (new_ovfl_page)
	{
	  /* redo: btree_start_overflow_page () */

	  assert (ovfl_changed);
	  if (ovfl_changed)
	    {
	      ovf_header->next_vpid = recins->ovfl_vpid;
	    }
	  else
	    {
	      VPID_SET_NULL (&(ovf_header->next_vpid));
	    }

	  if (btree_init_overflow_header (thread_p, recv->pgptr, ovf_header)
	      != NO_ERROR)
	    {
	      assert (false);
	      goto error;
	    }

	  /* insert the value in the new overflow page */
	  rec.type = REC_HOME;
	  rec.length = 0;
	  btree_append_oid (&rec, &recins->oid);
	  if (mvcc_Enabled == true)
	    {
	      int oid_size = OR_OID_SIZE;
	      if (is_unique == true)
		{
		  btree_append_oid (&rec, &recins->class_oid);
		  oid_size += OR_OID_SIZE;
		}
	      btree_append_mvcc_info (&rec, p_mvcc_rec_header, oid_size);
	    }

	  sp_success = spage_insert_at (thread_p, recv->pgptr, 1, &rec);
	  if (sp_success != SP_SUCCESS)
	    {
	      if (sp_success != SP_ERROR)
		{
		  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_GENERIC_ERROR, 0);
		}
	      assert (false);
	      goto error;
	    }
	}
      else
	{
	  /* redo: btree_insert_oid_overflow_page () */

	  assert (ovfl_changed == false);
	  assert (slotid == 1);

	  if (oid_inserted)
	    {
	      /* read the record */
	      assert (slotid > 0);
	      if (spage_get_record (recv->pgptr, slotid, &rec, COPY)
		  != S_SUCCESS)
		{
		  assert (false);
		  goto error;
		}

	      assert (rec.length >= OR_OID_SIZE);
	      assert (rec.length % 4 == 0);

	      if (check_valid)
		{
		  (void) btree_check_valid_record (thread_p, &btid_int, &rec,
						   BTREE_OVERFLOW_NODE, NULL);
		}

	      if (btree_insert_oid_with_order
		  (&rec, &recins->oid, &recins->class_oid, is_unique,
		   p_mvcc_rec_header) != NO_ERROR)
		{
		  assert (false);
		  goto error;
		}

	      if (check_valid)
		{
		  (void) btree_check_valid_record (thread_p, &btid_int, &rec,
						   BTREE_OVERFLOW_NODE, NULL);
		}

	      assert (slotid > 0);
	      sp_success = spage_update (thread_p, recv->pgptr, slotid, &rec);
	      if (sp_success != SP_SUCCESS)
		{
		  if (sp_success != SP_ERROR)
		    {
		      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
			      ER_GENERIC_ERROR, 0);
		    }
		  assert (false);
		  goto error;
		}
	    }

	  if (ovfl_changed)
	    {
	      ovf_header = btree_get_overflow_header (recv->pgptr);
	      if (ovf_header == NULL)
		{
		  goto error;
		}

	      ovf_header->next_vpid = recins->ovfl_vpid;
	    }
	}
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  if (btid_int.key_type != NULL)
    {
      tp_domain_free (btid_int.key_type);
    }

  return NO_ERROR;

error:

  if (btid_int.key_type != NULL)
    {
      tp_domain_free (btid_int.key_type);
    }

  assert (er_errid () != NO_ERROR);
  return er_errid ();
}

/*
 * btree_rv_redo_insert_mvcc_delid () -
 *   return: int
 *   recv(in): Recovery structure
 *
 * Note: Recover a leaf record oid insertion for redo purposes
 */
int
btree_rv_redo_insert_mvcc_delid (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  RECDES rec;
  INT16 slotid = recv->offset;
  int sp_success;
  char rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int oid_size = OR_OID_SIZE;
  int data_offset = 0, mvcc_delid_offset = 0;
  int oid_offset = 0;
  bool is_unique = false;
  bool is_overflow = false;
  bool has_key_domain = false;
  bool check_valid = false;
  bool have_mvcc_fixed_size = false;
  BTID_INT btid_int;

  btid_int.key_type = NULL;

  /* Read DELID insert recovery structure */
  oid_offset = *((int *) recv->data);
  data_offset += sizeof (oid_offset);

  is_unique = BTREE_INSERT_DELID_RCV_IS_UNIQUE (oid_offset);
  is_overflow = BTREE_INSERT_DELID_RCV_IS_OVERFLOW (oid_offset);
  has_key_domain = BTREE_INSERT_DELID_RCV_HAS_KEY_DOMAIN (oid_offset);
  BTREE_INSERT_DELID_RCV_CLEAR_FLAGS (oid_offset);

#if !defined (NDEBUG)
  if (has_key_domain)
    {
      (void) or_unpack_domain ((char *) (recv->data + data_offset),
			       &btid_int.key_type, NULL);
      btid_int.unique_pk = is_unique ? 1 : 0;
      if (btid_int.key_type != NULL)
	{
	  /* If we have key domain available, we can check record validity */
	  check_valid = true;
	}
    }
#endif

  _er_log_debug (ARG_FILE_LINE,
		 "BTREE_RECOVERY: btree_rv_redo_insert_mvcc_delid - "
		 "page %d|%d, offset=%d, mvccid=%d, "
		 "rec_mvcc_delid_ins: is_ovf=%d, unique=%d, oid_offset=%d, ",
		 pgbuf_get_volume_id (recv->pgptr),
		 pgbuf_get_page_id (recv->pgptr), recv->offset, recv->mvcc_id,
		 is_overflow, is_unique, oid_offset);

  rec.area_size = DB_PAGESIZE;
  rec.data = PTR_ALIGN (rec_buf, BTREE_MAX_ALIGN);

  if (spage_get_record (recv->pgptr, slotid, &rec, COPY) != S_SUCCESS)
    {
      assert (false);
      goto error;
    }

  if (check_valid)
    {
      /* Check record was valid before making any changes */
      (void) btree_check_valid_record (thread_p, &btid_int, &rec,
				       is_overflow ? BTREE_OVERFLOW_NODE :
				       BTREE_LEAF_NODE, NULL);
    }

  if (is_unique)
    {
      if (is_overflow == true || oid_offset > 0
	  || btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  /* unique index containing OID, CLASS OID */
	  oid_size += OR_OID_SIZE;
	}
    }

  /* Compute the offset for delete MVCCID */
  mvcc_delid_offset = oid_offset + oid_size;
  if ((is_unique && oid_offset > 0)
      || (is_overflow == true)
      || (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	  && (oid_offset == 0)))
    {
      /* Fixed size when:
       * 1. OID belongs to overflow page.
       * 2. OID is non-first in an unique leaf record.
       * 3. OID is first in a leaf record that has overflow OID's.
       * Fixed size means also insert MVCCID which must be skipped.
       */
      have_mvcc_fixed_size = true;
      mvcc_delid_offset += OR_MVCCID_SIZE;
    }
  else if (btree_leaf_key_oid_is_mvcc_flaged (rec.data + oid_offset,
					      BTREE_LEAF_OID_HAS_MVCC_INSID))
    {
      /* Skip insert MVCCID */
      mvcc_delid_offset += OR_MVCCID_SIZE;
    }

  if (have_mvcc_fixed_size
      || btree_leaf_key_oid_is_mvcc_flaged (rec.data + oid_offset,
					    BTREE_LEAF_OID_HAS_MVCC_DELID))
    {
      /* Replace current delete MVCCID */
      btree_set_mvcc_delid (&rec, oid_offset, mvcc_delid_offset,
			    &recv->mvcc_id);
    }
  else
    {
      /* Add delete MVCCID to record */
      btree_add_mvcc_delid (&rec, oid_offset, mvcc_delid_offset,
			    &recv->mvcc_id);
    }

  if (check_valid)
    {
      /* Check record is valid after applied redo changes */
      (void) btree_check_valid_record (thread_p, &btid_int, &rec,
				       is_overflow ? BTREE_OVERFLOW_NODE :
				       BTREE_LEAF_NODE, NULL);
    }

  /* Update record */
  sp_success = spage_update (thread_p, recv->pgptr, slotid, &rec);
  if (sp_success != SP_SUCCESS)
    {
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR,
		  0);
	}

      assert (false);
      goto error;
    }

  pgbuf_set_dirty (thread_p, recv->pgptr, DONT_FREE);

  if (btid_int.key_type != NULL)
    {
      tp_domain_free (btid_int.key_type);
    }

  return NO_ERROR;

error:
  if (btid_int.key_type != NULL)
    {
      tp_domain_free (btid_int.key_type);
    }
  return er_errid ();
}

/*
 * btree_rv_leafrec_dump_insert_oid () -
 *   return: nothing
 *   length(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump recovery data of leaf record oid insertion
 */
void
btree_rv_leafrec_dump_insert_oid (FILE * fp, int length, void *data)
{
  RECINS_STRUCT *recins = (RECINS_STRUCT *) data;

  fprintf (fp, "LEAF RECORD OID INSERTION STRUCTURE: \n");
  fprintf (fp, "Class OID: { %d, %d, %d }\n",
	   recins->class_oid.volid,
	   recins->class_oid.pageid, recins->class_oid.slotid);
  fprintf (fp, "OID: { %d, %d, %d } \n",
	   recins->oid.volid, recins->oid.pageid, recins->oid.slotid);
  fprintf (fp, "RECORD TYPE: %s \n",
	   (BTREE_INSERT_RCV_IS_RECORD_REGULAR (recins) ? "REGULAR" :
	    "OVERFLOW"));
  fprintf (fp, "Overflow Page Id: {%d , %d}\n", recins->ovfl_vpid.volid,
	   recins->ovfl_vpid.pageid);
  fprintf (fp,
	   "Oid_Inserted: %d \n Ovfl_Changed: %d \n" "New_Ovfl Page: %d \n",
	   BTREE_INSERT_RCV_IS_OID_INSERTED (recins),
	   BTREE_INSERT_RCV_IS_OVFL_CHANGED (recins),
	   BTREE_INSERT_RCV_IS_NEW_OVFLPG (recins));
}

/*
 * btree_rv_dump_redo_insert_mvcc_delid () - Dump redo log record of
 *					     insert delete MVCCID operation.
 *   return: nothing
 *   length(in): Length of Recovery Data
 *   data(in): The data being logged
 */
void
btree_rv_dump_redo_insert_mvcc_delid (FILE * fp, int length, void *data)
{
  int rcv_offset_and_flags = *((int *) data);
  bool is_unique = BTREE_INSERT_DELID_RCV_IS_UNIQUE (rcv_offset_and_flags);
  bool is_ovf = BTREE_INSERT_DELID_RCV_IS_OVERFLOW (rcv_offset_and_flags);
  bool has_packed_domain =
    BTREE_INSERT_DELID_RCV_HAS_KEY_DOMAIN (rcv_offset_and_flags);
  TP_DOMAIN *key_domain = NULL;

  if (has_packed_domain)
    {
      (void) or_unpack_domain ((char *) data + sizeof (int), &key_domain,
			       NULL);
    }

  BTREE_INSERT_DELID_RCV_CLEAR_FLAGS (rcv_offset_and_flags);

  fprintf (fp, "MVCC DELETE ID INSERTION: \n");
  fprintf (fp, "OID offset: %d \n", rcv_offset_and_flags);
  fprintf (fp, "UNQIUE: %s \n", is_unique ? "true" : "false");
  fprintf (fp, "RECORD TYPE: %s \n", is_ovf ? "REGULAR" : "OVERFLOW");
  fprintf (fp, "KEY TYPE: %d \n",
	   (key_domain == NULL) ? DB_TYPE_UNKNOWN : key_domain->type->id);

  if (key_domain != NULL)
    {
      tp_domain_free (key_domain);
    }
}

/*
 * btree_rv_nop () -
 *   return: int
 *   recv(in): Recovery structure
 *
 *
 * Note: Does nothing. This routine is used for to accompany some
 * compensating redo logs which are supposed to do nothing.
 */
int
btree_rv_nop (THREAD_ENTRY * thread_p, LOG_RCV * recv)
{
  return NO_ERROR;
}

/*
 * btree_multicol_key_is_null () -
 *   return: Return true if DB_VALUE is a NULL multi-column
 *           key and false otherwise.
 *   key(in): Pointer to multi-column key
 *
 * Note: Check the multi-column key for a NULL value. In terms of the B-tree,
 * a NULL multi-column key is a sequence in which each element is NULL.
 */
bool
btree_multicol_key_is_null (DB_VALUE * key)
{
  bool status = false;
  DB_MIDXKEY *midxkey;
  unsigned char *bits;
  int nbytes, i;

  if (DB_VALUE_TYPE (key) == DB_TYPE_MIDXKEY)
    {
      assert (!DB_IS_NULL (key));

      midxkey = DB_GET_MIDXKEY (key);
      assert (midxkey != NULL);

      /* ncolumns == -1 means already constructing step */
      if (midxkey && midxkey->ncolumns != -1)
	{
	  bits = (unsigned char *) midxkey->buf;
	  nbytes = OR_MULTI_BOUND_BIT_BYTES (midxkey->ncolumns);
	  for (i = 0; i < nbytes; i++)
	    {
	      if (bits[i] != (unsigned char) 0)
		{
		  return false;
		}
	    }

	  status = true;
	}
    }

  return status;
}

/*
 * btree_multicol_key_has_null () -
 *   return: Return true if DB_VALUE is a multi-column key
 *           and has a NULL element in it and false otherwise.
 *   key(in): Pointer to multi-column key
 *
 * Note: Check the multi-column  key has a NULL element.
 */
int
btree_multicol_key_has_null (DB_VALUE * key)
{
  int status = 0;
  DB_MIDXKEY *midxkey;
  int i;

  if (DB_VALUE_TYPE (key) == DB_TYPE_MIDXKEY)
    {
      assert (!DB_IS_NULL (key));

      midxkey = DB_GET_MIDXKEY (key);
      assert (midxkey != NULL);

      /* ncolumns == -1 means already constructing step */
      if (midxkey && midxkey->ncolumns != -1)
	{
	  for (i = 0; i < midxkey->ncolumns; i++)
	    {
	      if (OR_MULTI_ATT_IS_UNBOUND (midxkey->buf, i))
		{
		  return 1;
		}
	    }

	  return 0;
	}
    }

  return status;
}

/*
 * btree_find_key_from_leaf () -
 *   return:
 *   btid(in):
 *   pg_ptr(in):
 *   key_cnt(in):
 *   oid(in):
 *   key(in):
 *   clear_key(in):
 */
static DISK_ISVALID
btree_find_key_from_leaf (THREAD_ENTRY * thread_p,
			  BTID_INT * btid, PAGE_PTR pg_ptr,
			  int key_cnt, OID * oid,
			  DB_VALUE * key, bool * clear_key)
{
  RECDES rec;
  LEAF_REC leaf_pnt;
  VPID ovfl_vpid;
  int i, offset;

  VPID_SET_NULL (&leaf_pnt.ovfl);

  for (i = 1; i <= key_cnt; i++)
    {
      if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	{
	  return DISK_ERROR;
	}

      btree_read_record (thread_p, btid, pg_ptr, &rec, key, &leaf_pnt,
			 BTREE_LEAF_NODE, clear_key, &offset, PEEK_KEY_VALUE,
			 NULL);
      ovfl_vpid = leaf_pnt.ovfl;

      if (btree_find_oid_from_leaf (btid, &rec, offset, oid, NULL, NULL)
	  != NOT_FOUND)
	{
	  /* key will be cleared by caller */
	  return DISK_VALID;
	}

      if (!VPID_ISNULL (&ovfl_vpid))
	{
	  /* record has an overflow page continuation */
	  RECDES orec;
	  PAGE_PTR ovfp = NULL;
	  int oid_size = OR_OID_SIZE;

	  if (BTREE_IS_UNIQUE (btid->unique_pk))
	    {
	      oid_size += OR_OID_SIZE;
	    }

	  do
	    {
	      ovfp = pgbuf_fix (thread_p, &ovfl_vpid, OLD_PAGE,
				PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
	      if (ovfp == NULL)
		{
		  return DISK_ERROR;
		}

	      (void) pgbuf_check_page_ptype (thread_p, ovfp, PAGE_BTREE);

	      btree_get_next_overflow_vpid (ovfp, &ovfl_vpid);

	      (void) spage_get_record (ovfp, 1, &orec, PEEK);
	      if (btree_find_oid_from_ovfl (&orec, oid, oid_size, NULL) !=
		  NOT_FOUND)
		{
		  pgbuf_unfix_and_init (thread_p, ovfp);

		  return DISK_VALID;
		}

	      pgbuf_unfix_and_init (thread_p, ovfp);
	    }
	  while (!VPID_ISNULL (&ovfl_vpid));
	}

      if (*clear_key)
	{
	  pr_clear_value (key);
	}
    }

  return DISK_INVALID;
}

/*
 * btree_find_key_from_nleaf () -
 *   return:
 *   btid(in):
 *   pg_ptr(in):
 *   key_cnt(in):
 *   oid(in):
 *   key(in):
 *   clear_key(in):
 */
static DISK_ISVALID
btree_find_key_from_nleaf (THREAD_ENTRY * thread_p,
			   BTID_INT * btid, PAGE_PTR pg_ptr,
			   int key_cnt, OID * oid,
			   DB_VALUE * key, bool * clear_key)
{
  int i;
  NON_LEAF_REC nleaf_ptr;
  VPID page_vpid;
  PAGE_PTR page = NULL;
  RECDES rec;
  DISK_ISVALID status = DISK_INVALID;

  for (i = 1; i <= key_cnt; i++)
    {
      if (spage_get_record (pg_ptr, i, &rec, PEEK) != S_SUCCESS)
	{
	  return DISK_ERROR;
	}

      btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf_ptr);
      page_vpid = nleaf_ptr.pnt;

      page = pgbuf_fix (thread_p, &page_vpid, OLD_PAGE, PGBUF_LATCH_READ,
			PGBUF_UNCONDITIONAL_LATCH);
      if (page == NULL)
	{
	  return DISK_ERROR;
	}

      (void) pgbuf_check_page_ptype (thread_p, page, PAGE_BTREE);

      status = btree_find_key_from_page (thread_p, btid, page, oid, key,
					 clear_key);
      pgbuf_unfix_and_init (thread_p, page);

      if (status == DISK_VALID)
	{
	  break;
	}
    }

  return status;
}

/*
 * btree_find_key_from_page () -
 *   return:
 *   btid(in):
 *   pg_ptr(in):
 *   oid(in):
 *   key(in):
 *   clear_key(in):
 */
static DISK_ISVALID
btree_find_key_from_page (THREAD_ENTRY * thread_p,
			  BTID_INT * btid, PAGE_PTR pg_ptr,
			  OID * oid, DB_VALUE * key, bool * clear_key)
{
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;
  int key_cnt;
  DISK_ISVALID status;

  key_cnt = btree_node_number_of_keys (pg_ptr);

  header = btree_get_node_header (pg_ptr);
  if (header == NULL)
    {
      return DISK_ERROR;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {
      status = btree_find_key_from_nleaf (thread_p, btid, pg_ptr, key_cnt,
					  oid, key, clear_key);
    }
  else
    {
      status = btree_find_key_from_leaf (thread_p, btid, pg_ptr, key_cnt,
					 oid, key, clear_key);
    }

  return status;
}

/*
 * btree_find_key () -
 *   return:
 *   btid(in):
 *   oid(in):
 *   key(in):
 *   clear_key(in):
 */
DISK_ISVALID
btree_find_key (THREAD_ENTRY * thread_p, BTID * btid, OID * oid,
		DB_VALUE * key, bool * clear_key)
{
  VPID root_vpid;
  PAGE_PTR root = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTID_INT btid_int;
  DISK_ISVALID status;

  root_vpid.pageid = btid->root_pageid;	/* read root page */
  root_vpid.volid = btid->vfid.volid;
  root = pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
		    PGBUF_UNCONDITIONAL_LATCH);
  if (root == NULL)
    {
      return DISK_ERROR;
    }

  (void) pgbuf_check_page_ptype (thread_p, root, PAGE_BTREE);

  root_header = btree_get_root_header (root);
  if (root_header == NULL)
    {
      status = DISK_ERROR;
      goto end;
    }

  btid_int.sys_btid = btid;
  btree_glean_root_header_info (thread_p, root_header, &btid_int);
  status = btree_find_key_from_page (thread_p, &btid_int, root, oid,
				     key, clear_key);

end:

  assert (root != NULL);
  pgbuf_unfix_and_init (thread_p, root);

  return status;
}


int
btree_set_error (THREAD_ENTRY * thread_p, DB_VALUE * key,
		 OID * obj_oid, OID * class_oid, BTID * btid,
		 const char *bt_name,
		 int severity, int err_id, const char *filename, int lineno)
{
  char btid_msg_buf[OID_MSG_BUF_SIZE];
  char class_oid_msg_buf[OID_MSG_BUF_SIZE];
  char oid_msg_buf[OID_MSG_BUF_SIZE];
  char *index_name;
  char *class_name;
  char *keyval;

  assert (btid != NULL);

  /* init as empty string */
  btid_msg_buf[0] = class_oid_msg_buf[0] = oid_msg_buf[0] = 0;
  index_name = class_name = keyval = NULL;

  /* fetch index name from the class representation */
  if (class_oid != NULL && !OID_ISNULL (class_oid))
    {
      if (heap_get_indexinfo_of_btid (thread_p,
				      class_oid, btid,
				      NULL, NULL, NULL, NULL,
				      &index_name, NULL) != NO_ERROR)
	{
	  index_name = NULL;
	}
    }

  if (index_name && btid)
    {
      /* print valid btid */
      snprintf (btid_msg_buf, OID_MSG_BUF_SIZE, "(B+tree: %d|%d|%d)",
		btid->vfid.volid, btid->vfid.fileid, btid->root_pageid);
    }

  if (class_oid != NULL && !OID_ISNULL (class_oid))
    {
      class_name = heap_get_class_name (thread_p, class_oid);
      if (class_name)
	{
	  snprintf (class_oid_msg_buf, OID_MSG_BUF_SIZE,
		    "(CLASS_OID: %d|%d|%d)", class_oid->volid,
		    class_oid->pageid, class_oid->slotid);
	}
    }

  if (key && obj_oid)
    {
      keyval = pr_valstring (key);
      if (keyval)
	{
	  snprintf (oid_msg_buf, OID_MSG_BUF_SIZE, "(OID: %d|%d|%d)",
		    obj_oid->volid, obj_oid->pageid, obj_oid->slotid);
	}
    }

  er_set (severity, filename, lineno, err_id, 6,
	  (index_name) ? index_name : ((bt_name) ? bt_name :
				       "*UNKNOWN-INDEX*"), btid_msg_buf,
	  (class_name) ? class_name : "*UNKNOWN-CLASS*", class_oid_msg_buf,
	  (keyval) ? keyval : "*UNKNOWN-KEY*", oid_msg_buf);

  if (keyval)
    {
      free_and_init (keyval);
    }
  if (class_name)
    {
      free_and_init (class_name);
    }
  if (index_name)
    {
      free_and_init (index_name);
    }

  return NO_ERROR;
}

/*
 * btree_get_asc_desc - get asc/desc for column index from BTREE
 *
 *   return:  error code
 *   thread_p(in): THREAD_ENTRY
 *   btid(in): BTID
 *   col_idx(in): column index
 *   asc_desc(out): asc/desc for column index
 */
int
btree_get_asc_desc (THREAD_ENTRY * thread_p, BTID * btid, int col_idx,
		    int *asc_desc)
{
  DISK_ISVALID valid = DISK_ERROR;
  VPID r_vpid;			/* root page identifier */
  PAGE_PTR r_pgptr = NULL;	/* root page pointer */
  BTID_INT btid_int;
  BTREE_ROOT_HEADER *root_header = NULL;
  TP_DOMAIN *domain;
  int k, ret = NO_ERROR;

  if (btid == NULL || asc_desc == NULL)
    {
      return ER_FAILED;
    }

  ret = NO_ERROR;
  *asc_desc = 0;

  r_vpid.pageid = btid->root_pageid;
  r_vpid.volid = btid->vfid.volid;

  r_pgptr = pgbuf_fix (thread_p, &r_vpid, OLD_PAGE,
		       PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
  if (r_pgptr == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, r_pgptr, PAGE_BTREE);

  root_header = btree_get_root_header (r_pgptr);
  if (root_header == NULL)
    {
      goto exit_on_error;
    }

  btid_int.sys_btid = btid;

  ret = btree_glean_root_header_info (thread_p, root_header, &btid_int);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (btid_int.key_type->setdomain)
    {
      domain = btid_int.key_type->setdomain;
      for (k = 1; k <= col_idx; k++)
	{
	  domain = domain->next;
	  if (domain == NULL)
	    {
	      goto exit_on_error;
	    }
	}
    }
  else
    {
      domain = btid_int.key_type;
      if (col_idx != 0)
	{
	  return ER_FAILED;
	}
    }

  *asc_desc = domain->is_desc;
  pgbuf_unfix_and_init (thread_p, r_pgptr);

  return NO_ERROR;

exit_on_error:

  if (r_pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, r_pgptr);
    }

  return (ret == NO_ERROR
	  && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

}

/*
 * btree_get_locked_keys () -
 *   return: locked keys
 *   delete_btid(in): btid used to delete
 *   search_btid(in): btid used to search
 *   search_btid_duplicate_key_locked(in): true, if duplicate key has been
 *					   locked when searching in
 *					   search_btid
 */
BTREE_LOCKED_KEYS
btree_get_locked_keys (BTID * delete_btid, BTID * search_btid,
		       bool search_btid_duplicate_key_locked)
{
  if (delete_btid != NULL && search_btid != NULL &&
      BTID_IS_EQUAL (search_btid, delete_btid))
    {
      if (search_btid_duplicate_key_locked)
	{
	  return BTREE_CURRENT_KEYS_LOCKED;
	}
      else
	{
	  return BTREE_ALL_KEYS_LOCKED;
	}
    }

  return BTREE_NO_KEY_LOCKED;
}

static void
btree_set_unknown_key_error (THREAD_ENTRY * thread_p,
			     BTID * btid, DB_VALUE * key,
			     const char *debug_msg)
{
  int severity;
  PR_TYPE *pr_type;
  char *err_key;

  assert (btid != NULL);
  assert (key != NULL);

  if (log_is_in_crash_recovery () || thread_is_vacuum_worker (thread_p))
    {
      severity = ER_WARNING_SEVERITY;
    }
  else
    {
      severity = ER_ERROR_SEVERITY;
    }

  err_key = pr_valstring (key);
  pr_type = PR_TYPE_FROM_ID (DB_VALUE_DOMAIN_TYPE (key));

  er_set (severity, ARG_FILE_LINE, ER_BTREE_UNKNOWN_KEY, 5,
	  (err_key != NULL) ? err_key : "_NULL_KEY",
	  btid->vfid.fileid, btid->vfid.volid, btid->root_pageid,
	  (pr_type != NULL) ? pr_type->name : "INVALID KEY TYPE");

  er_log_debug (ARG_FILE_LINE, debug_msg);

  if (err_key != NULL)
    {
      free_and_init (err_key);
    }
}

/*
 * btree_get_next_page () -
 *   return:
 *
 *   page_p(in):
 */
static PAGE_PTR
btree_get_next_page (THREAD_ENTRY * thread_p, PAGE_PTR page_p)
{
  BTREE_NODE_HEADER *header = NULL;
  PAGE_PTR next_page = NULL;
  VPID next_vpid;

  if (page_p == NULL)
    {
      assert (page_p != NULL);
      return NULL;
    }

  header = btree_get_node_header (page_p);
  if (header == NULL)
    {
      return NULL;
    }

  next_vpid = header->next_vpid;
  if (VPID_ISNULL (&next_vpid))
    {
      goto exit_on_error;
    }

  next_page = pgbuf_fix (thread_p, &next_vpid, OLD_PAGE, PGBUF_LATCH_WRITE,
			 PGBUF_UNCONDITIONAL_LATCH);
  if (next_page == NULL)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, next_page, PAGE_BTREE);

  return next_page;

exit_on_error:

  if (next_page)
    {
      pgbuf_unfix_and_init (thread_p, next_page);
    }
  return NULL;
}

/*
 * btree_set_vpid_previous_vpid () - Sets the prev VPID of a page
 *   return: error code
 *   btid(in): BTID
 *   page_p(in):
 *   prev(in): a vpid to be set as previous for the input page
 */
static int
btree_set_vpid_previous_vpid (THREAD_ENTRY * thread_p, BTID_INT * btid,
			      PAGE_PTR page_p, VPID * prev)
{
  BTREE_NODE_HEADER *header = NULL;

  if (page_p == NULL)
    {
      return NO_ERROR;
    }

  header = btree_get_node_header (page_p);
  if (header == NULL)
    {
      return ER_FAILED;
    }

  btree_node_header_undo_log (thread_p, &btid->sys_btid->vfid, page_p);
  header->prev_vpid = *prev;
  btree_node_header_redo_log (thread_p, &btid->sys_btid->vfid, page_p);

  pgbuf_set_dirty (thread_p, page_p, DONT_FREE);

  return NO_ERROR;
}

int
btree_compare_key (DB_VALUE * key1, DB_VALUE * key2,
		   TP_DOMAIN * key_domain,
		   int do_coercion, int total_order, int *start_colp)
{
  int c = DB_UNK;
  DB_TYPE key1_type, key2_type;
  DB_TYPE dom_type;
  int dummy_size1, dummy_size2, dummy_diff_column;
  bool dom_is_desc = false, dummy_next_dom_is_desc;
  bool comparable = true;

  assert (key1 != NULL && key2 != NULL && key_domain != NULL);

  key1_type = DB_VALUE_DOMAIN_TYPE (key1);
  key2_type = DB_VALUE_DOMAIN_TYPE (key2);
  dom_type = TP_DOMAIN_TYPE (key_domain);

  if (DB_IS_NULL (key1))
    {
      if (DB_IS_NULL (key2))
	{
	  assert (false);
	  return DB_UNK;
	}

      return DB_LT;
    }

  if (DB_IS_NULL (key2))
    {
      if (DB_IS_NULL (key1))
	{
	  assert (false);
	  return DB_UNK;
	}

      return DB_GT;
    }

  if (dom_type == DB_TYPE_MIDXKEY)
    {
      /* safe code */
      if (key1_type != DB_TYPE_MIDXKEY)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_TP_CANT_COERCE, 2,
		  pr_type_name (key1_type), pr_type_name (dom_type));
	  assert (false);
	  return DB_UNK;
	}
      if (key2_type != DB_TYPE_MIDXKEY)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_TP_CANT_COERCE, 2,
		  pr_type_name (key2_type), pr_type_name (dom_type));
	  assert (false);
	  return DB_UNK;
	}

      c = pr_midxkey_compare (DB_GET_MIDXKEY (key1), DB_GET_MIDXKEY (key2),
			      do_coercion, total_order, -1, start_colp,
			      &dummy_size1, &dummy_size2, &dummy_diff_column,
			      &dom_is_desc, &dummy_next_dom_is_desc);
      assert_release (c == DB_UNK || (DB_LT <= c && c <= DB_GT));

      if (dom_is_desc)
	{
	  c = ((c == DB_GT) ? DB_LT : (c == DB_LT) ? DB_GT : c);
	}
    }
  else
    {
      assert (key1_type != DB_TYPE_MIDXKEY);
      assert (key2_type != DB_TYPE_MIDXKEY);

      assert (tp_valid_indextype (key1_type));
      assert (tp_valid_indextype (key2_type));

      /* safe code */
      if (key1_type == DB_TYPE_MIDXKEY)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_TP_CANT_COERCE, 2,
		  pr_type_name (key1_type), pr_type_name (dom_type));
	  assert (false);
	  return DB_UNK;
	}
      if (key2_type == DB_TYPE_MIDXKEY)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_TP_CANT_COERCE, 2,
		  pr_type_name (key2_type), pr_type_name (dom_type));
	  assert (false);
	  return DB_UNK;
	}

      if (TP_ARE_COMPARABLE_KEY_TYPES (key1_type, key2_type)
	  && TP_ARE_COMPARABLE_KEY_TYPES (key1_type, dom_type)
	  && TP_ARE_COMPARABLE_KEY_TYPES (key2_type, dom_type))
	{
	  c =
	    (*(key_domain->type->cmpval)) (key1, key2, do_coercion,
					   total_order, NULL,
					   key_domain->collation_id);
	}
      else
	{
	  c =
	    tp_value_compare_with_error (key1, key2, do_coercion, total_order,
					 &comparable);

	  if (!comparable)
	    {
	      return DB_UNK;
	    }
	}

      assert_release (c == DB_UNK || (DB_LT <= c && c <= DB_GT));

      /* for single-column desc index */
      if (key_domain->is_desc)
	{
	  c = ((c == DB_GT) ? DB_LT : (c == DB_LT) ? DB_GT : c);
	}
    }

  assert_release (c == DB_UNK || (DB_LT <= c && c <= DB_GT));

  return c;
}

/*
 * btree_range_opt_check_add_index_key () - adds a key in the array of top N
 *	      keys of multiple range search optimization structure
 *   return: error code
 *   thread_p(in):
 *   bts(in):
 *   multi_range_opt(in): multiple range search optimization structure
 *   p_new_oid(in): OID stored in the index record
 *   ck_pseudo_oid(in): pseudo-OID of current key, may be NULL if key locking
 *		        is not used
 *   nk_pseudo_oid(in): pseudo-OID of next key, may be NULL if key locking
 *		        is not used
 *   class_oid(in):
 *   key_added(out): if key is added or not
 */
static int
btree_range_opt_check_add_index_key (THREAD_ENTRY * thread_p,
				     BTREE_SCAN * bts,
				     MULTI_RANGE_OPT * multi_range_opt,
				     OID * p_new_oid, OID * ck_pseudo_oid,
				     OID * nk_pseudo_oid, OID * class_oid,
				     bool * key_added)
{
  int compare_id = 0;
  DB_MIDXKEY *new_mkey = NULL;
  DB_VALUE *new_key_value = NULL;
  int error = NO_ERROR, i = 0;
  bool use_unlocking = false;

  assert (multi_range_opt->use == true);

  if (DB_VALUE_DOMAIN_TYPE (&(bts->cur_key)) != DB_TYPE_MIDXKEY
      || multi_range_opt->sort_att_idx == NULL)
    {
      return ER_FAILED;
    }

  *key_added = true;

  assert (multi_range_opt->no_attrs != 0);
  if (multi_range_opt->no_attrs == 0)
    {
      return ER_FAILED;
    }

  new_mkey = DB_PULL_MIDXKEY (&(bts->cur_key));
  new_key_value =
    (DB_VALUE *) db_private_alloc (thread_p,
				   multi_range_opt->no_attrs *
				   sizeof (DB_VALUE));
  if (new_key_value == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      sizeof (DB_VALUE *) * multi_range_opt->no_attrs);
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  for (i = 0; i < multi_range_opt->no_attrs; i++)
    {
      DB_MAKE_NULL (&new_key_value[i]);
      error = pr_midxkey_get_element_nocopy (new_mkey,
					     multi_range_opt->sort_att_idx[i],
					     &new_key_value[i], NULL, NULL);
      if (error != NO_ERROR)
	{
	  goto exit;
	}
    }

  use_unlocking = false;
  if (bts->read_uncommitted == false
      || !BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      use_unlocking = true;
    }

  if (multi_range_opt->cnt == multi_range_opt->size)
    {
      int c = 0;
      DB_MIDXKEY *comp_mkey = NULL;
      DB_VALUE comp_key_value;
      bool reject_new_elem = false;
      RANGE_OPT_ITEM *last_item = NULL;

      last_item = multi_range_opt->top_n_items[multi_range_opt->size - 1];
      assert (last_item != NULL);

      comp_mkey = DB_PULL_MIDXKEY (&(last_item->index_value));

      /* if all keys are equal, the new element is rejected */
      reject_new_elem = true;
      for (i = 0; i < multi_range_opt->no_attrs; i++)
	{
	  DB_MAKE_NULL (&comp_key_value);
	  error =
	    pr_midxkey_get_element_nocopy (comp_mkey,
					   multi_range_opt->sort_att_idx[i],
					   &comp_key_value, NULL, NULL);
	  if (error != NO_ERROR)
	    {
	      goto exit;
	    }

	  c =
	    (*(multi_range_opt->sort_col_dom[i]->type->cmpval))
	    (&comp_key_value, &new_key_value[i], 1, 1, NULL,
	     multi_range_opt->sort_col_dom[i]->collation_id);
	  if (c != 0)
	    {
	      /* see if new element should be rejected or accepted and stop
	       * checking keys
	       */
	      reject_new_elem =
		(multi_range_opt->is_desc_order[i]) ? (c > 0) : (c < 0);
	      break;
	    }
	}

      if (reject_new_elem)
	{
	  /* do not add */
	  *key_added = false;

	  if (use_unlocking && class_oid != NULL && !OID_ISNULL (class_oid))
	    {
	      /* this instance OID is rejected : unlock it */
	      lock_unlock_object (thread_p, p_new_oid,
				  class_oid, bts->lock_mode, true);

	      /* unlock the pseudo-OID for current key and next key
	       * corresponding to the instance OID being rejected */
	      if (ck_pseudo_oid != NULL && !OID_ISNULL (ck_pseudo_oid))
		{
		  assert (!OID_ISNULL (ck_pseudo_oid));
		  assert (!OID_ISNULL (class_oid));
		  lock_unlock_object_donot_move_to_non2pl (thread_p,
							   ck_pseudo_oid,
							   class_oid,
							   bts->
							   key_lock_mode);
		}

	      if (nk_pseudo_oid != NULL && !OID_ISNULL (nk_pseudo_oid))
		{
		  assert (!OID_ISNULL (nk_pseudo_oid));
		  assert (!OID_ISNULL (class_oid));
		  lock_unlock_object_donot_move_to_non2pl (thread_p,
							   nk_pseudo_oid,
							   class_oid,
							   bts->
							   key_lock_mode);
		}
	    }

	  if (new_key_value != NULL)
	    {
	      db_private_free_and_init (thread_p, new_key_value);
	    }

	  return NO_ERROR;
	}

      if (use_unlocking && !OID_ISNULL (&(last_item->class_oid)))
	{
	  /* unlock the instance OID which is overwritten */
	  lock_unlock_object (thread_p, &(last_item->inst_oid),
			      &(last_item->class_oid), bts->lock_mode, true);

	  /* unlock the pseudo-OID for current key and next key corresponding
	   * to instance which is overwritten */
	  if (!OID_ISNULL (&(last_item->ck_ps_oid)))
	    {
	      assert (!OID_ISNULL (&(last_item->ck_ps_oid)));
	      assert (!OID_ISNULL (&(last_item->class_oid)));
	      lock_unlock_object_donot_move_to_non2pl (thread_p,
						       &(last_item->
							 ck_ps_oid),
						       &(last_item->
							 class_oid),
						       bts->key_lock_mode);
	    }

	  if (!OID_ISNULL (&(last_item->nk_ps_oid)))
	    {
	      assert (!OID_ISNULL (&(last_item->nk_ps_oid)));
	      assert (!OID_ISNULL (&(last_item->class_oid)));
	      lock_unlock_object_donot_move_to_non2pl (thread_p,
						       &(last_item->
							 nk_ps_oid),
						       &(last_item->
							 class_oid),
						       bts->key_lock_mode);
	    }
	}

      /* overwrite the last item with the new key and OIDs */
      db_value_clear (&(last_item->index_value));
      db_value_clone (&(bts->cur_key), &(last_item->index_value));
      COPY_OID (&(last_item->inst_oid), p_new_oid);

      OID_SET_NULL (&(last_item->ck_ps_oid));
      OID_SET_NULL (&(last_item->nk_ps_oid));
      OID_SET_NULL (&(last_item->class_oid));

      /* save current & next key pseudo-OID */
      if (use_unlocking)
	{
	  if (ck_pseudo_oid != NULL)
	    {
	      COPY_OID (&(last_item->ck_ps_oid), ck_pseudo_oid);
	    }
	  if (nk_pseudo_oid != NULL)
	    {
	      COPY_OID (&(last_item->nk_ps_oid), nk_pseudo_oid);
	    }
	  if (class_oid != NULL)
	    {
	      COPY_OID (&(last_item->class_oid), class_oid);
	    }
	}
    }
  else
    {
      RANGE_OPT_ITEM *curr_item = NULL;
      /* just insert on last position available */
      assert (multi_range_opt->cnt < multi_range_opt->size);

      curr_item = db_private_alloc (thread_p, sizeof (RANGE_OPT_ITEM));
      if (curr_item == NULL)
	{
	  error = ER_OUT_OF_VIRTUAL_MEMORY;
	  goto exit;
	}

      multi_range_opt->top_n_items[multi_range_opt->cnt] = curr_item;
      db_value_clone (&(bts->cur_key), &(curr_item->index_value));

      COPY_OID (&(curr_item->inst_oid), p_new_oid);
      OID_SET_NULL (&(curr_item->ck_ps_oid));
      OID_SET_NULL (&(curr_item->nk_ps_oid));
      OID_SET_NULL (&(curr_item->class_oid));

      /* save current & next key pseudo-OID */
      if (use_unlocking)
	{
	  if (ck_pseudo_oid != NULL)
	    {
	      COPY_OID (&(curr_item->ck_ps_oid), ck_pseudo_oid);
	    }
	  if (nk_pseudo_oid != NULL)
	    {
	      COPY_OID (&(curr_item->nk_ps_oid), nk_pseudo_oid);
	    }
	  if (class_oid != NULL)
	    {
	      COPY_OID (&(curr_item->class_oid), class_oid);
	    }
	}

      multi_range_opt->cnt++;

      if (multi_range_opt->sort_col_dom == NULL)
	{
	  multi_range_opt->sort_col_dom =
	    (TP_DOMAIN **) db_private_alloc (thread_p,
					     multi_range_opt->no_attrs *
					     sizeof (TP_DOMAIN *));
	  if (multi_range_opt->sort_col_dom == NULL)
	    {
	      error = ER_OUT_OF_VIRTUAL_MEMORY;
	      goto exit;
	    }

	  for (i = 0; i < multi_range_opt->no_attrs; i++)
	    {
	      multi_range_opt->sort_col_dom[i] =
		tp_domain_resolve_value (&new_key_value[i], NULL);
	    }
	}
    }

  /* find the position for this element */
  /* if there is only one element => nothing to do */
  if (multi_range_opt->cnt > 1)
    {
      int pos = 0;
      error =
	btree_top_n_items_binary_search (multi_range_opt->top_n_items,
					 multi_range_opt->sort_att_idx,
					 multi_range_opt->sort_col_dom,
					 multi_range_opt->is_desc_order,
					 new_key_value,
					 multi_range_opt->no_attrs, 0,
					 multi_range_opt->cnt - 1, &pos);
      if (error != NO_ERROR)
	{
	  goto exit;
	}
      if (pos != multi_range_opt->cnt - 1)
	{
	  RANGE_OPT_ITEM *temp_item;
	  int mem_size =
	    (multi_range_opt->cnt - 1 - pos) * sizeof (RANGE_OPT_ITEM *);

	  /* copy last item to temp */
	  temp_item = multi_range_opt->top_n_items[multi_range_opt->cnt - 1];

	  /* move all items one position to the right in order to free the
	   * position for the new item
	   */
	  memcpy (multi_range_opt->buffer, &multi_range_opt->top_n_items[pos],
		  mem_size);
	  memcpy (&multi_range_opt->top_n_items[pos + 1],
		  multi_range_opt->buffer, mem_size);

	  /* put new item at its designated position */
	  multi_range_opt->top_n_items[pos] = temp_item;
	}
      else
	{
	  /* the new item is already in the correct position */
	}
    }

exit:
  if (new_key_value != NULL)
    {
      db_private_free_and_init (thread_p, new_key_value);
    }
  return error;
}

/*
 * btree_top_n_items_binary_search () - searches for the right position for
 *				        the keys in new_key_values in top
 *					N item list
 *
 * return	       : error code
 * top_n_items (in)    : current top N item list
 * att_idxs (in)       : indexes for midxkey attributes
 * domains (in)	       : domains for midxkey attributes
 * desc_order (in)     : is descending order for midxkey attributes
 *			 if NULL, ascending order will be considered
 * new_key_values (in) : key values for the new item
 * no_keys (in)	       : number of keys that are compared
 * first (in)	       : position of the first item in current range
 * last (in)	       : position of the last item in current range
 * new_pos (out)       : the position where the new item fits
 *
 * NOTE	: At each step, split current range in half and compare with the
 *	  middle item. If all keys are equal save the position of middle item.
 *	  If middle item is better, look between middle and last, otherwise
 *	  look between first and middle.
 *	  The recursion stops when the range cannot be split anymore
 *	  (first + 1 <= last), when normally first is better and last is worse
 *	  and the new item should replace last. There is a special case when
 *	  the new item is better than all items in top N. In this case,
 *	  first must be 0 and an extra compare is made (to see if new item
 *	  should in fact replace first).
 */
static int
btree_top_n_items_binary_search (RANGE_OPT_ITEM ** top_n_items,
				 int *att_idxs, TP_DOMAIN ** domains,
				 bool * desc_order, DB_VALUE * new_key_values,
				 int no_keys, int first, int last,
				 int *new_pos)
{
  DB_MIDXKEY *comp_mkey = NULL;
  DB_VALUE comp_key_value;
  RANGE_OPT_ITEM *comp_item;
  int i, c, error = NO_ERROR;

  int middle;

  assert (last >= first && new_pos != NULL);
  if (last <= first + 1)
    {
      if (first == 0)
	{
	  /* need to check if the new key is smaller than the first */
	  comp_item = top_n_items[0];
	  comp_mkey = DB_PULL_MIDXKEY (&(comp_item->index_value));

	  for (i = 0; i < no_keys; i++)
	    {
	      DB_MAKE_NULL (&comp_key_value);
	      error =
		pr_midxkey_get_element_nocopy (comp_mkey, att_idxs[i],
					       &comp_key_value, NULL, NULL);
	      if (error != NO_ERROR)
		{
		  return error;
		}
	      c =
		(*(domains[i]->type->cmpval)) (&comp_key_value,
					       &new_key_values[i], 1, 1,
					       NULL,
					       domains[i]->collation_id);
	      if (c != 0)
		{
		  if ((desc_order != NULL && desc_order[i] ? c > 0 : c < 0))
		    {
		      /* new value is not better than the first */
		      break;
		    }
		  else
		    {
		      /* new value is better than the first */
		      new_pos = 0;
		      return NO_ERROR;
		    }
		}
	    }
	  /* new value is equal to first, fall through */
	}
      /* here: the new values should be between first and last */
      *new_pos = last;
      return NO_ERROR;
    }

  /* compare new value with the value in the middle of the current range */
  middle = (last + first) / 2;
  comp_item = top_n_items[middle];
  comp_mkey = DB_PULL_MIDXKEY (&(comp_item->index_value));

  for (i = 0; i < no_keys; i++)
    {
      DB_MAKE_NULL (&comp_key_value);
      error =
	pr_midxkey_get_element_nocopy (comp_mkey, att_idxs[i],
				       &comp_key_value, NULL, NULL);
      if (error != NO_ERROR)
	{
	  return error;
	}
      c =
	(*(domains[i]->type->cmpval)) (&comp_key_value, &new_key_values[i], 1,
				       1, NULL, domains[i]->collation_id);
      if (c != 0)
	{
	  if ((desc_order != NULL && desc_order[i] ? c > 0 : c < 0))
	    {
	      /* the new value is worse than the one in the middle */
	      first = middle;
	    }
	  else
	    {
	      /* the new value is better than the one in the middle */
	      last = middle;
	    }
	  return btree_top_n_items_binary_search (top_n_items, att_idxs,
						  domains, desc_order,
						  new_key_values, no_keys,
						  first, last, new_pos);
	}
    }
  /* all keys were equal, the new item can be put in current position */
  *new_pos = middle;
  return NO_ERROR;
}

/*
 * btree_iss_set_key () - save the current key
 *
 *   return: error code
 *   bts(in):
 *   iss(in):
 */
static int
btree_iss_set_key (BTREE_SCAN * bts, INDEX_SKIP_SCAN * iss)
{
  struct regu_variable_node *key = NULL;
  int ret = NO_ERROR;

  /* check environment */
  if (DB_VALUE_DOMAIN_TYPE (&bts->cur_key) != DB_TYPE_MIDXKEY
      || iss == NULL || iss->skipped_range == NULL)
    {
      assert_release (false);
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);

      return ER_FAILED;
    }

  /* get correct key to update value to (key1 for normal scan or key2 for
     reverse scan); the fetch range will have one of the keys NULLed */
  if (iss->skipped_range->key1 == NULL)
    {
      key = iss->skipped_range->key2;
    }
  else
    {
      key = iss->skipped_range->key1;
    }

  /* check the key */
  if (key == NULL || key->value.funcp == NULL
      || key->value.funcp->operand == NULL
      || key->value.funcp->operand->value.type != TYPE_DBVAL)
    {
      assert_release (false);
      return ER_FAILED;
    }

  /* save the found key as bound for next fetch */
  pr_clear_value (&key->value.funcp->operand->value.value.dbval);
  ret = pr_clone_value (&bts->cur_key,
			&key->value.funcp->operand->value.value.dbval);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  return NO_ERROR;
}

static bool
btree_is_new_file (BTID_INT * btid_int)
{
  if (btid_int->new_file == true)
    {
      assert (file_is_new_file (NULL, &(btid_int->sys_btid->vfid))
	      == FILE_NEW_FILE);
      return true;
    }
  else
    {
      assert (file_is_new_file (NULL, &(btid_int->sys_btid->vfid))
	      == FILE_OLD_FILE);
      return false;
    }
}


/*****************************************************************************/
/* For migrate_90beta_to_91                                                  */
/*****************************************************************************/
#define MIGRATE_90BETA_TO_91

#if defined(MIGRATE_90BETA_TO_91)

extern int btree_fix_overflow_oid_page_all_btrees (void);

static int btree_fix_ovfl_oid_pages_by_btid (THREAD_ENTRY * thread_p,
					     BTID * btid);
static int btree_fix_ovfl_oid_pages_tree (THREAD_ENTRY * thread_p,
					  BTID * btid, char *btname);
static int btree_fix_ovfl_oid_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				    PAGE_PTR pg_ptr, char *btname);
static int btree_compare_oid (const void *oid_mem1, const void *oid_mem2);

static int fixed_pages;

int
btree_fix_overflow_oid_page_all_btrees (void)
{
  int num_files, i;
  BTID btid;
  FILE_TYPE file_type;
  THREAD_ENTRY *thread_p;

  printf ("Start to fix BTREE Overflow OID pages\n\n");

  thread_p = thread_get_thread_entry_info ();

  num_files = file_get_numfiles (thread_p);
  if (num_files < 0)
    {
      return ER_FAILED;
    }

  for (i = 0; i < num_files; i++)
    {
      if (file_find_nthfile (thread_p, &btid.vfid, i) != 1)
	{
	  break;
	}

      file_type = file_get_type (thread_p, &btid.vfid);
      if (file_type == FILE_UNKNOWN_TYPE)
	{
	  return ER_FAILED;
	}

      if (file_type != FILE_BTREE)
	{
	  continue;
	}

      if (btree_fix_ovfl_oid_pages_by_btid (thread_p, &btid) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  return NO_ERROR;
}

static int
btree_fix_ovfl_oid_pages_by_btid (THREAD_ENTRY * thread_p, BTID * btid)
{
  char area[FILE_DUMP_DES_AREA_SIZE];
  char *fd = area;
  int fd_size = FILE_DUMP_DES_AREA_SIZE, size;
  FILE_BTREE_DES *btree_des;
  char *btname;
  VPID vpid;
  int ret = NO_ERROR;

  size = file_get_descriptor (thread_p, &btid->vfid, fd, fd_size);
  if (size < 0)
    {
      fd_size = -size;
      fd = (char *) db_private_alloc (thread_p, fd_size);
      if (fd == NULL)
	{
	  fd = area;
	  fd_size = FILE_DUMP_DES_AREA_SIZE;
	}
      else
	{
	  size = file_get_descriptor (thread_p, &btid->vfid, fd, fd_size);
	}
    }

  btree_des = (FILE_BTREE_DES *) fd;

  /* get the index name of the index key */
  ret = heap_get_indexinfo_of_btid (thread_p, &(btree_des->class_oid),
				    btid, NULL, NULL, NULL,
				    NULL, &btname, NULL);
  if (ret != NO_ERROR)
    {
      goto exit_on_end;
    }

  if (file_find_nthpages (thread_p, &btid->vfid, &vpid, 0, 1) != 1)
    {
      ret = ER_FAILED;
      goto exit_on_end;
    }

  btid->root_pageid = vpid.pageid;
  ret = btree_fix_ovfl_oid_pages_tree (thread_p, btid, btname);

exit_on_end:

  if (fd != area)
    {
      db_private_free_and_init (thread_p, fd);
    }

  if (btname)
    {
      free_and_init (btname);
    }

  return ret;
}

static int
btree_fix_ovfl_oid_pages_tree (THREAD_ENTRY * thread_p, BTID * btid,
			       char *btname)
{
  DISK_ISVALID valid = DISK_ERROR;
  VPID vpid;
  PAGE_PTR pgptr = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_HEADER *header = NULL;
  BTID_INT btid_int;

  /* fetch the root page */

  vpid.pageid = btid->root_pageid;
  vpid.volid = btid->vfid.volid;

  pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ,
		     PGBUF_UNCONDITIONAL_LATCH);
  if (pgptr == NULL)
    {
      return ER_FAILED;
    }

  (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_BTREE);

  root_header = btree_get_root_header (pgptr);
  if (root_header == NULL)
    {
      pgbuf_unfix_and_init (thread_p, pgptr);
      return ER_FAILED;
    }

  btid_int.sys_btid = btid;
  if (btree_glean_root_header_info (thread_p, root_header,
				    &btid_int) != NO_ERROR)
    {
      pgbuf_unfix_and_init (thread_p, pgptr);
      return ER_FAILED;
    }

  pgbuf_unfix_and_init (thread_p, pgptr);

  if (BTREE_IS_UNIQUE (btid_int.unique_pk))
    {
      return NO_ERROR;
    }

  pgptr = btree_find_leftmost_leaf (thread_p, btid, &vpid, NULL);
  if (pgptr == NULL)
    {
      return ER_FAILED;
    }

  fixed_pages = 0;
  fprintf (stdout, "Index: %-50s %8d", btname, fixed_pages);

  /* traverse leaf page links */

  while (true)
    {
      if (btree_fix_ovfl_oid_page (thread_p, &btid_int, pgptr,
				   btname) != NO_ERROR)
	{
	  pgbuf_unfix_and_init (thread_p, pgptr);
	  fprintf (stdout, "\n");
	  return ER_FAILED;
	}

      header = btree_get_node_header (pgptr);
      if (header == NULL)
	{
	  pgbuf_unfix_and_init (thread_p, pgptr);
	  fprintf (stdout, "\n");
	  return ER_FAILED;
	}

      vpid = header->next_vpid;

      pgbuf_unfix_and_init (thread_p, pgptr);

      if (VPID_ISNULL (&vpid))
	{
	  break;
	}

      pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ,
			 PGBUF_UNCONDITIONAL_LATCH);
      if (pgptr == NULL)
	{
	  fprintf (stdout, "\n");
	  return ER_FAILED;
	}

      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_BTREE);
    }

  fprintf (stdout, "\n");

  return NO_ERROR;
}

static int
btree_fix_ovfl_oid_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
			 PAGE_PTR pg_ptr, char *btname)
{
  RECDES leaf_rec, ovfl_rec;
  int key_cnt, i, offset;
  LEAF_REC leaf_pnt;
  bool dummy;
  VPID ovfl_vpid;
  PAGE_PTR ovfl_page = NULL;
  char *rv_data = NULL;
  int rv_data_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  BTREE_NODE_HEADER *header = NULL;
  int size = OR_OID_SIZE;

  if (mvcc_Enabled)
    {
      if (BTREE_IS_UNIQUE (btid->unique_pk))
	{
	  size += OR_OID_SIZE;
	}

      size += 2 * OR_MVCCID_SIZE;
    }

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  key_cnt = btree_node_number_of_keys (pg_ptr);

  header = btree_get_node_header (pg_ptr);

  assert_release (header != NULL);
  assert_release (header->node_level == 1);	/* BTREE_LEAF_NODE */

  for (i = 1; i <= key_cnt; i++)
    {
      if (spage_get_record (pg_ptr, i, &leaf_rec, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}

      VPID_SET_NULL (&leaf_pnt.ovfl);
      btree_read_record (thread_p, btid, pg_ptr, &leaf_rec, NULL, &leaf_pnt,
			 BTREE_LEAF_NODE, &dummy, &offset, PEEK_KEY_VALUE,
			 NULL);

      ovfl_vpid = leaf_pnt.ovfl;

      while (!VPID_ISNULL (&ovfl_vpid))
	{
	  ovfl_page = pgbuf_fix (thread_p, &ovfl_vpid, OLD_PAGE,
				 PGBUF_LATCH_WRITE,
				 PGBUF_UNCONDITIONAL_LATCH);
	  if (ovfl_page == NULL)
	    {
	      return ER_FAILED;
	    }

	  (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

	  btree_get_next_overflow_vpid (ovfl_page, &ovfl_vpid);

	  if (spage_get_record (ovfl_page, 1, &ovfl_rec, PEEK) != S_SUCCESS)
	    {
	      pgbuf_unfix_and_init (thread_p, ovfl_page);
	      return ER_FAILED;
	    }

	  /* undo log only */
	  btree_rv_write_log_record (rv_data, &rv_data_len, &ovfl_rec,
				     BTREE_LEAF_NODE);
	  log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
				 &btid->sys_btid->vfid, ovfl_page,
				 1, rv_data_len, rv_data);

	  qsort (ovfl_rec.data, CEIL_PTVDIV (ovfl_rec.length, size),
		 size, btree_compare_oid);

	  pgbuf_set_dirty (thread_p, ovfl_page, FREE);

	  fprintf (stdout, "\rIndex: %-50s %8d", btname, ++fixed_pages);
	  if (fixed_pages % 100 == 0)
	    {
	      fflush (stdout);
	    }
	}
    }

  fflush (stdout);
  return NO_ERROR;
}

static int
btree_compare_oid (const void *oid_mem1, const void *oid_mem2)
{
  OID oid1, oid2;

  BTREE_GET_OID (oid_mem1, &oid1);
  BTREE_CLEAR_RECORD_FLAGS_FROM_OID (&oid1);

  BTREE_GET_OID (oid_mem2, &oid2);
  BTREE_CLEAR_RECORD_FLAGS_FROM_OID (&oid2);

  return oid_compare (&oid1, &oid2);
}
#endif /* MIGRATE_90BETA_TO_91 */

#if !defined(NDEBUG)
static int
btree_verify_node (THREAD_ENTRY * thread_p, BTID_INT * btid_int,
		   PAGE_PTR page_ptr)
{
  int ret = NO_ERROR;
  int key_cnt;
  BTREE_NODE_HEADER *header = NULL;
  BTREE_NODE_TYPE node_type;

  bool clear_key = false;
  int key_type = BTREE_NORMAL_KEY;

  assert_release (btid_int != NULL);
  assert_release (page_ptr != NULL);

  /* check header validation */

  key_cnt = btree_node_number_of_keys (page_ptr);

  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      return ER_FAILED;
    }

  if (key_cnt > 0)
    {
      assert (header->max_key_len > 0);
    }

  assert (header->split_info.pivot >= 0 && header->split_info.pivot <= 1);
  assert (header->split_info.index >= 0);
  assert (header->node_level > 0);

  assert (header->prev_vpid.volid >= NULL_VOLID);
  assert (header->prev_vpid.pageid >= NULL_PAGEID);
  assert (header->next_vpid.volid >= NULL_VOLID);
  assert (header->next_vpid.pageid >= NULL_PAGEID);

#if 0				/* DO NOT DELETE ME */
  /*
   * FOR TEST
   *   usually should admit below assertions.
   *   but assert is possible in normal case rarely.
   *   so, turn on this block in develop stage if you want.
   */

  assert (header->node_level < 20);

  assert (header->prev_vpid.volid < 1000);
  assert (header->prev_vpid.pageid < 1000000);
  assert (header->next_vpid.volid < 1000);
  assert (header->next_vpid.pageid < 1000000);
#endif

  if ((prm_get_integer_value (PRM_ID_ER_BTREE_DEBUG) &
       BTREE_DEBUG_HEALTH_FULL) == 0)
    {
      return NO_ERROR;
    }

  node_type =
    (header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  if (node_type == BTREE_NON_LEAF_NODE)
    {
      ret = btree_verify_nonleaf_node (thread_p, btid_int, page_ptr);
    }
  else
    {
      ret = btree_verify_leaf_node (thread_p, btid_int, page_ptr);
    }

  assert_release (ret == NO_ERROR);

  return ret;
}

static int
btree_verify_nonleaf_node (THREAD_ENTRY * thread_p, BTID_INT * btid_int,
			   PAGE_PTR page_ptr)
{
  BTREE_NODE_HEADER *header = NULL;
  TP_DOMAIN *key_domain;
  int key_cnt;
  int i;
  int offset;
  int c;
  bool clear_prev_key, clear_curr_key;
  DB_VALUE prev_key, curr_key;
  RECDES rec;
  NON_LEAF_REC non_leaf_pnt;

  assert_release (btid_int != NULL);
  assert_release (page_ptr != NULL);

  clear_prev_key = clear_curr_key = false;
  key_domain = btid_int->key_type;

  key_cnt = btree_node_number_of_keys (page_ptr);
  assert_release (key_cnt >= 1);

  /* check key order; exclude neg-inf separator */
  for (i = 1; i < key_cnt; i++)
    {
      if (spage_get_record (page_ptr, i, &rec, PEEK) != S_SUCCESS)
	{
	  assert (false);
	  return ER_FAILED;
	}

      btree_read_record_helper (thread_p, btid_int, &rec, &prev_key,
				&non_leaf_pnt, BTREE_NON_LEAF_NODE,
				&clear_prev_key, &offset, PEEK_KEY_VALUE);

      if (spage_get_record (page_ptr, i + 1, &rec, PEEK) != S_SUCCESS)
	{
	  assert (false);
	  btree_clear_key_value (&clear_prev_key, &prev_key);
	  return ER_FAILED;
	}

      btree_read_record_helper (thread_p, btid_int, &rec, &curr_key,
				&non_leaf_pnt, BTREE_NON_LEAF_NODE,
				&clear_curr_key, &offset, PEEK_KEY_VALUE);

      c =
	btree_compare_key (&prev_key, &curr_key, btid_int->key_type, 1, 1,
			   NULL);

      btree_clear_key_value (&clear_curr_key, &curr_key);
      btree_clear_key_value (&clear_prev_key, &prev_key);

      if (c != DB_LT)
	{
	  if (i == 1)
	    {
	      header = btree_get_node_header (page_ptr);
	      if (header == NULL)
		{
		  return ER_FAILED;
		}

	      if (VPID_ISNULL (&(header->next_vpid)))
		{
		  /* This page is first non-leaf page.
		   * So, this key is neg-inf dummy key */

		  assert (header->next_vpid.volid == NULL_VOLID);

		  return NO_ERROR;
		}
	    }

	  btree_dump_page (thread_p, stdout, NULL, btid_int, NULL, page_ptr,
			   NULL, 2, 2);
	  assert (false);
	  return ER_FAILED;
	}
    }

  return NO_ERROR;
}

static int
btree_verify_leaf_node (THREAD_ENTRY * thread_p, BTID_INT * btid_int,
			PAGE_PTR page_ptr)
{
  BTREE_NODE_HEADER *header = NULL;
  TP_DOMAIN *key_domain;
  VPID prev_vpid, next_vpid;
  int key_cnt, offset, oid_cnt;
  int i, k, c;
  bool clear_prev_key, clear_curr_key;
  DB_VALUE prev_key, curr_key;
  RECDES rec;
  LEAF_REC leaf_pnt;
  OID oid, class_oid;
  OR_BUF buf;
  int oid_size;
  short mvcc_flags;

  assert_release (btid_int != NULL);
  assert_release (page_ptr != NULL);

  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
    {
      oid_size = (2 * OR_OID_SIZE);
    }
  else
    {
      oid_size = OR_OID_SIZE;
    }

  clear_prev_key = clear_curr_key = false;

  key_domain = btid_int->key_type;

  key_cnt = btree_node_number_of_keys (page_ptr);

  /* read the header record */
  header = btree_get_node_header (page_ptr);
  if (header == NULL)
    {
      return ER_FAILED;
    }

  prev_vpid = header->prev_vpid;
  next_vpid = header->next_vpid;

  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
    {
      oid_size += OR_OID_SIZE;
    }

  /* check key order */
  for (i = 1; i < key_cnt; i++)
    {
      if (spage_get_record (page_ptr, i, &rec, PEEK) != S_SUCCESS)
	{
	  btree_dump_page (thread_p, stdout, NULL, btid_int, NULL, page_ptr,
			   NULL, 2, 2);
	  assert (false);
	  return ER_FAILED;
	}

      btree_read_record_helper (thread_p, btid_int, &rec, &prev_key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_prev_key,
				&offset, PEEK_KEY_VALUE);
      /*
       * record oid check
       */
      oid_cnt =
	btree_leaf_get_num_oids (&rec, offset, BTREE_LEAF_NODE, oid_size);

      btree_leaf_get_first_oid (btid_int, &rec, &oid, &class_oid, NULL);
      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{
	  if (oid.pageid != NULL_PAGEID || oid.volid != 0 || oid.slotid != 0)
	    {
	      btree_dump_page (thread_p, stdout, NULL, btid_int, NULL,
			       page_ptr, NULL, 2, 2);
	      assert (false);
	    }
	}
      else
	{
	  if (oid.pageid <= NULL_PAGEID || oid.volid <= NULL_VOLID
	      || oid.slotid <= NULL_SLOTID)
	    {
	      btree_dump_page (thread_p, stdout, NULL, btid_int, NULL,
			       page_ptr, NULL, 2, 2);
	      assert (false);
	    }

	  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
	    {
	      if (class_oid.pageid <= NULL_PAGEID
		  || class_oid.volid <= NULL_VOLID
		  || class_oid.slotid <= NULL_SLOTID)
		{
		  btree_dump_page (thread_p, stdout, NULL, btid_int, NULL,
				   page_ptr, NULL, 2, 2);
		  assert (false);
		}
	    }
	}

      or_init (&buf, rec.data + offset, rec.length - offset);
      {
	if ((rec.length - offset) == 4)
	  {
	    int key_len = btree_get_key_length (&prev_key);
	    printf ("## key_len: %d, offset: %d, reclen: %d\n", key_len,
		    offset, rec.length);
	    db_value_print (&prev_key);
	    printf ("\n");
	    btree_dump_page (thread_p, stdout, NULL, btid_int, NULL, page_ptr,
			     NULL, 2, 2);
	    assert (false);
	  }

	for (k = 1; k < oid_cnt; k++)
	  {
	    mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);
	    or_get_oid (&buf, &oid);
	    oid.volid = oid.volid & ~BTREE_LEAF_OID_MVCC_MASK;
	    if (mvcc_Enabled)
	      {
		if (BTREE_IS_UNIQUE (btid_int->unique_pk))
		  {
		    or_get_oid (&buf, &class_oid);
		  }
		buf.ptr +=
		  BTREE_GET_MVCC_INFO_SIZE_FROM_MVCC_FLAG (mvcc_flags);
	      }

	    if (oid.pageid <= NULL_PAGEID && oid.volid <= NULL_VOLID
		&& oid.slotid <= NULL_SLOTID)
	      {
		btree_dump_page (thread_p, stdout, NULL, btid_int, NULL,
				 page_ptr, NULL, 2, 2);
		assert (false);
	      }
	  }
      }

      /*
       * key order check
       */
      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{
	  continue;
	}

      if (spage_get_record (page_ptr, i + 1, &rec, PEEK) != S_SUCCESS)
	{
	  btree_dump_page (thread_p, stdout, NULL, btid_int, NULL, page_ptr,
			   NULL, 2, 2);
	  assert (false);
	  btree_clear_key_value (&clear_prev_key, &prev_key);
	  return ER_FAILED;
	}

      if (btree_leaf_is_flaged (&rec, BTREE_LEAF_RECORD_FENCE))
	{
	  btree_clear_key_value (&clear_prev_key, &prev_key);
	  continue;
	}

      btree_read_record_helper (thread_p, btid_int, &rec, &curr_key,
				&leaf_pnt, BTREE_LEAF_NODE, &clear_curr_key,
				&offset, PEEK_KEY_VALUE);

      c =
	btree_compare_key (&prev_key, &curr_key, btid_int->key_type, 1, 1,
			   NULL);

      btree_clear_key_value (&clear_curr_key, &curr_key);
      btree_clear_key_value (&clear_prev_key, &prev_key);

      if (c != DB_LT)
	{
	  btree_dump_page (thread_p, stdout, NULL, btid_int, NULL, page_ptr,
			   NULL, 2, 2);
	  assert (false);
	  return ER_FAILED;
	}
    }

  return NO_ERROR;
}
#endif

/*
 * btree_ils_adjust_range () - adjust scanning range for loose index scan
 *   return: error code or NO_ERROR
 *   thread_p(in): thread entry
 *   key_range(in/out): key range to adjust
 *   curr_key(in): current key in btree scan
 *   prefix_len(in): loose scan prefix length
 *   use_desc_index(in): using descending index scan
 *   part_key_desc(in): partial key has descending domain
 */
int
btree_ils_adjust_range (THREAD_ENTRY * thread_p, KEY_VAL_RANGE * key_range,
			DB_VALUE * curr_key, int prefix_len,
			bool use_desc_index, bool part_key_desc)
{
  DB_VALUE new_key, *new_key_dbvals, *target_key;
  TP_DOMAIN *dom;
  DB_MIDXKEY midxkey;
  RANGE old_range;
  bool swap_ranges = false;
  int i;

  /* check environment */
  if (DB_VALUE_DOMAIN_TYPE (curr_key) != DB_TYPE_MIDXKEY)
    {
      assert_release (false);
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);

      return ER_FAILED;
    }

  /* fetch target key */
  if (use_desc_index)
    {
      if (!part_key_desc)
	{
	  swap_ranges = true;
	}
    }
  else
    {
      if (part_key_desc)
	{
	  swap_ranges = true;
	}
    }

  if (swap_ranges)
    {
      /* descending index scan, we adjust upper bound */
      target_key = &key_range->key2;
    }
  else
    {
      /* ascending index scan, we adjust lower bound */
      target_key = &key_range->key1;
    }

  /* allocate key buffer */
  new_key_dbvals =
    (DB_VALUE *) db_private_alloc (thread_p,
				   curr_key->data.midxkey.ncolumns
				   * sizeof (DB_VALUE));
  if (new_key_dbvals == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, curr_key->data.midxkey.ncolumns * sizeof (DB_VALUE));
      return ER_FAILED;
    }

  /* determine target key and adjust range */
  old_range = key_range->range;
  switch (key_range->range)
    {
    case INF_INF:
      if (swap_ranges)
	{
	  key_range->range = INF_LT;	/* (INF, INF) => (INF, ?) */
	}
      else
	{
	  key_range->range = GT_INF;	/* (INF, INF) => (?, INF) */
	}
      break;

    case INF_LE:
      if (swap_ranges)
	{
	  key_range->range = INF_LT;	/* (INF, ?] => (INF, ?) */
	}
      else
	{
	  key_range->range = GT_LE;	/* (INF, ?] => (?, ?] */
	}
      break;

    case INF_LT:
      if (swap_ranges)
	{
	  /* range remains unchanged */
	}
      else
	{
	  key_range->range = GT_LT;	/* (INF, ?) => (?, ?) */
	}
      break;

    case GE_LE:
      if (swap_ranges)
	{
	  key_range->range = GE_LT;	/* [?, ?] => [?, ?) */
	}
      else
	{
	  key_range->range = GT_LE;	/* [?, ?] => (?, ?] */
	}
      break;

    case GE_LT:
      if (swap_ranges)
	{
	  /* range remains unchanged */
	}
      else
	{
	  key_range->range = GT_LT;	/* [?, ?) => (?, ?) */
	}
      break;

    case GE_INF:
      if (swap_ranges)
	{
	  key_range->range = GE_LT;	/* [?, INF) => [?, ?) */
	}
      else
	{
	  key_range->range = GT_INF;	/* [?, INF) => (?, INF)  */
	}
      break;

    case GT_LE:
      if (swap_ranges)
	{
	  key_range->range = GT_LT;	/* (?, ?] => (?, ?) */
	}
      else
	{
	  /* range remains unchanged */
	}
      break;

    case GT_LT:
      /* range remains unchanged */
      break;

    case GT_INF:
      if (swap_ranges)
	{
	  key_range->range = GT_LT;	/* (?, INF) => (?, ?) */
	}
      else
	{
	  /* range remains unchanged */
	}
      break;

    default:
      assert_release (false);	/* should not happen */
      break;
    }

  /* copy prefix of current key into target key */
  for (i = 0; i < prefix_len; i++)
    {
      pr_midxkey_get_element_nocopy (&curr_key->data.midxkey, i,
				     &new_key_dbvals[i], NULL, NULL);
    }

  /* build suffix */

  dom = curr_key->data.midxkey.domain->setdomain;

  /* get to domain */
  for (i = 0; i < prefix_len; i++)
    {
      dom = dom->next;
    }

  /* minimum or maximum suffix */
  for (i = prefix_len; i < curr_key->data.midxkey.ncolumns; i++)
    {
      if ((dom->is_desc && !use_desc_index)
	  || (!dom->is_desc && use_desc_index))
	{
	  DB_MAKE_NULL (&new_key_dbvals[i]);
	}
      else
	{
	  db_value_domain_max (&new_key_dbvals[i], dom->type->id,
			       dom->precision, dom->scale, dom->codeset,
			       dom->collation_id, &dom->enumeration);
	}
      dom = dom->next;
    }

  /* build midxkey */
  midxkey.buf = NULL;
  midxkey.domain = curr_key->data.midxkey.domain;
  midxkey.ncolumns = 0;
  midxkey.size = 0;
  db_make_midxkey (&new_key, &midxkey);
  new_key.need_clear = true;
  pr_midxkey_add_elements (&new_key, new_key_dbvals,
			   curr_key->data.midxkey.ncolumns,
			   curr_key->data.midxkey.domain->setdomain);

#if !defined(NDEBUG)
  if (DB_IS_NULL (target_key))
    {
      assert (!DB_IS_NULL (&new_key));
    }
  else if (old_range == key_range->range)
    {
      int cmp_res;

      /* range did not modify, check if we're advancing */
      cmp_res =
	btree_compare_key (target_key, &new_key, midxkey.domain, 1, 1, NULL);
      if (use_desc_index)
	{
	  assert (cmp_res == DB_GT);
	}
      else
	{
	  assert (cmp_res == DB_LT);
	}
    }
#endif

  /* register key in range */
  pr_clear_value (target_key);
  pr_clone_value (&new_key, target_key);
  pr_clear_value (&new_key);

  db_private_free (thread_p, new_key_dbvals);

  /* all ok */
  return NO_ERROR;
}

/*
 * btree_get_next_node_info () - Scans b-tree node by node and obtains info.
 *
 * return	  : Scan code.
 * thread_p (in)  : Thread entry.
 * btid (in)	  : B-tree identifier.
 * btns (in)	  : B-tree node scan data.
 * node_info (in) : Array of value pointers to store b-tree node information.
 */
SCAN_CODE
btree_get_next_node_info (THREAD_ENTRY * thread_p, BTID * btid,
			  BTREE_NODE_SCAN * btns, DB_VALUE ** node_info)
{
  RECDES rec;
  SCAN_CODE result;
  BTREE_NODE_HEADER *node_header;
  BTREE_NODE_TYPE node_type;
  BTREE_NODE_SCAN_QUEUE_ITEM *new_item = NULL, *crt_item = NULL;
  int key_cnt, i;
  NON_LEAF_REC nleaf;
  LEAF_REC leaf_pnt;
  void *rec_header = NULL;
  DB_VALUE key_value;
  bool clear_key = false;
  int dummy;

  assert (btns->crt_page == NULL);

  if (BTREE_NODE_SCAN_IS_QUEUE_EMPTY (btns))
    {
      if (!btns->first_call)
	{
	  /* Finished scanning for b-tree pages */
	  result = S_END;
	  goto end;
	}

      /* First call */

      /* Add root page to queue */
      new_item =
	(BTREE_NODE_SCAN_QUEUE_ITEM *)
	malloc (sizeof (BTREE_NODE_SCAN_QUEUE_ITEM));
      if (new_item == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, sizeof (BTREE_NODE_SCAN_QUEUE_ITEM));
	  goto error;
	}
      new_item->crt_vpid.pageid = btid->root_pageid;
      new_item->crt_vpid.volid = btid->vfid.volid;
      new_item->next = NULL;
      BTREE_NODE_SCAN_ADD_PAGE_TO_QUEUE (btns, new_item);

      btns->first_call = false;
    }

  BTREE_NODE_SCAN_POP_PAGE_FROM_QUEUE (btns, crt_item);
  btns->crt_vpid = crt_item->crt_vpid;
  btns->crt_page =
    pgbuf_fix (thread_p, &btns->crt_vpid, OLD_PAGE, PGBUF_LATCH_READ,
	       PGBUF_UNCONDITIONAL_LATCH);
  if (btns->crt_page == NULL)
    {
      goto error;
    }

  node_header = btree_get_node_header (btns->crt_page);
  node_type =
    (node_header->node_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;
  key_cnt = btree_node_number_of_keys (btns->crt_page);


  rec_header =
    (node_type == BTREE_NON_LEAF_NODE) ? (void *) &nleaf : (void *) &leaf_pnt;

  if (node_type == BTREE_NON_LEAF_NODE)
    {
      /* Add children to queue */
      for (i = 1; i <= key_cnt; i++)
	{
	  if (spage_get_record (btns->crt_page, i, &rec, PEEK) != S_SUCCESS)
	    {
	      goto error;
	    }
	  btree_read_fixed_portion_of_non_leaf_record (&rec, &nleaf);
	  new_item =
	    (BTREE_NODE_SCAN_QUEUE_ITEM *)
	    malloc (sizeof (BTREE_NODE_SCAN_QUEUE_ITEM));
	  if (new_item == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_OUT_OF_VIRTUAL_MEMORY, 1,
		      sizeof (BTREE_NODE_SCAN_QUEUE_ITEM));
	      goto error;
	    }
	  new_item->crt_vpid.pageid = nleaf.pnt.pageid;
	  new_item->crt_vpid.volid = nleaf.pnt.volid;
	  new_item->next = NULL;
	  BTREE_NODE_SCAN_ADD_PAGE_TO_QUEUE (btns, new_item);
	}
    }

  /* Get b-tree page info */

  /* Get volume id and page id */
  DB_MAKE_INT (node_info[BTREE_NODE_INFO_VOLUMEID], btns->crt_vpid.volid);
  DB_MAKE_INT (node_info[BTREE_NODE_INFO_PAGEID], btns->crt_vpid.pageid);

  /* Get node type */
  db_value_clear (node_info[BTREE_NODE_INFO_NODE_TYPE]);
  DB_MAKE_STRING (node_info[BTREE_NODE_INFO_NODE_TYPE],
		  (node_type == BTREE_NON_LEAF_NODE) ? "non-leaf" : "leaf");

  /* Get key count */
  DB_MAKE_INT (node_info[BTREE_NODE_INFO_KEY_COUNT], key_cnt);

  if (key_cnt > 0)
    {
      /* Get first key */
      if (spage_get_record (btns->crt_page, 1, &rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}
      btree_read_record (thread_p, &btns->btid_int, btns->crt_page, &rec,
			 &key_value, rec_header, node_type, &clear_key,
			 &dummy, PEEK_KEY_VALUE, NULL);
      db_value_clear (node_info[BTREE_NODE_INFO_FIRST_KEY]);
      db_value_clone (&key_value, node_info[BTREE_NODE_INFO_FIRST_KEY]);

      /* Get last key */
      if (spage_get_record (btns->crt_page, key_cnt, &rec, PEEK) != S_SUCCESS)
	{
	  goto error;
	}
      btree_read_record (thread_p, &btns->btid_int, btns->crt_page, &rec,
			 &key_value, rec_header, node_type, &clear_key,
			 &dummy, PEEK_KEY_VALUE, NULL);
      db_value_clear (node_info[BTREE_NODE_INFO_LAST_KEY]);
      db_value_clone (&key_value, node_info[BTREE_NODE_INFO_LAST_KEY]);
    }
  else
    {
      /* Empty node */
      db_value_clear (node_info[BTREE_NODE_INFO_FIRST_KEY]);
      DB_MAKE_NULL (node_info[BTREE_NODE_INFO_FIRST_KEY]);

      db_value_clear (node_info[BTREE_NODE_INFO_LAST_KEY]);
      DB_MAKE_NULL (node_info[BTREE_NODE_INFO_LAST_KEY]);
    }

  result = S_SUCCESS;

end:
  if (btns->crt_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, btns->crt_page);
    }

  if (crt_item != NULL)
    {
      free_and_init (crt_item);
    }
  return result;

error:
  result = S_ERROR;
  goto end;
}

/*
 * btree_range_search () -
 *   return: OIDs count
 *   btid(in): B+-tree identifier
 *   bts(in): B+-tree scan structure
 *   key1(in): the lower bound key value of key range
 *   key2(in): the upper bound key value of key range
 *   range(in): the range of key range
 *   num_classes(in): number of classes contained in class_oids_ptr
 *   class_oids_ptr(in): target classes that are queried
 *   oids_ptr(in): memory space for stroing scanned OIDs
 *   oids_size(in): the size of the memory space(oids_ptr)
 *   filter(in): key filter
 *   isidp(in):
 *   need_construct_btid_int(in):
 *   need_count_only(in):
 *   ils_prefix_len(in): prefix length for index loose scan
 *
 * Note: This functions performs key range search function.
 * Instance level locking function is added in this function.
 */
int
btree_range_search (THREAD_ENTRY * thread_p, BTID * btid,
		    SCAN_OPERATION_TYPE scan_op_type,
		    BTREE_SCAN * bts, KEY_VAL_RANGE * key_val_range,
		    int num_classes, OID * class_oids_ptr, OID * oids_ptr,
		    int oids_size, FILTER_INFO * filter,
		    INDX_SCAN_ID * index_scan_id_p,
		    bool need_construct_btid_int, bool need_count_only,
		    DB_BIGINT * key_limit_upper,
		    DB_BIGINT * key_limit_lower, bool need_to_check_null,
		    int ils_prefix_len)
{
  int i, j;
  OID temp_oid;
  MVCC_REC_HEADER mvcc_header, *p_mvcc_header = NULL;
  int which_action = BTREE_CONTINUE;
  BTREE_RANGE_SEARCH_HELPER btrs_helper;
  int readonly_purpose = false;
  int rec_cp_oid_cnt = 0, oids_count;
  bool skip_curr_key_remainig_oids = false;

  int new_size;
  char *new_ptr = NULL;

#if !defined(NDEBUG)
  if (BTREE_INVALID_INDEX_ID (btid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_INDEX_ID,
	      3, btid->vfid.fileid, btid->vfid.volid, btid->root_pageid);
      return -1;
    }
#endif

  if (mvcc_Enabled)
    {
      MVCC_SET_INSID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_DELID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_NEXT_VERSION (&mvcc_header, &oid_Null_oid);
      p_mvcc_header = &mvcc_header;
    }

  /* initialize key filter */
  bts->key_filter = filter;	/* valid pointer or NULL */

  /* copy use desc index information in the BTS to have it available in
   * the btree functions.
   */
  if (index_scan_id_p->indx_info)
    {
      bts->use_desc_index = index_scan_id_p->indx_info->use_desc_index;
    }
  else
    {
      bts->use_desc_index = 0;
    }

  /* The first request of btree_range_search() */
  if (VPID_ISNULL (&(bts->C_vpid)))
    {
#if !defined(NDEBUG)
      /* check oids_size */
      if (oids_size < OR_OID_SIZE)
	{
	  er_log_debug (ARG_FILE_LINE,
			"btree_range_search: Not enough area to store oid set.\n");
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_GENERIC_ERROR, 0);
	  return -1;
	}
#endif

      /* check range */
      switch (key_val_range->range)
	{
	case EQ_NA:
	case GT_LT:
	case GT_LE:
	case GE_LT:
	case GE_LE:
	case GE_INF:
	case GT_INF:
	case INF_LE:
	case INF_LT:
	case INF_INF:
	  break;
	default:
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_BTREE_INVALID_RANGE,
		  0);
	  return -1;
	}

      if (mvcc_Enabled && !(index_scan_id_p->mvcc_need_locks))
	{
	  /* do not acquire any lock during btree scan */
	  readonly_purpose = true;
	}
      else
	{
	  readonly_purpose = (int) READONLY_SCAN (scan_op_type);
	}

      /* initialize the bts */
      if (btree_initialize_bts (thread_p, bts, btid, readonly_purpose,
				class_oids_ptr, key_val_range,
				filter, need_construct_btid_int,
				index_scan_id_p->copy_buf,
				index_scan_id_p->copy_buf_len,
				index_scan_id_p->for_update,
				index_scan_id_p->mvcc_need_locks) != NO_ERROR)
	{
	  goto error;
	}

      /* scan should not be restarted for now */
      bts->restart_scan = 0;

      /* if (key_desc && scan_asc) || (key_asc && scan_desc),
       * then swap lower value and upper value
       */
      btrs_helper.swap_key_range = false;

      if (!bts->use_desc_index)
	{
	  if (BTREE_IS_PART_KEY_DESC (&(bts->btid_int)))
	    {
	      btrs_helper.swap_key_range = true;
	    }
	}
      else
	{
	  if (!BTREE_IS_PART_KEY_DESC (&(bts->btid_int)))
	    {
	      btrs_helper.swap_key_range = true;
	    }
	}

      if (btrs_helper.swap_key_range)
	{
	  DB_VALUE *tmp_key;

	  tmp_key = bts->key_range.lower_key;
	  bts->key_range.lower_key = bts->key_range.upper_key;
	  bts->key_range.upper_key = tmp_key;

	  switch (bts->key_range.range)
	    {
	    case GT_LE:
	      {
		bts->key_range.range = GE_LT;
		break;
	      }
	    case GE_LT:
	      {
		bts->key_range.range = GT_LE;
		break;
	      }
	    case GE_INF:
	      {
		bts->key_range.range = INF_LE;
		break;
	      }
	    case INF_LE:
	      {
		bts->key_range.range = GE_INF;
		break;
	      }
	    case GT_INF:
	      {
		bts->key_range.range = INF_LT;
		break;
	      }
	    case INF_LT:
	      {
		bts->key_range.range = GT_INF;
		break;
	      }
	    default:
	      break;
	    }
	}
    }
  else
    {
      mnt_bt_resumes (thread_p);
    }

  if (bts->read_uncommitted == false
      && mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      /* temporary disabled skip_curr_key_remaining_oids */
#if 0
      skip_curr_key_remainig_oids = true;
#endif
    }

  btree_range_search_init_helper (thread_p, &btrs_helper, bts,
				  index_scan_id_p, oids_size, oids_ptr,
				  ils_prefix_len);

search_again:

  if (btree_prepare_range_search (thread_p, bts) != NO_ERROR)
    {
      goto error;
    }

get_oidcnt_and_oidptr:
  /* get 'rec_oid_cnt' and 'rec_oid_ptr' */
  if (btree_get_oid_count_and_pointer (thread_p, bts, &btrs_helper,
				       key_limit_upper, index_scan_id_p,
				       need_to_check_null, &which_action)
      != NO_ERROR)
    {
      goto error;
    }

  switch (which_action)
    {
    case BTREE_CONTINUE:
      /* Fall through to start locking */
      break;

    case BTREE_GETOID_AGAIN_WITH_CHECK:
      goto get_oidcnt_and_oidptr;

    case BTREE_RESTART_SCAN:
      bts->restart_scan = 1;
      /* fall through */
    case BTREE_GOTO_END_OF_SCAN:
      goto end_of_scan;

    default:
      /* Unexpected case */
      assert (0);
      goto error;
    }

  if (!BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      assert (!BTREE_IS_PRIMARY_KEY (bts->btid_int.unique_pk));

      if (OID_ISNULL (&bts->cls_oid))
	{
	  BTREE_GET_OID (btrs_helper.rec_oid_ptr, &temp_oid);
	  if (heap_get_class_oid (thread_p, &bts->cls_oid, &temp_oid,
				  DONT_NEED_SNAPSHOT) == NULL)
	    {
	      goto error;
	    }
	}
      assert (!OID_ISNULL (&bts->cls_oid));
    }

start_locking:

  /* This steps handles OID and key locking and copying of OID's */
  /* If read uncommitted is true, or if it is not server mode, OID's are
   * copied directly with no locking.
   * When OID buffer is full, it is handled in two ways:
   *  - If server mode && read uncommitted: extend OID buffer to make room
   *    for all OID's belonging to current key. Resume search after this key
   *    is completely handled.
   *  - Otherwise, copy OID's until the buffer is full. Resume search from
   *    the last copied OID.
   */

#if !defined(NDEBUG)
  /* check the validity for locking and copying OIDs */
  if (btrs_helper.rec_oid_cnt <= 0)
    {
      er_log_debug (ARG_FILE_LINE,
		    "index inconsistency..(rec_oid_cnt(%d) <= 0)\n",
		    btrs_helper.rec_oid_cnt);
      goto error;
    }
#endif

  if ((btrs_helper.rec_oid_cnt - bts->oid_pos) < 0)
    {
      goto locking_done;
    }

  btrs_helper.read_prev_key = true;

  if (bts->read_uncommitted)
    {
      /* Read uncommitted case: no lock is required, just read all OID's.
       * This is used also in MVCC (no read uncommitted) to read visible objects
       * without locking them. The objects are filter out using snapshot.
       */

      skip_curr_key_remainig_oids = false;
      if (btrs_helper.keep_on_copying)
	{
	  /* Currently, the OID count exceeds the default maximum OID count in
	   * buffer. The buffer was extended and the current iteration of
	   * btree_range_search should end when all OID's belonging to current
	   * key are obtained.
	   */
	  if (VPID_ISNULL (&(bts->O_vpid)) && bts->oid_pos == 0)
	    {
	      /* when the first OID of the next key is found, stop current
	       * iteration of btree_range_search
	       */

	      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
	      btree_clear_key_value (&btrs_helper.clear_prev_key,
				     &btrs_helper.prev_key);

	      /* do not clear bts->cur_key. It is needed for
	       * btree_prepare_next_search ().
	       */
	      goto resume_next_search;
	    }
	}

      if (mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
	{
	  /* in MVCC, even if there are multiple OIDs in unique BTREE,
	   * only one is visible.
	   */
	  rec_cp_oid_cnt = 1;
	}
      else
	{
	  rec_cp_oid_cnt = btrs_helper.rec_oid_cnt - bts->oid_pos;
	}

      if (need_count_only == true)
	{
	  /* do not consider size of list file or oid buffer */
	  btrs_helper.cp_oid_cnt = rec_cp_oid_cnt;
	}
      else if (ils_prefix_len > 0)
	{
	  btrs_helper.cp_oid_cnt = 1;
	}
      else if (SCAN_IS_INDEX_COVERED (index_scan_id_p))
	{
	  if ((rec_cp_oid_cnt)
	      > (btrs_helper.pg_oid_cnt - btrs_helper.oids_cnt))
	    {
	      /*
	       * Recalculate the maximum size of list file to read
	       * all records stored in the current page.
	       */
	      btrs_helper.pg_oid_cnt = btrs_helper.oids_cnt + rec_cp_oid_cnt;
	      btrs_helper.keep_on_copying = true;
	    }

	  btrs_helper.cp_oid_cnt = rec_cp_oid_cnt;
	}
      else
	{
	  while (true)
	    {
	      if ((rec_cp_oid_cnt)
		  <= (btrs_helper.pg_oid_cnt - btrs_helper.oids_cnt))
		{
		  btrs_helper.cp_oid_cnt = rec_cp_oid_cnt;
		  break;
		}

	      if (btrs_helper.pg_oid_cnt < 10)
		{
		  /*
		   * some special purpose :
		   * It's purpose is to find out uniqueness in unique index.
		   * Therefore, copying only part of OID set(more than 1 OID)
		   * is sufficient. That is, the caller can identify
		   * the uniqueness through the number of copied OIDs.
		   */
		  btrs_helper.cp_oid_cnt =
		    btrs_helper.pg_oid_cnt - btrs_helper.oids_cnt;
		  break;
		}

	      /* OID memory space is insufficient */
	      if (VPID_ISNULL (&(bts->O_vpid)) && bts->oid_pos == 0)
		{
		  /* when the first OID of each key is found */
		  if (btrs_helper.oids_cnt > 0)
		    {
		      LSA_COPY (&bts->cur_leaf_lsa,
				pgbuf_get_lsa (bts->C_page));
		      btree_clear_key_value (&btrs_helper.clear_prev_key,
					     &btrs_helper.prev_key);

		      /* do not clear bts->cur_key for btree_prepare_next_search */

		      goto resume_next_search;
		    }

		  /* oids_cnt == 0 */
		  er_log_debug (ARG_FILE_LINE,
				"btree_range_search() : OID memory space"
				" is too small");
		}

	      new_size = (btrs_helper.pg_oid_cnt * OR_OID_SIZE) + oids_size;
	      new_ptr = (char *) os_realloc (oids_ptr, new_size);
	      if (new_ptr == NULL)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_OUT_OF_VIRTUAL_MEMORY, 1, new_size);
		  /* memory space allocation has failed. */
		  er_log_debug (ARG_FILE_LINE,
				"btree_range_search() : Part of OIDs are"
				" copied in Uncommitted Read or the size of"
				" OID set is so large");
		  /* copy some of the remaining OIDs */
		  btrs_helper.cp_oid_cnt =
		    btrs_helper.pg_oid_cnt - btrs_helper.oids_cnt;
		  break;
		}

	      /* The memory space allocation has succeeded. */
	      index_scan_id_p->oid_list.oidp = oids_ptr = (OID *) new_ptr;
	      btrs_helper.pg_oid_cnt = new_size / OR_OID_SIZE;
	      btrs_helper.mem_oid_ptr = oids_ptr + btrs_helper.oids_cnt;
	      index_scan_id_p->curr_oidp = (OID *) new_ptr;
	      btrs_helper.keep_on_copying = true;
	    }

	  assert (btrs_helper.cp_oid_cnt >= 1);
	  assert (btrs_helper.rec.data != NULL);
	}

      /* copy corresponding OIDs */
      if (!BTREE_IS_UNIQUE (bts->btid_int.unique_pk) || num_classes == 0)
	{
	  oids_count = btrs_helper.oids_cnt;
	  /*
	   * 1. current index is a non-unique index. or
	   * 2. current index is an unique index. &&
	   *    current query is based on all classes
	   *    contained in the class hierarchy.
	   */
	  for (i = 0; i < btrs_helper.cp_oid_cnt; i++)
	    {
	      btree_leaf_get_oid_from_oidptr (bts, btrs_helper.rec_oid_ptr,
					      btrs_helper.node_type,
					      &temp_oid,
					      &btrs_helper.class_oid,
					      p_mvcc_header);
	      if (btree_handle_current_oid (thread_p, bts, &btrs_helper,
					    key_limit_lower, key_limit_upper,
					    index_scan_id_p, need_count_only,
					    &temp_oid, p_mvcc_header,
					    &which_action) != NO_ERROR)
		{
		  goto error;
		}
	      if (which_action == BTREE_GOTO_END_OF_SCAN)
		{
		  goto end_of_scan;
		}
	      else if (which_action == BTREE_RESTART_SCAN)
		{
		  bts->restart_scan = 1;
		  goto end_of_scan;
		}
	      assert (which_action == BTREE_CONTINUE);
	      if (mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk)
		  && oids_count < btrs_helper.oids_cnt)
		{
		  /* in MVCC, even if there are multiple OIDs in unique BTREE,
		   * only one is visible, so skip remaining OIDs.
		   * The visible OID for current transaction may be in any
		   * position.
		   */
		  skip_curr_key_remainig_oids = true;
		  break;
		}
	    }
	}
      else
	{
	  /*
	   * current index is an unique index. &&
	   * current query is based on some classes
	   * contained in the class hierarchy.
	   */

	  for (i = 0; i < btrs_helper.cp_oid_cnt; i++)
	    {
	      /* The class oid comparison must be performed. */
	      btree_leaf_get_oid_from_oidptr (bts, btrs_helper.rec_oid_ptr,
					      btrs_helper.node_type,
					      &btrs_helper.inst_oid,
					      &btrs_helper.class_oid,
					      p_mvcc_header);
	      for (j = 0; j < num_classes; j++)
		{
		  if (OID_EQ (&btrs_helper.class_oid, &class_oids_ptr[j]))
		    {
		      break;
		    }
		}
	      if (j < num_classes)
		{		/* satisfying OID */
		  if (btree_handle_current_oid (thread_p, bts, &btrs_helper,
						key_limit_lower,
						key_limit_upper,
						index_scan_id_p,
						need_count_only,
						&btrs_helper.inst_oid,
						p_mvcc_header,
						&which_action) != NO_ERROR)
		    {
		      goto error;
		    }
		  if (which_action == BTREE_GOTO_END_OF_SCAN)
		    {
		      goto end_of_scan;
		    }
		  else if (which_action == BTREE_RESTART_SCAN)
		    {
		      bts->restart_scan = 1;
		      goto end_of_scan;
		    }
		  assert (which_action == BTREE_CONTINUE);
		}
	      else
		{
		  /* Just advance rec_oid_ptr */
		  btrs_helper.rec_oid_ptr =
		    btree_leaf_advance_oidptr (bts, btrs_helper.rec_oid_ptr,
					       btrs_helper.offset,
					       btrs_helper.node_type);
		}
	    }
	}

      goto locking_done;
    }
  else
    {				/* bts->read_uncommitted == false */
      /*
       * bts->tran_isolation :
       * TRAN_READ_COMMITTED
       * TRAN_REPEATABLE_READ
       * TRAN_SERIALIZABLE
       */

      if (btrs_helper.saved_inst_oid.pageid != NULL_PAGEID
	  || (bts->prev_oid_pos == -1 && btrs_helper.curr_key_locked))
	{
	  if (btree_range_search_handle_previous_locks (thread_p, bts,
							&btrs_helper,
							key_limit_lower,
							key_limit_upper,
							index_scan_id_p,
							need_count_only,
							&which_action)
	      != NO_ERROR)
	    {
	      goto error;
	    }
	  switch (which_action)
	    {
	    case BTREE_CONTINUE:
	      /* Fall through */
	      break;

	    case BTREE_RESTART_SCAN:
	      bts->restart_scan = 1;
	      /* fall through */
	    case BTREE_GOTO_END_OF_SCAN:
	      goto end_of_scan;

	    case BTREE_GOTO_LOCKING_DONE:
	      goto locking_done;

	    default:
	      /* Unexpected case */
	      assert (0);
	      goto error;
	    }
	}

      /* compute 'cp_oid_cnt' */
      if (btrs_helper.is_condition_satisfied == false)
	{
	  if (mvcc_Enabled)
	    {
	      /* no need to lock keys in MVCC */
	      goto locking_done;
	    }

	  if (((bts->tran_isolation == TRAN_SERIALIZABLE
		|| scan_op_type != S_SELECT)
	       && (bts->prev_KF_satisfied == false || bts->prev_oid_pos > 0
		   || btrs_helper.is_key_range_satisfied))
	      || bts->prev_oid_pos == -1)
	    {
	      btrs_helper.cp_oid_cnt = 1;
	    }
	  else
	    {
	      /* no need to lock the object which is next to the scan range
	       * under NON-Serializable isolation.
	       */
	      goto locking_done;
	    }
	}
      else
	{
	  if (mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
	    {
	      /* in MVCC, when need locking, even if there are multiple OIDs
	       * in unique BTREE, only the first may be visible and lockable.
	       * If the first OID is not visible, skip the record.
	       */
	      rec_cp_oid_cnt = 1;
	    }
	  else
	    {
	      rec_cp_oid_cnt = btrs_helper.rec_oid_cnt - bts->oid_pos;
	    }

	  if (need_count_only == true)
	    {			/* do not concern buffer size */
	      btrs_helper.cp_oid_cnt = rec_cp_oid_cnt;
	    }
	  else if (ils_prefix_len > 0)
	    {
	      btrs_helper.cp_oid_cnt = 1;
	    }
	  else
	    {
	      /* Covering index has also a limitation on the size of list file. */
	      btrs_helper.cp_oid_cnt =
		MIN (btrs_helper.pg_oid_cnt - btrs_helper.oids_cnt,
		     rec_cp_oid_cnt);
	    }

	  if (btrs_helper.cp_oid_cnt <= 0)
	    {			/* for uncommitted read */
	      goto locking_done;
	    }
	}


      /*
       * If bts->key_range_max_value_equal or mvcc_Enabled is true,
       * lock on the next key is not required
       */
      if ((btrs_helper.is_key_range_satisfied == false)
	  && (bts->key_range_max_value_equal || mvcc_Enabled))
	{
	  goto end_of_scan;
	}

      /*
       * locking and copying corresponding OIDs
       */
      for (i = 0; i < btrs_helper.cp_oid_cnt; i++)
	{
	  if (btree_handle_current_oid_and_locks (thread_p, bts, &btrs_helper,
						  btid, key_limit_lower,
						  key_limit_upper,
						  index_scan_id_p,
						  need_count_only,
						  num_classes, class_oids_ptr,
						  scan_op_type, i,
						  &which_action) != NO_ERROR)
	    {
	      goto error;
	    }
	  switch (which_action)
	    {
	    case BTREE_CONTINUE:
	      break;
	    case BTREE_GOTO_LOCKING_DONE:
	      goto locking_done;
	    case BTREE_RESTART_SCAN:
	      bts->restart_scan = 1;
	      /* fall trough */
	    case BTREE_GOTO_END_OF_SCAN:
	      goto end_of_scan;
	    case BTREE_GETOID_AGAIN_WITH_CHECK:
	      goto get_oidcnt_and_oidptr;
	    case BTREE_SEARCH_AGAIN_WITH_CHECK:
#if !defined (SERVER_MODE)
	      assert_release (false);
#endif
	      goto search_again;
	    default:
	      assert (0);
	      goto error;
	    }
	}			/* for (i = 0; i < cp_oid_cnt; i++) */
    }

locking_done:

  if (bts->read_uncommitted == false)
    {
      /* if key range condition is not satisfied */
      if (btrs_helper.is_key_range_satisfied == false)
	{
	  goto end_of_scan;
	}

      /* if key filter condition is not satisfied */
      if (btrs_helper.is_key_filter_satisfied == false)
	{
	  /* clear 'prev_oid_pos' and 'prev_ovfl_vpid' */
	  bts->prev_oid_pos = 0;
	  VPID_SET_NULL (&(bts->prev_ovfl_vpid));
	  if (btree_find_next_index_record (thread_p, bts) != NO_ERROR)
	    {
	      goto error;
	    }

	  goto get_oidcnt_and_oidptr;
	}
    }

  if (need_count_only == false
      && btrs_helper.oids_cnt == btrs_helper.pg_oid_cnt)
    {
      /* We have no more room. */
      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
      btree_clear_key_value (&btrs_helper.clear_prev_key,
			     &btrs_helper.prev_key);

      /* do not clear bts->cur_key for btree_prepare_next_search */

      goto resume_next_search;
    }

  /* oids_cnt < pg_oid_cnt : We have more room in the OID set space. */
  if (skip_curr_key_remainig_oids)
    {
      /* in MVCC, even if there are multiple OIDs in unique BTREE,
       * only one is visible and if is already fetched (and locked in
       * read committed). So, in this case, advance to the next record.
       */
      if (btree_find_next_index_record (thread_p, bts) != NO_ERROR)
	{
	  goto error;
	}

      goto get_oidcnt_and_oidptr;
    }

  if (need_count_only == false && bts->oid_pos < btrs_helper.rec_oid_cnt)
    {
      /*
       * All OIDs in the index entry are locked and copied.
       * But, when the space for keeping the OIDs is insufficient,
       * a part of the OIDs is locked and copied.
       * Therefore, the truth that 'bts->oid_pos' is smaller
       * than 'rec_oid_cnt' means following things:
       * Some page update has occurred during the unconditional
       * instance locking and the currently locked OID has been changed.
       */
      goto start_locking;
    }
  else
    {
      bts->prev_oid_pos = btrs_helper.rec_oid_cnt - 1;
      bts->prev_ovfl_vpid = bts->O_vpid;

      /* bts->oid_pos >= rec_oid_cnt */
      /* leaf_pnt is still having valid values. */
      if (btree_get_next_oidset_pos (thread_p, bts,
				     &btrs_helper.leaf_pnt.ovfl) != NO_ERROR)
	{
	  goto error;
	}

      goto get_oidcnt_and_oidptr;
    }

error:

  btrs_helper.oids_cnt = -1;

  /*
   * we need to make sure that
   * BTREE_END_OF_SCAN() return true in the error cases.
   */

  /* fall through */

end_of_scan:

  btree_clear_key_value (&btrs_helper.clear_prev_key, &btrs_helper.prev_key);

  if (!bts->restart_scan)
    {
      /* clear all the used keys */
      btree_scan_clear_key (bts);
    }

  /* set the end of scan */
  VPID_SET_NULL (&(bts->C_vpid));
  VPID_SET_NULL (&(bts->O_vpid));

resume_next_search:

  /* unfix all the index pages */
  if (bts->P_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->P_page);
    }

  if (bts->C_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  if (bts->O_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, bts->O_page);
    }

  if (key_limit_upper && btrs_helper.oids_cnt != -1)
    {
      if ((DB_BIGINT) btrs_helper.oids_cnt >= *key_limit_upper)
	{
	  *key_limit_upper = 0;
	}
      else
	{
	  *key_limit_upper -= btrs_helper.oids_cnt;
	}
    }

  return btrs_helper.oids_cnt;
}

/*
 * btree_range_search_init_helper () - Initialize btree_range_search_helper
 *				       at the start of a search.
 *
 * return		: Void.
 * thread_p (in)	: Thread entry.
 * btrs_helper (out)	: B-tree range search helper.
 * bts (in)		: B-tree scan data.
 * index_scan_id_p (in) : Index scan data.
 * oids_size (in)	: The total size of OID memory storage.
 * oids_ptr (in)	: Pointer to OID memory storage.
 */
static void
btree_range_search_init_helper (THREAD_ENTRY * thread_p,
				BTREE_RANGE_SEARCH_HELPER * btrs_helper,
				BTREE_SCAN * bts,
				INDX_SCAN_ID * index_scan_id_p,
				int oids_size, OID * oids_ptr,
				int ils_prefix_len)
{
  btrs_helper->mem_oid_ptr = NULL;
  btrs_helper->rec_oid_ptr = NULL;

  btrs_helper->oids_cnt = 0;
  btrs_helper->cp_oid_cnt = 0;

  btrs_helper->swap_key_range = false;

  btrs_helper->is_key_range_satisfied = true;
  btrs_helper->is_key_filter_satisfied = true;
  btrs_helper->is_condition_satisfied = true;

  OID_SET_NULL (&btrs_helper->class_oid);
  OID_SET_NULL (&btrs_helper->inst_oid);

  btrs_helper->rec.data = NULL;

  if (SCAN_IS_INDEX_COVERED (index_scan_id_p))
    {
      btrs_helper->pg_oid_cnt = index_scan_id_p->indx_cov.max_tuples;
      btrs_helper->mem_oid_ptr = NULL;
    }
  else
    {
      btrs_helper->pg_oid_cnt = oids_size / OR_OID_SIZE;
      btrs_helper->mem_oid_ptr = oids_ptr;
    }

  /* When using Index Skip Scan, this function is sometimes called just to
   * get the next value for the first column of the index. In this case we
   * will exit immediately after the first element is found, returning it.
   * This will get handled by scan_next_scan_local, which will construct a
   * real range based on the value we return and call this function again,
   * this time stating that it wants to perform a regular index scan by
   * setting isidp->iss.current_op to ISS_OP_PERFORM_REGULAR_SCAN.
   */
  btrs_helper->iss_get_first_result_only =
    (index_scan_id_p->iss.use
     && ((index_scan_id_p->iss.current_op == ISS_OP_GET_FIRST_KEY)
	 || (index_scan_id_p->iss.current_op
	     == ISS_OP_SEARCH_NEXT_DISTINCT_KEY)));

  btrs_helper->oids_cnt = 0;	/* # of copied OIDs */

  /* get the size of each OID information in the index */
  if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      btrs_helper->oid_size = 2 * OR_OID_SIZE;
    }
  else
    {
      btrs_helper->oid_size = OR_OID_SIZE;
    }

  /* restart after first OID iff doing index loose scan */
  btrs_helper->restart_on_first = (ils_prefix_len > 0);

  btrs_helper->CLS_satisfied = true;

  btrs_helper->keep_on_copying = false;

  OID_SET_NULL (&btrs_helper->ck_pseudo_oid);
  OID_SET_NULL (&btrs_helper->saved_ck_pseudo_oid);
  OID_SET_NULL (&btrs_helper->nk_pseudo_oid);
  OID_SET_NULL (&btrs_helper->saved_nk_pseudo_oid);
  OID_SET_NULL (&btrs_helper->saved_nk_class_oid);
  OID_SET_NULL (&btrs_helper->saved_class_oid);
  OID_SET_NULL (&btrs_helper->saved_inst_oid);

  btrs_helper->end_of_leaf_level = false;
  btrs_helper->curr_key_locked = false;
  btrs_helper->next_key_locked = false;
  btrs_helper->current_lock_request = false;

  btrs_helper->read_prev_key = true;
  btrs_helper->clear_prev_key = false;
  DB_MAKE_NULL (&btrs_helper->prev_key);
}

/*
 * btree_get_oid_count_and_pointer () - Used in the context of
 *					btree_range_search function, this
 *					obtains a set of OIDs once the
 *					search is positioned to on a key in a
 *					a leaf node, or on an overflow OID
 *					node.
 *
 * return		   : Error code.
 * thread_p (in)	   : Thread entry.
 * bts (in)		   : B-tree scan data.
 * btrs_helper (in/out)	   : B-tree range search helper.
 * key_limit_upper (in)	   : NULL or upper key limit value.
 * index_scan_id_p (in)	   : Index scan data.
 * need_to_check_null (in) : True if filter condition should check null.
 * which_action (out)	   : Which action follows.
 *
 * NOTE: There are a few outcomes for this function:
 *	 1. If end of leaf level is reached
 *	    - End scan if read_uncommitted is true, or if key range max value
 *	    was reached.
 *	    - Otherwise lock the "end" of index (unique pseudo-oid).
 *	 2. Current page is an overflow OID node:
 *	    - Get the remaining OID's if oid_pos is not at the end of OID set.
 *	    - Otherwise go to next set of OID's.
 *	 3. Current page is a leaf node:
 *	    - A new key (oid_pos == 0):
 *	      - Key range is satisfied
 *		- Key filter satisfied, continue to start locking
 *		- Key filter is not satisfied, advance to next key
 *	      - Key range is not satisfied:
 *		- If read uncommitted end the scan.
 *		- Continue to start locking otherwise.
 *	    - An older key is resumed (oid_pos != 0)
 *		- If oid_pos is position to the end of OID set go to next OID
 *		set.
 *		- Otherwise obtain the remaining OID's.
 */
static int
btree_get_oid_count_and_pointer (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
				 BTREE_RANGE_SEARCH_HELPER * btrs_helper,
				 DB_BIGINT * key_limit_upper,
				 INDX_SCAN_ID * index_scan_id_p,
				 bool need_to_check_null, int *which_action)
{
  bool dummy_clear;

  assert (bts != NULL);
  assert (btrs_helper != NULL);
  assert (which_action != NULL);

  /* This function is followed by start_locking step */
  *which_action = BTREE_CONTINUE;

  if (key_limit_upper != NULL
      && (DB_BIGINT) btrs_helper->oids_cnt >= *key_limit_upper)
    {
      /* Upper key limit is reached, stop searching */
      *which_action = BTREE_GOTO_END_OF_SCAN;
      return NO_ERROR;
    }

  if (VPID_ISNULL (&(bts->C_vpid)))
    {
      /* It reached at the end of leaf level */
      OID N_oid;

      btrs_helper->end_of_leaf_level = true;
      if (bts->read_uncommitted || mvcc_Enabled)
	{
	  /* Read uncommitted, no other lock is required
	   * In MVCC do not lock any key.
	   */
	  *which_action = BTREE_GOTO_END_OF_SCAN;
	  return NO_ERROR;
	}

      /*
       * If bts->key_range_max_value_equal is true,
       * lock on the next key is not required,
       * even if the index is non-unique
       */
      if (bts->key_range_max_value_equal)
	{
	  *which_action = BTREE_GOTO_END_OF_SCAN;
	  return NO_ERROR;
	}

      btrs_helper->is_key_range_satisfied = false;
      btrs_helper->is_condition_satisfied = false;

      bts->oid_pos = 0;
      btrs_helper->rec_oid_cnt = 1;

      N_oid.volid = bts->btid_int.sys_btid->vfid.volid;
      N_oid.pageid = bts->btid_int.sys_btid->root_pageid;
      N_oid.slotid = 0;

      btrs_helper->rec_oid_ptr = &btrs_helper->oid_space[0];
      btrs_helper->rec.data = btrs_helper->rec_oid_ptr;

      OR_PUT_OID (btrs_helper->rec_oid_ptr, &N_oid);

      btrs_helper->rec.length = btrs_helper->oid_size;

      btrs_helper->offset = btrs_helper->rec.length;
      btrs_helper->node_type = BTREE_LEAF_NODE;

      if (bts->read_cur_key)
	{
	  if (bts->prev_oid_pos != -1 && DB_IS_NULL (&btrs_helper->prev_key)
	      && DB_IS_NULL (&bts->cur_key) == false)
	    {
	      /* we have reached the end of leaf level,
	       * Save the previous key, in order to handle the next processing
	       * after unconditional locking, if the page has been changed
	       * during unconditional locking
	       */
	      pr_clone_value (&bts->cur_key, &btrs_helper->prev_key);
	      btrs_helper->clear_prev_key = true;
	    }
	}

      /* Continue to start locking */
      return NO_ERROR;
    }

  /* Find the position of OID list to be searched in the index entry */
  if (bts->O_page != NULL)
    {
      /*
       * bts->O_page != NULL : current overflow page
       * Why COPY method be used in reading an index record ?
       * PEEK method can be used instead of COPY method.
       * The reason is described when which_action is BTREE_CONTINUE
       * after the unconditional locking.
       */
      if (spage_get_record (bts->O_page, 1, &btrs_helper->rec, PEEK)
	  != S_SUCCESS)
	{
	  return ER_FAILED;
	}
      assert (btrs_helper->rec.length % 4 == 0);
      assert ((btrs_helper->oid_size == OR_OID_SIZE)
	      || (mvcc_Enabled && BTREE_IS_UNIQUE (bts->btid_int.unique_pk)
		  && btrs_helper->oid_size == 2 * OR_OID_SIZE));

      btrs_helper->offset = 0;
      btrs_helper->node_type = BTREE_OVERFLOW_NODE;

      /* # of OIDs contained in the overflow page */
      assert (mvcc_Enabled || btrs_helper->oid_size == OR_OID_SIZE);
      btrs_helper->rec_oid_cnt =
	btree_leaf_get_num_oids (&btrs_helper->rec, btrs_helper->offset,
				 btrs_helper->node_type,
				 btrs_helper->oid_size);
      if (bts->oid_pos < btrs_helper->rec_oid_cnt)
	{
	  /* Set rec_oid_ptr offset by oid_pos OID's */
	  btrs_helper->rec_oid_ptr =
	    btree_leaf_get_nth_oid_ptr (&bts->btid_int, &btrs_helper->rec,
					btrs_helper->node_type,
					btrs_helper->offset, bts->oid_pos);
	}
      else
	{
	  /* Go to next set of OID's */
	  bts->prev_oid_pos = btrs_helper->rec_oid_cnt - 1;
	  bts->prev_ovfl_vpid = bts->O_vpid;

	  /* the 2nd argument, first_ovfl_vpid, is NULL */
	  if (btree_get_next_oidset_pos (thread_p, bts, (VPID *) NULL) !=
	      NO_ERROR)
	    {
	      return ER_FAILED;
	    }

	  *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;
	  return NO_ERROR;
	}
    }
  else
    {
      /* bts->O_page == NULL : current leaf page */
      assert (bts->slot_id > 0);
      if (spage_get_record
	  (bts->C_page, bts->slot_id, &btrs_helper->rec, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}
      assert (btrs_helper->rec.length % 4 == 0);
      btrs_helper->node_type = BTREE_LEAF_NODE;

      if (bts->oid_pos > 0)
	{			/* same key value */
	  /* The key range and key filter checking is not needed. */
	  (void) btree_read_record (thread_p, &bts->btid_int,
				    bts->C_page,
				    &btrs_helper->rec, NULL,
				    (void *) &btrs_helper->
				    leaf_pnt,
				    btrs_helper->node_type,
				    &dummy_clear,
				    &btrs_helper->offset,
				    PEEK_KEY_VALUE, bts);

	  /* get 'rec_oid_cnt' and 'rec_oid_ptr' */
	  btrs_helper->rec_oid_cnt =
	    btree_leaf_get_num_oids (&btrs_helper->rec, btrs_helper->offset,
				     btrs_helper->node_type,
				     btrs_helper->oid_size);
	  if (bts->oid_pos < btrs_helper->rec_oid_cnt)
	    {
	      btrs_helper->rec_oid_ptr =
		btree_leaf_get_nth_oid_ptr (&bts->btid_int, &btrs_helper->rec,
					    btrs_helper->node_type,
					    btrs_helper->offset,
					    bts->oid_pos);
	    }
	  else
	    {
	      bts->prev_oid_pos = btrs_helper->rec_oid_cnt - 1;
	      VPID_SET_NULL (&(bts->prev_ovfl_vpid));

	      /*
	       * check if next OID set is in overflow page
	       * or next index record. bts->oid_pos, slot_id may be changed
	       */
	      if (btree_get_next_oidset_pos (thread_p, bts,
					     &btrs_helper->leaf_pnt.ovfl)
		  != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;
	      return NO_ERROR;
	    }
	}
      else
	{			/* new key value */
	  if (bts->read_uncommitted == false && bts->read_cur_key
	      && btrs_helper->read_prev_key)
	    {
	      btree_clear_key_value (&btrs_helper->clear_prev_key,
				     &btrs_helper->prev_key);

	      pr_clone_value (&bts->cur_key, &btrs_helper->prev_key);
	      /* pr_clone_value allocates and copies a DB_VALUE */
	      btrs_helper->clear_prev_key = true;

	      bts->read_cur_key = false;	/* reset read_cur_key */
	    }

	  btree_clear_key_value (&bts->clear_cur_key, &bts->cur_key);

	  (void) btree_read_record (thread_p, &bts->btid_int,
				    bts->C_page,
				    &btrs_helper->rec,
				    &bts->cur_key,
				    (void *) &btrs_helper->
				    leaf_pnt,
				    btrs_helper->node_type,
				    &bts->clear_cur_key,
				    &btrs_helper->offset,
				    COPY_KEY_VALUE, bts);

	  /* the last argument means that key value must be copied. */

	  bts->read_cur_key = true;

	  /* get 'rec_oid_cnt' and 'rec_oid_ptr' */
	  btrs_helper->rec_oid_cnt =
	    btree_leaf_get_num_oids (&btrs_helper->rec, btrs_helper->offset,
				     btrs_helper->node_type,
				     btrs_helper->oid_size);
	  btrs_helper->rec_oid_ptr = btrs_helper->rec.data;

	  /* save the result of key filtering on the previous key value */
	  if (btrs_helper->saved_inst_oid.pageid == NULL_PAGEID)
	    {
	      bts->prev_KF_satisfied =
		(int) btrs_helper->is_key_filter_satisfied;
	    }

	  /* apply key range and key filter to the new key value */
	  if (btree_apply_key_range_and_filter (thread_p, bts,
						index_scan_id_p->iss.use,
						&btrs_helper->
						is_key_range_satisfied,
						&btrs_helper->
						is_key_filter_satisfied,
						need_to_check_null)
	      != NO_ERROR)
	    {
	      return ER_FAILED;
	    }

	  if (btrs_helper->is_key_range_satisfied == false)
	    {
	      btrs_helper->is_condition_satisfied = false;
	      if (bts->read_uncommitted || mvcc_Enabled)
		{
		  /* In MVCC, nothing to do since no need to lock keys */
		  *which_action = BTREE_GOTO_END_OF_SCAN;
		}
	    }
	  else
	    {
	      bts->read_keys++;

	      if (btrs_helper->is_key_filter_satisfied == false)
		{
		  btrs_helper->is_condition_satisfied = false;
		  if (bts->read_uncommitted || mvcc_Enabled)
		    {
		      /* In MVCC, skip record since no need to lock keys */
		      /* clear 'prev_oid_pos' and 'prev_ovfl_vpid' */
		      bts->prev_oid_pos = 0;
		      VPID_SET_NULL (&(bts->prev_ovfl_vpid));

		      if (btree_find_next_index_record (thread_p, bts)
			  != NO_ERROR)
			{
			  return ER_FAILED;
			}

		      *which_action = BTREE_GETOID_AGAIN_WITH_CHECK;
		      return NO_ERROR;
		    }
		}
	      else
		{
		  btrs_helper->is_condition_satisfied = true;
		  bts->qualified_keys++;
		}
	    }
	}
    }

  return NO_ERROR;
}

/*
 * btree_handle_current_oid () - Handles one OID and advance rec_oid_ptr.
 *				 Used in the context of btree_range_search
 *				 function.
 *
 * return		: Error code.
 * thread_p (in)	: Thread entry.
 * bts (in)		: B-tree scan data.
 * btrs_helper (in)	: B-tree range search helper.
 * key_limit_lower (in) : NULL or lower key limit.
 * key_limit_upper (in) : NULL or upper key limit.
 * index_scan_id_p (in) : Index scan data.
 * need_count_only (in) : True if only need to count OID's.
 * inst_oid (in)	: Pointer to current OID (Note, the caller must read
 *			  OID from rec_oid_ptr before calling this function.
 * which_action (in)	: Action following this call of function.
 */
static int
btree_handle_current_oid (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
			  BTREE_RANGE_SEARCH_HELPER * btrs_helper,
			  DB_BIGINT * key_limit_lower,
			  DB_BIGINT * key_limit_upper,
			  INDX_SCAN_ID * index_scan_id_p,
			  bool need_count_only,
			  OID * inst_oid, MVCC_REC_HEADER * p_mvcc_header,
			  int *which_action)
{
  bool mro_continue;
  SCAN_CODE scan_code = S_SUCCESS;

  assert (bts != NULL);
  assert (btrs_helper != NULL);
  assert (which_action != NULL);

  *which_action = BTREE_CONTINUE;

  if (mvcc_Enabled == true && p_mvcc_header != NULL)
    {
      MVCC_SNAPSHOT *mvcc_snapshot =
	index_scan_id_p->scan_cache.mvcc_snapshot;

      if (mvcc_snapshot != NULL && mvcc_snapshot->snapshot_fnc != NULL)
	{
	  if (mvcc_snapshot->snapshot_fnc (thread_p, p_mvcc_header,
					   mvcc_snapshot) != true)
	    {
	      scan_code = S_SNAPSHOT_NOT_SATISFIED;
	    }
	}
    }
  if (scan_code != S_SUCCESS && scan_code != S_SNAPSHOT_NOT_SATISFIED)
    {
      return ER_FAILED;
    }

  if (scan_code == S_SNAPSHOT_NOT_SATISFIED)
    {
      /* skip OID's */
      /* fall through */
    }
  else if (key_limit_lower
	   && (DB_BIGINT) btrs_helper->oids_cnt < *key_limit_lower)
    {
      /* do not copy OID, just update key_limit_lower */
      *key_limit_lower -= 1;
    }
  else if (key_limit_upper
	   && (DB_BIGINT) btrs_helper->oids_cnt >= *key_limit_upper)
    {
      /* skip OID's */
      /* fall through */
    }
  else
    {
      if (need_count_only == false)
	{
	  /* normal scan - copy OID */
	  if (btrs_helper->iss_get_first_result_only)
	    {
	      /* Index skip scan - scan is handled elsewhere */
	      if (btree_iss_set_key (bts, &index_scan_id_p->iss) != NO_ERROR)
		{
		  return ER_FAILED;
		}
	      btrs_helper->oids_cnt++;
	      *which_action = BTREE_GOTO_END_OF_SCAN;
	      return NO_ERROR;
	    }
	  else if (index_scan_id_p->multi_range_opt.use)
	    {
	      /* Multiple range optimization */
	      /* Add current key to TOP N sorted keys */
	      /* Pass current key and next pseudo OID's to handle lock release
	       * when a candidate is thrown out of TOP N structure.
	       */
	      if (btree_range_opt_check_add_index_key (thread_p, bts,
						       &index_scan_id_p->
						       multi_range_opt,
						       inst_oid,
						       &btrs_helper->
						       ck_pseudo_oid,
						       &btrs_helper->
						       nk_pseudo_oid,
						       &btrs_helper->
						       class_oid,
						       &mro_continue)
		  != NO_ERROR)
		{
		  return ER_FAILED;
		}
	      if (!mro_continue)
		{
		  /* Current item didn't fit in the TOP N keys, and the
		   * following items in current btree_range_search iteration
		   * will not be better. Go to end of scan.
		   */
		  btrs_helper->is_key_range_satisfied = false;
		  *which_action = BTREE_GOTO_END_OF_SCAN;
		  return NO_ERROR;
		}
	    }
	  else if (SCAN_IS_INDEX_COVERED (index_scan_id_p))
	    {
	      /* handle loose scan */
	      if (btrs_helper->restart_on_first)
		{
		  *which_action = BTREE_RESTART_SCAN;
		}

	      /* Covering Index */
	      if (btree_dump_curr_key (thread_p, bts, bts->key_filter,
				       inst_oid, index_scan_id_p) != NO_ERROR)
		{
		  return ER_FAILED;
		}
	    }
	  else
	    {
	      /* No special case: store OID's in OID buffer */
	      COPY_OID (btrs_helper->mem_oid_ptr, inst_oid);
	      btrs_helper->mem_oid_ptr++;
	    }
	}
      /* Increment OID's count */
      btrs_helper->oids_cnt++;
    }

  /* Advance rec_oid_ptr */
  btrs_helper->rec_oid_ptr =
    btree_leaf_advance_oidptr (bts, btrs_helper->rec_oid_ptr,
			       btrs_helper->offset, btrs_helper->node_type);

  return NO_ERROR;
}

/*
 * btree_range_search_handle_previous_locks () - Used in the context of
 *					 btree_range_search function.
 *					 It is called prior to obtaining
 *					 locks on OID, current key and next
 *					 key, when some locks were already
 *					 obtained. This can happen when
 *					 a lock fails.
 *
 * return		: Error code.
 * thread_p (in)	: Thread entry.
 * bts (in)		: B-tree scan data.
 * btrs_helper (in)	: B-tree range search helper.
 * key_limit_lower (in) : NULL or lower key limit.
 * key_limit_upper (in) : NULL or upper key limit.
 * index_scan_id_p (in) : Index scan data.
 * need_count_only (in) : True if only OID count is needed.
 * which_action (in)	: Action following this function call.
 */
static int
btree_range_search_handle_previous_locks (THREAD_ENTRY * thread_p,
					  BTREE_SCAN * bts,
					  BTREE_RANGE_SEARCH_HELPER *
					  btrs_helper,
					  DB_BIGINT * key_limit_lower,
					  DB_BIGINT * key_limit_upper,
					  INDX_SCAN_ID * index_scan_id_p,
					  bool need_count_only,
					  int *which_action)
{
  OID temp_oid;
  MVCC_REC_HEADER mvcc_header, *p_mvcc_header = NULL;

  if (mvcc_Enabled)
    {
      MVCC_SET_INSID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_DELID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_NEXT_VERSION (&mvcc_header, &oid_Null_oid);
      p_mvcc_header = &mvcc_header;
    }

  assert (bts != NULL && btrs_helper != NULL && which_action != NULL);
  assert (btrs_helper->saved_inst_oid.pageid != NULL_PAGEID
	  || (bts->prev_oid_pos == -1 && btrs_helper->curr_key_locked));

  if (btrs_helper->rec_oid_cnt > 1 && btrs_helper->next_key_locked)
    {
      /* remove next key lock since only current key lock is needed
       * when delete or update multi OID key
       */
      assert (!OID_ISNULL (OID_ISNULL (&btrs_helper->nk_pseudo_oid)
			   ? &btrs_helper->saved_nk_pseudo_oid
			   : &btrs_helper->nk_pseudo_oid));
      assert (!OID_ISNULL (&btrs_helper->saved_nk_class_oid));
      lock_unlock_object_donot_move_to_non2pl (thread_p,
					       OID_ISNULL (&btrs_helper->
							   nk_pseudo_oid) ?
					       &btrs_helper->
					       saved_nk_pseudo_oid :
					       &btrs_helper->nk_pseudo_oid,
					       &btrs_helper->
					       saved_nk_class_oid,
					       bts->key_lock_mode);
      OID_SET_NULL (&btrs_helper->saved_nk_pseudo_oid);
      OID_SET_NULL (&btrs_helper->saved_nk_class_oid);
      btrs_helper->next_key_locked = false;
    }

  /*
   * The instance level locking on current key had already
   * been performed. Now, check if the locking is valid.
   * get the current instance OID
   */
  btree_leaf_get_oid_from_oidptr (bts, btrs_helper->rec_oid_ptr,
				  btrs_helper->node_type,
				  &btrs_helper->inst_oid,
				  &btrs_helper->class_oid, p_mvcc_header);
  if (btrs_helper->curr_key_locked)
    {
      btree_make_pseudo_oid (btrs_helper->inst_oid.pageid,
			     btrs_helper->inst_oid.slotid,
			     btrs_helper->inst_oid.volid,
			     bts->btid_int.sys_btid, &temp_oid);
    }

  /* compare the OID with the unconditionally locked OID */
  if ((OID_EQ (&btrs_helper->saved_inst_oid, &btrs_helper->inst_oid)
       && bts->oid_pos > 0)
      || (btrs_helper->curr_key_locked
	  && OID_EQ (&btrs_helper->saved_ck_pseudo_oid, &temp_oid)))
    {
      /* clear saved OID information */
      btrs_helper->saved_inst_oid.pageid = NULL_PAGEID;
      OID_SET_NULL (&btrs_helper->saved_ck_pseudo_oid);

      /* unfix the previous leaf page if it is fixed */
      /* It is possible in a isolation of TRAN_SERIALIZABLE */
      if (bts->P_page != NULL)
	{
	  pgbuf_unfix_and_init (thread_p, bts->P_page);
	  VPID_SET_NULL (&(bts->P_vpid));
	}

      /*
       * In an isolation level of TRAN_SERIALIZABLE,
       * 'is_condition_satisfied' flag might be false.
       */
      if (btrs_helper->is_condition_satisfied == false)
	{
	  *which_action = BTREE_GOTO_LOCKING_DONE;
	  return NO_ERROR;
	}
      else
	{
	  if (btrs_helper->CLS_satisfied == true)
	    {
	      if (btree_handle_current_oid (thread_p, bts, btrs_helper,
					    key_limit_lower, key_limit_upper,
					    index_scan_id_p,
					    need_count_only,
					    &btrs_helper->inst_oid,
					    p_mvcc_header, which_action)
		  != NO_ERROR)
		{
		  return ER_FAILED;
		}
	      if (*which_action != BTREE_CONTINUE)
		{
		  /* which_action is handled in btree_range_search */
		  return NO_ERROR;
		}
	    }
	  else
	    {
	      btrs_helper->rec_oid_ptr =
		btree_leaf_advance_oidptr (bts, btrs_helper->rec_oid_ptr,
					   btrs_helper->offset,
					   btrs_helper->node_type);
	    }
	}
      /*
       * If the current index is an unique index,
       * check if the current index is consistent.
       */
      if (BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
	{
	  if (bts->oid_pos > 1)
	    {
	      er_log_debug (ARG_FILE_LINE,
			    "index inconsistency.(unique violation).\n");
	      return ER_FAILED;
	    }

	  /* bts->oid_pos == 1 */
	  if (btrs_helper->rec_oid_cnt == 1)
	    {
	      /* (rec_oid_cnt - bts->oid_pos) == 0 */
	      *which_action = BTREE_GOTO_LOCKING_DONE;
	    }
	  /*
	   * If rec_oid_cnt > 1, other OIDs are in uncommitted state.
	   * In this case, do locking to wait uncommitted transactions.
	   */
	}
      return NO_ERROR;
    }

  /* !(OID_EQ (&btrs_helper->saved_inst_oid, &btrs_helper->inst_oid)
   *   && bts->oid_pos > 0)
   *  || (btrs_helper->curr_key_locked
   *      && OID_EQ (&btrs_helper->saved_ck_pseudo_oid, &temp_oid))
   */

#if !defined (SERVER_MODE)
  assert_release (false);
#endif

  if (!OID_EQ (&btrs_helper->saved_inst_oid, &btrs_helper->inst_oid))
    {

      if (!OID_ISNULL (&btrs_helper->saved_inst_oid))
	{
	  lock_unlock_object (thread_p, &btrs_helper->saved_inst_oid,
			      &btrs_helper->saved_class_oid,
			      bts->lock_mode, true);
	}

      /* release key locks if they have been previously locked */
      if (btrs_helper->next_key_locked)
	{
	  /* TO DO saved_clas_oid */
	  assert (!OID_ISNULL (OID_ISNULL (&btrs_helper->
					   nk_pseudo_oid)
			       ? &btrs_helper->saved_nk_pseudo_oid
			       : &btrs_helper->nk_pseudo_oid));
	  assert (!OID_ISNULL (&btrs_helper->saved_nk_class_oid));
	  lock_unlock_object_donot_move_to_non2pl (thread_p,
						   OID_ISNULL (&btrs_helper->
							       nk_pseudo_oid)
						   ? &btrs_helper->
						   saved_nk_pseudo_oid :
						   &btrs_helper->
						   nk_pseudo_oid,
						   &btrs_helper->
						   saved_nk_class_oid,
						   bts->key_lock_mode);
	}

      if (btrs_helper->curr_key_locked)
	{
	  assert (!OID_ISNULL (&btrs_helper->saved_ck_pseudo_oid));
	  assert (!OID_ISNULL (&btrs_helper->saved_class_oid));
	  lock_unlock_object_donot_move_to_non2pl (thread_p,
						   &btrs_helper->
						   saved_ck_pseudo_oid,
						   &btrs_helper->
						   saved_class_oid,
						   bts->key_lock_mode);
	}

      /*
       * Note the implementation of lock_unlock_object().
       * Even though certain class lock has been escalated,
       * the request for releasing instance lock of the class
       * must be processed correctly.
       */

      /* clear saved OID information */
      btrs_helper->saved_inst_oid.pageid = NULL_PAGEID;
      OID_SET_NULL (&btrs_helper->saved_nk_pseudo_oid);
      OID_SET_NULL (&btrs_helper->saved_ck_pseudo_oid);
      OID_SET_NULL (&btrs_helper->saved_nk_class_oid);
    }

  return NO_ERROR;
}

/*
 * btree_handle_current_oid_and_locks () - Used in the context of
 *					   btree_range_search, reads one OID
 *					   from OID set, it locks it then
 *					   it stores it. If this is the first
 *					   OID of a new key, locks on current
 *					   key and next key may be needed too.
 *
 * return		: Error code.
 * thread_p (in)	: Thread entry.
 * bts (in)		: B-tree scan data.
 * btrs_helper (in)	: B-tree range search helper.
 * key_limit_lower (in) : NULL or lower key limit.
 * key_limit_upper (in) : NULL or upper key limit.
 * index_scan_id_p (in) : Index scan data.
 * need_count_only (in) : True if only counting OID's is required.
 * num_classes (in)	: Number of class OID's in class_oids_ptr.
 * class_oids_ptr (in)	: List of class OID's to read from an unique index
 *			  on a hierarchy of classes.
 * scan_op_type (in)	: Scan operation type.
 * oid_index (in)	: Index of current OID in OID set.
 * which_action (in)	: Which action follows this function.
 *
 * NOTE: This function tries to obtain locks on OID, current key and next key
 *	 before handling current OID.
 *	 Current key and next key locks are required only if current OID is
 *	 the first OID of a new key.
 *	 Current key lock is not required if it was already obtained in a
 *	 previous step.
 *	 Next key lock is not required for S_SELECT scan operation.
 *	 No locks are required on lock escalation.
 *
 *	 When a lock fails, an unconditional lock is obtained. If no changes
 *	 have been made, search can continue, otherwise, search must go back
 *	 immediately after previous key.
 */
static int
btree_handle_current_oid_and_locks (THREAD_ENTRY * thread_p, BTREE_SCAN * bts,
				    BTREE_RANGE_SEARCH_HELPER * btrs_helper,
				    BTID * btid,
				    DB_BIGINT * key_limit_lower,
				    DB_BIGINT * key_limit_upper,
				    INDX_SCAN_ID * index_scan_id_p,
				    bool need_count_only,
				    int num_classes,
				    OID * class_oids_ptr,
				    int scan_op_type,
				    int oid_index, int *which_action)
{
  int j;
  int lock_ret;
  bool dummy_clear;
  MVCC_REC_HEADER mvcc_header, *p_mvcc_header = NULL;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;

  assert (bts != NULL);
  assert (btrs_helper != NULL);
  assert (which_action != NULL);
  assert (bts->read_uncommitted == false);

  *which_action = BTREE_CONTINUE;

  btrs_helper->current_lock_request = true;
  /* reset CLS_satisfied flag to true */
  btrs_helper->CLS_satisfied = true;

  if (mvcc_Enabled)
    {
      MVCC_SET_INSID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_DELID (&mvcc_header, MVCCID_NULL);
      MVCC_SET_NEXT_VERSION (&mvcc_header, &oid_Null_oid);
      p_mvcc_header = &mvcc_header;
      /* set snapshot used to check visibility before locking */
      mvcc_snapshot = index_scan_id_p->scan_cache.mvcc_snapshot;
    }

  /* get current class OID and instance OID */
  btree_leaf_get_oid_from_oidptr (bts, btrs_helper->rec_oid_ptr,
				  btrs_helper->node_type,
				  &btrs_helper->inst_oid,
				  &btrs_helper->class_oid, p_mvcc_header);

  /* checking phase */
  if (!BTREE_IS_UNIQUE (bts->btid_int.unique_pk))
    {
      assert (!BTREE_IS_PRIMARY_KEY (bts->btid_int.unique_pk));
      /*
       * Single class index
       * Current index is one of the following two indexes.
       * 1. non-unique index
       * 2. unique index that having single class index form
       * In current implementation,
       * only non-unique index can be in this situation.
       */

      COPY_OID (&btrs_helper->class_oid, &bts->cls_oid);
    }
  else
    {
      /*
       * Class hierarchy index
       * Current index is an unique index
       * that having class hierarchy index form.
       */

      assert (!OID_ISNULL (&btrs_helper->class_oid));

      /* check if the current class OID is query-based class */
      if (num_classes > 0 && btrs_helper->is_condition_satisfied == true)
	{
	  /*
	   * Current unique index is a class hierarchy index and
	   * current scan is based on some classes of the class hierarchy.
	   * In this case, some satisfying OIDs will be scanned.
	   */
	  for (j = 0; j < num_classes; j++)
	    {
	      if (OID_EQ (&btrs_helper->class_oid, &class_oids_ptr[j]))
		{
		  break;
		}
	    }
	  if (j >= num_classes)
	    {
	      btrs_helper->CLS_satisfied = false;
	    }
	}
    }				/* else */

  if (mvcc_Enabled == true)
    {
      if (btrs_helper->is_condition_satisfied == false
	  || btrs_helper->CLS_satisfied == false
	  || (mvcc_snapshot
	      && mvcc_snapshot->snapshot_fnc (thread_p, p_mvcc_header,
					      mvcc_snapshot) != true))
	{
	  /* skip the OID if condition or snapshot not satisfied */
	  btrs_helper->rec_oid_ptr =
	    btree_leaf_advance_oidptr (bts, btrs_helper->rec_oid_ptr,
				       btrs_helper->offset,
				       btrs_helper->node_type);

	  return NO_ERROR;
	}
    }

  /* 
   * instance level locking must be performed
   */

  btrs_helper->curr_key_locked = false;
  btrs_helper->next_key_locked = false;

  if (OID_EQ (&btrs_helper->saved_inst_oid, &btrs_helper->inst_oid))
    {
      assert (lock_get_object_lock (&btrs_helper->inst_oid,
				    &btrs_helper->class_oid,
				    LOG_FIND_THREAD_TRAN_INDEX (thread_p))
	      >= bts->lock_mode);
      lock_ret = LK_GRANTED;
      btrs_helper->saved_inst_oid.pageid = -1;
    }
  else if (btrs_helper->is_key_range_satisfied == false
	   || btrs_helper->current_lock_request == false)
    {
      /* skip OID locking */
      lock_ret = LK_GRANTED;
      if (btrs_helper->end_of_leaf_level == true)
	{
	  if (mvcc_Enabled == true)
	    {
	      /* in MVCC do not lock keys */
	      *which_action = BTREE_GOTO_END_OF_SCAN;
	      return NO_ERROR;
	    }

	  /* skip next key lock */
	  goto curr_key_locking;
	}
    }
  else
    {
      /*
       * CONDITIONAL lock request (current waitsecs : 0)
       */
      assert (!OID_ISNULL (&btrs_helper->inst_oid));
      assert (!OID_ISNULL (&btrs_helper->class_oid));
      lock_ret = lock_object_on_iscan (thread_p, &btrs_helper->inst_oid,
				       &btrs_helper->class_oid,
				       btid, bts->lock_mode, LK_COND_LOCK,
				       index_scan_id_p->
				       scan_cache.scanid_bit);
    }
  if (lock_ret == LK_NOTGRANTED_DUE_ABORTED)
    {
      return ER_FAILED;
    }
  else if (lock_ret == LK_GRANTED)
    {
      OID_SET_NULL (&btrs_helper->ck_pseudo_oid);
      OID_SET_NULL (&btrs_helper->nk_pseudo_oid);

      /* in MVCC, do not lock keys */
      if (mvcc_Enabled == false
	  && (bts->oid_pos + oid_index) == 0 && VPID_ISNULL (&(bts->O_vpid))
	  && !VPID_ISNULL (&(bts->C_vpid)))
	{
	  if (btrs_helper->is_condition_satisfied && scan_op_type != S_SELECT
	      && btrs_helper->rec_oid_cnt == 1)
	    {
	      /* next key lock acquired for DELETE, UPDATE */
	      if (btree_lock_next_key (thread_p, bts,
				       &btrs_helper->saved_inst_oid,
				       index_scan_id_p->scan_cache.scanid_bit,
				       &btrs_helper->prev_key,
				       &btrs_helper->saved_nk_pseudo_oid,
				       &btrs_helper->saved_nk_class_oid,
				       which_action) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      btrs_helper->next_key_locked = true;
	      if (*which_action != BTREE_CONTINUE)
		{
		  COPY_OID (&btrs_helper->saved_class_oid,
			    &btrs_helper->class_oid);
		  COPY_OID (&btrs_helper->saved_inst_oid,
			    &btrs_helper->inst_oid);

		  if (bts->prev_oid_pos != -1
		      && DB_IS_NULL (&btrs_helper->prev_key) == false)
		    {
		      /* do not reset previous key */
		      btrs_helper->read_prev_key = false;
		    }

		  /* let btree_range_search handle which_action */
		  return NO_ERROR;
		}

	      COPY_OID (&btrs_helper->nk_pseudo_oid,
			&btrs_helper->saved_nk_pseudo_oid);
	      OID_SET_NULL (&btrs_helper->saved_nk_pseudo_oid);
	    }

	  /* lock the current key if not previously locked
	   * current key lock requested for SELECT, UPDATE, DELETE
	   */
	  if ((btrs_helper->current_lock_request == true)
	      && ((bts->prev_KF_satisfied == false
		   || bts->prev_oid_pos != 0)))
	    {
	      /* current key lock needed */
	    curr_key_locking:

	      if (btree_lock_current_key (thread_p, bts,
					  &btrs_helper->saved_inst_oid,
					  &btrs_helper->inst_oid,
					  &btrs_helper->class_oid,
					  index_scan_id_p->
					  scan_cache.scanid_bit,
					  &btrs_helper->prev_key,
					  &btrs_helper->saved_ck_pseudo_oid,
					  which_action) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      btrs_helper->curr_key_locked = true;
	      if (*which_action != BTREE_CONTINUE)
		{
		  if (btrs_helper->is_key_range_satisfied)
		    {
		      COPY_OID (&btrs_helper->saved_inst_oid,
				&btrs_helper->inst_oid);
		    }
		  COPY_OID (&btrs_helper->saved_class_oid,
			    &btrs_helper->class_oid);
		  if (bts->prev_oid_pos != -1
		      && DB_IS_NULL (&btrs_helper->prev_key) == false)
		    {
		      btrs_helper->read_prev_key = false;
		    }

		  /* let btree_range_search handle which_action */
		  return NO_ERROR;
		}

	      COPY_OID (&btrs_helper->ck_pseudo_oid,
			&btrs_helper->saved_ck_pseudo_oid);
	      OID_SET_NULL (&btrs_helper->saved_ck_pseudo_oid);
	      if (btrs_helper->rec_oid_cnt > 1)
		{
		  index_scan_id_p->duplicate_key_locked = true;
		}
	    }
	}

      if (btrs_helper->is_condition_satisfied && btrs_helper->CLS_satisfied)
	{
	  /* Object visible and successfully conditional locked -> no need
	   * to check any other snapshot
	   */
	  if (btree_handle_current_oid (thread_p, bts, btrs_helper,
					key_limit_lower, key_limit_upper,
					index_scan_id_p, need_count_only,
					&btrs_helper->inst_oid, p_mvcc_header,
					which_action) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	  if (*which_action == BTREE_GOTO_END_OF_SCAN
	      || *which_action == BTREE_RESTART_SCAN)
	    {
	      return NO_ERROR;
	    }
	  assert (*which_action == BTREE_CONTINUE);
	}
      else
	{
	  btrs_helper->rec_oid_ptr =
	    btree_leaf_advance_oidptr (bts, btrs_helper->rec_oid_ptr,
				       btrs_helper->offset,
				       btrs_helper->node_type);
	}
      return NO_ERROR;
    }

  /* unfix all the index pages */
  if (bts->P_page != NULL)
    {
      LSA_COPY (&btrs_helper->prev_leaf_lsa, pgbuf_get_lsa (bts->P_page));
      pgbuf_unfix_and_init (thread_p, bts->P_page);
    }

  if (bts->C_page != NULL)
    {
      LSA_COPY (&bts->cur_leaf_lsa, pgbuf_get_lsa (bts->C_page));
      pgbuf_unfix_and_init (thread_p, bts->C_page);
    }

  if (bts->O_page != NULL)
    {
      LSA_COPY (&btrs_helper->ovfl_page_lsa, pgbuf_get_lsa (bts->O_page));
      pgbuf_unfix_and_init (thread_p, bts->O_page);
    }

  if (bts->use_desc_index)
    {
      /* permission not granted and descending index scan -> abort
       * to avoid a deadlock
       */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DESC_ISCAN_ABORTED, 3,
	      bts->btid_int.sys_btid->vfid.volid,
	      bts->btid_int.sys_btid->vfid.fileid,
	      bts->btid_int.sys_btid->root_pageid);
      return ER_FAILED;
    }

  /*
   * Following page ids are maintained.
   * bts->P_vpid, bts->C_vpid, bts->O_vpid
   */

  /* UNCONDITIONAL lock request */
  assert (!OID_ISNULL (&btrs_helper->inst_oid));
  assert (!OID_ISNULL (&btrs_helper->class_oid));
  lock_ret =
    lock_object_on_iscan (thread_p, &btrs_helper->inst_oid,
			  &btrs_helper->class_oid, btid, bts->lock_mode,
			  LK_UNCOND_LOCK,
			  index_scan_id_p->scan_cache.scanid_bit);
  if (lock_ret != LK_GRANTED)
    {
      /* LK_NOTGRANTED_DUE_ABORTED, LK_NOTGRANTED_DUE_TIMEOUT */
      return ER_FAILED;
    }

  if (!VPID_ISNULL (&(bts->P_vpid)))
    {
      /* The previous leaf page does exist. */
      if (btree_handle_prev_leaf_after_locking (thread_p, bts, oid_index,
						&btrs_helper->prev_leaf_lsa,
						&btrs_helper->prev_key,
						which_action) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }
  else
    {
      /* The previous leaf page does not exist. */
      if (btree_handle_curr_leaf_after_locking (thread_p, bts, oid_index,
						&btrs_helper->ovfl_page_lsa,
						&btrs_helper->prev_key,
						&btrs_helper->saved_inst_oid,
						which_action) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  /*
   * if max_value_equal is true
   * then compare previous key value and max_value of range
   * if two aren't equal, reset max_value_equal as false
   * in MVCC, no need to update max_value_equal, since we don't search
   * from previous key after unconditional locking
   */

  if (mvcc_Enabled == false
      && *which_action != BTREE_CONTINUE && bts->key_range_max_value_equal)
    {
      if (bts->prev_oid_pos == -1)
	{
	  bts->key_range_max_value_equal = false;
	}
      else if (bts->key_range.range == GT_LE
	       || bts->key_range.range == GE_LE
	       || bts->key_range.range == INF_LE)
	{
	  int c;
	  BTREE_KEYRANGE *range;
	  BTID_INT *btid_int;

	  range = &bts->key_range;
	  btid_int = &bts->btid_int;
	  c =
	    btree_compare_key (range->upper_key, &btrs_helper->prev_key,
			       btid_int->key_type, 1, 1, NULL);
	  if (c == DB_UNK)
	    {
	      /* error should have been set */
	      return ER_FAILED;
	    }

	  /* EQUALITY test only - doesn't care the reverse index */
	  if (c != 0)
	    {
	      bts->key_range_max_value_equal = false;
	    }
	}
    }

  if (*which_action == BTREE_CONTINUE)
    {
      OID_SET_NULL (&btrs_helper->ck_pseudo_oid);
      OID_SET_NULL (&btrs_helper->nk_pseudo_oid);

      /* in MVCC, do not lock keys */
      if (mvcc_Enabled == false
	  && (bts->oid_pos + oid_index) == 0 && VPID_ISNULL (&(bts->O_vpid))
	  && !VPID_ISNULL (&(bts->C_vpid)))
	{
	  if (btrs_helper->is_condition_satisfied && scan_op_type != S_SELECT
	      && btrs_helper->rec_oid_cnt == 1)
	    {
	      /* next key lock requested for DELETE, UPDATE */
	      if (btree_lock_next_key (thread_p, bts,
				       &btrs_helper->saved_inst_oid,
				       index_scan_id_p->scan_cache.scanid_bit,
				       &btrs_helper->prev_key,
				       &btrs_helper->saved_nk_pseudo_oid,
				       &btrs_helper->saved_nk_class_oid,
				       which_action) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      btrs_helper->next_key_locked = true;
	      if (*which_action != BTREE_CONTINUE)
		{
		  COPY_OID (&btrs_helper->saved_class_oid,
			    &btrs_helper->class_oid);
		  COPY_OID (&btrs_helper->saved_inst_oid,
			    &btrs_helper->inst_oid);

		  if (bts->prev_oid_pos != -1
		      && DB_IS_NULL (&btrs_helper->prev_key) == false)
		    {
		      /* do not reset previous key */
		      btrs_helper->read_prev_key = false;
		    }

		  /* let btree_range_search handle which_action */
		  return NO_ERROR;
		}

	      COPY_OID (&btrs_helper->nk_pseudo_oid,
			&btrs_helper->saved_nk_pseudo_oid);
	      OID_SET_NULL (&btrs_helper->saved_nk_pseudo_oid);
	    }

	  /* lock the current key if not previously locked
	   * current key lock request for SELECT, UPDATE, DELETE
	   */
	  if ((btrs_helper->current_lock_request == true)
	      && ((bts->prev_KF_satisfied == false
		   || bts->prev_oid_pos != 0)))
	    {
	      /* current key lock needed */
	      if (btree_lock_current_key (thread_p, bts,
					  &btrs_helper->saved_inst_oid,
					  &btrs_helper->inst_oid,
					  &btrs_helper->class_oid,
					  index_scan_id_p->
					  scan_cache.scanid_bit,
					  &btrs_helper->prev_key,
					  &btrs_helper->saved_ck_pseudo_oid,
					  which_action) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      btrs_helper->curr_key_locked = true;
	      if (*which_action != BTREE_CONTINUE)
		{
		  if (btrs_helper->is_key_range_satisfied)
		    {
		      COPY_OID (&btrs_helper->saved_class_oid,
				&btrs_helper->class_oid);
		      COPY_OID (&btrs_helper->saved_inst_oid,
				&btrs_helper->inst_oid);
		    }

		  if (bts->prev_oid_pos != -1
		      && DB_IS_NULL (&btrs_helper->prev_key) == false)
		    {
		      /* do not reset previous key */
		      btrs_helper->read_prev_key = false;
		    }

		  /* let btree_range_search handle which_action */
		  return NO_ERROR;
		}

	      COPY_OID (&btrs_helper->ck_pseudo_oid,
			&btrs_helper->saved_ck_pseudo_oid);
	      OID_SET_NULL (&btrs_helper->saved_ck_pseudo_oid);
	      if (btrs_helper->rec_oid_cnt > 1)
		{
		  index_scan_id_p->duplicate_key_locked = true;
		}
	    }
	}

      if (bts->O_page != NULL)
	{
	  if (spage_get_record (bts->O_page, 1, &btrs_helper->rec, PEEK)
	      != S_SUCCESS)
	    {
	      return ER_FAILED;
	    }
	  assert (btrs_helper->rec.length % 4 == 0);

	  btrs_helper->rec_oid_ptr =
	    btrs_helper->rec.data + (bts->oid_pos * btrs_helper->oid_size);
	  btrs_helper->offset = 0;
	  btrs_helper->node_type = BTREE_OVERFLOW_NODE;
	}
      else if (bts->C_page != NULL)
	{
	  assert (bts->slot_id > 0);
	  if (spage_get_record (bts->C_page, bts->slot_id, &btrs_helper->rec,
				PEEK) != S_SUCCESS)
	    {
	      return ER_FAILED;
	    }
	  assert (btrs_helper->rec.length % 4 == 0);

	  btree_read_record (thread_p, &bts->btid_int, bts->C_page,
			     &btrs_helper->rec, NULL,
			     (void *) &btrs_helper->leaf_pnt,
			     BTREE_LEAF_NODE, &dummy_clear,
			     &btrs_helper->offset, PEEK_KEY_VALUE, NULL);
	  if (bts->oid_pos == 0)
	    {
	      btrs_helper->rec_oid_ptr = btrs_helper->rec.data;
	    }
	  else
	    {
	      btrs_helper->rec_oid_ptr =
		(btrs_helper->rec.data + btrs_helper->offset
		 + ((bts->oid_pos - 1) * btrs_helper->oid_size));
	    }
	  btrs_helper->node_type = BTREE_LEAF_NODE;
	}

      btree_leaf_get_oid_from_oidptr (bts, btrs_helper->rec_oid_ptr,
				      btrs_helper->node_type,
				      &btrs_helper->inst_oid,
				      &btrs_helper->class_oid, p_mvcc_header);
      if (btrs_helper->is_condition_satisfied && btrs_helper->CLS_satisfied)
	{
	  /* need to check snapshot dirty */
	  if (btree_handle_current_oid (thread_p, bts, btrs_helper,
					key_limit_lower, key_limit_upper,
					index_scan_id_p, need_count_only,
					&btrs_helper->inst_oid, p_mvcc_header,
					which_action) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	  if (*which_action == BTREE_GOTO_END_OF_SCAN
	      || *which_action == BTREE_RESTART_SCAN)
	    {
	      return NO_ERROR;
	    }
	  assert (*which_action == BTREE_CONTINUE);
	}
      else
	{
	  btrs_helper->rec_oid_ptr =
	    btree_leaf_advance_oidptr (bts, btrs_helper->rec_oid_ptr,
				       btrs_helper->offset,
				       btrs_helper->node_type);
	}
      return NO_ERROR;
    }
  /*
   * other values of which_action :
   * BTREE_GETOID_AGAIN_WITH_CHECK
   * BTREE_SEARCH_AGAIN_WITH_CHECK
   */

  /*
   * The current key value had been saved in bts->cur_key.
   * The current class_oid and inst_oid will be saved
   * in saved_class_oid and saved_inst_oid, respectively.
   */
  COPY_OID (&btrs_helper->saved_class_oid, &btrs_helper->class_oid);
  COPY_OID (&btrs_helper->saved_inst_oid, &btrs_helper->inst_oid);

  if (bts->prev_oid_pos != -1 && DB_IS_NULL (&btrs_helper->prev_key) == false)
    {
      /* do not reset previous key */
      btrs_helper->read_prev_key = false;
    }

  return NO_ERROR;
}

/*
 * btree_prepare_range_search () - Prepares range search on first call or
 *				   when resumed.
 *
 * return	 : Error code.
 * thread_p (in) : Thread entry.
 * bts (in/out)	 : B-tree scan data.
 */
static int
btree_prepare_range_search (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  if (VPID_ISNULL (&(bts->C_vpid)))
    {
      /* initialize 'prev_oid_pos' and 'prev_ovfl_vpid' */
      bts->prev_oid_pos = -1;
      VPID_SET_NULL (&(bts->prev_ovfl_vpid));

      /* the first request */
      if (btree_prepare_first_search (thread_p, bts) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }
  else
    {
      /* not the first request */
      if (btree_prepare_next_search (thread_p, bts) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  return NO_ERROR;
}

static const char *
node_type_to_string (short node_type)
{
  return (node_type == BTREE_LEAF_NODE) ? "LEAF" : "NON_LEAF";
}

/*
 * btree_index_start_scan () -  start scan function for
 *                              show index header/capacity
 *   return: NO_ERROR, or ER_code
 *
 *   thread_p(in):
 *   show_type(in):
 *   arg_values(in):
 *   arg_cnt(in):
 *   ptr(in/out): index header/capacity context
 */
int
btree_index_start_scan (THREAD_ENTRY * thread_p, int show_type,
			DB_VALUE ** arg_values, int arg_cnt, void **ptr)
{
  int i, error = NO_ERROR;
  OID oid;
  OR_CLASSREP *classrep = NULL;
  int idx_in_cache;
  SHOW_INDEX_SCAN_CTX *ctx = NULL;
  LC_FIND_CLASSNAME status;
  OR_PARTITION *parts = NULL;
  int parts_count = 0;
  DB_CLASS_PARTITION_TYPE partition_type;
  char *class_name = NULL;

  *ptr = NULL;
  ctx =
    (SHOW_INDEX_SCAN_CTX *) db_private_alloc (thread_p,
					      sizeof (SHOW_INDEX_SCAN_CTX));
  if (ctx == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }
  memset (ctx, 0, sizeof (SHOW_INDEX_SCAN_CTX));

  ctx->show_type = show_type;
  ctx->is_all = (show_type == SHOWSTMT_ALL_INDEXES_HEADER
		 || show_type == SHOWSTMT_ALL_INDEXES_CAPACITY);

  class_name = db_get_string (arg_values[0]);

  status = xlocator_find_class_oid (thread_p, class_name, &oid, NULL_LOCK);
  if (status == LC_CLASSNAME_ERROR || status == LC_CLASSNAME_DELETED)
    {
      error = ER_LC_UNKNOWN_CLASSNAME;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 1, class_name);
      goto cleanup;
    }

  classrep =
    heap_classrepr_get (thread_p, &oid, NULL, 0, &idx_in_cache, true);
  if (classrep == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  if (ctx->is_all)
    {
      assert (arg_cnt == 2);

      partition_type = (DB_CLASS_PARTITION_TYPE) db_get_int (arg_values[1]);
      ctx->indexes_count = classrep->n_indexes;
    }
  else
    {
      assert (arg_cnt == 3);

      /* get index name which user specified */
      ctx->index_name =
	db_private_strdup (thread_p, db_get_string (arg_values[1]));
      if (ctx->index_name == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  error = er_errid ();
	  goto cleanup;
	}

      partition_type = (DB_CLASS_PARTITION_TYPE) db_get_int (arg_values[2]);
      ctx->indexes_count = 1;
    }

  /* save oids to context so that we can get btree info when scan next */
  if (partition_type == DB_PARTITIONED_CLASS)
    {
      error =
	heap_get_class_partitions (thread_p, &oid, &parts, &parts_count);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      ctx->class_oids =
	(OID *) db_private_alloc (thread_p, sizeof (OID) * parts_count);
      if (ctx->class_oids == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  error = er_errid ();
	  goto cleanup;
	}

      for (i = 0; i < parts_count; i++)
	{
	  COPY_OID (&ctx->class_oids[i], &parts[i].class_oid);
	}

      ctx->class_oid_count = parts_count;
    }
  else
    {
      ctx->class_oids = (OID *) db_private_alloc (thread_p, sizeof (OID));
      if (ctx->class_oids == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  error = er_errid ();
	  goto cleanup;
	}

      COPY_OID (&ctx->class_oids[0], &oid);
      ctx->class_oid_count = 1;
    }

  *ptr = ctx;
  ctx = NULL;

cleanup:

  if (classrep != NULL)
    {
      heap_classrepr_free (classrep, &idx_in_cache);
    }

  if (parts != NULL)
    {
      heap_clear_partition_info (thread_p, parts, parts_count);
    }

  if (ctx != NULL)
    {
      if (ctx->index_name != NULL)
	{
	  db_private_free_and_init (thread_p, ctx->index_name);
	}

      if (ctx->class_oids != NULL)
	{
	  db_private_free_and_init (thread_p, ctx->class_oids);
	}

      db_private_free_and_init (thread_p, ctx);
    }

  return error;
}

/*
 * btree_index_next_scan () -  next scan function for
 *                             show index header/capacity
 *   return: S_ERROR, S_SUCCESS, or S_END
 *
 *   thread_p(in):
 *   cursor(in):
 *   out_values(out):
 *   out_cnt(in):
 *   ptr(in): index header/capacity context
 */
SCAN_CODE
btree_index_next_scan (THREAD_ENTRY * thread_p, int cursor,
		       DB_VALUE ** out_values, int out_cnt, void *ptr)
{
  SCAN_CODE ret;
  BTID *btid_p;
  const char *btname;
  char *class_name = NULL;
  OR_CLASSREP *classrep = NULL;
  SHOW_INDEX_SCAN_CTX *ctx = NULL;
  OID *oid_p = NULL;
  int idx_in_cache;
  int selected_index = 0;
  int i, index_idx, oid_idx;

  ctx = (SHOW_INDEX_SCAN_CTX *) ptr;
  if (cursor >= ctx->indexes_count * ctx->class_oid_count)
    {
      return S_END;
    }

  assert (ctx->indexes_count >= 1);
  index_idx = cursor % ctx->indexes_count;
  oid_idx = cursor / ctx->indexes_count;

  oid_p = &ctx->class_oids[oid_idx];

  class_name = heap_get_class_name (thread_p, oid_p);
  if (class_name == NULL)
    {
      ret = S_ERROR;
      goto cleanup;
    }

  classrep =
    heap_classrepr_get (thread_p, oid_p, NULL, 0, &idx_in_cache, true);
  if (classrep == NULL)
    {
      ret = S_ERROR;
      goto cleanup;
    }

  if (ctx->is_all)
    {
      btname = classrep->indexes[index_idx].btname;
      btid_p = &classrep->indexes[index_idx].btid;
    }
  else
    {
      selected_index = -1;
      for (i = 0; i < classrep->n_indexes; i++)
	{
	  if (intl_identifier_casecmp
	      (classrep->indexes[i].btname, ctx->index_name) == 0)
	    {
	      selected_index = i;
	      break;
	    }
	}

      if (selected_index == -1)
	{
	  /* it must be found since passed semantic check */
	  assert (false);

	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OBJ_INDEX_NOT_FOUND,
		  0);
	  ret = S_ERROR;
	  goto cleanup;
	}

      btname = classrep->indexes[selected_index].btname;
      btid_p = &classrep->indexes[selected_index].btid;
    }

  if (ctx->show_type == SHOWSTMT_INDEX_HEADER
      || ctx->show_type == SHOWSTMT_ALL_INDEXES_HEADER)
    {
      ret =
	btree_scan_for_show_index_header (thread_p, out_values, out_cnt,
					  class_name, btname, btid_p);
    }
  else
    {
      assert (ctx->show_type == SHOWSTMT_INDEX_CAPACITY
	      || ctx->show_type == SHOWSTMT_ALL_INDEXES_CAPACITY);

      ret =
	btree_scan_for_show_index_capacity (thread_p, out_values, out_cnt,
					    class_name, btname, btid_p);
    }

cleanup:

  if (classrep != NULL)
    {
      heap_classrepr_free (classrep, &idx_in_cache);
    }

  if (class_name != NULL)
    {
      free_and_init (class_name);
    }

  return ret;
}

/*
 * btree_index_end_scan () -  end scan function
 *                            for show index header/capacity
 *   return: NO_ERROR, or ER_code
 *
 *   thread_p(in):
 *   ptr(in/out): index header/capacity context
 */
int
btree_index_end_scan (THREAD_ENTRY * thread_p, void **ptr)
{
  SHOW_INDEX_SCAN_CTX *ctx = NULL;

  ctx = (SHOW_INDEX_SCAN_CTX *) (*ptr);
  if (ctx != NULL)
    {
      if (ctx->index_name != NULL)
	{
	  db_private_free_and_init (thread_p, ctx->index_name);
	}

      if (ctx->class_oids != NULL)
	{
	  db_private_free_and_init (thread_p, ctx->class_oids);
	}

      db_private_free_and_init (thread_p, ctx);
    }

  *ptr = NULL;

  return NO_ERROR;
}

/*
 * btree_scan_for_show_index_header () - scan index header information
 *   return: S_ERROR, S_SUCCESS, or S_END
 *
 *   thread_p(in):
 *   out_values(out):
 *   out_cnt(in):
 *   class_name(in);
 *   index_name(in);
 *   btid_p(in);
 */
static SCAN_CODE
btree_scan_for_show_index_header (THREAD_ENTRY * thread_p,
				  DB_VALUE ** out_values, int out_cnt,
				  const char *class_name,
				  const char *index_name, BTID * btid_p)
{
  int idx = 0, root_level = 0;
  int error = NO_ERROR;
  VPID root_vpid;
  PAGE_PTR root_page_ptr = NULL;
  BTREE_ROOT_HEADER *root_header = NULL;
  BTREE_NODE_TYPE node_type;
  char buf[256] = { 0 };
  OR_BUF or_buf;
  TP_DOMAIN *key_type;

  /* get root header point */
  root_vpid.pageid = btid_p->root_pageid;
  root_vpid.volid = btid_p->vfid.volid;

  root_page_ptr =
    pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
	       PGBUF_UNCONDITIONAL_LATCH);
  if (root_page_ptr == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  root_header = btree_get_root_header (root_page_ptr);
  if (root_header == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  /* scan index header into out_values */
  error = db_make_string_copy (out_values[idx], class_name);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  error = db_make_string_copy (out_values[idx], index_name);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) btid_to_string (buf, sizeof (buf), btid_p);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) vpid_to_string (buf, sizeof (buf), &root_header->node.prev_vpid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) vpid_to_string (buf, sizeof (buf), &root_header->node.next_vpid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  root_level = root_header->node.node_level;
  node_type = (root_level > 1) ? BTREE_NON_LEAF_NODE : BTREE_LEAF_NODE;

  error = db_make_string (out_values[idx], node_type_to_string (node_type));
  idx++;

  db_make_int (out_values[idx], root_header->node.max_key_len);
  idx++;

  db_make_int (out_values[idx], root_header->num_oids);
  idx++;

  db_make_int (out_values[idx], root_header->num_nulls);
  idx++;

  db_make_int (out_values[idx], root_header->num_keys);
  idx++;

  buf[0] = '\0';
  if (!OID_ISNULL (&root_header->topclass_oid))
    {
      oid_to_string (buf, sizeof (buf), &root_header->topclass_oid);
    }
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  db_make_int (out_values[idx], root_header->unique_pk);
  idx++;

  (void) vfid_to_string (buf, sizeof (buf), &root_header->ovfid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  or_init (&or_buf, root_header->packed_key_domain, -1);
  key_type = or_get_domain (&or_buf, NULL, NULL);
  error =
    db_make_string_copy (out_values[idx],
			 pr_type_name (TP_DOMAIN_TYPE (key_type)));
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  assert (idx == out_cnt);

cleanup:

  if (root_page_ptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, root_page_ptr);
    }

  return (error == NO_ERROR) ? S_SUCCESS : S_ERROR;
}

/*
 * btree_insert_init_locks () - Initialize lock request for btree_insert.
 *
 * return :
 * THREAD_ENTRY * thread_p (in) :
 * bool is_active (in) :
 * OID * class_oid (in) :
 * BTID_INT * btid_int (in) :
 * int * class_lock (in) :
 * bool * next_key_lock_request (in) :
 * bool * mvcc_lock_request (in) :
 */
static int
btree_insert_init_locks (THREAD_ENTRY * thread_p, bool is_active,
			 OID * class_oid, BTID_INT * btid_int,
			 LOCK * class_lock, int *next_key_lock_request,
			 int *mvcc_lock_request)
{
  int tran_index;

  assert (next_key_lock_request != NULL && mvcc_lock_request != NULL
	  && class_lock != NULL && btid_int != NULL && class_oid != NULL);

  if (!is_active)
    {
      *next_key_lock_request = false;
      *mvcc_lock_request = false;
      *class_lock = NULL_LOCK;
      return NO_ERROR;
    }

  tran_index = LOG_FIND_THREAD_TRAN_INDEX (thread_p);
  *class_lock =
    lock_get_object_lock (class_oid, oid_Root_class_oid, tran_index);

  /* Is active */
  if (mvcc_Enabled)
    {
      *next_key_lock_request = false;
      *mvcc_lock_request = false;

      switch (*class_lock)
	{
	case SCH_M_LOCK:
	case X_LOCK:
	case SIX_LOCK:
	case IX_LOCK:
	  if (BTREE_IS_UNIQUE (btid_int->unique_pk))
	    {
	      *mvcc_lock_request = true;
	    }
	  break;
	case S_LOCK:
	case IS_LOCK:
	case NULL_LOCK:
	default:
	  assert (false);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}
    }
  else
    {
      *mvcc_lock_request = false;
      *next_key_lock_request = false;

      switch (*class_lock)
	{
	case SCH_M_LOCK:
	case X_LOCK:
	case SIX_LOCK:
	case IX_LOCK:
	  *next_key_lock_request = true;
	  break;
	case S_LOCK:
	case IS_LOCK:
	case NULL_LOCK:
	default:
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}

      assert (mvcc_Enabled || *next_key_lock_request == true);
      if (!BTREE_IS_UNIQUE (btid_int->unique_pk))
	{
	  assert (!BTREE_IS_PRIMARY_KEY (btid_int->unique_pk));
	  if (IS_WRITE_EXCLUSIVE_LOCK (*class_lock))
	    {
	      *next_key_lock_request = false;
	    }
	  else
	    {
	      *next_key_lock_request = true;
	    }
	}
    }

  return NO_ERROR;
}


/*
 * btree_key_find_first_visible_row () - MVCC find first visible row
 *   return: whether the visible row has been found
 *   btid(in): B+tree index identifier
 *   rec(in): Record descriptor
 *   offset(in): Offset of the second OID in key buffer
 *   node_type(in): node type
 *   oid(out): Object identifier of the visible row or NULL_OID
 *   class_oid(out): Object class identifier
 *   max_oids(in): max OIDs to search for
 */
static BTREE_SEARCH
btree_key_find_first_visible_row (THREAD_ENTRY * thread_p,
				  BTID_INT * btid_int, RECDES * rec,
				  int offset, BTREE_NODE_TYPE node_type,
				  OID * oid, OID * class_oid, int max_oids)
{
  PAGE_PTR pgptr = NULL, forward_pgptr = NULL;
  MVCC_REC_HEADER mvcc_rec_header;
  OR_BUF buf;
  int mvcc_flags = 0, length = 0;
  bool is_first = true;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;
  int oids_count = 0;

  assert (btid_int != NULL && rec != NULL && rec->data != NULL
	  && oid != NULL && class_oid != NULL);

  OID_SET_NULL (oid);
  OID_SET_NULL (class_oid);
  mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;

  length = rec->length;
  if (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      length -= DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
    }

  or_init (&buf, rec->data, length);
  while (buf.ptr < buf.endptr)
    {
      /* Get MVCC flags */
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buf.ptr);

      /* Read object OID */
      if (or_get_oid (&buf, oid) != NO_ERROR)
	{
	  goto error;
	}
      /* Clear flags */
      BTREE_CLEAR_MVCC_FLAGS_FROM_OID (oid);
      BTREE_CLEAR_RECORD_FLAGS_FROM_OID (oid);

      if (BTREE_IS_UNIQUE (btid_int->unique_pk))
	{
	  if (node_type == BTREE_OVERFLOW_NODE || !is_first
	      || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS))
	    {
	      /* Read class OID */
	      if (or_get_oid (&buf, class_oid) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      /* Class OID is top class OID */
	      COPY_OID (class_oid, &btid_int->topclass_oid);
	    }
	}

      /* Get MVCC information */
      if (btree_or_get_mvccinfo (&buf, &mvcc_rec_header, mvcc_flags)
	  != NO_ERROR)
	{
	  goto error;
	}

      if ((mvcc_snapshot_dirty).snapshot_fnc (thread_p, &mvcc_rec_header,
					      &mvcc_snapshot_dirty))
	{
	  /* visible row found it */
	  if (MVCCID_IS_VALID (mvcc_snapshot_dirty.lowest_active_mvccid)
	      || MVCCID_IS_VALID (mvcc_snapshot_dirty.
				  highest_completed_mvccid))
	    {
	      /* oid is modified by other active transaction */
	      return BTREE_ACTIVE_KEY_FOUND;
	    }
	  else
	    {
	      /* inserted by committed transaction */
	      return BTREE_KEY_FOUND;
	    }
	}

      if (max_oids > 0)
	{
	  oids_count++;
	  if (oids_count >= max_oids)
	    {
	      /* the maximum number of OIDs has been reached => key not found */
	      break;
	    }
	}

      if (node_type == BTREE_LEAF_NODE && is_first)
	{
	  /* Must skip over the key value to the next object */
	  or_seek (&buf, offset);
	}

      is_first = false;
    }

  return BTREE_KEY_NOTFOUND;

error:
  OID_SET_NULL (oid);
  OID_SET_NULL (class_oid);
  return BTREE_ERROR_OCCURRED;
}

/*
 * btree_mvcc_insert () - MVCC insert key in btree
 *   return: (the key to be inserted or NULL)
 *   btid(in): B+tree index identifier
 *   key(in): Key to be inserted
 *   cls_oid(in): Class object identifier
 *   oid(in): Object identifier to be inserted for the key
 *   op_type(in): operation types
 *                SINGLE_ROW_INSERT, SINGLE_ROW_UPDATE, SINGLE_ROW_MODIFY
 *                MULTI_ROW_INSERT, MULTI_ROW_UPDATE
 *   unique_stat_info(in): unique statistics info
 *   unique(in): true, is unique index
 *   p_mvcc_rec_header(in): mvcc rec header
 *
 */
DB_VALUE *
btree_perform_insert (THREAD_ENTRY * thread_p, BTID * btid, DB_VALUE * key,
		      OID * cls_oid, OID * oid, int op_type,
		      BTREE_UNIQUE_STATS * unique_stat_info,
		      int *unique, MVCC_REC_HEADER * p_mvcc_rec_header)
{
  return btree_insert (thread_p, btid, key, cls_oid, oid, op_type,
		       unique_stat_info, unique, p_mvcc_rec_header);
}

/*
 * xbtree_mvcc_find_unique () - MVCC find unique key in btree
 *   return: search result
 *   btid(in): B+tree index identifier
 *   scan_op_type(in): scan operation type
 *   key(in): key to find
 *   class_oid(in): class oid
 *   oid(out): found OID
 *   is_all_class_srch(in): the if search is based on all classes
 *			contained in the class hierarchy
 *
 * Note: This returns the oid for the given key.
 *	 This function must be called only in MVCC.
 */
static BTREE_SEARCH
xbtree_mvcc_find_unique (THREAD_ENTRY * thread_p, BTID * btid,
			 SCAN_OPERATION_TYPE scan_op_type, DB_VALUE * key,
			 OID * class_oid, OID * oid, bool is_all_class_srch)
{
  BTREE_SCAN btree_scan;
  int oid_cnt = 0;
  BTREE_SEARCH status;
  INDX_SCAN_ID index_scan_id;
  KEY_VAL_RANGE key_val_range;
  OID *oid_ptr = NULL;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;

  assert (mvcc_Enabled == true && btid != NULL && class_oid != NULL &&
	  oid != NULL);

  if (mvcc_Enabled)
    {
      mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;
    }

  BTREE_INIT_SCAN (&btree_scan);
  oid_cnt = 0;

  oid_ptr = (OID *) db_private_alloc (thread_p, ISCAN_OID_BUFFER_SIZE);
  if (oid_ptr == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, ISCAN_OID_BUFFER_SIZE);
      return ER_FAILED;
    }
  scan_init_index_scan (&index_scan_id, oid_ptr, &mvcc_snapshot_dirty);

  if (scan_op_type == S_SELECT && tf_is_catalog_class (class_oid))
    {
      index_scan_id.mvcc_need_locks = false;
    }
  else
    {
      index_scan_id.mvcc_need_locks = true;
    }

  if (key == NULL || db_value_is_null (key)
      || btree_multicol_key_is_null (key))
    {
      status = BTREE_KEY_NOTFOUND;
    }
  else
    {
      assert (!pr_is_set_type (DB_VALUE_DOMAIN_TYPE (key)));

      PR_SHARE_VALUE (key, &key_val_range.key1);
      PR_SHARE_VALUE (key, &key_val_range.key2);
      key_val_range.range = GE_LE;
      key_val_range.num_index_term = 0;

      status = BTREE_KEY_NOTFOUND;
      do
	{
	  oid_cnt =
	    btree_keyval_search (thread_p, btid, scan_op_type, &btree_scan,
				 &key_val_range, class_oid, oid_ptr,
				 ISCAN_OID_BUFFER_SIZE,
				 NULL, &index_scan_id, is_all_class_srch);
	  oid_ptr = index_scan_id.oid_list.oidp;
	  if (DB_VALUE_DOMAIN_TYPE (key) == DB_TYPE_MIDXKEY &&
	      key->data.midxkey.domain == NULL)
	    {
	      /* set the appropriate domain, as it might be needed for printing
	       * if the unique constraint is violated */
	      key->data.midxkey.domain = btree_scan.btid_int.key_type;
	    }

	  if (oid_cnt == -1)
	    {
	      status = BTREE_ERROR_OCCURRED;
	      break;
	    }
	  else if (oid_cnt == 1 && status == BTREE_KEY_NOTFOUND)
	    {
	      COPY_OID (oid, index_scan_id.oid_list.oidp);
	      status = BTREE_KEY_FOUND;
	    }
	  else if (oid_cnt >= 1)
	    {
	      status = BTREE_ERROR_OCCURRED;
	      break;
	    }
	}
      while (!BTREE_END_OF_SCAN (&btree_scan));
    }

  btree_scan_clear_key (&btree_scan);
  if (oid_ptr)
    {
      db_private_free_and_init (thread_p, oid_ptr);
    }

  return status;
}

/*
 * xbtree_find_unique () - find unique key in btree
 *   return:
 *   btid(in): B+tree index identifier
 *   scan_op_type(in): scan operation type
 *   key(in): key to find
 *   class_oid(in): class oid
 *   oid(out): found OID
 *   is_all_class_srch(in): the if search is based on all classes
 *			contained in the class hierarchy
 *
 * Note: This returns the oid for the given key.
 */
BTREE_SEARCH
xbtree_find_unique (THREAD_ENTRY * thread_p, BTID * btid,
		    SCAN_OPERATION_TYPE scan_op_type,
		    DB_VALUE * key, OID * class_oid, OID * oid,
		    bool is_all_class_srch)
{
  if (mvcc_Enabled)
    {
      return xbtree_mvcc_find_unique (thread_p, btid, scan_op_type, key,
				      class_oid, oid, is_all_class_srch);
    }
  else
    {
      return xbtree_non_mvcc_find_unique (thread_p, btid, scan_op_type, key,
					  class_oid, oid, is_all_class_srch);
    }
}

/*
 * btree_insert_mvcc_into_leaf_rec () -
 *   return:
 *   btid(in): B+tree index identifier
 *   page_ptr(in): Leaf page pointer to which the key is to be inserted
 *   node_type(in): Type of node
 *   key(in): Key to be inserted
 *   cls_oid(in):
 *   oid(in): Object identifier to be inserted together with the key
 *   p_mvcc_rec_header: MVCC tree info
 */
static int
btree_insert_mvcc_delid_into_page (THREAD_ENTRY * thread_p,
				   BTID_INT * btid, PAGE_PTR page_ptr,
				   BTREE_NODE_TYPE node_type,
				   DB_VALUE * key, OID * cls_oid,
				   OID * oid, INT16 slot_id, RECDES * rec,
				   int oid_offset,
				   MVCC_REC_HEADER * p_mvcc_rec_header)
{
#define BTREE_INSERT_DELID_REDO_CRUMBS_MAX 2
#define BTREE_INSERT_DELID_UNDO_CRUMBS_MAX 1
  int ret = NO_ERROR;
  int key_type = BTREE_NORMAL_KEY;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int oid_size = OR_OID_SIZE;
  int mvcc_delid_offset;
  bool have_mvcc_fixed_size = false;
  int rcv_offset_and_flags;
  LOG_CRUMB redo_crumbs[BTREE_INSERT_DELID_REDO_CRUMBS_MAX];
  int n_redo_crumbs = 0;
  LOG_CRUMB undo_crumbs[BTREE_INSERT_DELID_UNDO_CRUMBS_MAX];
  int n_undo_crumbs = 0;
  LOG_DATA_ADDR addr;
#if !defined (NDEBUG)
  /* We need to pack key domain to check record validity after recovery */
  int domain_size = or_packed_domain_size (btid->key_type, 0);
  char domain_buf[BTID_DOMAIN_BUFFER_SIZE], *domain_ptr = NULL;
#endif

  assert ((mvcc_Enabled == true) && (page_ptr != NULL)
	  && node_type != BTREE_NON_LEAF_NODE
	  && (p_mvcc_rec_header != NULL)
	  && (!(p_mvcc_rec_header->mvcc_flag & OR_MVCC_FLAG_VALID_INSID)));

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  /* Initialize redo recovery structure */
  rcv_offset_and_flags = oid_offset;

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      if (node_type == BTREE_OVERFLOW_NODE || oid_offset > 0
	  || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  /* unique index containing OID, CLASS OID */
	  oid_size += OR_OID_SIZE;
	}
      rcv_offset_and_flags |= BTREE_INSERT_DELID_RCV_FLAG_UNIQUE;
    }

  mvcc_delid_offset = oid_offset + oid_size;
  if ((BTREE_IS_UNIQUE (btid->unique_pk) && oid_offset > 0)
      || (node_type == BTREE_OVERFLOW_NODE)
      || (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	  && oid_offset == 0))
    {
      have_mvcc_fixed_size = true;
      mvcc_delid_offset += OR_MVCCID_SIZE;
      if (node_type == BTREE_OVERFLOW_NODE)
	{
	  rcv_offset_and_flags |= BTREE_INSERT_DELID_RCV_FLAG_OVERFLOW;
	}
    }
  else if (btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					      BTREE_LEAF_OID_HAS_MVCC_INSID))
    {
      assert (node_type == BTREE_LEAF_NODE);
      mvcc_delid_offset += OR_MVCCID_SIZE;
    }

  /* Create redo crumbs */
  /* Append redo recovery data structure */
  redo_crumbs[n_redo_crumbs].length = sizeof (rcv_offset_and_flags);
  redo_crumbs[n_redo_crumbs++].data = &rcv_offset_and_flags;

#if !defined (NDEBUG)
  /* Append key type domain */
  if (domain_size > BTID_DOMAIN_BUFFER_SIZE)
    {
      domain_ptr = (char *) db_private_alloc (thread_p, domain_size);
      if (domain_ptr == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, domain_size);
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  goto exit_on_error;
	}
    }
  else
    {
      domain_ptr = domain_buf;
    }
  /* Set key domain flag into offset */
  rcv_offset_and_flags |= BTREE_INSERT_DELID_RCV_FLAG_KEY_DOMAIN;
  /* Pack key domain and add to crumbs */
  (void) or_pack_domain (domain_ptr, btid->key_type, 0, 0);
  redo_crumbs[n_redo_crumbs].length = domain_size;
  redo_crumbs[n_redo_crumbs++].data = domain_ptr;
#endif

  /* Initialize log address data */
  addr.pgptr = page_ptr;
  addr.offset = slot_id;
  addr.vfid = &btid->sys_btid->vfid;

  if (btree_is_new_file (btid))
    {
      /* TODO: Fix me.
       * An MVCC delid is inserted, but undo log is not processed by vacuum.
       * Record is never cleaned. There should be one of the next fixes:
       * 1. If btree_is_new_file (btid) do not insert delete MVCCID. We have
       *    exclusive access on index and we can physically delete record, so
       *    vacuum won't be required.
       * 2. Add delete MVCCID but change logging so vacuum can detect it and
       *    remove it later.
       * The first solution may be better if applicable.
       */
      btree_rv_write_log_record (rv_data, &rv_data_len, rec, BTREE_LEAF_NODE);

      log_append_undo_data (thread_p, RVBT_NDRECORD_UPD, &addr,
			    rv_data_len, rv_data);
    }
  else
    {
      MVCC_BTREE_OP_ARGUMENTS mvcc_args;
      mvcc_args.purpose = MVCC_BTREE_INSERT_DELID;
      mvcc_args.delete_mvccid = MVCC_GET_DELID (p_mvcc_rec_header);
      ret = btree_rv_save_keyval (btid, key, cls_oid, oid,
				  &mvcc_args, &rv_key, &rv_key_len);
      if (ret != NO_ERROR)
	{
	  return ret;
	}

      /* Create undo crumbs for undoredo logging */
      undo_crumbs[n_undo_crumbs].length = rv_key_len;
      undo_crumbs[n_undo_crumbs++].data = rv_key;

      assert (n_undo_crumbs <= BTREE_INSERT_DELID_UNDO_CRUMBS_MAX);

      log_append_undoredo_crumbs (thread_p,
				  RVBT_KEYVAL_INS_LFRECORD_MVCC_DELID, &addr,
				  n_undo_crumbs, n_redo_crumbs,
				  undo_crumbs, redo_crumbs);
    }

  if (have_mvcc_fixed_size
      || btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					    BTREE_LEAF_OID_HAS_MVCC_DELID))
    {
#if !defined(NDEBUG)
      MVCCID old_mvcc_delid;
      int mvcc_insid_size = 0;

      /* check that old MVCC DELID is NULL */
      if (have_mvcc_fixed_size
	  || btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
						BTREE_LEAF_OID_HAS_MVCC_INSID))
	{
	  mvcc_insid_size = OR_BIGINT_SIZE;
	}
      OR_GET_MVCCID (rec->data + oid_offset + oid_size + mvcc_insid_size,
		     &old_mvcc_delid);
      assert (!MVCCID_IS_VALID (old_mvcc_delid));
#endif

      btree_set_mvcc_delid (rec, oid_offset, mvcc_delid_offset,
			    &(p_mvcc_rec_header->delid_chn.mvcc_del_id));

    }
  else
    {
      btree_add_mvcc_delid (rec, oid_offset, mvcc_delid_offset,
			    &(p_mvcc_rec_header->delid_chn.mvcc_del_id));
    }

  RANDOM_EXIT (thread_p);

  assert (rec->length % 4 == 0);

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, rec, node_type, key);
#endif

  if (spage_update (thread_p, page_ptr, slot_id, rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  if (btree_is_new_file (btid))
    {
      /* TODO: Fix me.
       * Read the comment at undo logging.
       */
      log_append_redo_crumbs (thread_p, RVBT_KEYVAL_INS_LFRECORD_MVCC_DELID,
			      &addr, n_redo_crumbs, redo_crumbs);
    }

  RANDOM_EXIT (thread_p);

  pgbuf_set_dirty (thread_p, page_ptr, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

#if !defined (NDEBUG)
  if (domain_ptr != NULL && domain_ptr != domain_buf)
    {
      db_private_free_and_init (thread_p, domain_ptr);
    }
#endif /* !NDEBUG */

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
*
* btree_delete_mvcc_delid_from_page () - delete MVCC delid from page
*   return: error code
*   btid(in): B+tree index identifier
*   leaf_page(in):  Leaf page pointer
*   slot_id(in): slot id
*   key(in):  B+tree key
*   oid(in):  key OID
*   class_oid(in): class OID
*   rec(in): the key record
*   del_oid_offset(in): OID offset
*/
static int
btree_delete_mvcc_delid_from_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				   PAGE_PTR page_ptr,
				   BTREE_NODE_TYPE node_type, INT16 slot_id,
				   DB_VALUE * key, OID * oid, OID * class_oid,
				   RECDES * rec, int del_oid_offset,
				   MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int ret = NO_ERROR;
  char *rv_data, *rv_key = NULL;
  int rv_data_len, rv_key_len, oid_size = OR_OID_SIZE;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  int mvcc_delid_offset;
  int mvcc_insid_size = 0;
  bool have_mvcc_fixed_size = false;

  rv_data = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  if (BTREE_IS_UNIQUE (btid->unique_pk))
    {
      if (node_type != BTREE_LEAF_NODE || del_oid_offset > 0
	  || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS))
	{
	  /* unique index containing OID, CLASS OID */
	  oid_size += OR_OID_SIZE;
	}
    }

  if ((BTREE_IS_UNIQUE (btid->unique_pk) && del_oid_offset > 0)
      || (node_type == BTREE_OVERFLOW_NODE)
      || (del_oid_offset == 0
	  && btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS)))
    {
      have_mvcc_fixed_size = true;
    }

  mvcc_delid_offset = del_oid_offset + oid_size;

  if (!btree_leaf_key_oid_is_mvcc_flaged (rec->data + del_oid_offset,
					  BTREE_LEAF_OID_HAS_MVCC_DELID))
    {
      return NO_ERROR;
    }

  if (have_mvcc_fixed_size
      || btree_leaf_key_oid_is_mvcc_flaged (rec->data + del_oid_offset,
					    BTREE_LEAF_OID_HAS_MVCC_INSID))
    {
      mvcc_delid_offset += OR_MVCCID_SIZE;
    }

  /* to do replace_old_delid */
  ret =
    btree_rv_save_keyval (btid, key, class_oid, oid, mvcc_args, &rv_key,
			  &rv_key_len);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (btree_is_new_file (btid))
    {
      (void) btree_rv_write_log_record (rv_data, &rv_data_len, rec,
					BTREE_LEAF_NODE);
      log_append_undo_data2 (thread_p, RVBT_NDRECORD_UPD,
			     &btid->sys_btid->vfid, page_ptr, slot_id,
			     rv_data_len, rv_data);
    }
  else
    {
      log_append_undo_data2 (thread_p, RVBT_KEYVAL_DEL_RECORD_MVCC_DELID,
			     &btid->sys_btid->vfid,
			     page_ptr, slot_id, rv_key_len, rv_key);
    }

  if (have_mvcc_fixed_size == true)
    {
      /* set null MVCCID, in order to leave enough MVCC free space for
       * first OID */
      MVCCID mvcc_id = MVCCID_NULL;
      btree_set_mvcc_delid (rec, del_oid_offset, mvcc_delid_offset, &mvcc_id);
    }
  else
    {
      /* delete MVCCID */
      btree_delete_mvcc_delid (rec, del_oid_offset, mvcc_delid_offset);
    }

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, rec, node_type, key);
#endif

  if (spage_update (thread_p, page_ptr, slot_id, rec) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  (void) btree_rv_write_log_record (rv_data, &rv_data_len, rec,
				    BTREE_LEAF_NODE);
  /* check whether is correct for not new file */
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD, &btid->sys_btid->vfid,
			 page_ptr, slot_id, rv_data_len, rv_data);

  pgbuf_set_dirty (thread_p, page_ptr, DONT_FREE);

end:

  if (rv_key != NULL)
    {
      db_private_free_and_init (thread_p, rv_key);
    }

  return ret;

exit_on_error:

  ret = (ret == NO_ERROR
	 && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;

  goto end;
}

/*
 * btree_delete_mvcc_insid_from_page () - Delete MVCC insert ID from b-tree
 *					  entry. This is called during the
 *					  the vacuum process (there it only
 *					  need redo logging).
 *
 * return	   : Error code.
 * thread_p (in)   : Thread entry.
 * btid (in)	   : B+tree identifier.
 * page_ptr (in)   : B+tree leaf page where an entry to be vacuumed is found.
 * slot_id (in)	   : Slot identifier for index key.
 * key (in)	   : Key value.
 * oid (in)	   : Object identifier.
 * class_oid (in)  : Class identifier.
 * rec (in)	   : Pointer to the key's record descriptor.
 * oid_offset (in) : Offset to the OID entry that is being vacuumed.
 */
static int
btree_delete_mvcc_insid_from_page (THREAD_ENTRY * thread_p, BTID_INT * btid,
				   PAGE_PTR page_ptr,
				   BTREE_NODE_TYPE node_type, INT16 slot_id,
				   DB_VALUE * key, OID * oid, OID * class_oid,
				   RECDES * rec, int oid_offset,
				   MVCC_BTREE_OP_ARGUMENTS * mvcc_args)
{
  int oid_size = OR_OID_SIZE, insid_offset = 0, insid_size = OR_MVCCID_SIZE;
  int error_code = NO_ERROR;
  int rv_data_length = 0;
  char *rv_data_p = NULL, *rv_key = NULL;
  char rv_data_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  MVCCID all_visible_mvccid = MVCCID_ALL_VISIBLE;
  bool is_size_fixed = false;

  rv_data_p = PTR_ALIGN (rv_data_buf, BTREE_MAX_ALIGN);

  if (BTREE_IS_UNIQUE (btid->unique_pk)
      && (node_type == BTREE_OVERFLOW_NODE || oid_offset > 0
	  || btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_SUBCLASS)))
    {
      /* unique index containing OID, CLASS OID */
      oid_size += OR_OID_SIZE;
    }

  if (!btree_leaf_key_oid_is_mvcc_flaged (rec->data + oid_offset,
					  BTREE_LEAF_OID_HAS_MVCC_INSID))
    {
      /* Insert MVCCID was already removed. */
      return NO_ERROR;
    }

  /* The index entry has a fixed size (OIDs + MVCC_INSID + MVCC_DELID) if:
   * 1. the index is unique and the entry is not first.
   * 2. entry is in overflow page.
   * 3. the key has overflow OID's and current entry is first.
   * When the entry size has to be fixed, just replace old insert MVCCID with
   * MVCCID_ALL_VISIBLE.
   */
  is_size_fixed =
    (BTREE_IS_UNIQUE (btid->unique_pk) && oid_offset > 0)
    || (node_type == BTREE_OVERFLOW_NODE)
    || (btree_leaf_is_flaged (rec, BTREE_LEAF_RECORD_OVERFLOW_OIDS)
	&& oid_offset == 0);
  insid_offset = oid_offset + oid_size;

  if (is_size_fixed)
    {
      /* Just replace old insert MVCCID with */
      OR_PUT_MVCCID (rec->data + insid_offset, &all_visible_mvccid);
    }
  else
    {
      /* Remove MVCCID */
      btree_delete_mvcc_insid (rec, oid_offset, insid_offset);
    }

#if !defined (NDEBUG)
  btree_check_valid_record (thread_p, btid, rec, node_type, key);
#endif

  if (spage_update (thread_p, page_ptr, slot_id, rec) != SP_SUCCESS)
    {
      return ER_FAILED;
    }

  vacuum_er_log (VACUUM_ER_LOG_BTREE | VACUUM_ER_LOG_WORKER,
		 "VACUUM: thread(%d) remove insert MVCCID: ",
		 "btid(%d, %d %d), oid(%d, %d, %d), class_oid(%d, %d, %d), "
		 "mvccid=%lld", thread_get_current_entry_index (),
		 (int) btid->sys_btid->root_pageid,
		 (int) btid->sys_btid->vfid.volid,
		 (int) btid->sys_btid->vfid.fileid, (int) oid->volid,
		 (int) oid->pageid, (int) oid->slotid, (int) class_oid->volid,
		 (int) class_oid->pageid, (int) class_oid->slotid,
		 mvcc_args->insert_mvccid);

  /* TODO: find a better solution for logging */
  (void) btree_rv_write_log_record (rv_data_p, &rv_data_length, rec,
				    BTREE_LEAF_NODE);
  log_append_redo_data2 (thread_p, RVBT_NDRECORD_UPD,
			 &btid->sys_btid->vfid, page_ptr, slot_id,
			 rv_data_length, rv_data_p);

  pgbuf_set_dirty (thread_p, page_ptr, DONT_FREE);

  return NO_ERROR;
}

/*
*
* btree_set_mvcc_header_ids_for_update () - set ids of mvcc header for update
*   return: nothing
*   thread_p(in): thread entry
*   do_delete_only(in):	true, if need to set del_id only
*   do_insert_only(in): true, if need to set ins_id only
*   mvcc_id(in): mvcc id to set
*   mvcc_rec_header(in):  mvcc record header
*
* Note: do_delete_only and do_insert_only can't be both true
*/
void
btree_set_mvcc_header_ids_for_update (THREAD_ENTRY * thread_p,
				      bool do_delete_only,
				      bool do_insert_only,
				      MVCCID * mvcc_id,
				      MVCC_REC_HEADER * mvcc_rec_header)
{
  assert (mvcc_Enabled == true && mvcc_rec_header != NULL);

  assert (do_delete_only == false || do_insert_only == false);

  BTREE_INIT_MVCC_HEADER (&mvcc_rec_header[0]);
  if (do_delete_only == false && do_insert_only == false)
    {
      MVCC_SET_FLAG_BITS (&mvcc_rec_header[0], OR_MVCC_FLAG_VALID_DELID);
      MVCC_SET_DELID (&mvcc_rec_header[0], *mvcc_id);

      BTREE_INIT_MVCC_HEADER (&mvcc_rec_header[1]);
      MVCC_SET_FLAG_BITS (&mvcc_rec_header[1], OR_MVCC_FLAG_VALID_INSID);
      MVCC_SET_INSID (&mvcc_rec_header[1], *mvcc_id);

      return;
    }

  if (do_delete_only == true)
    {
      MVCC_SET_FLAG_BITS (&mvcc_rec_header[0], OR_MVCC_FLAG_VALID_DELID);
      MVCC_SET_DELID (&mvcc_rec_header[0], *mvcc_id);

      return;
    }

  /* insert only case */
  MVCC_SET_FLAG_BITS (&mvcc_rec_header[0], OR_MVCC_FLAG_VALID_INSID);
  MVCC_SET_INSID (&mvcc_rec_header[0], *mvcc_id);
}

/*
 * btree_unpack_mvccinfo () - Check b-tree MVCC flags and unpack any MVCC info
 *			      into MVCC header.
 *
 * return		 : Pointer after the packed MVCC info.
 * ptr (in)		 : Pointer to packed MVCC info.
 * p_mvcc_header (out)	 : MVCC Record header.
 * btree_mvcc_flags (in) : Flags that describe the packed MVCC info.
 */
char *
btree_unpack_mvccinfo (char *ptr, MVCC_REC_HEADER * p_mvcc_header,
		       short btree_mvcc_flags)
{
  assert (mvcc_Enabled && p_mvcc_header != NULL && ptr != NULL);

  BTREE_INIT_MVCC_HEADER (p_mvcc_header);

  if ((btree_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID) != 0)
    {
      /* Get insert MVCCID */
      ptr = or_unpack_mvccid (ptr, &MVCC_GET_INSID (p_mvcc_header));
      if (MVCCID_IS_VALID (MVCC_GET_INSID (p_mvcc_header)))
	{
	  /* Set valid insert MVCCID */
	  MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID);
	}
    }
  else
    {
      /* Insert MVCCID is all visible */
      MVCC_SET_INSID (p_mvcc_header, MVCCID_ALL_VISIBLE);
    }

  if ((btree_mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID) != 0)
    {
      /* Get delete MVCCID */
      ptr = or_unpack_mvccid (ptr, &MVCC_GET_DELID (p_mvcc_header));
      if (MVCCID_IS_VALID (MVCC_GET_DELID (p_mvcc_header)))
	{
	  /* Set valid delete MVCCID */
	  MVCC_SET_FLAG_BITS (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID);
	}
    }
  return ptr;
}

/*
 * btree_pack_mvccinfo () - Pack MVCC information into b-tree record.
 *
 * return	      : Pointer after the packed MVCC information.
 * ptr (in)	      : Pointer where MVCC information will be packed.
 * p_mvcc_header (in) : MVCC information (saved as a record header).
 */
char *
btree_pack_mvccinfo (char *ptr, MVCC_REC_HEADER * p_mvcc_header)
{
  if (p_mvcc_header == NULL)
    {
      /* No MVCC info to pack */
      return ptr;
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID))
    {
      ptr = or_pack_mvccid (ptr, MVCC_GET_INSID (p_mvcc_header));
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID))
    {
      ptr = or_pack_mvccid (ptr, MVCC_GET_DELID (p_mvcc_header));
    }
  return ptr;
}

/*
 * btree_packed_mvccinfo_size () - Packed MVCC info size.
 *
 * return	      : Packed MVCC info size.
 * p_mvcc_header (in) : MVCC info.
 */
int
btree_packed_mvccinfo_size (MVCC_REC_HEADER * p_mvcc_header)
{
  int size = 0;
  if (p_mvcc_header == NULL)
    {
      /* Nothing to pack */
      return size;
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID))
    {
      size += OR_MVCCID_SIZE;
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID))
    {
      size += OR_MVCCID_SIZE;
    }
  return size;
}

/*
 * btree_or_get_mvccinfo () - Check b-tree MVCC flags and unpack any MVCC info
 *			      into MVCC header.
 *
 * return		 : Error code.
 * buf (in/out)		 : OR Buffer.
 * p_mvcc_header (out)	 : MVCC Record header.
 * btree_mvcc_flags (in) : Flags that describe the packed MVCC info.
 */
static int
btree_or_get_mvccinfo (OR_BUF * buf, MVCC_REC_HEADER * p_mvcc_header,
		       short btree_mvcc_flags)
{
  int size = BTREE_GET_MVCC_SIZE_FROM_REC_HEADER_FLAG (btree_mvcc_flags);
  if (buf->ptr + size > buf->endptr)
    {
      /* Overflow error */
      return or_overflow (buf);
    }
  /* Unpack and update pointer */
  buf->ptr =
    btree_unpack_mvccinfo (buf->ptr, p_mvcc_header, btree_mvcc_flags);
  return NO_ERROR;
}

/*
 * btree_or_put_mvccinfo () - Set MVCC information into buffer (should be used
 *			      for b-tree records. Only insert/delete MVCCID's
 *			      will be set depending on MVCC flags.
 *
 * return	      : Error code. 
 * buf (in/out)	      : OR Buffer.
 * p_mvcc_header (in) : MVCC info (saved as record header).
 */
static int
btree_or_put_mvccinfo (OR_BUF * buf, MVCC_REC_HEADER * p_mvcc_header)
{
  int error_code = NO_ERROR;

  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID))
    {
      error_code = or_put_mvccid (buf, MVCC_GET_INSID (p_mvcc_header));
      if (error_code != NO_ERROR)
	{
	  return error_code;
	}
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID))
    {
      error_code = or_put_mvccid (buf, MVCC_GET_DELID (p_mvcc_header));
      if (error_code != NO_ERROR)
	{
	  return error_code;
	}
    }
  return error_code;
}

/*
 * btree_set_mvcc_flags_into_oid () - Set MVCC info flags in the volid field
 *				      of OID.
 *
 * return	      : Void.
 * p_mvcc_header (in) : MVCC info.
 * oid (in/out)	      : Object identifier.
 */
void
btree_set_mvcc_flags_into_oid (MVCC_REC_HEADER * p_mvcc_header, OID * oid)
{
  if (p_mvcc_header == NULL)
    {
      /* No flag to set */
      return;
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_INSID))
    {
      oid->volid |= BTREE_LEAF_OID_HAS_MVCC_INSID;
    }
  if (MVCC_IS_FLAG_SET (p_mvcc_header, OR_MVCC_FLAG_VALID_DELID))
    {
      oid->volid |= BTREE_LEAF_OID_HAS_MVCC_DELID;
    }
}

/*
 * btree_clear_mvcc_flags_from_oid () - 
 *
 * return	      : Void.
 * p_mvcc_header (in) : MVCC info.
 * oid (in/out)	      : Object identifier.
 */
void
btree_clear_mvcc_flags_from_oid (OID * oid)
{
  oid->volid &= ~BTREE_LEAF_OID_MVCC_MASK;
}

/*
 * btree_compare_btids () - B-tree identifier comparator.
 *
 * return	  : Positive value is the first identifier is bigger,
 *		    negative if the second identifier is bigger and 0 if the
 *		    identifiers are equal.
 * mem_btid1 (in) : Pointer to first btid value.
 * mem_btid2 (in) : Pointer to second btid value.
 */
int
btree_compare_btids (const void *mem_btid1, const void *mem_btid2)
{
  const BTID *btid1 = (const BTID *) mem_btid1;
  const BTID *btid2 = (const BTID *) mem_btid2;
  if (btid1 == btid2)
    {
      return 0;
    }

  if (btid1->root_pageid > btid2->root_pageid)
    {
      return 1;
    }
  else if (btid1->root_pageid < btid2->root_pageid)
    {
      return -1;
    }

  if (btid1->vfid.fileid > btid2->vfid.fileid)
    {
      return 1;
    }
  else if (btid1->vfid.fileid < btid2->vfid.fileid)
    {
      return -1;
    }

  if (btid1->vfid.volid > btid2->vfid.volid)
    {
      return 1;
    }
  else if (btid1->vfid.volid < btid2->vfid.volid)
    {
      return -1;
    }

  return 0;
}

/*
 * btree_check_valid_record () - Check that record data is valid.
 *
 * return		: Error code.
 * thread_p (in)	: Thread entry.
 * btid (in)		: B-tree data.
 * recp (in)		: Record descriptor.
 * node_type (in)	: Node type (overflow or leaf).
 * key (in)		: Expected key value (will be checked if not null,
 *			  and if node type is leaf and if key doesn't have
 *			  overflow pages).
 */
int
btree_check_valid_record (THREAD_ENTRY * thread_p, BTID_INT * btid,
			  RECDES * recp, BTREE_NODE_TYPE node_type,
			  DB_VALUE * key)
{
  OID oid, class_oid;
  MVCCID mvccid;
  int vpid_size = 0;
  OR_BUF buffer;
  short mvcc_flags;
  bool is_first_oid = true;
  bool has_fixed_size = false;
  bool has_overflow_pages = false;


  if (btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_OIDS))
    {
      has_overflow_pages = true;
      vpid_size = DB_ALIGN (DISK_VPID_SIZE, INT_ALIGNMENT);
    }

  or_init (&buffer, recp->data, recp->length - vpid_size);
  while (buffer.ptr < buffer.endptr)
    {
      /* Get mvcc flags */
      mvcc_flags = btree_leaf_key_oid_get_mvcc_flag (buffer.ptr);
      /* If MVCC is enabled, there are several cases when the object entry
       * must have fixed size, which means that insert/delete MVCCID must be
       * present:
       * 1. Overflow objects.
       * 2. First object if leaf record if there are overflow OID's.
       * 3. Any non-first object if index is unique.
       */
      has_fixed_size =
	mvcc_Enabled
	&& ((node_type == BTREE_OVERFLOW_NODE)
	    || (has_overflow_pages && is_first_oid)
	    || (BTREE_IS_UNIQUE (btid->unique_pk) && !is_first_oid));
      if (has_fixed_size)
	{
	  assert ((mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID)
		  && (mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID));
	}
      /* Get and check OID */
      if (or_get_oid (&buffer, &oid) != NO_ERROR)
	{
	  assert (false);
	  return ER_FAILED;
	}
      BTREE_CLEAR_MVCC_FLAGS_FROM_OID (&oid);
      BTREE_CLEAR_RECORD_FLAGS_FROM_OID (&oid);
      if (oid.pageid <= 0 || oid.slotid <= 0
	  || oid.slotid > ((short) (DB_PAGESIZE / sizeof (PGSLOTID)))
	  || oid.volid < 0)
	{
	  assert (false);
	  return ER_FAILED;
	}
      if (BTREE_IS_UNIQUE (btid->unique_pk)
	  && (node_type == BTREE_OVERFLOW_NODE || !is_first_oid
	      || btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_SUBCLASS)))
	{
	  /* Get and check class OID */
	  if (or_get_oid (&buffer, &class_oid) != NO_ERROR)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	  if (class_oid.pageid <= 0 || class_oid.slotid <= 0
	      || class_oid.slotid >
	      ((short) (DB_PAGESIZE / sizeof (PGSLOTID)))
	      || class_oid.volid < 0)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	}
      if (mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_INSID)
	{
	  if (!mvcc_Enabled)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	  /* Get and check insert MVCCID */
	  if (or_get_mvccid (&buffer, &mvccid) != NO_ERROR)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	  if (!MVCCID_IS_VALID (mvccid)
	      || !mvcc_id_precedes (mvccid, log_Gl.hdr.mvcc_next_id))
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	}
      if (mvcc_flags & BTREE_LEAF_OID_HAS_MVCC_DELID)
	{
	  if (!mvcc_Enabled)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	  /* Get and check delete MVCCID */
	  if (or_get_mvccid (&buffer, &mvccid) != NO_ERROR)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	  if (mvccid != MVCCID_NULL
	      && !mvcc_id_precedes (mvccid, log_Gl.hdr.mvcc_next_id))
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	}
      if (is_first_oid && (node_type == BTREE_LEAF_NODE))
	{
	  /* Key value is also saved */
	  if (!btree_leaf_is_flaged (recp, BTREE_LEAF_RECORD_OVERFLOW_KEY))
	    {
	      /* Get key value */
	      DB_VALUE rec_key_value;
	      TP_DOMAIN *key_domain = NULL;
	      PR_TYPE *pr_type = NULL;

	      DB_MAKE_NULL (&rec_key_value);
	      key_domain = btid->key_type;
	      pr_type = key_domain->type;
	      if ((*(pr_type->index_readval)) (&buffer, &rec_key_value,
					       key_domain, -1, true, NULL, 0)
		  != NO_ERROR)
		{
		  assert (false);
		  return ER_FAILED;
		}
	      if (key != NULL
		  && btree_compare_key (key, &rec_key_value, key_domain, 1, 1,
					NULL) != 0)
		{
		  /* Expected key is not the same with the key found in record
		   * data.
		   */
		  /* This is possible when key fence is used. Should disable
		   * this verification or should include the fence for compare
		   */
		  /* For now, do nothing */
		}
	      db_value_clear (&rec_key_value);
	    }
	  else
	    {
	      /* Skip overflow key vpid */
	      buffer.ptr += DISK_VPID_SIZE;
	    }
	  buffer.ptr = PTR_ALIGN (buffer.ptr, OR_INT_SIZE);
	}
      is_first_oid = false;
    }
  if (buffer.ptr != buffer.endptr)
    {
      assert (false);
      return ER_FAILED;
    }
  return NO_ERROR;
}

/*
 * btree_scan_for_show_index_capacity () - scan index capacity information
 *   return: S_ERROR, S_SUCCESS, or S_END
 *
 *   thread_p(in):
 *   out_values(out):
 *   out_cnt(in):
 *   class_name(in);
 *   index_name(in);
 *   btid_p(in);
 */
static SCAN_CODE
btree_scan_for_show_index_capacity (THREAD_ENTRY * thread_p,
				    DB_VALUE ** out_values, int out_cnt,
				    const char *class_name,
				    const char *index_name, BTID * btid_p)
{
  int idx = 0;
  int error = NO_ERROR;
  BTREE_CAPACITY cpc;
  PAGE_PTR root_page_ptr = NULL;
  VPID root_vpid;
  char buf[256] = { 0 };

  /* get btree capacity */
  root_vpid.pageid = btid_p->root_pageid;
  root_vpid.volid = btid_p->vfid.volid;
  root_page_ptr =
    pgbuf_fix (thread_p, &root_vpid, OLD_PAGE, PGBUF_LATCH_READ,
	       PGBUF_UNCONDITIONAL_LATCH);
  if (root_page_ptr == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  error = btree_index_capacity (thread_p, btid_p, &cpc);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  /* scan index capacity into out_values */
  error = db_make_string_copy (out_values[idx], class_name);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  error = db_make_string_copy (out_values[idx], index_name);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) btid_to_string (buf, sizeof (buf), btid_p);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  db_make_int (out_values[idx], cpc.dis_key_cnt);
  idx++;

  db_make_int (out_values[idx], cpc.tot_val_cnt);
  idx++;

  db_make_int (out_values[idx], cpc.avg_val_per_key);
  idx++;

  db_make_int (out_values[idx], cpc.leaf_pg_cnt);
  idx++;

  db_make_int (out_values[idx], cpc.nleaf_pg_cnt);
  idx++;

  db_make_int (out_values[idx], cpc.tot_pg_cnt);
  idx++;

  db_make_int (out_values[idx], cpc.height);
  idx++;

  db_make_int (out_values[idx], cpc.avg_key_len);
  idx++;

  db_make_int (out_values[idx], cpc.avg_rec_len);
  idx++;

  (void) util_byte_to_size_string (buf, 64, (UINT64) (cpc.tot_space));
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) util_byte_to_size_string (buf, 64, (UINT64) (cpc.tot_used_space));
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  (void) util_byte_to_size_string (buf, 64, (UINT64) (cpc.tot_free_space));
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  db_make_int (out_values[idx], cpc.avg_pg_key_cnt);
  idx++;

  (void) util_byte_to_size_string (buf, 64, (UINT64) (cpc.avg_pg_free_sp));
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  assert (idx == out_cnt);

cleanup:

  if (root_page_ptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, root_page_ptr);
    }

  return (error == NO_ERROR) ? S_SUCCESS : S_ERROR;
}

static bool
btree_leaf_lsa_eq (THREAD_ENTRY * thread_p, LOG_LSA * a, LOG_LSA * b)
{
  assert (a != NULL);
  assert (b != NULL);

#if !defined(SERVER_MODE)
  assert_release (LSA_EQ (a, b));
#endif

  return LSA_EQ (a, b) ? true : false;
}

/*
 * btree_key_find_first_visible_row_from_all_ovf () - MVCC find first visible
 *						    row in OID overflow pages
 *   return: whether the visible row has been found
 *   btid_int(in): B+tree index identifier
 *   first_ovfl_vpid(in): First overflow vpid 
 *   oid(out): Object identifier of the visible row or NULL_OID
 *   class_oid(out): Object class identifier
 */
static BTREE_SEARCH
btree_key_find_first_visible_row_from_all_ovf (THREAD_ENTRY * thread_p,
					       BTID_INT * btid_int,
					       VPID * first_ovfl_vpid,
					       OID * oid, OID * class_oid)
{
  RECDES ovfl_copy_rec;
  char ovfl_copy_rec_buf[IO_MAX_PAGE_SIZE + BTREE_MAX_ALIGN];
  VPID next_ovfl_vpid;
  PAGE_PTR ovfl_page = NULL;
  BTREE_SEARCH result = BTREE_KEY_NOTFOUND;

  assert (mvcc_Enabled == true && oid != NULL && class_oid != NULL);

  ovfl_copy_rec.area_size = DB_PAGESIZE;
  ovfl_copy_rec.data = PTR_ALIGN (ovfl_copy_rec_buf, BTREE_MAX_ALIGN);
  next_ovfl_vpid = *first_ovfl_vpid;

  /* find first visible OID into overflow page */
  while (!VPID_ISNULL (&next_ovfl_vpid))
    {
      ovfl_page = pgbuf_fix (thread_p, &next_ovfl_vpid, OLD_PAGE,
			     PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (ovfl_page == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, ovfl_page, PAGE_BTREE);

      if (spage_get_record (ovfl_page, 1, &ovfl_copy_rec, COPY) != S_SUCCESS)
	{
	  goto error;
	}
      assert (ovfl_copy_rec.length % 4 == 0);

      result = btree_key_find_first_visible_row (thread_p, btid_int,
						 &ovfl_copy_rec, 0,
						 BTREE_OVERFLOW_NODE,
						 oid, class_oid, -1);
      if (result == BTREE_ERROR_OCCURRED)
	{
	  goto error;
	}
      else if (result != BTREE_KEY_NOTFOUND)
	{
	  pgbuf_unfix_and_init (thread_p, ovfl_page);
	  return result;
	}

      btree_get_next_overflow_vpid (ovfl_page, &next_ovfl_vpid);
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return BTREE_KEY_NOTFOUND;

error:

  if (ovfl_page != NULL)
    {
      pgbuf_unfix_and_init (thread_p, ovfl_page);
    }

  return BTREE_ERROR_OCCURRED;
}
