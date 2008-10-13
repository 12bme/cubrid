package cubrid.jdbc.driver;

import java.sql.*;


/**
 * Title:        CUBRID JDBC Driver
 * Description:
 * @version 2.0
 */


/**
 * �� class�� CUBRIDStatement�� CUBRIDPreparedStatement��
 * query timeout����� �����ϴµ� ���ȴ�.
 *
 * �����ÿ� �ð��� CUBRIDStatement object�� �ް� ������ ��
 * run()�� ȣ��Ǹ� �־��� �ð����� sleep�� �Ŀ�
 * �־��� CUBRIDStatement object�� cancel()�� ȣ�����ش�.
 *
 * �־��� CUBRIDStatement object�� query�� ������ ���� ������ �Ǹ�
 * queryended()�� ȣ���Ͽ� �˷��ְ� �Ǿ� �ִ�.
 *
 * query�� ������ server���� ������ ���������� �ұ��ϰ� ����
 * queryended()�� ȣ����� �ʾƼ� query�� ������ ����ϵ���
 * �õ��� �� ������ �߻��� ������ �����Ƿ�,
 * queryended()�Լ��� ȣ���ϴ� �Ͱ�
 * �־��� CUBRIDStatement object�� cancel()�� ȣ���ϴ� ����
 * ����ȭ�Ͽ� ������ �ϳ��� �������϶����� �ٸ� �ϳ��� block�ǵ��� �Ѵ�.
 */
class CUBRIDCancelQueryThread extends Thread {

/*=======================================================================
 |      PRIVATE VARIABLES 
 =======================================================================*/

/*
 * query�� ����� ������ ��ٸ� �ð�
 */
private int timeout;

/*
 * ����� query�� �������� CUBRIDStatement object
 */
private CUBRIDStatement stmt;

/*
 * query�� ������ ���������� ��Ÿ���� flag�̴�.
 * true�̸� query�� ������ �������Ƿ� �־��� CUBRIDStatement object��
 * cancel()�� ȣ������ �ʴ´�.
 * �� ���� target CUBRIDStatement object�� query�� ������ ������ true�� set�Ѵ�.
 */
private boolean end = false;

/*=======================================================================
 |      CONSTRUCTOR
 =======================================================================*/

/*
 * ������ �ð��� CUBRIDStatement object�� �־�����.
 */
CUBRIDCancelQueryThread(CUBRIDStatement cancel_stmt, int time)
{
  stmt = cancel_stmt;
  timeout = time;
}

/*=======================================================================
 |      PUBLIC METHODS
 =======================================================================*/

/*
 * �־��� timeout�� ���� sleep�� �Ŀ�
 * end���� false�̸�
 * CUBRIDStatement�� cancel�� ȣ���Ѵ�.
 */
public void run()
{
  try {
    Thread.sleep(timeout*1000);
    synchronized (this) {
      if ( end == false ) {
	stmt.cancel();
      }
    }
  }
  catch (Exception e) {
  }
}

/*
 * CUBRIDStatement object�� ���� ����Ǹ�
 * query�� ������ �������� �˸��� ����
 * end���� true�� set�Ѵ�.
 */
synchronized void queryended()
{
  end = true;
  interrupt();
}

}  // end of class CUBRIDCancelQueryThread
