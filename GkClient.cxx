//////////////////////////////////////////////////////////////////
//
// GkClient.cxx
//
// Copyright (c) Citron Network Inc. 2001-2002
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitely grant the right to link this code
// with the OpenH323 library.
//
// initial author: Chih-Wei Huang <cwhuang@linux.org.tw>
// initial version: 02/27/2002
//
//////////////////////////////////////////////////////////////////

#if (_MSC_VER >= 1200)
#pragma warning( disable : 4291 ) // warning about no matching operator delete
#pragma warning( disable : 4800 ) // warning about forcing value to bool
#endif

#include "GkClient.h"
#include "gk_const.h"
#include "Toolkit.h"
#include "RasSrv.h"
#include <h323pdu.h> 

const char *EndpointSection = "Endpoint";
const char *RewriteE164Section = "Endpoint::RewriteE164";
const char *H225_ProtocolID= "0.0.8.2250.0.2";

class GKPendingList : public PendingList {
public:
	GKPendingList(H323RasSrv *rs, int ttl) : PendingList(rs, ttl) {}

	bool Insert(const H225_AdmissionRequest & obj_arq, const endptr & reqEP, int);
	bool ProcessACF(const H225_RasMessage &, int);
	bool ProcessARJ(H225_CallIdentifier &, int);
};

bool GKPendingList::Insert(const H225_AdmissionRequest & obj_arq, const endptr & reqEP, int reqNum)
{
	PWaitAndSignal lock(usedLock);
	arqList.push_back(new PendingARQ(reqNum, obj_arq, reqEP, 0));
	return true;
}

bool GKPendingList::ProcessACF(const H225_RasMessage & obj_ras, int reqNum)
{
	PWaitAndSignal lock(usedLock);
	iterator Iter = FindBySeqNum(reqNum);
	if (Iter != arqList.end()) {
		endptr called = RegistrationTable::Instance()->InsertRec(const_cast<H225_RasMessage &>(obj_ras));
		if (called) {
			(*Iter)->DoACF(RasSrv, called);
			Remove(Iter);
		} else {
			PTRACE(2, "GKC\tUnable to add EP for this ACF!");
		}
		return true;
	}
	return false;
}

bool GKPendingList::ProcessARJ(H225_CallIdentifier & callid, int reqNum)
{
	PWaitAndSignal lock(usedLock);
	iterator Iter = FindBySeqNum(reqNum);
	if (Iter != arqList.end()) {
		H225_AdmissionRequest arq;
		endptr ep;
		(*Iter)->GetRequest(arq, ep);
		if (arq.HasOptionalField(H225_AdmissionRequest::e_callIdentifier))
			callid = arq.m_callIdentifier;
		// try neighbors...
		if (!RasSrv->SendLRQ(arq, ep))
			(*Iter)->DoARJ(RasSrv);
		Remove(Iter);
		return true;
	}
	return false;
}

GkClient::GkClient(H323RasSrv *rasSrv) : m_rasSrv(rasSrv)
{
	PString gk(GkConfig()->GetString(EndpointSection, "Gatekeeper", "no"));
	if (gk == "no") { // no gatekeeper to register
		m_ttl = 0;
		m_callAddr = m_rasAddr = 0;
		m_arqPendingList = 0;
		return;
	}
        PINDEX p = gk.Find(':');
        PIPSocket::GetHostAddress(gk.Left(p), m_gkaddr);
	m_gkport = (p != P_MAX_INDEX) ? gk.Mid(p+1).AsUnsigned() : GK_DEF_UNICAST_RAS_PORT;

	m_password = GkConfig()->GetString(EndpointSection, "Password", "");
	m_callAddr = new H225_TransportAddress(rasSrv->GetCallSignalAddress(m_gkaddr));
	m_rasAddr = new H225_TransportAddress(rasSrv->GetRasAddress(m_gkaddr));

	m_rewriteInfo = GkConfig()->GetAllKeyValues(RewriteE164Section);
	m_retry = GkConfig()->GetInteger(EndpointSection, "RRQRetryInterval", 10);
	m_arqPendingList = new GKPendingList(rasSrv, GkConfig()->GetInteger(EndpointSection, "ARQTimeout", 2));
	SendRRQ();
}

GkClient::~GkClient()
{
	delete m_callAddr;
	delete m_rasAddr;
	delete m_arqPendingList;
}

inline void GkClient::SendRas(const H225_RasMessage & ras)
{
	m_rasSrv->SendRas(ras, m_gkaddr, m_gkport);
}

bool GkClient::CheckGKIP(PIPSocket::Address gkip)
{
	if (gkip != m_gkaddr) {
		PTRACE(2, "GKC\tReceived RAS not from my GK? ignore!");
		return false;
	}
	return true;
}

void GkClient::CheckRegistration()
{
	if (m_ttl > 0 && (PTime() - m_registeredTime) > m_ttl)
		SendRRQ();
	if (m_arqPendingList)
		m_arqPendingList->Check();
}

void GkClient::BuildFullRRQ(H225_RegistrationRequest & rrq)
{
	rrq.m_discoveryComplete = FALSE;

	rrq.m_callSignalAddress.SetSize(1);
	rrq.m_callSignalAddress[0] = *m_callAddr;

	rrq.m_terminalType.IncludeOptionalField(H225_EndpointType::e_gatekeeper);

	PINDEX as, p;
	PString t(GkConfig()->GetString(EndpointSection, "Type", "gateway").ToLower());
	if (t[0] == 't') {
		rrq.m_terminalType.IncludeOptionalField(H225_EndpointType::e_terminal);
	} else if (t[0] == 'g') {
		rrq.m_terminalType.IncludeOptionalField(H225_EndpointType::e_gateway);
		PString prefix(GkConfig()->GetString(EndpointSection, "Prefix", ""));
		PStringArray prefixes=prefix.Tokenise(",;", FALSE);
		as = prefixes.GetSize();
		if (as > 0) {
			rrq.m_terminalType.m_gateway.IncludeOptionalField(H225_GatewayInfo::e_protocol);
			rrq.m_terminalType.m_gateway.m_protocol.SetSize(1);
			H225_SupportedProtocols & protocol = rrq.m_terminalType.m_gateway.m_protocol[0];
			protocol.SetTag(H225_SupportedProtocols::e_voice);
			H225_VoiceCaps & voicecap = (H225_VoiceCaps &)protocol;
			voicecap.m_supportedPrefixes.SetSize(as);
			for (PINDEX p = 0; p < as; ++p)
				H323SetAliasAddress(prefixes[p], voicecap.m_supportedPrefixes[p].m_prefix);
		}
		rrq.IncludeOptionalField(H225_RegistrationRequest::e_multipleCalls);
		rrq.m_multipleCalls = TRUE;
	} // else what?

	rrq.IncludeOptionalField(H225_RegistrationRequest::e_terminalAlias);
	PString h323id(GkConfig()->GetString(EndpointSection, "H323ID", "OpenH323Gatekeeper"));
	PStringArray h323ids=h323id.Tokenise(" ,;\t", FALSE);
	as = h323ids.GetSize();
	rrq.m_terminalAlias.SetSize(as);
	for (p = 0; p < as; ++p)
		H323SetAliasAddress(h323ids[p], rrq.m_terminalAlias[p]);
	m_h323Id = h323ids[0];

	PString e164(GkConfig()->GetString(EndpointSection, "E164", ""));
	PStringArray e164s=e164.Tokenise(" ,;\t", FALSE);
	PINDEX s = e164s.GetSize() + as;
	rrq.m_terminalAlias.SetSize(s);
	for (p = as; p < s; ++p)
		H323SetAliasAddress(e164s[p-as], rrq.m_terminalAlias[p]);
	m_e164 = e164s[0];

	int ttl = GkConfig()->GetInteger(EndpointSection, "TimeToLive", 0);
	if (ttl > 0) {
		rrq.IncludeOptionalField(H225_RegistrationRequest::e_timeToLive);
		rrq.m_timeToLive = ttl;
	}

	rrq.m_keepAlive = FALSE;
	SetPassword(rrq);
}

void GkClient::BuildLightWeightRRQ(H225_RegistrationRequest & rrq)
{
	rrq.m_discoveryComplete = TRUE;

	rrq.IncludeOptionalField(H225_RegistrationRequest::e_endpointIdentifier);
	rrq.m_endpointIdentifier = m_endpointId;
	rrq.IncludeOptionalField(H225_RegistrationRequest::e_gatekeeperIdentifier);
	rrq.m_gatekeeperIdentifier = m_gatekeeperId;
	rrq.m_keepAlive = TRUE;
	SetPassword(rrq);
}

void GkClient::SendRRQ()
{
	m_ttl = m_retry * 1000;
	H225_RasMessage rrq_ras;
	rrq_ras.SetTag(H225_RasMessage::e_registrationRequest);
	H225_RegistrationRequest & rrq = rrq_ras;

	rrq.m_requestSeqNum = m_rasSrv->GetRequestSeqNum();
	rrq.m_protocolIdentifier.SetValue(H225_ProtocolID);
	rrq.m_discoveryComplete = FALSE;
        rrq.m_rasAddress.SetSize(1);
	rrq.m_rasAddress[0] = *m_rasAddr;

	IsRegistered() ? BuildLightWeightRRQ(rrq) : BuildFullRRQ(rrq);
	m_registeredTime = PTime();
	SendRas(rrq_ras);
}

void GkClient::OnRCF(const H225_RegistrationConfirm & rcf, PIPSocket::Address gkip)
{
	if (!CheckGKIP(gkip))
		return;

	if (!IsRegistered()) {
		PTRACE(2, "GKC\tRegister successfully to GK " << m_gkaddr);
		m_endpointId = rcf.m_endpointIdentifier;
		m_gatekeeperId = rcf.m_gatekeeperIdentifier;
	}
	m_ttl = rcf.HasOptionalField(H225_RegistrationConfirm::e_timeToLive) ?
		(rcf.m_timeToLive - m_retry) * 1000 : 0;
}

void GkClient::OnRRJ(const H225_RegistrationReject & rrj, PIPSocket::Address gkip)
{
	if (!CheckGKIP(gkip))
		return;
	PTRACE(1, "GKC\tRegistration Rejected: " << rrj.m_rejectReason.GetTagName());
	m_endpointId = PString();

	if (rrj.m_rejectReason.GetTag() == H225_RegistrationRejectReason::e_fullRegistrationRequired)
		SendRRQ();
	else
		m_ttl = m_retry * 1000;
}

void GkClient::SendURQ()
{
	H225_RasMessage urq_ras;
	urq_ras.SetTag(H225_RasMessage::e_unregistrationRequest);
	H225_UnregistrationRequest & urq = urq_ras;
	urq.m_requestSeqNum = m_rasSrv->GetRequestSeqNum();
	urq.IncludeOptionalField(H225_UnregistrationRequest::e_gatekeeperIdentifier);
	urq.m_gatekeeperIdentifier = m_gatekeeperId;
	urq.IncludeOptionalField(H225_UnregistrationRequest::e_endpointIdentifier);
	urq.m_endpointIdentifier = m_endpointId;
	urq.m_callSignalAddress.SetSize(1);
	urq.m_callSignalAddress[0] = *m_callAddr;
	SetPassword(urq);

	m_endpointId = PString();
	SendRas(urq_ras);
}

bool GkClient::OnURQ(const H225_UnregistrationRequest & urq, PIPSocket::Address gkip)
{
	if (!CheckGKIP(gkip) || m_endpointId != urq.m_endpointIdentifier.GetValue()) // not me?
		return false;

	m_endpointId = PString();
	switch (urq.m_reason.GetTag())
	{
		case H225_UnregRequestReason::e_reregistrationRequired:
		case H225_UnregRequestReason::e_ttlExpired:
			SendRRQ();
			break;

		default:
			m_registeredTime = PTime();
			m_ttl = m_retry * 1000;
			break;
	}
	return true;
}

int GkClient::BuildARQ(H225_AdmissionRequest & arq)
{
	// Don't set call model, let the GK decide it
	arq.RemoveOptionalField(H225_AdmissionRequest::e_callModel); 

	arq.m_endpointIdentifier = m_endpointId;

	arq.IncludeOptionalField(H225_AdmissionRequest::e_srcCallSignalAddress);
	arq.m_srcCallSignalAddress = *m_callAddr;

	arq.IncludeOptionalField(H225_AdmissionRequest::e_gatekeeperIdentifier);
	arq.m_gatekeeperIdentifier = m_gatekeeperId;
	SetPassword(arq);

	return (arq.m_requestSeqNum = m_rasSrv->GetRequestSeqNum());
}

void GkClient::SendARQ(const H225_AdmissionRequest & arq, const endptr & reqEP)
{
	H225_RasMessage arq_ras;
	arq_ras.SetTag(H225_RasMessage::e_admissionRequest);
	H225_AdmissionRequest & arq_obj = arq_ras;
	arq_obj = arq; // copy and then modify
	int reqNum = BuildARQ(arq_obj);

	RewriteE164(arq_obj.m_srcInfo, true);

	m_arqPendingList->Insert(arq, reqEP, reqNum);
	SendRas(arq_ras);
}

void GkClient::SendARQ(const H225_Setup_UUIE & setup, unsigned crv, const callptr & call)
{
	H225_RasMessage arq_ras;
	arq_ras.SetTag(H225_RasMessage::e_admissionRequest);
	H225_AdmissionRequest & arq = arq_ras;
	int reqNum = BuildARQ(arq);

	arq.m_callReferenceValue = crv;
	arq.m_conferenceID = setup.m_conferenceID;
	if (setup.HasOptionalField(H225_Setup_UUIE::e_callIdentifier))
		arq.IncludeOptionalField(H225_AdmissionRequest::e_callIdentifier);
	if (setup.HasOptionalField(H225_Setup_UUIE::e_sourceAddress)) {
		arq.m_srcInfo = setup.m_sourceAddress;
	} else {
		// no sourceAddress privided in Q.931 Setup?
		// since srcInfo is mandatory, set my aliases as the srcInfo
		arq.m_srcInfo.SetSize(1);
		H323SetAliasAddress(m_h323Id, arq.m_srcInfo[0]);
		if (!m_e164) {
			arq.m_srcInfo.SetSize(2);
			H323SetAliasAddress(m_e164, arq.m_srcInfo[1]);
		}
	}
	if (setup.HasOptionalField(H225_Setup_UUIE::e_destinationAddress)) {
		arq.IncludeOptionalField(H225_AdmissionRequest::e_destinationInfo);
		arq.m_destinationInfo = setup.m_destinationAddress;
		RewriteE164(arq.m_destinationInfo, true);
	}
	arq.m_answerCall = TRUE;

	m_arqAnsweredList[reqNum] = call;
	SendRas(arq_ras);
}

void GkClient::OnACF(const H225_RasMessage & obj_acf, PIPSocket::Address gkip)
{
	if (!CheckGKIP(gkip))
		return;

	const H225_AdmissionConfirm & acf = obj_acf;
	int reqNum = acf.m_requestSeqNum.GetValue();
	if (m_arqPendingList->ProcessACF(obj_acf, reqNum))
		return;

	// an ACF to an answer ARQ
	iterator Iter = m_arqAnsweredList.find(reqNum);
	if (Iter != m_arqAnsweredList.end()) {
		m_arqAnsweredList.erase(Iter);
	} else {
		PTRACE(2, "GKC\tUnknown ACF, ignore!");
	}
}

void GkClient::OnARJ(const H225_RasMessage & obj_arj, PIPSocket::Address gkip)
{
	if (!CheckGKIP(gkip))
		return;

	const H225_AdmissionReject & arj = obj_arj;
	int reqNum = arj.m_requestSeqNum.GetValue();
	H225_CallIdentifier callid;
	if (m_arqPendingList->ProcessARJ(callid, reqNum))
		return;

	// an ARJ to an answer ARQ
	iterator Iter = m_arqAnsweredList.find(reqNum);
	if (Iter != m_arqAnsweredList.end()) {
		callptr call = Iter->second;
		PTRACE(2, "GKC\tGot ARJ for call " << call->GetCallNumber() << ", reason " << arj.m_rejectReason.GetTagName());
		// TODO: routeCallToGatekeeper
		call->Disconnect(true);
		m_arqAnsweredList.erase(Iter);
	} else {
		PTRACE(2, "GKC\tUnknown ARJ, ignore!");
	}
}

bool GkClient::OnDRQ(const H225_DisengageRequest & drq, PIPSocket::Address gkip)
{
	callptr call = drq.HasOptionalField(H225_DisengageRequest::e_callIdentifier) ? CallTable::Instance()->FindCallRec(drq.m_callIdentifier) : CallTable::Instance()->FindCallRec(drq.m_callReferenceValue);
	if (m_gkaddr == gkip && drq.m_endpointIdentifier.GetValue() == m_endpointId) {
		if (call)
			call->Disconnect(true);
		return true;
	} else if (call && call->IsRegistered()) {
		H225_RasMessage drq_ras;
		drq_ras.SetTag(H225_RasMessage::e_disengageRequest);
		H225_DisengageRequest & drq_obj = drq_ras;
		drq_obj = drq;
		drq_obj.m_endpointIdentifier = m_endpointId;
		drq_obj.m_gatekeeperIdentifier = m_gatekeeperId;
		SetPassword(drq_obj);
		SendRas(drq_ras);
	}
	return false;
}

bool GkClient::RewriteE164(H225_AliasAddress & alias, bool fromInternal)
{
	if (alias.GetTag() != H225_AliasAddress::e_dialedDigits) 
		return false;
		        
	PString e164 = H323GetAliasAddressString(alias);

	bool changed = RewriteString(e164, fromInternal);
	if (changed)
		H323SetAliasAddress(e164, alias);

	return changed;
}

bool GkClient::RewriteE164(H225_ArrayOf_AliasAddress & aliases, bool fromInternal)
{
	bool changed = false;
	for (PINDEX i = 0; i < aliases.GetSize(); ++i)
		if (RewriteE164(aliases[i], fromInternal))
			changed = true;
	return changed;
}

bool GkClient::RewriteE164(Q931 & SetupMesg, H225_Setup_UUIE & Setup, bool fromInternal)
{
	unsigned plan, type;
	PString Number;

	if (fromInternal) {
		SetupMesg.GetCallingPartyNumber(Number, &plan, &type);
		if (RewriteString(Number, true)) {
			SetupMesg.SetCallingPartyNumber(Number, plan, type);
			if (Setup.HasOptionalField(H225_Setup_UUIE::e_sourceAddress))
				RewriteE164(Setup.m_sourceAddress, true); 
			return true;
		}
	} else {
		SetupMesg.GetCalledPartyNumber(Number, &plan, &type);
		if (RewriteString(Number, false)) {
			SetupMesg.SetCalledPartyNumber(Number, plan, type);
			if (Setup.HasOptionalField(H225_Setup_UUIE::e_destinationAddress))
				RewriteE164(Setup.m_destinationAddress, false);
			return true;
		}
	}
	return false;
}

bool GkClient::RewriteString(PString & alias, bool fromInternal)
{
	for (int i = m_rewriteInfo.GetSize(); --i >= 0; ) {
		PString prefix, insert;
		if (fromInternal) {
			insert = m_rewriteInfo.GetKeyAt(i);
			prefix = m_rewriteInfo.GetDataAt(i);
		} else {
			prefix = m_rewriteInfo.GetKeyAt(i);
			insert = m_rewriteInfo.GetDataAt(i);
		}
		if (prefix.IsEmpty() || alias.Find(prefix) == 0) {
			alias = insert + alias.Mid(prefix.GetLength());
			return true;
		}
	}
	return false;
}

void GkClient::SetCryptoTokens(H225_ArrayOf_CryptoH323Token & cryptoTokens, const PString & id)
{
//	auth.SetLocalId(m_h323Id);
	auth.SetLocalId(id);
	auth.SetPassword(m_password);
	auth.Prepare(cryptoTokens); 
}

