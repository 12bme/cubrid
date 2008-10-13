/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

import cubrid.sql.CUBRIDOID;

/**
* query statement, getSchemaInfo, getByOid�� ������ �� ������� Result Set��
* manage�ϱ� ���� class�̴�. �� column���� Object array�� �����Ǹ� column
* value�� null������ check�ϴ� boolean type array�� ������ �ִ�.
*
* since 1.0
*/

class UResultTuple {

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

private int index;
private CUBRIDOID oid;
// private boolean wasNull[] = {};
private Object attributes[];

/*=======================================================================
 |	CONSTRUCTOR
 =======================================================================*/

UResultTuple(int tupleIndex, int attributeNumber)
{
  index = tupleIndex;
  attributes = new Object[attributeNumber];
  // wasNull = new boolean[attributeNumber];
}

/*=======================================================================
 |	PACKAGE ACCESS METHODS
 =======================================================================*/

/*
 * Result Set�� clear��Ű�� ���� ���ȴ�.
 */

void close()
{
  for (int i=0 ; attributes != null && i < attributes.length ; i++)
    attributes[i] = null;
  attributes = null;
  // wasNull = null;
  oid = null;
}

/*
 * index��° column value�� return�Ѵ�.
 */

Object getAttribute(int tIndex)
{
  /*
  if (tIndex < 0 || attributes == null || tIndex >= attributes.length)
    return null;
  */
  return attributes[tIndex];
}

/*
 * ���� tuple�� oid���� return�Ѵ�.
 */

CUBRIDOID getOid()
{
  return oid;
}

boolean oidIsIncluded()
{
  if (oid == null)
    return false;
  return true;
}

/*
 * ���� tuple�� index��° column value�� data�� set�Ѵ�.
 */

void setAttribute(int tIndex, Object data)
{
  /*
  if (wasNull == null || attributes == null || tIndex < 0 ||
      tIndex > wasNull.length - 1 || tIndex > attributes.length - 1)
  {
    return;
  }
  wasNull[tIndex] = (data == null) ? true : false;
  */

  attributes[tIndex] = data;
}

/*
 * ���� tuple�� oid�� set�Ѵ�.
 */

void setOid(CUBRIDOID o)
{
  oid = o;
}

/*
 * tuple index�� return�Ѵ�.
 */

int tupleNumber()
{
  return index;
}

/*
 * ���� tuple�� index��° column�� null������ return�Ѵ�.
 *

boolean wasNull(int tIndex)
{
  return ((wasNull != null) ? wasNull[tIndex] : false);
}
 */

}  // end of class UResultTuple
