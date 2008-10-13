#pragma once

void show_error(char *msg, int code, T_CCI_ERROR *error);

namespace Util {

// �ϸ�ũ �迭���� �־��� ���� ������ ���� �ε����� ���Ѵ�.
//DBROWCOUNT FindBookmark(const CAtlArray<DBROWCOUNT> &rgBookmarks, DBROWCOUNT iRowset);

// IDBProperties�� ���� DataSource ������ �� ��
// server�� �����ؼ� �� handle�� *phConn�� ��ȯ�Ѵ�.
HRESULT Connect(IDBProperties *pDBProps, int *phConn);

// server�� ������ ���� *phConn ���� 0���� �����.
HRESULT Disconnect(int *phConn);

// ���̺��� �����ϸ� S_OK, �������� ������ S_FALSE
HRESULT DoesTableExist(int hConn, char *szTableName);

// ���̺��� ����, req handle�� result count�� ��ȯ�Ѵ�.
HRESULT OpenTable(int hConn, const CComBSTR &strTableName, int *phReq, int *pcResult, char flag, bool bAsynch=false, int maxrows=0);

HRESULT GetUniqueTableName(CComBSTR& strTableName);
HRESULT GetTableNames(int hConn, CAtlArray<CStringA> &rgTableNames);
HRESULT GetIndexNamesInTable(int hConn, char* table_name, CAtlArray<CStringA> &rgIndexNames, CAtlArray<int> &rgIndexTypes);

// SQL ������ ���̺� �̸��� �̾Ƴ���.
void ExtractTableName(const CComBSTR &strCommandText, CComBSTR &strTableName);

//��û�� �������̽��� CCI_PREPARE_UPDATABLE�� �ʿ�� �ϴ��� üũ�Ѵ�.
bool RequestedRIIDNeedsUpdatability(REFIID riid);
//bool RequestedRIIDNeedsOID(REFIID riid);
//bool CheckOIDFromProperties(ULONG cSets, const DBPROPSET rgSets[]);
bool CheckUpdatabilityFromProperties(ULONG cSets, const DBPROPSET rgSets[]);

// IColumnsInfo�� ���� ������ �����ϴ� Ŭ����
class CColumnsInfo
{
public:
	int m_cColumns;
	ATLCOLUMNINFO *m_pInfo;
	CAtlArray<CStringA>* m_defaultVal;

	CColumnsInfo() : m_cColumns(0), m_pInfo(0), m_defaultVal(0){}
	~CColumnsInfo() { FreeColumnInfo(); }

	// m_cColumns, m_pInfo ���� ä���.
	// ����) �̹� ���� �ִ��� ���δ� �˻����� �ʴ´�.
	HRESULT GetColumnInfo(T_CCI_COL_INFO* info, T_CCI_SQLX_CMD cmd_type, int cCol, bool bBookmarks=false, ULONG ulMaxLen=0);
	HRESULT GetColumnInfo(int hReq, bool bBookmarks=false, ULONG ulMaxLen=0);
	HRESULT GetColumnInfoCommon(T_CCI_COL_INFO* info, T_CCI_SQLX_CMD cmd_type, bool bBookmarks=false, ULONG ulMaxLen=0);

	// m_pInfo�� �޸𸮸� �����ϰ�, ��� ������ �ʱ�ȭ�Ѵ�.
	void FreeColumnInfo();
};

// Commit�̳� Abort ���� �� �Ҹ���.
class ITxnCallback
{
public:
	virtual void TxnCallback(const ITxnCallback *pOwner) = 0;
};

}
