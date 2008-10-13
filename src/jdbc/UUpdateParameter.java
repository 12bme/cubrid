/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

/**
* class UStatement method updateRow���� update�� row�� column parameter����
* �����ϱ� ���� class�̴�.
* update�ϰ��� �ϴ� attribute�� info�� �� �� ���� ������ �־��� value��
* java type�� ���� CUBRID type���� match�Ͽ���. ���� user�� �ش� attribute��
* type�� �̸��˰� �� type�� �ش�Ǵ� java type columnValues�� �Ѱܾ� �Ѵ�.
*
* since 2.0
*/

class UUpdateParameter extends UParameter {

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

private int indexes[];	/* parameter�� column index */

/*=======================================================================
 |	CONSTRUCTOR
 =======================================================================*/

public UUpdateParameter(UColumnInfo columnInfo[], int[] columnIndexes,
			Object[] columnValues)
	throws UJciException
{
  super(columnValues.length);

  /* check acceptable argument */
  if (columnIndexes == null || columnValues == null ||
      columnIndexes.length != columnValues.length)
  {
    throw new UJciException(UErrorCode.ER_INVALID_ARGUMENT);
  }

  for (int i=0 ; i < columnIndexes.length ; i++) {
    if (columnIndexes[i] < 0 || columnIndexes[i] > columnInfo.length)
      throw new UJciException(UErrorCode.ER_INVALID_ARGUMENT);
  }

  UColumnInfo info[] = columnInfo;
  byte[] pTypes = new byte[number];
  indexes = new int[number];

  for (int i=0 ; i < types.length ; i++) {
    pTypes[i] = info[columnIndexes[i]].getColumnType();
  }

  setParameters(pTypes, columnValues);

  for (int i=0 ; i < number ; i++) {
    /* JCI index�� ������ 0����, server�� index�� ������ 1���� ���� */
    indexes[i] = columnIndexes[i] + 1;
  }
}

/*=======================================================================
 | PACKAGE ACCESS METHODS
 =======================================================================*/

/*
 * parameter�� output buffer�� write�Ѵ�.
 */

synchronized void writeParameter(UOutputBuffer outBuffer)
	throws UJciException
{
  for (int i=0 ; i < number ; i++) {
    outBuffer.addInt(indexes[i]);
    outBuffer.addByte(types[i]);
    outBuffer.writeParameter(types[i], values[i]);
  }
}

}  // end of class UUpdateParameter
