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
 * broker_log_replay.c -
 */

#ident "$Id$"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if !defined(WINDOWS)
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
#include "getopt.h"
#endif
#include <assert.h>

#include "cas_common.h"
#include "cas_cci.h"
#include "broker_log_util.h"
#include "porting.h"
#include "error_code.h"

#define STAT_MAX_DIFF_TIME              (60000)	/* 60 * 1000 : 10 min */
#define SORT_BUF_MAX                    (4096)
#define SQL_INFO_TITLE_LEN              (50)

#define CCI_ERR_FILE_NAME               "replay.err"
#define PASS_SQL_FILE_NAME              "skip.sql"

#define free_and_init(ptr) \
        do { \
          if ((ptr)) { \
            free ((ptr)); \
            (ptr) = NULL; \
          } \
        } while (0)

#define REMOVE_WHITE_SPACE(ptr)                        \
        do {                                           \
          while ((ptr) && *(ptr) == ' ') {             \
              (ptr)++;                                 \
          }                                            \
         } while(0)

#define REMOVE_NEW_LINE(ptr)                           \
        do {                                           \
          while ((ptr) && *(ptr) == '\n') {            \
              (ptr)++;                                 \
          }                                            \
         } while(0)

typedef enum temp_read_result READ_RESULT;
enum temp_read_result
{
  READ_STOP = -1,
  READ_CONTINUE = 0,
  READ_SUCCESS = 1
};

typedef struct t_sql_info T_SQL_INFO;
struct t_sql_info
{
  char *sql;
  char *rewrite_sql;
  char *bind_str;
  double exec_time;
  double sql_log_time;
  double diff_time;
};

typedef struct t_summary_info T_SUMMARY_INFO;
struct t_summary_info
{
  int num_total_query;
  int num_exec_query;
  int num_skip_query;
  int num_err_query;
  int num_diff_time_query;
  double sum_diff_time;
  double max_diff_time;
  double min_diff_time;
};

typedef struct t_sql_result T_SQL_RESULT;
struct t_sql_result
{
  int diff_time;
  char *sql_info;
};

static int log_replay (FILE * infp, FILE * outfp);
static char *get_next_log_line (FILE * infp, T_STRING * linebuf_tstr,
				int *lineno);

static char *get_query_stmt_from_plan (int req);
static int log_prepare (FILE * cci_err, FILE * pass_sql, int con,
			char *sql_log, T_SQL_INFO * sql_info,
			T_SUMMARY_INFO * summary);
static int get_cci_type (char *p);
static int log_bind_value (int req, T_STRING * linebuf, char *sql_log,
			   char *output_result, int remain_bind_buf);
static int log_execute (int con_h, int req, char *sql_log,
			double *execute_time);

static void get_sql_time_info (char *sql_log, T_SQL_INFO * info);
static void update_diff_time_statistics (double diff_time);

static int print_temp_result (char *sql_log, T_SQL_INFO * info);
static void update_summary_info (T_SUMMARY_INFO * summary,
				 T_SQL_INFO * sql_info);
static void print_summary_info (T_SUMMARY_INFO * summary);
static void print_result (FILE * outfp, double max_diff_time,
			  double min_diff_time, int tmp_line_len_max);

static char *make_sql_info (char *info_buf, char *start_p, int diff_time, int buf_size);
static int result_sort_func (const void *arg1, const void *arg2);
static READ_RESULT get_temp_file_line (char *read_buf,
				       unsigned int read_buf_size,
				       int *diff_time, char **endp);
static int print_result_without_sort (FILE * outfp, int print_diff_time_lower,
				      int read_buf_max);
static int print_result_with_sort (FILE * outfp, int print_diff_time_lower,
				   int num_query, int read_buf_max);

static int get_args (int argc, char *argv[]);

static int open_file (char *infilename, char *outfilename, FILE ** infp,
		      FILE ** outfp);
static void close_file (FILE * infp, FILE * outfp);

static char *host = NULL;
static int broker_port = 0;
static char *dbname = NULL;
static char *dbuser = NULL;
static char *dbpasswd = NULL;

static char from_date[128] = "";
static char to_date[128] = "";
static char check_date_flag = 0;

static char rewrite_query_flag = 0;

static int break_time = 0;
static double print_result_diff_time_lower = 0;
static char set_diff_time_lower_flag = 0;

static FILE *br_tmpfp = NULL;

static unsigned int num_slower_queries[STAT_MAX_DIFF_TIME] = { 0 };
static unsigned int num_faster_queries[STAT_MAX_DIFF_TIME] = { 0 };

/*
 * log_replay() - log_replay main routine
 *   return: NO_ERROR or ER_FAILED
 *   infp(in): input file pointer
 *   outfp(in): output file pointer
 */
static int
log_replay (FILE * infp, FILE * outfp)
{
  char *linebuf;
  int lineno = 0;
  char *msg_p;
  T_STRING *linebuf_tstr = NULL;
  char date_str[DATE_STR_LEN + 1];
  char bind_str_buf[1024];
  int remain_bind_buf;
  char conn_url[1024];
  int con_h, req;
  int result = 0;
  int bind_str_offset = 0;
  int temp_line_len = 0;
  int temp_line_len_max = 0;
  FILE *cci_errfp = NULL;
  FILE *pass_sqlfp = NULL;
  T_SQL_INFO sql_info;
  T_SUMMARY_INFO summary;
  T_CCI_ERROR err_buf;

  memset (&summary, 0, sizeof (T_SUMMARY_INFO));

  snprintf (conn_url, sizeof (conn_url), "cci:cubrid:%s:%u:%s:::",
	    host, broker_port, dbname);
  con_h = cci_connect_with_url (conn_url, dbuser, dbpasswd);
  if (con_h < 0)
    {
      fprintf (stderr, "cci connect error. url [%s]\n", conn_url);
      return ER_FAILED;
    }

  result = cci_set_autocommit (con_h, CCI_AUTOCOMMIT_FALSE);
  if (result < 0)
    {
      fprintf (stderr, "cannot set autocommit mode");
      goto end;
    }

  cci_errfp = fopen (CCI_ERR_FILE_NAME, "w");
  if (cci_errfp == NULL)
    {
      fprintf (stderr, "fopen error[%s]\n", CCI_ERR_FILE_NAME);
      result = ER_FAILED;
      goto end;
    }

  pass_sqlfp = fopen (PASS_SQL_FILE_NAME, "w");
  if (pass_sqlfp == NULL)
    {
      fprintf (stderr, "fopen error[%s]\n", PASS_SQL_FILE_NAME);
      result = ER_FAILED;
      goto end;
    }

  linebuf_tstr = t_string_make (1024);
  if (linebuf_tstr == NULL)
    {
      fprintf (stderr, "malloc error\n");
      result = ER_FAILED;
      goto end;
    }

  while (1)
    {
      linebuf = get_next_log_line (infp, linebuf_tstr, &lineno);
      if (linebuf == NULL)
	{
	  break;
	}
      if (is_cas_log (linebuf) == 0)
	{
	  continue;
	}

      if (check_date_flag == 1)
	{
	  GET_CUR_DATE_STR (date_str, linebuf);
	  if (ut_check_log_valid_time (date_str, from_date, to_date) < 0)
	    {
	      continue;
	    }
	}

      msg_p = get_msg_start_ptr (linebuf);
      if (strncmp (msg_p, "execute", 7) != 0)
	{
	  continue;
	}

      memset (&sql_info, '\0', sizeof (T_SQL_INFO));

      req =
	log_prepare (cci_errfp, pass_sqlfp, con_h, msg_p, &sql_info,
		     &summary);
      if (req < 0)
	{
	  free_and_init (sql_info.sql);
	  free_and_init (sql_info.rewrite_sql);
	  continue;
	}

      while (1)
	{
	  linebuf = get_next_log_line (infp, linebuf_tstr, &lineno);
	  if (linebuf == NULL)
	    {
	      break;
	    }
	  if (is_cas_log (linebuf) == 0)
	    {
	      continue;
	    }

	  msg_p = get_msg_start_ptr (linebuf);
	  if (strncmp (msg_p, "bind ", 5) == 0)
	    {
	      if (sql_info.bind_str == NULL)
		{
		  memset (bind_str_buf, '\0', sizeof (bind_str_buf));
		  sql_info.bind_str = bind_str_buf;;
		  remain_bind_buf = sizeof (bind_str_buf);
		}

	      bind_str_offset =
		log_bind_value (req, linebuf_tstr, msg_p, sql_info.bind_str,
				remain_bind_buf);
	      if (bind_str_offset < 0)
		{
		  fprintf (stderr, "log bind error [line:%d]\n", lineno);
		  break;
		}
	      sql_info.bind_str += bind_str_offset;
	      remain_bind_buf -= bind_str_offset;
	    }
	  else if (strncmp (msg_p, "execute", 7) == 0)
	    {
	      result = log_execute (con_h, req, msg_p, &sql_info.exec_time);
              if (result < 0)
               {
                 fprintf (cci_errfp, "cci execute error\n");
                 if (sql_info.rewrite_sql)
                   {
                     fprintf (cci_errfp, "rewrite sql[%s]\n", sql_info.rewrite_sql);
                   }
                 fprintf (cci_errfp, "sql[%s]\n", sql_info.sql);
                 if (sql_info.bind_str)
                   {
                     fprintf (cci_errfp, "bind[%s]\n", bind_str_buf);
                   }
                 summary.num_err_query++;
                 cci_close_req_handle (req);
                 break;
               }
	      cci_close_req_handle (req);

	      summary.num_exec_query++;

	      msg_p = strstr (msg_p, "time");
	      if (msg_p == NULL)
		{
		  /* unexpected sql log. pass this sql to write result */
		  break;
		}

	      if (sql_info.bind_str != NULL)
		{
		  /* restore bind_str with first address of bind string buffer */
		  sql_info.bind_str = bind_str_buf;;
		}

	      get_sql_time_info (msg_p + 5, &sql_info);

	      update_summary_info (&summary, &sql_info);

	      if (set_diff_time_lower_flag == 1
		  && sql_info.diff_time < print_result_diff_time_lower)
		{
		  break;
		}

	      update_diff_time_statistics (sql_info.diff_time);

	      /* we write result in temp file to order after all sql_log execute */
	      temp_line_len = print_temp_result (msg_p, &sql_info);

	      temp_line_len_max = MAX (temp_line_len_max, temp_line_len);

	      break;
	    }
	  else
	    {
	      break;
	    }
	}

      free_and_init (sql_info.sql);
      free_and_init (sql_info.rewrite_sql);
    }

  fflush (br_tmpfp);

  print_summary_info (&summary);

  print_result (outfp, summary.max_diff_time, summary.min_diff_time,
		temp_line_len_max);

end:
  cci_disconnect (con_h, &err_buf);

  if (cci_errfp)
    {
      fclose (cci_errfp);
    }

  if (pass_sqlfp)
    {
      fclose (pass_sqlfp);
    }

  if (linebuf_tstr)
    {
      free_and_init (linebuf_tstr->data);
    }
  free_and_init (linebuf_tstr);

  return result;
}

/*
 * get_next_log_line() -
 *   return: address of linebuf
 *   infp(in):
 *   linebuf_tstr(in):
 *   lineno(in/out):
 */
static char *
get_next_log_line (FILE * infp, T_STRING * linebuf_tstr, int *lineno)
{
  char *linebuf;

  assert (lineno != NULL);

  if (ut_get_line (infp, linebuf_tstr, NULL, NULL) < 0)
    {
      fprintf (stderr, "malloc error\n");
      return NULL;
    }
  if (t_string_len (linebuf_tstr) <= 0)
    {
      return NULL;
    }
  linebuf = t_string_str (linebuf_tstr);

  if (linebuf[strlen (linebuf) - 1] == '\n')
    {
      linebuf[strlen (linebuf) - 1] = '\0';
    }

  (*lineno)++;

  return linebuf;
}

/*
 * get_query_stmt_from_plan() -
 *   return: sql statment from query plan or NULL
 *   req(in):
 */
static char *
get_query_stmt_from_plan (int req)
{
  char *plan;
  char *sql_stmt = NULL, *p;
  char *result_sql;
  const char *query_header_str = "Query stmt:";

  if (cci_get_query_plan (req, &plan) < 0)
    {
      return NULL;
    }

  p = plan;

  while (1)
   {
     /* we find the last Query stmt: */

     p = strstr (p, query_header_str);
     if (p == NULL)
      {
        break;
      }
     sql_stmt = p;
     p += strlen (query_header_str);
   }

  if (sql_stmt == NULL)
    {
       cci_query_info_free (plan);
       return NULL;
    }

  sql_stmt += strlen (query_header_str);

  REMOVE_NEW_LINE (sql_stmt);

  p = strchr (sql_stmt, '\n');
  if (p)
    {
      *p = '\0';
    }
  else
    {
      cci_query_info_free (plan);
      return NULL;
    }

  result_sql = strdup (sql_stmt);
  if (result_sql == NULL)
    {
      fprintf (stderr, "malloc error\n");
    }

  cci_query_info_free (plan);

  return result_sql;
}

/*
 * log_prepare() -
 *   return: request handle id or ER_FAILED
 *   cci_errfp(in): cci error file pointer
 *   pass_sql(in): pass sql file pointer
 *   con(in): connection handle id
 *   sql_log(in):
 *   sql_info(out):
 *   summary(out):
 */
static int
log_prepare (FILE * cci_errfp, FILE * pass_sql, int con, char *sql_log,
	     T_SQL_INFO * sql_info, T_SUMMARY_INFO * summary)
{
  int req, exec_h_id;
  int prepare_flag, execute_flag;
  int result;
  char *endp;
  char *rewrite_query;
  T_CCI_ERROR err_buf;
  T_CCI_CUBRID_STMT cmd_type = -1;

  sql_log = ut_get_execute_type (sql_log, &prepare_flag, &execute_flag);
  if (sql_log == NULL)
    {
      return ER_FAILED;
    }

  if (strncmp (sql_log, "srv_h_id ", 9) != 0)
    {
      return ER_FAILED;
    }

  result = str_to_int32 (&exec_h_id, &endp, (sql_log + 9), 10);
  if (result != 0)
    {
      return ER_FAILED;
    }

  summary->num_total_query++;

  sql_log = endp + 1;

  req = cci_prepare (con, sql_log, prepare_flag, &err_buf);
  if (req < 0)
    {
      summary->num_err_query++;
      fprintf (cci_errfp, "cci prepare error [sql:%s]\n", sql_log);
      return ER_FAILED;
    }

  (void) cci_get_result_info (req, &cmd_type, NULL);

  if (rewrite_query_flag == 1
      && (cmd_type == CUBRID_STMT_UPDATE || cmd_type == CUBRID_STMT_DELETE))
    {
      rewrite_query = get_query_stmt_from_plan (req);
      if (rewrite_query == NULL)
	{
	  summary->num_skip_query++;
	  fprintf (pass_sql, "pass sql [%s]\n", sql_log);
	  cci_close_req_handle (req);
	  return ER_FAILED;
	}

      cci_close_req_handle (req);

      req = cci_prepare (con, rewrite_query, prepare_flag, &err_buf);
      if (req < 0)
	{
	  summary->num_err_query++;
	  free (rewrite_query);
	  fprintf (cci_errfp, "cci prepare error [sql:%s]\n", rewrite_query);
	  return ER_FAILED;
	}

      sql_info->rewrite_sql = rewrite_query;

      (void) cci_get_result_info (req, &cmd_type, NULL);
    }

  if (cmd_type != CUBRID_STMT_SELECT)
    {
      /* skip this sql stmt */
      cci_close_req_handle (req);
      return ER_FAILED;
    }

  sql_info->sql = strdup (sql_log);
  if (sql_info->sql == NULL)
    {
      fprintf (stderr, "malloc error\n");
      return ER_FAILED;
    }

  return req;
}

/*
 * get_cci_type() - get bind cci type from sql log
 *   return: CCI_U_TYPE
 *   p(in):
 */
static int
get_cci_type (char *p)
{
  int type;

  if (strcmp (p, "NULL") == 0)
    {
      type = CCI_U_TYPE_NULL;
    }
  else if (strcmp (p, "CHAR") == 0)
    {
      type = CCI_U_TYPE_CHAR;
    }
  else if (strcmp (p, "VARCHAR") == 0)
    {
      type = CCI_U_TYPE_STRING;
    }
  else if (strcmp (p, "NCHAR") == 0)
    {
      type = CCI_U_TYPE_NCHAR;
    }
  else if (strcmp (p, "VARNCHAR") == 0)
    {
      type = CCI_U_TYPE_VARNCHAR;
    }
  else if (strcmp (p, "BIT") == 0)
    {
      type = CCI_U_TYPE_BIT;
    }
  else if (strcmp (p, "VARBIT") == 0)
    {
      type = CCI_U_TYPE_VARBIT;
    }
  else if (strcmp (p, "NUMERIC") == 0)
    {
      type = CCI_U_TYPE_NUMERIC;
    }
  else if (strcmp (p, "BIGINT") == 0)
    {
      type = CCI_U_TYPE_BIGINT;
    }
  else if (strcmp (p, "INT") == 0)
    {
      type = CCI_U_TYPE_INT;
    }
  else if (strcmp (p, "SHORT") == 0)
    {
      type = CCI_U_TYPE_SHORT;
    }
  else if (strcmp (p, "MONETARY") == 0)
    {
      type = CCI_U_TYPE_MONETARY;
    }
  else if (strcmp (p, "FLOAT") == 0)
    {
      type = CCI_U_TYPE_FLOAT;
    }
  else if (strcmp (p, "DOUBLE") == 0)
    {
      type = CCI_U_TYPE_DOUBLE;
    }
  else if (strcmp (p, "DATE") == 0)
    {
      type = CCI_U_TYPE_DATE;
    }
  else if (strcmp (p, "TIME") == 0)
    {
      type = CCI_U_TYPE_TIME;
    }
  else if (strcmp (p, "TIMESTAMP") == 0)
    {
      type = CCI_U_TYPE_TIMESTAMP;
    }
  else if (strcmp (p, "DATETIME") == 0)
    {
      type = CCI_U_TYPE_DATETIME;
    }
  else if (strcmp (p, "OBJECT") == 0)
    {
      type = CCI_U_TYPE_OBJECT;
    }
  else if (strcmp (p, "BLOB") == 0)
    {
      type = CCI_U_TYPE_NULL;
    }
  else if (strcmp (p, "CLOB") == 0)
    {
      type = CCI_U_TYPE_NULL;
    }
  else if (strcmp (p, "ENUM") == 0)
    {
      type = CCI_U_TYPE_ENUM;
    }
  else
    {
      type = -1;
    }

  return type;
}

/*
 * log_bind_value() - 
 *   return: offset of bind string buffer or ER_FAILED
 *   req(in):
 *   linebuf(in):
 *   sql_log(in):
 *   output_result(out):
 *   remain_bind_buf(in):
 */
static int
log_bind_value (int req, T_STRING * linebuf, char *sql_log,
		char *output_result, int remain_bind_buf)
{
  char *p, *q, *r;
  char *value_p;
  char *endp;
  int type, res;
  int bind_idx;
  int bind_len;
  int result = 0;
  int offset = 0;

  assert (req > 0);

  sql_log += 5;

  result = str_to_int32 (&bind_idx, &endp, sql_log, 10);
  if (result < 0)
    {
      fprintf (stderr, "invalid bind index\n");
      return ER_FAILED;
    }

  p = strchr (sql_log, ':');
  if (p == NULL)
    {
      return ER_FAILED;
    }
  p += 2;
  q = strchr (p, ' ');
  if (q == NULL)
    {
      if (strcmp (p, "NULL") == 0)
	{
	  value_p = (char *) "";
	}
      else
	{
	  return ER_FAILED;
	}
    }
  else
    {
      bind_len = t_string_bind_len (linebuf);
      if (bind_len > 0)
	{
	  r = strchr (q, ')');
	  if (r == NULL)
	    {
	      return ER_FAILED;
	    }
	  *q = '\0';
	  *r = '\0';
	  value_p = r + 1;
	}
      else
	{
	  *q = '\0';
	  value_p = q + 1;
	}
    }

  type = get_cci_type (p);
  if (type == CCI_U_TYPE_NULL)
    {
      value_p = (char *) "";
    }
  else if (type == -1)
    {
      fprintf (stderr, "unknown cci type\n");
      return ER_FAILED;
    }

  if ((type == CCI_U_TYPE_VARBIT) || (type == CCI_U_TYPE_BIT))
    {
      T_CCI_BIT vptr;
      memset ((char *) &vptr, 0x00, sizeof (T_CCI_BIT));
      vptr.size = bind_len;
      vptr.buf = (char *) value_p;
      res =
	cci_bind_param (req, bind_idx,
			CCI_A_TYPE_BIT, (void *) &(vptr),
			(T_CCI_U_TYPE) type, CCI_BIND_PTR);
    }
  else
    {
      cci_bind_param (req, bind_idx, CCI_A_TYPE_STR, value_p, type, 0);
    }

  if (remain_bind_buf <= 0)
    {
      return NO_ERROR;
    }

  offset = snprintf (output_result, remain_bind_buf, "%s,", value_p);

  return offset;
}

/*
 * log_execute() - 
 *   return: NO_ERROR or ER_FAILED
 *   con_h(in):
 *   req(in):
 *   sql_log(in):
 *   execute_time(out):
 */
static int
log_execute (int con_h, int req, char *sql_log, double *execute_time)
{
  char *msg_p;
  int prepare_flag;
  int execute_flag;
  int res;
  struct timeval begin, end;
  T_CCI_ERROR cci_error;
  CCI_AUTOCOMMIT_MODE mode;

  assert (req > 0);

  *execute_time = 0;

  mode = cci_get_autocommit (con_h);
  if (mode == CCI_AUTOCOMMIT_TRUE)
    {
      res = cci_set_autocommit (con_h, CCI_AUTOCOMMIT_FALSE);
      if (res < 0)
	{
	  fprintf (stderr, "cannot set autocommit mode");
	  return ER_FAILED;
	}
    }

  msg_p = ut_get_execute_type (sql_log, &prepare_flag, &execute_flag);
  if (msg_p == NULL)
    {
      return ER_FAILED;
    }

  gettimeofday (&begin, NULL);

  if (break_time > 0)
    {
      SLEEP_MILISEC (0, break_time);
    }

  res = cci_execute (req, (char) execute_flag, 0, &cci_error);
  if (res < 0)
    {
      return res;
    }

  gettimeofday (&end, NULL);
  *execute_time = ut_diff_time (&begin, &end);

  res = cci_end_tran (con_h, CCI_TRAN_ROLLBACK, &cci_error);

  return res;
}

/*
 * get_sql_time_info() -
 *   return: void
 *   sql_info(in):
 *   info(in/out):
 */
static void
get_sql_time_info (char *sql_log, T_SQL_INFO * info)
{
  assert (info != NULL);

  sscanf (sql_log, "%lf", &info->sql_log_time);

  info->diff_time = info->exec_time - info->sql_log_time;

  return;
}

/*
 * update_diff_time_statistics() -
 *   return: void
 *   diff_time(in):
 */
static void
update_diff_time_statistics (double diff_time)
{
  /*
   * we save count of query witch have specific diff time
   * to use when order with diff time
   */
  int diff_in_msec;

  diff_in_msec = (int) (diff_time * 1000);

  if (diff_in_msec >= STAT_MAX_DIFF_TIME)
    {
      num_slower_queries[STAT_MAX_DIFF_TIME - 1]++;
    }
  else if (diff_in_msec >= 0)
    {
      num_slower_queries[diff_in_msec]++;
    }
  else if (diff_in_msec <= (-STAT_MAX_DIFF_TIME))
    {
      num_faster_queries[STAT_MAX_DIFF_TIME - 1]++;
    }
  else
    {
      assert (diff_in_msec < 0);
      num_faster_queries[(-diff_in_msec)]++;
    }

  return;
}

/*
 * print_temp_result() - save temp result until all sql are executed
 *   return: line length
 *   sql_log(in):
 *   info(in):
 */
static int
print_temp_result (char *sql_log, T_SQL_INFO * info)
{
  int bind_len;
  int line_len = 0;
  char *rewrite_sql = info->rewrite_sql;
  char *bind_str = info->bind_str;

  line_len += fprintf (br_tmpfp, "%d %d %d %s",
		       (int) (info->diff_time * 1000),
		       (int) (info->exec_time * 1000),
		       (int) (info->sql_log_time * 1000), info->sql);

  if (rewrite_sql != NULL)
    {
      line_len += fprintf (br_tmpfp, "REWRITE:%s", rewrite_sql);
    }
  if (bind_str != NULL)
    {
      bind_len = strlen (bind_str);
      if (bind_len > 0)
	{
	  bind_str[bind_len - 1] = '\0';	/* it is for removing last ',' */

	  line_len += fprintf (br_tmpfp, "BIND:%s", bind_str);
	}
    }

  line_len += fprintf (br_tmpfp, "\n");

  return line_len;
}

/*
 * update_summary_info() - 
 *   return: void
 *   summary(out):
 *   sql_info(out):
 */
static void
update_summary_info (T_SUMMARY_INFO * summary, T_SQL_INFO * sql_info)
{
  summary->sum_diff_time += sql_info->diff_time;

  if (sql_info->diff_time >= print_result_diff_time_lower)
    {
      summary->num_diff_time_query++;
    }

  summary->max_diff_time = MAX (summary->max_diff_time, sql_info->diff_time);
  /* min_diff_time is used for knowing lower bound of diff time  when order result */
  summary->min_diff_time = MIN (summary->min_diff_time, sql_info->diff_time);

  return;
}

/*
 * print_summary_info () -
 *   return: void
 *   summary(in):
 */
static void
print_summary_info (T_SUMMARY_INFO * summary)
{
  double avg_diff_time = 0;

  if (summary->num_exec_query != 0)
    {
      avg_diff_time = summary->sum_diff_time / summary->num_exec_query;
    }

  fprintf (stdout,
	   "------------------- Result Summary --------------------------\n");
  fprintf (stdout, "* %-35s : %d\n", "Total queries",
	   summary->num_total_query);
  fprintf (stdout, "* %-35s : %d\n", "Skiped queries (out : skip.sql)",
	   summary->num_skip_query);
  fprintf (stdout, "* %-35s : %d\n", "Error queries (out : replay.err)",
	   summary->num_err_query);
  fprintf (stdout, "* %-35s : %d\n", "Longer than difftime lower",
	   summary->num_diff_time_query);
  fprintf (stdout, "* %-35s : %.3f\n", "Max different time",
	   summary->max_diff_time);
  fprintf (stdout, "* %-35s : %.3f\n", "Average different time",
	   avg_diff_time);

  return;
}

/*
 * make_sql_info() - make output result from temp file
 *   return: memory address of result or NULL
 *   sql_info(in):
 *   start_p(in):
 *   diff_time(in):
 *   buf_size(in):
 */
static char *
make_sql_info (char *sql_info, char *start_p, int diff_time, int buf_size)
{
  char *endp, *p;
  char *sql = NULL, *rewrite_sql = NULL;
  char *bind_str = NULL;
  int res, offset = 0;
  int sql_log_time, exec_time;

  res = str_to_int32 (&exec_time, &endp, start_p, 10);

  p = endp + 1;
  res = str_to_int32 (&sql_log_time, &endp, p, 10);

  sql = endp + 1;
  p = strstr (sql, "REWRITE:");
  if (p)
    {
      *p = '\0';
      p += 8;
      rewrite_sql = p;
      endp = p;
    }

  p = strstr (endp, "BIND:");
  if (p)
    {
      *p = '\0';
      p += 5;
      bind_str = p;
    }

  offset = snprintf (sql_info, buf_size,
		     "EXEC TIME (REPLAY / SQL_LOG / DIFF) : %.3f / %.3f / %.3f\n"
		     "SQL : %s\n",
		     ((double) exec_time) / 1000,
		     ((double) sql_log_time) / 1000, ((double) diff_time) / 1000, sql);

  if (rewrite_sql && (buf_size - offset) > 0)
    {
      offset += snprintf (sql_info + offset, (buf_size - offset),
			  "REWRITE SQL : %s\n", rewrite_sql);
    }

  if (bind_str && (buf_size - offset) > 0)
    {
      offset += snprintf (sql_info + offset, (buf_size - offset),
			  "BIND : %s\n", bind_str);
    }

  return sql_info;
}

/*
 * result_sort_func() - compare sql with diff time
 *   return: difference of diff time of two sql
 *   arg1(in):
 *   arg2(in):
 */
static int
result_sort_func (const void *arg1, const void *arg2)
{
  int diff_time1;
  int diff_time2;

  diff_time1 = ((T_SQL_RESULT *) arg1)->diff_time;
  diff_time2 = ((T_SQL_RESULT *) arg2)->diff_time;

  return (diff_time2 - diff_time1);
}

/*
 * get_temp_file_lien() -
 *   return: READ_RESULT (SUCCESS / CONTINUE / STOP) 
 *   read_buf(in):
 *   read_buf_size(in):
 *   diff_time(out):
 *   endp(out):
 */
static READ_RESULT
get_temp_file_line (char *read_buf, unsigned int read_buf_size,
		    int *diff_time, char **endp)
{
  char *p;
  int res;

  p = fgets (read_buf, read_buf_size, br_tmpfp);
  if (p)
    {
      res = str_to_int32 (diff_time, endp, read_buf, 10);
      if (res != 0)
	{
	  return READ_CONTINUE;
	}

      p = strchr (read_buf, '\n');
      if (p)
	{
	  *(p) = '\0';

	  return READ_SUCCESS;
	}
      else
	{
	  /* continue reading temp file. buf this read_buf isn't written in output file */
	  return READ_CONTINUE;
	}
    }
  /* end reading temp file */
  return READ_STOP;
}

/*
 * print_result_without_sort() -
 *   return: NO_ERROR or ER_FAILED
 *   outfp(in):
 *   print_diff_time_lower(in): min diff time which will be print
 *   read_buf_max(in): size of read buffer
 */
static int
print_result_without_sort (FILE * outfp, int print_diff_time_lower,
			   int read_buf_max)
{
  int diff_time;
  int res = 0;
  char *endp;
  char *read_buf;
  FILE *next_tmp_fp;
  T_SQL_RESULT result;

  next_tmp_fp = tmpfile ();
  if (next_tmp_fp == NULL)
    {
      fprintf (stderr, "temp file open error\n");
      return ER_FAILED;
    }

  read_buf = (char *) malloc (read_buf_max);
  if (read_buf == NULL)
    {
      fprintf (stderr, "malloc error (%d)\n", read_buf_max);
      fclose (next_tmp_fp);
      return ER_FAILED;
    }

  lseek (fileno (br_tmpfp), 0, SEEK_SET);
  while (1)
    {
      res = get_temp_file_line (read_buf, read_buf_max, &diff_time, &endp);
      if (res == READ_STOP)
	{
	  break;
	}
      else if (res == READ_CONTINUE)
	{
	  continue;
	}

      if (diff_time < print_diff_time_lower)
	{
	  fprintf (next_tmp_fp, "%s\n", read_buf);
	  continue;
	}

      result.sql_info = (char *) malloc (read_buf_max + SQL_INFO_TITLE_LEN);
      if (result.sql_info == NULL)
	{
          fprintf (stderr, "malloc error (%d)\n", read_buf_max + SQL_INFO_TITLE_LEN);
          fclose (next_tmp_fp);
	  free_and_init (read_buf);
	  return ER_FAILED;
	}

      make_sql_info (result.sql_info, endp + 1, diff_time,
		     read_buf_max + SQL_INFO_TITLE_LEN);

      fprintf (outfp, "%s\n", result.sql_info);
      free_and_init (result.sql_info);
    }

  fclose (br_tmpfp);

  fflush (next_tmp_fp);

  /* save next temp file pointer in global variable */
  br_tmpfp = next_tmp_fp;

  free_and_init (read_buf);
  return NO_ERROR;
}

/*
 * print_result_with_sort() -
 *   return: NO_ERROR or ER_FAILED
 *   outfp(in):
 *   print_diff_time_lower(in):
 *   num_query(in):
 *   read_buf_max(in): size of read buffer
 */
static int
print_result_with_sort (FILE * outfp, int print_diff_time_lower,
			int num_query, int read_buf_max)
{
  int diff_time;
  int i = 0;
  int res = 0;
  char *endp;
  char *read_buf = NULL;
  FILE *next_tmp_fp;
  T_SQL_RESULT *result;

  if (num_query <= 0)
    {
      return NO_ERROR;
    }

  next_tmp_fp = tmpfile ();
  if (next_tmp_fp == NULL)
    {
      fprintf (stderr, "temp file open error\n");
      return ER_FAILED;
    }

  result = (T_SQL_RESULT *) malloc (sizeof (T_SQL_RESULT) * num_query);
  if (result == NULL)
    {
      fprintf (stderr, "malloc error (%ld)\n",
	       sizeof (T_SQL_RESULT) * num_query);
      fclose (next_tmp_fp);
      return ER_FAILED;
    }
  memset (result, '\0', sizeof (T_SQL_RESULT) * num_query);

  read_buf = (char *) malloc (read_buf_max);
  if (read_buf == NULL)
    {
      fprintf (stderr, "malloc error(%d)\n", read_buf_max);
      goto error;
    }

  lseek (fileno (br_tmpfp), 0, SEEK_SET);

  while (1)
    {
      res = get_temp_file_line (read_buf, read_buf_max, &diff_time, &endp);
      if (res == READ_STOP)
	{
	  break;
	}
      else if (res == READ_CONTINUE)
	{
	  continue;
	}

      if (diff_time >= print_diff_time_lower)
	{
	  result[i].sql_info =
	    (char *) malloc (read_buf_max + SQL_INFO_TITLE_LEN);
	  if (result[i].sql_info == NULL)
	    {
	      fprintf (stderr, "malloc error(%d)\n",
		       read_buf_max + SQL_INFO_TITLE_LEN);
	      goto error;
	    }

	  result[i].diff_time = diff_time;
	  make_sql_info (result[i].sql_info, endp + 1, diff_time,
			 read_buf_max + SQL_INFO_TITLE_LEN);
	  i++;
	}
      else
	{
	  /* 
	   * if sql's diff time is shorter than diff_time_lower, 
	   * write in next temp file. it will be read next time.
	   */
	  fprintf (next_tmp_fp, "%s\n", read_buf);
	}
    }

  num_query = i;
  /* sort result order by diff time */
  qsort (result, num_query, sizeof (T_SQL_RESULT), result_sort_func);

  for (i = 0; i < num_query; i++)
    {
      fprintf (outfp, "%s\n", result[i].sql_info);
      free_and_init (result[i].sql_info);
    }

  free_and_init (read_buf);

  free_and_init (result);

  fclose (br_tmpfp);

  fflush (outfp);
  fflush (next_tmp_fp);

  /* save next temp file pointer in global variable */
  br_tmpfp = next_tmp_fp;

  return NO_ERROR;

error:
  free_and_init (read_buf);

  free_and_init (result);

  num_query = i;
  for (i = 0; i < num_query; i++)
    {
      free_and_init (result[i].sql_info);
    }
  fclose (next_tmp_fp);

  return ER_FAILED;
}

/* 
 * print_result() -
 *   outfp(in):
 *   max_diff_time(in):
 *   min_diff_time(in):
 *   temp_line_len_max(in): line max of temp file result.
 */
static void
print_result (FILE * outfp, double max_diff_time, double min_diff_time,
	      int temp_line_len_max)
{
  int i, num_sort = 0;
  int max_diff_in_msec = (int) (max_diff_time * 1000) + 1;
  int min_diff_in_msec = (int) (min_diff_time * 1000) - 1;

  max_diff_in_msec = MIN (max_diff_in_msec, STAT_MAX_DIFF_TIME - 1);
  min_diff_in_msec = MAX (min_diff_in_msec, -(STAT_MAX_DIFF_TIME - 1));

  for (i = max_diff_in_msec; i >= 0; i--)
    {
      num_sort += num_slower_queries[i];
      if (num_sort > SORT_BUF_MAX)
	{
	  if (print_result_with_sort (outfp, i + 1,
				  num_sort - num_slower_queries[i],
				  temp_line_len_max) < 0)
            {
              return;
            }
	  /* 
	   * we don't sort last diff time query to avoid ordering size excess SORT_BUF_SIZE 
	   * in genaral, many sql have same diff time. so, this help decrease needless sorting 
	   */
	  if (print_result_without_sort (outfp, i, temp_line_len_max) < 0)
            {
              return;
            }

	  num_sort = 0;
	}
    }

  for (i = -1; i >= min_diff_in_msec; i--)
    {
      num_sort += num_faster_queries[(-i)];
      if (num_sort > SORT_BUF_MAX)
	{
	  if (print_result_with_sort (outfp, i + 1,
				  num_sort - num_faster_queries[(-i)],
				  temp_line_len_max) < 0)
            {
              return;
            }

	  if (print_result_without_sort (outfp, i, temp_line_len_max) < 0)
            {
              return;
            }

	  num_sort = 0;
	}
    }

  if (num_sort > 0)
    {
      print_result_with_sort (outfp, min_diff_in_msec, num_sort,
			      temp_line_len_max);
    }

  return;
}

/*
 * get_args() -
 *   return: option indicator or ER_FAILED
 *   argc(in):
 *   argv(in):
 */
static int
get_args (int argc, char *argv[])
{
  int c;

  while (1)
    {
      c = getopt (argc, argv, "rd:u:p:I:P:F:T:h:D:");
      if (c == EOF)
	{
	  break;
	}
      switch (c)
	{
	case 'I':
	  host = optarg;
	  break;
	case 'P':
	  broker_port = atoi (optarg);
	  break;
	case 'd':
	  dbname = optarg;
	  break;
	case 'u':
	  dbuser = optarg;
	  break;
	case 'p':
	  dbpasswd = optarg;
	  break;
	case 'r':
	  rewrite_query_flag = 1;
	  break;
	case 'h':
	  break_time = atoi (optarg);
	  break;
	case 'D':
	  print_result_diff_time_lower = atof (optarg);
	  set_diff_time_lower_flag = 1;
	  break;
	case 'F':
	  if (str_to_log_date_format (optarg, from_date) < 0)
	    {
	      goto date_format_err;
	    }
	  check_date_flag = 1;
	  break;
	case 'T':
	  if (str_to_log_date_format (optarg, to_date) < 0)
	    {
	      goto date_format_err;
	    }
	  check_date_flag = 1;
	  break;
	default:
	  goto usage;
	}
    }

  if (host == NULL)
    {
      host = (char *) "localhost";
    }

  if (dbuser == NULL)
    {
      dbuser = (char *) "PUBLIC";
    }

  if (dbpasswd == NULL)
    {
      dbpasswd = (char *) "";
    }

  if (optind + 1 >= argc)
    {
      goto usage;
    }

  return optind;

usage:
  fprintf (stderr,
	   "usage : %s infile outfile [OPTION] \n"
	   "\n"
	   "valid options:\n"
	   "  -I   broker host\n"
	   "  -P   broker port\n"
	   "  -d   database name\n"
	   "  -u   user name\n"
	   "  -p   user password\n"
	   "  -h   break time between query execute\n"
	   "  -r   enable to rewrite update/delete query to select\n"
	   "  -D   minimum value of time difference make print result\n"
	   "  -F   datetime when start to replay sql_log\n"
	   "  -T   datetime when end to replay sql_log\n", argv[0]);
  return ER_FAILED;
date_format_err:
  fprintf (stderr, "invalid date. valid date format is yy-mm-dd hh:mm:ss.\n");
  return ER_FAILED;
}

/*
 * open_file() -
 *   return: NO_ERROR or ER_FAILED
 *   infilename(in):
 *   outfilename(in):
 *   infp(out):
 *   outfp(out):
 */
static int
open_file (char *infilename, char *outfilename, FILE ** infp, FILE ** outfp)
{
  *infp = fopen (infilename, "r");
  if (*infp == NULL)
    {
      fprintf (stderr, "fopen error[%s]\n", infilename);
      return ER_FAILED;
    }

  if (outfilename == NULL)
    {
      *outfp = stdout;
    }
  else
    {
      *outfp = fopen (outfilename, "w");
      if (*outfp == NULL)
	{
	  fprintf (stderr, "fopen error[%s]\n", outfilename);
	  goto error;;
	}
    }

  br_tmpfp = tmpfile ();
  if (br_tmpfp == NULL)
    {
      fprintf (stderr, "temp file open error\n");
      goto error;
    }

  return NO_ERROR;

error:
  close_file (*infp, *outfp);
  return ER_FAILED;
}

/*
 * close_file() - 
 *   return: void
 *   infp(in):
 *   outfp(in):
 */
static void
close_file (FILE * infp, FILE * outfp)
{
  if (infp != NULL)
    {
      fclose (infp);
    }

  fflush (outfp);
  if (outfp != NULL && outfp != stdout)
    {
      fclose (outfp);
    }

  if (br_tmpfp != NULL)
    {
      fclose (br_tmpfp);
    }

  return;
}

int
main (int argc, char *argv[])
{
  int start_arg;
  char *infilename = NULL;
  char *outfilename = NULL;
  FILE *outfp, *infp;
  int res;

  if ((start_arg = get_args (argc, argv)) < 0)
    {
      return ER_FAILED;
    }

  infilename = argv[start_arg];
  if (start_arg + 1 <= argc)
    {
      outfilename = argv[start_arg + 1];
    }
  if (open_file (infilename, outfilename, &infp, &outfp) < 0)
    {
      return ER_FAILED;
    }

  res = log_replay (infp, outfp);

  close_file (infp, outfp);

  return res;
}