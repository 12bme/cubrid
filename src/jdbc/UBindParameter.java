/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

import java.util.Vector;

/*
 * PreparedStatement�� ���Ǵ� parameter���� �����ϴ� class�̴�.
 * 1.0���� parameter�� �����ϴ� class UBindParameterInfo�� 1�������� parameter��
 * �����Ͽ� 2.0������ ����ϱⰡ ����Ͽ� ������� ��ü class�̴�.
 * parameter�� 1���� array�� element�� ���� java.util.Vector class�� �����ȴ�.
 *
 * since 2.0
 */

class UBindParameter extends UParameter {

/*=======================================================================
 |	PRIVATE CONSTANT VALUES
 =======================================================================*/

private final static byte PARAM_MODE_UNKNOWN = 0;
private final static byte PARAM_MODE_IN = 1;
private final static byte PARAM_MODE_OUT = 2;
private final static byte PARAM_MODE_INOUT = 3;

/*=======================================================================
 |	PACKAGE ACCESS VARIABLES
 =======================================================================*/

byte paramMode[];

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

private boolean isBinded[];

/*=======================================================================
 |	PUBLIC CONSTANT VALUES
 =======================================================================*/

UBindParameter(int parameterNumber)
{
  super(parameterNumber);

  isBinded = new boolean[number];
  paramMode = new byte[number];

  clear();
}

/*=======================================================================
 |	PACKAGE ACCESS METHODS
 =======================================================================*/

/*
 * parameter�� current cursor���� ��� parameter���� bind�Ǿ������� check�Ѵ�.
 */
boolean checkAllBinded()
{
  for(int i=0 ; i < number ; i++) {
    if (isBinded[i] == false && paramMode[i] == PARAM_MODE_UNKNOWN)
      return false;
  }
  return true;
}

void clear()
{
  for (int i=0 ; i < number ; i++) {
    isBinded[i] = false;
    paramMode[i] = PARAM_MODE_UNKNOWN;
    values[i] = null;
    types[i] = UUType.U_TYPE_NULL;
  }
}

synchronized void close()
{
  for (int i=0 ; i < number ; i++)
    values[i] = null;
  isBinded = null;
  paramMode = null;
  values = null;
  types = null;
}

/*
 * current cursor�� index�� ° parameter value�� set�Ѵ�.
 */
synchronized void setParameter(int index, byte bType, Object bValue)
	throws UJciException
{
  if (index < 0 || index >= number)
    throw new UJciException(UErrorCode.ER_INVALID_ARGUMENT);

  types[index] = bType;
  values[index] = bValue;

  isBinded[index] = true;
  paramMode[index] |= PARAM_MODE_IN;
}

void setOutParam(int index) throws UJciException
{
  if (index < 0 || index >= number)
    throw new UJciException(UErrorCode.ER_INVALID_ARGUMENT);
  paramMode[index] |= PARAM_MODE_OUT;
}

synchronized void writeParameter(UOutputBuffer outBuffer)
	throws UJciException
{
  for (int i=0 ; i < number ; i++) {
    if (values[i] == null) {
      outBuffer.addByte(UUType.U_TYPE_NULL);
      outBuffer.addNull();
    }
    else {
      outBuffer.addByte((byte) types[i]);
      outBuffer.writeParameter(((byte) types[i]), values[i]);
    }
  }
}

}  // end of class UBindParameter
