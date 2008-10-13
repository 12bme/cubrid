#include "stdafx.h"
#include "Session.h"

/*
 * Commit ��å
 *   (implicit commit: IRowsetChange��� AutoCommit() ȣ��)
 *   (explicit commit: ITransaction::Commit �Ǵ� Rowset�� ����)
 *
 * 1. implicit commit in manual-commit mode
 *
 *		�ƹ� �ϵ� ���� �ʴ´�.
 *
 * 2. implicit commit in auto-commit mode
 *
 *		TxnCallback�� ȣ���ؼ� �ٸ� Rowset�� ����� �����.
 *
 * 3. explicit commit in manual-commit mode
 *
 *		������ Commit �ϰ� TxnCallback�� ȣ���ؼ� ��� Rowset�� ����� �����.
 *
 * 4. explicit commit in auto-commit mode
 *
 *		������ Commit �Ѵ�(�ٸ� Rowset�� �̹� �����̰�, ���� Rowset�� �ݴ� ��)
 *
 */

HRESULT CCUBRIDSession::DoCASCCICommit(bool bCommit)
{
	int hConn;
	HRESULT hr = GetConnectionHandle(&hConn);
	if(FAILED(hr)) return E_FAIL;

	T_CCI_ERROR err_buf;
	int rc = cci_end_tran(hConn, bCommit?CCI_TRAN_COMMIT:CCI_TRAN_ROLLBACK, &err_buf);
	if(rc<0) return bCommit?XACT_E_COMMITFAILED:E_FAIL;

	// TxnCallback�� ȣ���� ����� �����.
	POSITION pos = m_grpTxnCallbacks.GetHeadPosition();
	while(pos)
		m_grpTxnCallbacks.GetNext(pos)->TxnCallback(0);

	return S_OK;
}

HRESULT CCUBRIDSession::AutoCommit(const Util::ITxnCallback *pOwner)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::AutoCommit\n");

	if(m_bAutoCommit)
	{
		if(pOwner)
		{
			// TxnCallback�� ȣ���� pOwner�� �����ϰ�� ����� �����.
			POSITION pos = m_grpTxnCallbacks.GetHeadPosition();
			while(pos)
				m_grpTxnCallbacks.GetNext(pos)->TxnCallback(pOwner);

			//HRESULT hr = DoCASCCICommit(true);
			//if(FAILED(hr)) return hr;
			//return S_OK;
		}
		else
		{
			// ��� ���� �Ǵϱ� ���� ���� Commit�� �Ѵ�.
			DoCASCCICommit(true);
		}
	}

	return S_OK;
}

HRESULT CCUBRIDSession::RowsetCommit()
{
	return DoCASCCICommit(true);
}

// bCheckOnly : isoLevel�� ������ �������� üũ�Ѵ�.
HRESULT CCUBRIDSession::SetCASCCIIsoLevel(ISOLEVEL isoLevel, bool bCheckOnly)
{
	int hConn;
	if(!bCheckOnly)
	{
		HRESULT hr = GetConnectionHandle(&hConn);
		if(FAILED(hr)) return E_FAIL;
	}

	int cci_isolevel;
	switch(isoLevel)
	{
	case ISOLATIONLEVEL_READUNCOMMITTED:
		cci_isolevel = TRAN_COMMIT_CLASS_UNCOMMIT_INSTANCE; break;
	case ISOLATIONLEVEL_READCOMMITTED:
		cci_isolevel = TRAN_COMMIT_CLASS_COMMIT_INSTANCE; break;
	case ISOLATIONLEVEL_REPEATABLEREAD:
		cci_isolevel = TRAN_REP_CLASS_REP_INSTANCE; break;
	case ISOLATIONLEVEL_SERIALIZABLE:
		cci_isolevel = TRAN_SERIALIZABLE; break;
	default:
		return XACT_E_ISOLATIONLEVEL;
	}

	if(!bCheckOnly)
	{
		T_CCI_ERROR err_buf;
		int rc = cci_set_db_parameter(hConn, CCI_PARAM_ISOLATION_LEVEL, &cci_isolevel, &err_buf);
		if(rc<0) return E_FAIL;
		m_isoLevel = isoLevel;
	}

	ATLTRACE(atlTraceDBProvider, 2, "Current Isolation Level:[%ld]\n", isoLevel);

	return S_OK;
}

void CCUBRIDSession::EnterAutoCommitMode()
{
	m_bAutoCommit = true;

	CComVariant var;
	GetPropValue(&DBPROPSET_SESSION, DBPROP_SESS_AUTOCOMMITISOLEVELS, &var);
	SetCASCCIIsoLevel(V_I4(&var));
}

STDMETHODIMP CCUBRIDSession::GetOptionsObject(ITransactionOptions **ppOptions)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::GetOptionsObject\n");
	return DB_E_NOTSUPPORTED;
}

STDMETHODIMP CCUBRIDSession::StartTransaction(ISOLEVEL isoLevel, ULONG isoFlags,
			ITransactionOptions *pOtherOptions, ULONG *pulTransactionLevel)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::StartTransaction\n");

	// we do not support nested transactions
	if(!m_bAutoCommit) return XACT_E_XTIONEXISTS;
	if(isoFlags!=0) return XACT_E_NOISORETAIN;

	HRESULT hr = SetCASCCIIsoLevel(isoLevel);
	if(FAILED(hr)) return hr;

	// flat transaction�̹Ƿ� �׻� 1
	if(pulTransactionLevel) *pulTransactionLevel = 1;
	m_bAutoCommit = false;
	return S_OK;
}

STDMETHODIMP CCUBRIDSession::Commit(BOOL fRetaining, DWORD grfTC, DWORD grfRM)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::Commit\n");

	if(grfTC==XACTTC_ASYNC_PHASEONE || grfTC==XACTTC_SYNC_PHASEONE || grfRM!=0) return XACT_E_NOTSUPPORTED;
	if(m_bAutoCommit) return XACT_E_NOTRANSACTION;

	HRESULT hr = DoCASCCICommit(true);
	if(FAILED(hr)) return hr;

	// fRetaining==true �� transaction�� �����Ѵ�.
	if(!fRetaining) m_bAutoCommit = true; //EnterAutoCommitMode();

	return S_OK;
}
    
STDMETHODIMP CCUBRIDSession::Abort(BOID *pboidReason, BOOL fRetaining, BOOL fAsync)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::Abort\n");

	if(fAsync) return XACT_E_NOTSUPPORTED;
	if(m_bAutoCommit) return XACT_E_NOTRANSACTION;

	HRESULT hr = DoCASCCICommit(false);
	if(FAILED(hr)) return hr;

	// fRetaining==true �� transaction�� �����Ѵ�.
	if(!fRetaining) m_bAutoCommit = true; //EnterAutoCommitMode();

	return S_OK;
}

STDMETHODIMP CCUBRIDSession::GetTransactionInfo(XACTTRANSINFO *pinfo)
{
	ATLTRACE(atlTraceDBProvider, 2, "CCUBRIDSession::GetTransactionInfo\n");

	if(!pinfo) return E_INVALIDARG;
	if(m_bAutoCommit) return XACT_E_NOTRANSACTION;

	int hConn;
	HRESULT hr = GetConnectionHandle(&hConn);
	if(FAILED(hr)) return E_FAIL;

	memset(pinfo, 0, sizeof(*pinfo));
	memcpy(&pinfo->uow, &hConn, sizeof(int));
	pinfo->isoLevel = m_isoLevel;
	pinfo->grfTCSupported = XACTTC_NONE;

	return S_OK;
}

