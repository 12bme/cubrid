/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

import java.math.BigDecimal;
import java.sql.Date;
import java.sql.Time;
import java.sql.Timestamp;

import cubrid.sql.CUBRIDOID;

/**
 * Collection type�� data�� add�ϰų� delete�� �� JCI������ add�ϰ����ϴ� attribute��
 * collection base type������ �� �� ���� ������ End user�κ��� �Է¹��� data��
 * type���κ��� CUBRID type��
 */

class UAParameter extends UParameter {

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

private String attributeName;

/*=======================================================================
 |	CONSTRUCTOR
 =======================================================================*/

UAParameter(String pName, Object pValue) throws UJciException
{
  super(1);

  byte[] pTypes = new byte[1];
  Object attributeValue[] = new Object[1];

  attributeName = pName;
  attributeValue[0] = pValue;
  if (pValue == null) {
    pTypes[0] = UUType.U_TYPE_NULL;
  }
  else {
    pTypes[0] = UUType.getObjectDBtype(pValue);
    if (pTypes[0] == UUType.U_TYPE_NULL || pTypes[0] == UUType.U_TYPE_SEQUENCE)
      throw new UJciException(UErrorCode.ER_INVALID_ARGUMENT);
  }

  setParameters(pTypes, attributeValue);
}

/*=======================================================================
 |	PACKAGE ACCESS METHODS
 =======================================================================*/

synchronized void writeParameter(UOutputBuffer outBuffer)
	throws UJciException
{
  if (attributeName != null)
    outBuffer.addStringWithNull(attributeName);
  else
    outBuffer.addNull();
  outBuffer.addByte(types[0]);
  outBuffer.writeParameter(types[0], values[0]);
}

}  // end of class UAParameter
