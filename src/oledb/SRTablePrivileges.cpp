#include "stdafx.h"
#include "DataSource.h"
#include "Session.h"
#include "Error.h"

static HRESULT GetCurrentUser(CSRTablePrivileges *pSR, CComVariant &var)
{
	CCUBRIDDataSource *pDS = CCUBRIDSession::GetSessionPtr(pSR)->GetDataSourcePtr();
	pDS->GetPropValue(&DBPROPSET_DBINIT, DBPROP_AUTH_USERID, &var);
	return S_OK;
}

static void GetRestrictions(ULONG cRestrictions, const VARIANT *rgRestrictions, char *table_name)
{
	// restriction�� ���ٰ� �׻� cRestrictions==0�� �ƴϴ�.
	// ���� vt!=VT_EMPTY���� � �˻������ �Ѵ�.

	if(cRestrictions>=3 && V_VT(&rgRestrictions[2])==VT_BSTR && V_BSTR(&rgRestrictions[2])!=NULL)
	{	// TABLE_NAME restriction
		CW2A name(V_BSTR(&rgRestrictions[2]));
		ATLTRACE2("\tTable Name = %s\n", (LPSTR)name);

		strncpy(table_name, name, 1023);
		table_name[1023] = 0; // ensure zero-terminated string
	}
}

// S_OK : ����
// S_FALSE : �����ʹ� ���������� Consumer�� ���ϴ� �����Ͱ� �ƴ�
// E_FAIL : ����
static HRESULT FetchData(int hReq, CTablePrivilegesRow &tprData)
{
	char *value;
	int ind, res;
	T_CCI_ERROR err_buf;

	res = cci_fetch(hReq, &err_buf);
	if(res<0) return RaiseError(E_FAIL, 1, __uuidof(IDBSchemaRowset), err_buf.err_msg);

	res = cci_get_data(hReq, 1, CCI_A_TYPE_STR, &value, &ind);
	if(res<0) return RaiseError(E_FAIL, 0, __uuidof(IDBSchemaRowset));
	wcsncpy(tprData.m_szTableName, CA2W(value), 128);
	tprData.m_szTableName[128] = 0;

	res = cci_get_data(hReq, 2, CCI_A_TYPE_STR, &value, &ind);
	if(res<0) return RaiseError(E_FAIL, 0, __uuidof(IDBSchemaRowset));
	// CCI�� "ALTER", "EXECUTE"� ��ȯ�ϴµ� ��� ó���ؾ� ���� �𸣰ڴ�.
	if(strcmp(value, "SELECT")!=0 && strcmp(value, "DELETE")!=0 && strcmp(value, "INSERT")!=0
	   && strcmp(value, "UPDATE")!=0 && strcmp(value, "REFERENCES")!=0)
		return S_FALSE;
	wcscpy(tprData.m_szPrivilegeType, CA2W(value));

	res = cci_get_data(hReq, 3, CCI_A_TYPE_STR, &value, &ind);
	if(res<0) return RaiseError(E_FAIL, 0, __uuidof(IDBSchemaRowset));
	if(strcmp(value, "NO")==0)
		tprData.m_bIsGrantable = VARIANT_FALSE;
	else
		tprData.m_bIsGrantable = VARIANT_TRUE;

	return S_OK;
}

HRESULT CSRTablePrivileges::Execute(LONG * /*pcRowsAffected*/,
				ULONG cRestrictions, const VARIANT *rgRestrictions)
{
	ATLTRACE2(atlTraceDBProvider, 2, "CSRTablePrivileges::Execute\n");

	ClearError();

	int hConn = -1;
	HRESULT hr = CCUBRIDSession::GetSessionPtr(this)->GetConnectionHandle(&hConn);
	if(FAILED(hr)) return hr;

	char table_name[1024]; table_name[0] = 0;
	GetRestrictions(cRestrictions, rgRestrictions, table_name);

	CComVariant grantee;
	hr = GetCurrentUser(this, grantee);
	if(FAILED(hr)) return hr;

	{
		T_CCI_ERROR err_buf;
		int hReq = cci_schema_info(hConn, CCI_SCH_CLASS_PRIVILEGE, (table_name[0]?table_name:NULL),
							NULL, CCI_CLASS_NAME_PATTERN_MATCH, &err_buf);
		if(hReq<0)
		{
			ATLTRACE2("cci_schema_info fail\n");
			return RaiseError(E_FAIL, 1, __uuidof(IDBSchemaRowset), err_buf.err_msg);
		}

		int res = cci_cursor(hReq, 1, CCI_CURSOR_FIRST, &err_buf);
        if(res==CCI_ER_NO_MORE_DATA) goto done;
		if(res<0) goto error;

		while(1)
		{
			CTablePrivilegesRow tprData;
			wcscpy(tprData.m_szGrantor, L"DBA");
			wcscpy(tprData.m_szGrantee, V_BSTR(&grantee));
			hr = FetchData(hReq, tprData);
			if(FAILED(hr))
			{
				cci_close_req_handle(hReq);
				return hr;
			}

			if(hr==S_OK) // S_FALSE�� �߰����� ����
			{
				_ATLTRY
				{
					// TABLE_NAME, PRIVILEGE_TYPE ������ �����ؾ� �Ѵ�.
					size_t nPos;
					for( nPos=0 ; nPos<m_rgRowData.GetCount() ; nPos++ )
					{
						int res = wcscmp(m_rgRowData[nPos].m_szTableName, tprData.m_szTableName);
						if(res>0) break;
						if(res==0 && wcscmp(m_rgRowData[nPos].m_szPrivilegeType, tprData.m_szPrivilegeType)>0) break;
					}
					m_rgRowData.InsertAt(nPos, tprData);
				}
				_ATLCATCHALL()
				{
					ATLTRACE2("out of memory\n");
					cci_close_req_handle(hReq);
					return E_OUTOFMEMORY;
				}
			}

			res = cci_cursor(hReq, 1, CCI_CURSOR_CURRENT, &err_buf);
			if(res==CCI_ER_NO_MORE_DATA) goto done;
			if(res<0) goto error;
		}

error:
		ATLTRACE2("fail to fetch data\n");
		cci_close_req_handle(hReq);
		return RaiseError(E_FAIL, 1, __uuidof(IDBSchemaRowset), err_buf.err_msg);
done:
		cci_close_req_handle(hReq);
	}

	return S_OK;
}

DBSTATUS CSRTablePrivileges::GetDBStatus(CSimpleRow *pRow, ATLCOLUMNINFO *pInfo)
{
	ATLTRACE2(atlTraceDBProvider, 3, "CSRTablePrivileges::GetDBStatus\n");
	switch(pInfo->iOrdinal)
	{
	case 1: // GRANTOR
	case 2: // GRANTEE
	case 5: // TABLE_NAME
	case 6: // PRIVILEGE_TYPE
	case 7: // IS_GRANTABLE
		return DBSTATUS_S_OK;
	default:
		return DBSTATUS_S_ISNULL;
	}
}
