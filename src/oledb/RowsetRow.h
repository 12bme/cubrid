#pragma once

class CCUBRIDRowsetRowColumn;
class CCUBRIDRowset;

/*
 * Storage�� �� Row�� Local Copy �Ǵ� Deferred Update �����͸� �����صδ� Ŭ����.
 *
 * m_rgColumns: BOOKMARK �÷� ����. Storage ���� ������ ����
 * m_iRowset: 0���� ����. Storage ���� ��ġ.
 */
class CCUBRIDRowsetRow
{
public:
	typedef DBCOUNTITEM KeyType;
	DWORD m_dwRef;
	DBPENDINGSTATUS m_status;
	KeyType m_iRowset;
	KeyType m_iOriginalRowset; // not used
	char m_szOID[32]; // OID of Row
private:
	CCUBRIDRowsetRowColumn *m_rgColumns;
	DBORDINAL m_cColumns;
	ATLCOLUMNINFO *m_pInfo;
	CComPtr<IDataConvert> m_spConvert;
	CAtlArray<CStringA>* m_defaultVal;

private:
	// m_rgColumns �޸𸮸� ����
	void FreeData();
public:
	CCUBRIDRowsetRow(DBCOUNTITEM iRowset, DBORDINAL cCols, ATLCOLUMNINFO *pInfo, 
				CComPtr<IDataConvert> &spConvert, CAtlArray<CStringA>* defaultVal = NULL)
		: m_dwRef(0), m_rgColumns(0), m_status(0), m_iRowset(iRowset),
		  m_iOriginalRowset(iRowset), m_cColumns(cCols), m_pInfo(pInfo), m_defaultVal(defaultVal),
		  m_spConvert(spConvert)
	{
		ATLTRACE(atlTraceDBProvider, 3, "CCUBRIDRowsetRow::CCUBRIDRowsetRow\n");
		m_szOID[0] = NULL;
	}

	~CCUBRIDRowsetRow()
	{
		ATLTRACE(atlTraceDBProvider, 3, "CCUBRIDRowsetRow::~CCUBRIDRowsetRow\n");
		FreeData();
	}

	DWORD AddRefRow() { return CComObjectThreadModel::Increment((LPLONG)&m_dwRef); } 
	DWORD ReleaseRow() { return CComObjectThreadModel::Decrement((LPLONG)&m_dwRef); }

	HRESULT Compare(CCUBRIDRowsetRow *pRow)
	{
		ATLASSERT(pRow);
		return ( m_iRowset==pRow->m_iRowset ? S_OK : S_FALSE );
	}

	//===== ReadData: �ٸ� ���� �����͸� �� Ŭ���� ������ �о���δ�.
public:
	// hReq�� ���� �����͸� �о����
	HRESULT ReadData(int hReq, bool bOIDOnly=false, bool bSensitive=false);
	// pBinding�� ���� �̷���� pData�� ���� �����͸� �о����
	HRESULT ReadData(ATLBINDINGS *pBinding, void *pData);
	HRESULT ReadData(int hReq, char* szOID);

	//===== WriteData: �� Ŭ������ �����͸� �ٸ� ���� �����Ѵ�.
public:
	// m_rgColumns�� �����͸� storage�� ����
	HRESULT WriteData(int hConn, int hReq, CComBSTR &strTableName);
	// m_rgColumns�� �����͸� pBinding�� ���� pData�� ����
	HRESULT WriteData(ATLBINDINGS *pBinding, void *pData, DBROWCOUNT dwBookmark, CCUBRIDRowset* pRowset = NULL);
	// m_rgColumns�� �����͸� rgColumns�� ����
	HRESULT WriteData(DBORDINAL cColumns, DBCOLUMNACCESS rgColumns[]);

	//===== Compare: �� Ŭ������ �����Ͱ� ���ǿ� �����ϴ� �� �˻�
public:
	// ���� row�� rBinding�� ���ǿ� �´��� �˻�
	HRESULT Compare(void *pFindData, DBCOMPAREOP CompareOp, DBBINDING &rBinding);
};
