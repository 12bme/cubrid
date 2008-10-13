
/**
 * Title:        CUBRID Java Client Interface<p>
 * Description:  CUBRID Java Client Interface<p>
 * @version 2.0
 */

package cubrid.jdbc.jci;

import java.io.IOException;
import java.io.DataInputStream;

/**
* Java�� c�ʹ� �޸� string�� ���� null character�� �������� �ʴ´�. �̷���
* language���� string management�� ���̸� ����Ͽ� CAS�� string communication��
* manage�ϴ� class�̴�. ���� Ư�� type value�� precision length��ŭ space��
* �߰��ϱ� ���� method ���� �����ϰ� �ִ�.
*
* since 1.0
*/

abstract class UManageStringInCType {

/* Internal Variable */

  final static String spaceString = new String(" ");

/* Internal Interface */

/* Ư�� type�� string representation���� precision��ŭ�� ���̸� ������ �ϴ�
*   ��찡 �ִ�. �̷��� ��츦 ���� space�� precision��ŭ �߰����ִ� method�̴�. */

  static String stringWithSpace(String originalData, int length) {
    if (originalData == null)
      originalData="";
    for (int i = originalData.length() ; i < length ; i++)
      originalData += spaceString;
    return originalData;
  }
}

