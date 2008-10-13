/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

import cubrid.sql.CUBRIDOID;

/**
* Statement�� execute�� ������� Result�鿡 ���õ� ������ �����ϴ� class�̴�.
* �� statement result���� �ϳ��� UResultInfo instance�� result info�� �����Ѵ�.
*
* since 2.0
*/

public class UResultInfo {

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

byte statementType;
private int resultCount;
private boolean isResultSet; /* if result is resultset, true. otherwise false */
private CUBRIDOID oid;
private long srv_cache_time;

/*=======================================================================
 |	CONSTRUCTOR
 =======================================================================*/

UResultInfo(byte type, int count)
{
  statementType = type;
  resultCount = count;
  if (statementType == CUBRIDCommandType.SQLX_CMD_SELECT ||
      statementType == CUBRIDCommandType.SQLX_CMD_CALL ||
      statementType == CUBRIDCommandType.SQLX_CMD_GET_STATS ||
      statementType == CUBRIDCommandType.SQLX_CMD_EVALUATE)
  {
    isResultSet = true;
  }
  else {
    isResultSet = false;
  }
  oid = null;
  srv_cache_time = 0L;
}

/*=======================================================================
 |	PUBLIC METHODS
 =======================================================================*/

/*
 *Statement�� execute�Ͽ� ������� result count�� return�Ѵ�.
 */

public int getResultCount()
{
  return resultCount;
}

/*
 * ����� Statement�� ResultSet�� result�� ���� query���̰ų� method call,
 * evaluate������ �ƴ����� �Ǵ��Ѵ�. ResultSet�� ��� true, �׷��� ���� ���
 * false�� return�Ѵ�.
 */

public boolean isResultSet()
{
  return isResultSet;
}

/*=======================================================================
 |	PACKAGE ACCESS METHODS
 =======================================================================*/

CUBRIDOID getCUBRIDOID()
{
  return oid;
}

void setResultOid(CUBRIDOID o)
{
  if (statementType == CUBRIDCommandType.SQLX_CMD_INSERT && resultCount == 1)
    oid = o;
}

void setSrvCacheTime(int sec, int usec)
{
  srv_cache_time = sec;
  srv_cache_time = (srv_cache_time << 32) | usec;
}

long getSrvCacheTime()
{
  return srv_cache_time;
}

}  // end of class UResultInfo
