/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

/*
 * class UConnection method executeBatch,
 * ( Interface Statement method batchExecute�� ���� interface )
 * class UStatement method batchExecute
 * ( Interface PreparedStatement method batchExecute�� ���� interface )
 * �� ���� batch statement�� ���� �ΰ��� interface���� ������� result��
 * �����ϴ� class�̴�.
 *
 * since 2.0
 */

public class UBatchResult {

/*=======================================================================
 |	PRIVATE VARIABLES
 =======================================================================*/

private boolean	errorFlag;
private int resultNumber;	/* batch job���� ����� statement�� ���� */
private int result[];		/* batch statement�� �� result count */
private int statementType[];	/* batch statement�� �� statement type */
private int errorCode[];	/* batch statement�� �� error code */
private String errorMessage[];	/* batch statement�� �� error message */

/*=======================================================================
 |	CONSTRUCTOR
 =======================================================================*/

UBatchResult(int number)
{
  resultNumber = number;
  result = new int[resultNumber];
  statementType = new int[resultNumber];
  errorCode = new int[resultNumber];
  errorMessage = new String[resultNumber];
  errorFlag = false;
}

/*=======================================================================
 |	PUBLIC METHODS
 =======================================================================*/

/*
 * batch statement�� execution �� ������� �� statement�� error code��
 * return�Ѵ�.
 */

public int[] getErrorCode()
{
  return errorCode;
}

/*
 * batch statement�� execution �� ������� �� statement�� error Message��
 * return�Ѵ�.
 */

public String[] getErrorMessage()
{
  return errorMessage;
}

/*
 * batch statement�� execution �� ������� �� statement�� result count��
 * return�Ѵ�.
 * error�� �߻��Ͽ��� ��� result count�� -3�̴�.
 */

public int[] getResult()
{
  return result;
}

/*
 * batch job���� execute�� statement ������ return�Ѵ�.
 */

public int getResultNumber()
{
  return resultNumber;
}

/*
 * batch statement�� �� statement�� type�� return�Ѵ�.
 * class CUBRIDCommandType���� type�� identify�� �� �ִ�.
 */

public int[] getStatementType()
{
  return statementType;
}

/*=======================================================================
 *	PACKAGE ACCESS METHODS
 =======================================================================*/

/*
 * error�� �߻����� ���� statement�� ����� set�ϱ� ���� interface�̴�.
 * error code�� 0����, error message�� null�� �����ȴ�.
 */

synchronized void setResult(int index, int count)
{
  if (index < 0 || index >= resultNumber)
    return;
  result[index] = count;
  errorCode[index] = 0;
  errorMessage[index] = null;
}

/*
 * error�� �߻��� statement�� ����� set�ϱ� ���� interface�̴�.
 * result count�� -3����, error code�� error message�� server�ʿ��� �Ѿ��
 * ������ set�Ѵ�.
 */

synchronized void setResultError(int index, int code, String message)
{
  if (index < 0 || index >= resultNumber)
    return;
  result[index] = -3;
  errorCode[index] = code;
  errorMessage[index] = message;
  errorFlag = true;
}

/*
 * index�� �ش��ϴ� statement�� type�� �����ϱ� ���� interface�̴�.
 * statement type�� class CUBRIDCommandType���� identify�� �� �ִ�.
 */

public boolean getErrorFlag()
{
  return errorFlag;
}

synchronized void setStatementType(int index, int type)
{
  if (index < 0 || index >= resultNumber)
    return;
  statementType[index] = type;
}

} // end of class UBatchResult
