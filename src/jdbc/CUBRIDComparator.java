package cubrid.jdbc.driver;

import java.util.Comparator;

/**
 * Title:        CUBRID JDBC Driver
 * Description:
 * @version 2.0
 */

/**
 * �� class�� CUBRIDDatabaseMetaData���� ��������� ResultSet��
 * CUBRIDResultSetWithoutQuery�� row���� sorting�� ���� class�μ�
 * sorting�� ���� ���Լ��� ������ �Լ��̴�.
 *
 * sorting����� DatabaseMetaData�� �Լ��� ���� �ٸ��Ƿ� � �Լ�����
 * ������ ResultSet���� �˱� ���ؼ� �����ÿ� �Լ��̸��� �ο��޴´�.
 * �ο����� �Լ��� ���� �ٸ� ���Լ��� ȣ�����ش�.
 */

class CUBRIDComparator implements Comparator {

/*=======================================================================
 |      PRIVATE VARIABLES 
 =======================================================================*/

/**
 * DatabaseMetaData�� � method�� ���� ������ ResultSet��
 * ���Լ��� �� ���ΰ��� ��Ÿ����.
 */
private String dbmd_method;

/*=======================================================================
 |      CONSTRUCTOR
 =======================================================================*/

/**
 * ���ϰԵ� ResultSet�� DatabaseMetaData�� � method
 * ���� ������ ���ΰ��� �� �� �ֵ��� method�� �̸��� constructor�� �־�����.
 */

CUBRIDComparator(String whatfor)
{
  dbmd_method = whatfor;
}

/*=======================================================================
 |      java.util.Comparator interface
 =======================================================================*/

/**
 * �־��� method�̸��� ���� ������ ���Լ��� ȣ�����ش�.
 */
public int compare(Object o1, Object o2)
{
  if (dbmd_method.endsWith("getTables"))
    return compare_getTables(o1, o2);
  if (dbmd_method.endsWith("getColumns"))
    return compare_getColumns(o1, o2);
  if (dbmd_method.endsWith("getColumnPrivileges"))
    return compare_getColumnPrivileges(o1, o2);
  if (dbmd_method.endsWith("getTablePrivileges"))
    return compare_getTablePrivileges(o1, o2);
  if (dbmd_method.endsWith("getBestRowIdentifier"))
    return compare_getBestRowIdentifier(o1, o2);
  if (dbmd_method.endsWith("getIndexInfo"))
    return compare_getIndexInfo(o1, o2);
  if (dbmd_method.endsWith("getSuperTables"))
    return compare_getSuperTables(o1, o2);
  return 0;
}

/*=======================================================================
 |      PRIVATE METHODS
 =======================================================================*/

/**
 * CUBRIDDatabaseMetaData.getTables()�� ���Լ�
 */
private int compare_getTables(Object o1, Object o2)
{
  int t;
  t = ((String)((Object[])o1)[3]).compareTo((String)((Object[])o2)[3]);
  if (t != 0) return t;
  return ((String)((Object[])o1)[2]).compareTo((String)((Object[])o2)[2]);
}

/**
 * CUBRIDDatabaseMetaData.getColumns()�� ���Լ�
 */
private int compare_getColumns(Object o1, Object o2)
{
  int t;
  t = ((String)((Object[])o1)[2]).compareTo((String)((Object[])o2)[2]);
  if (t != 0) return t;
  return ((Integer)((Object[])o1)[16]).compareTo((Integer)((Object[])o2)[16]);
}

/**
 * CUBRIDDatabaseMetaData.getColumnPrivileges()�� ���Լ�
 */
private int compare_getColumnPrivileges(Object o1, Object o2)
{
  int t;
  t = ((String)((Object[])o1)[3]).compareTo((String)((Object[])o2)[3]);
  if (t != 0) return t;
  return ((String)((Object[])o1)[6]).compareTo((String)((Object[])o2)[6]);
}

/**
 * CUBRIDDatabaseMetaData.getTablePrivileges()�� ���Լ�
 */
private int compare_getTablePrivileges(Object o1, Object o2)
{
  int t;
  t = ((String)((Object[])o1)[2]).compareTo((String)((Object[])o2)[2]);
  if (t != 0) return t;
  return ((String)((Object[])o1)[5]).compareTo((String)((Object[])o2)[5]);
}

/**
 * CUBRIDDatabaseMetaData.getBestRowIdentifier()�� ���Լ�
 */
private int compare_getBestRowIdentifier(Object o1, Object o2)
{
  return ((Short)((Object[])o1)[0]).compareTo((Short)((Object[])o2)[0]);
}

/**
 * CUBRIDDatabaseMetaData.getIndexInfo()�� ���Լ�
 */
private int compare_getIndexInfo(Object o1, Object o2)
{
  int t;

  if (((Boolean)((Object[])o1)[3]).booleanValue() &&
      !((Boolean)((Object[])o2)[3]).booleanValue()) return 1;
  if (!((Boolean)((Object[])o1)[3]).booleanValue() &&
      ((Boolean)((Object[])o2)[3]).booleanValue()) return -1;

  t = ((Short)((Object[])o1)[6]).compareTo((Short)((Object[])o2)[6]);
  if (t != 0) return t;

  if (((Object[])o1)[5] == null) return 0;
  t = ((String)((Object[])o1)[5]).compareTo((String)((Object[])o2)[5]);
  if (t != 0) return t;

  return ((Integer)((Object[])o1)[7]).compareTo((Integer)((Object[])o2)[7]);
}

/**
 * CUBRIDDatabaseMetaData.getSuperTables()�� ���Լ�
 */
private int compare_getSuperTables(Object o1, Object o2)
{
  int t;
  t = ((String)((Object[])o1)[2]).compareTo((String)((Object[])o2)[2]);
  if (t != 0) return t;
    return ((String)((Object[])o1)[3]).compareTo((String)((Object[])o2)[3]);
}

}  // end of class CUBRIDComparator
