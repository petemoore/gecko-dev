/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// XXX: This must be done prior to including cert.h (directly or indirectly).
// CERT_AddTempCertToPerm is exposed as __CERT_AddTempCertToPerm, but it is
// only exported so PSM can use it for this specific purpose.
#define CERT_AddTempCertToPerm __CERT_AddTempCertToPerm

#include "nsNSSCertificateDB.h"

#include "CertVerifier.h"
#include "ExtendedValidation.h"
#include "NSSCertDBTrustDomain.h"
#include "SharedSSLState.h"
#include "mozilla/Base64.h"
#include "mozilla/unused.h"
#include "nsArray.h"
#include "nsArrayUtils.h"
#include "nsCOMPtr.h"
#include "nsCRT.h"
#include "nsComponentManagerUtils.h"
#include "nsICertificateDialogs.h"
#include "nsIFile.h"
#include "nsIMutableArray.h"
#include "nsIObserverService.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "nsIPrompt.h"
#include "nsNSSCertHelper.h"
#include "nsNSSCertTrust.h"
#include "nsNSSCertificate.h"
#include "nsNSSComponent.h"
#include "nsNSSHelper.h"
#include "nsNSSShutDown.h"
#include "nsPK11TokenDB.h"
#include "nsPKCS12Blob.h"
#include "nsReadableUtils.h"
#include "nsThreadUtils.h"
#include "pkix/Time.h"
#include "pkix/pkixtypes.h"

#include "nspr.h"
#include "certdb.h"
#include "secerr.h"
#include "nssb64.h"
#include "secasn1.h"
#include "secder.h"
#include "ssl.h"
#include "plbase64.h"

#ifdef XP_WIN
#include <winsock.h> // for ntohl
#endif

using namespace mozilla;
using namespace mozilla::psm;
using mozilla::psm::SharedSSLState;

extern LazyLogModule gPIPNSSLog;

static nsresult
attemptToLogInWithDefaultPassword()
{
#ifdef NSS_DISABLE_DBM
  // The SQL NSS DB requires the user to be authenticated to set certificate
  // trust settings, even if the user's password is empty. To maintain
  // compatibility with the DBM-based database, try to log in with the
  // default empty password. This will allow, at least, tests that need to
  // change certificate trust to pass on all platforms. TODO(bug 978120): Do
  // proper testing and/or implement a better solution so that we are confident
  // that this does the correct thing outside of xpcshell tests too.
  ScopedPK11SlotInfo slot(PK11_GetInternalKeySlot());
  if (!slot) {
    return MapSECStatus(SECFailure);
  }
  if (PK11_NeedUserInit(slot)) {
    // Ignore the return value. Presumably PK11_InitPin will fail if the user
    // has a non-default password.
    (void) PK11_InitPin(slot, nullptr, nullptr);
  }
#endif

  return NS_OK;
}

NS_IMPL_ISUPPORTS(nsNSSCertificateDB, nsIX509CertDB)

nsNSSCertificateDB::~nsNSSCertificateDB()
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return;
  }

  shutdown(calledFromObject);
}

NS_IMETHODIMP
nsNSSCertificateDB::FindCertByNickname(const nsAString& nickname,
                                       nsIX509Cert** _rvCert)
{
  NS_ENSURE_ARG_POINTER(_rvCert);
  *_rvCert = nullptr;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  ScopedCERTCertificate cert;
  char *asciiname = nullptr;
  NS_ConvertUTF16toUTF8 aUtf8Nickname(nickname);
  asciiname = const_cast<char*>(aUtf8Nickname.get());
  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("Getting \"%s\"\n", asciiname));
  cert = PK11_FindCertFromNickname(asciiname, nullptr);
  if (!cert) {
    cert = CERT_FindCertByNickname(CERT_GetDefaultCertDB(), asciiname);
  }
  if (cert) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("got it\n"));
    nsCOMPtr<nsIX509Cert> pCert = nsNSSCertificate::Create(cert.get());
    if (pCert) {
      pCert.forget(_rvCert);
      return NS_OK;
    }
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsNSSCertificateDB::FindCertByDBKey(const char* aDBKey,nsIX509Cert** _cert)
{
  NS_ENSURE_ARG_POINTER(aDBKey);
  NS_ENSURE_ARG(aDBKey[0]);
  NS_ENSURE_ARG_POINTER(_cert);
  *_cert = nullptr;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  UniqueCERTCertificate cert;
  nsresult rv = FindCertByDBKey(aDBKey, cert);
  if (NS_FAILED(rv)) {
    return rv;
  }
  // If we can't find the certificate, that's not an error. Just return null.
  if (!cert) {
    return NS_OK;
  }
  nsCOMPtr<nsIX509Cert> nssCert = nsNSSCertificate::Create(cert.get());
  if (!nssCert) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  nssCert.forget(_cert);
  return NS_OK;
}

nsresult
nsNSSCertificateDB::FindCertByDBKey(const char* aDBKey,
                                    UniqueCERTCertificate& cert)
{
  static_assert(sizeof(uint64_t) == 8, "type size sanity check");
  static_assert(sizeof(uint32_t) == 4, "type size sanity check");
  // (From nsNSSCertificate::GetDbKey)
  // The format of the key is the base64 encoding of the following:
  // 4 bytes: {0, 0, 0, 0} (this was intended to be the module ID, but it was
  //                        never implemented)
  // 4 bytes: {0, 0, 0, 0} (this was intended to be the slot ID, but it was
  //                        never implemented)
  // 4 bytes: <serial number length in big-endian order>
  // 4 bytes: <DER-encoded issuer distinguished name length in big-endian order>
  // n bytes: <bytes of serial number>
  // m bytes: <DER-encoded issuer distinguished name>
  nsAutoCString decoded;
  nsAutoCString tmpDBKey(aDBKey);
  // Filter out any whitespace for backwards compatibility.
  tmpDBKey.StripWhitespace();
  nsresult rv = Base64Decode(tmpDBKey, decoded);
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (decoded.Length() < 16) {
    return NS_ERROR_ILLEGAL_INPUT;
  }
  const char* reader = decoded.BeginReading();
  uint64_t zeroes = *reinterpret_cast<const uint64_t*>(reader);
  if (zeroes != 0) {
    return NS_ERROR_ILLEGAL_INPUT;
  }
  reader += sizeof(uint64_t);
  uint32_t serialNumberLen = ntohl(*reinterpret_cast<const uint32_t*>(reader));
  reader += sizeof(uint32_t);
  uint32_t issuerLen = ntohl(*reinterpret_cast<const uint32_t*>(reader));
  reader += sizeof(uint32_t);
  if (decoded.Length() != 16ULL + serialNumberLen + issuerLen) {
    return NS_ERROR_ILLEGAL_INPUT;
  }
  CERTIssuerAndSN issuerSN;
  issuerSN.serialNumber.len = serialNumberLen;
  issuerSN.serialNumber.data = (unsigned char*)reader;
  reader += serialNumberLen;
  issuerSN.derIssuer.len = issuerLen;
  issuerSN.derIssuer.data = (unsigned char*)reader;
  reader += issuerLen;
  MOZ_ASSERT(reader == decoded.EndReading());

  cert.reset(CERT_FindCertByIssuerAndSN(CERT_GetDefaultCertDB(), &issuerSN));
  return NS_OK;
}

SECStatus
collect_certs(void *arg, SECItem **certs, int numcerts)
{
  CERTDERCerts *collectArgs;
  SECItem *cert;
  SECStatus rv;

  collectArgs = (CERTDERCerts *)arg;

  collectArgs->numcerts = numcerts;
  collectArgs->rawCerts = (SECItem *) PORT_ArenaZAlloc(collectArgs->arena,
                                           sizeof(SECItem) * numcerts);
  if (!collectArgs->rawCerts)
    return(SECFailure);

  cert = collectArgs->rawCerts;

  while ( numcerts-- ) {
    rv = SECITEM_CopyItem(collectArgs->arena, cert, *certs);
    if ( rv == SECFailure )
      return(SECFailure);
    cert++;
    certs++;
  }

  return (SECSuccess);
}

CERTDERCerts*
nsNSSCertificateDB::getCertsFromPackage(const UniquePLArenaPool& arena,
                                        uint8_t* data, uint32_t length,
                                        const nsNSSShutDownPreventionLock& /*proofOfLock*/)
{
  CERTDERCerts* collectArgs =
    (CERTDERCerts*)PORT_ArenaZAlloc(arena.get(), sizeof(CERTDERCerts));
  if (!collectArgs) {
    return nullptr;
  }

  collectArgs->arena = arena.get();
  if (CERT_DecodeCertPackage(char_ptr_cast(data), length, collect_certs,
                             collectArgs) != SECSuccess) {
    return nullptr;
  }

  return collectArgs;
}

nsresult
nsNSSCertificateDB::handleCACertDownload(nsIArray *x509Certs,
                                         nsIInterfaceRequestor *ctx,
                                         const nsNSSShutDownPreventionLock &proofOfLock)
{
  // First thing we have to do is figure out which certificate we're 
  // gonna present to the user.  The CA may have sent down a list of 
  // certs which may or may not be a chained list of certs.  Until
  // the day we can design some solid UI for the general case, we'll
  // code to the > 90% case.  That case is where a CA sends down a
  // list that is a hierarchy whose root is either the first or 
  // the last cert.  What we're gonna do is compare the first 
  // 2 entries, if the second was signed by the first, we assume
  // the root cert is the first cert and display it.  Otherwise,
  // we compare the last 2 entries, if the second to last cert was
  // signed by the last cert, then we assume the last cert is the
  // root and display it.

  uint32_t numCerts;

  x509Certs->GetLength(&numCerts);
  NS_ASSERTION(numCerts > 0, "Didn't get any certs to import.");
  if (numCerts == 0)
    return NS_OK; // Nothing to import, so nothing to do.

  nsCOMPtr<nsIX509Cert> certToShow;
  nsCOMPtr<nsISupports> isupports;
  uint32_t selCertIndex;
  if (numCerts == 1) {
    // There's only one cert, so let's show it.
    selCertIndex = 0;
    certToShow = do_QueryElementAt(x509Certs, selCertIndex);
  } else {
    nsCOMPtr<nsIX509Cert> cert0;    // first cert
    nsCOMPtr<nsIX509Cert> cert1;    // second cert
    nsCOMPtr<nsIX509Cert> certn_2;  // second to last cert
    nsCOMPtr<nsIX509Cert> certn_1;  // last cert

    cert0 = do_QueryElementAt(x509Certs, 0);
    cert1 = do_QueryElementAt(x509Certs, 1);
    certn_2 = do_QueryElementAt(x509Certs, numCerts-2);
    certn_1 = do_QueryElementAt(x509Certs, numCerts-1);

    nsXPIDLString cert0SubjectName;
    nsXPIDLString cert1IssuerName;
    nsXPIDLString certn_2IssuerName;
    nsXPIDLString certn_1SubjectName;

    cert0->GetSubjectName(cert0SubjectName);
    cert1->GetIssuerName(cert1IssuerName);
    certn_2->GetIssuerName(certn_2IssuerName);
    certn_1->GetSubjectName(certn_1SubjectName);

    if (cert1IssuerName.Equals(cert0SubjectName)) {
      // In this case, the first cert in the list signed the second,
      // so the first cert is the root.  Let's display it. 
      selCertIndex = 0;
      certToShow = cert0;
    } else 
    if (certn_2IssuerName.Equals(certn_1SubjectName)) { 
      // In this case the last cert has signed the second to last cert.
      // The last cert is the root, so let's display it.
      selCertIndex = numCerts-1;
      certToShow = certn_1;
    } else {
      // It's not a chain, so let's just show the first one in the 
      // downloaded list.
      selCertIndex = 0;
      certToShow = cert0;
    }
  }

  if (!certToShow)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsICertificateDialogs> dialogs;
  nsresult rv = ::getNSSDialogs(getter_AddRefs(dialogs), 
                                NS_GET_IID(nsICertificateDialogs),
                                NS_CERTIFICATEDIALOGS_CONTRACTID);
                       
  if (NS_FAILED(rv))
    return rv;
 
  SECItem der;
  rv=certToShow->GetRawDER(&der.len, (uint8_t **)&der.data);

  if (NS_FAILED(rv))
    return rv;

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("Creating temp cert\n"));
  CERTCertDBHandle *certdb = CERT_GetDefaultCertDB();
  ScopedCERTCertificate tmpCert(CERT_FindCertByDERCert(certdb, &der));
  if (!tmpCert) {
    tmpCert = CERT_NewTempCertificate(certdb, &der,
                                      nullptr, false, true);
  }
  free(der.data);
  der.data = nullptr;
  der.len = 0;
  
  if (!tmpCert) {
    NS_ERROR("Couldn't create cert from DER blob");
    return NS_ERROR_FAILURE;
  }

  if (!CERT_IsCACert(tmpCert.get(), nullptr)) {
    DisplayCertificateAlert(ctx, "NotACACert", certToShow, proofOfLock);
    return NS_ERROR_FAILURE;
  }

  if (tmpCert->isperm) {
    DisplayCertificateAlert(ctx, "CaCertExists", certToShow, proofOfLock);
    return NS_ERROR_FAILURE;
  }

  uint32_t trustBits;
  bool allows;
  rv = dialogs->ConfirmDownloadCACert(ctx, certToShow, &trustBits, &allows);
  if (NS_FAILED(rv))
    return rv;

  if (!allows)
    return NS_ERROR_NOT_AVAILABLE;

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("trust is %d\n", trustBits));
  nsXPIDLCString nickname;
  nickname.Adopt(CERT_MakeCANickname(tmpCert.get()));

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("Created nick \"%s\"\n", nickname.get()));

  nsNSSCertTrust trust;
  trust.SetValidCA();
  trust.AddCATrust(!!(trustBits & nsIX509CertDB::TRUSTED_SSL),
                   !!(trustBits & nsIX509CertDB::TRUSTED_EMAIL),
                   !!(trustBits & nsIX509CertDB::TRUSTED_OBJSIGN));

  SECStatus srv = __CERT_AddTempCertToPerm(tmpCert.get(),
                                           const_cast<char*>(nickname.get()),
                                           trust.GetTrust());

  if (srv != SECSuccess)
    return NS_ERROR_FAILURE;

  // Import additional delivered certificates that can be verified.

  // build a CertList for filtering
  UniqueCERTCertList certList(CERT_NewCertList());
  if (!certList) {
    return NS_ERROR_FAILURE;
  }

  // get all remaining certs into temp store

  for (uint32_t i=0; i<numCerts; i++) {
    if (i == selCertIndex) {
      // we already processed that one
      continue;
    }

    certToShow = do_QueryElementAt(x509Certs, i);
    certToShow->GetRawDER(&der.len, (uint8_t **)&der.data);

    CERTCertificate *tmpCert2 = 
      CERT_NewTempCertificate(certdb, &der, nullptr, false, true);

    free(der.data);
    der.data = nullptr;
    der.len = 0;

    if (!tmpCert2) {
      NS_ERROR("Couldn't create temp cert from DER blob");
      continue;  // Let's try to import the rest of 'em
    }

    CERT_AddCertToListTail(certList.get(), tmpCert2);
  }

  return ImportValidCACertsInList(certList, ctx, proofOfLock);
}

NS_IMETHODIMP
nsNSSCertificateDB::ImportCertificates(uint8_t* data, uint32_t length,
                                       uint32_t type,
                                       nsIInterfaceRequestor* ctx)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // We currently only handle CA certificates.
  if (type != nsIX509Cert::CA_CERT) {
    return NS_ERROR_FAILURE;
  }

  UniquePLArenaPool arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
  if (!arena) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  CERTDERCerts* certCollection = getCertsFromPackage(arena, data, length,
                                                     locker);
  if (!certCollection) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIMutableArray> array = nsArrayBase::Create();
  if (!array) {
    return NS_ERROR_FAILURE;
  }

  // Now let's create some certs to work with
  for (int i = 0; i < certCollection->numcerts; i++) {
    SECItem* currItem = &certCollection->rawCerts[i];
    nsCOMPtr<nsIX509Cert> cert =
      nsNSSCertificate::ConstructFromDER(reinterpret_cast<char*>(currItem->data),
                                         currItem->len);
    if (!cert) {
      return NS_ERROR_FAILURE;
    }
    nsresult rv = array->AppendElement(cert, false);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  return handleCACertDownload(array, ctx, locker);
}

/**
 * Filters an array of certs by usage and imports them into temporary storage.
 *
 * @param numcerts
 *        Size of the |certs| array.
 * @param certs
 *        Pointer to array of certs to import.
 * @param usage
 *        Usage the certs should be filtered on.
 * @param caOnly
 *        Whether to import only CA certs.
 * @param filteredCerts
 *        List of certs that weren't filtered out and were successfully imported.
 */
static nsresult
ImportCertsIntoTempStorage(int numcerts, SECItem* certs,
                           const SECCertUsage usage, const bool caOnly,
                           const nsNSSShutDownPreventionLock& /*proofOfLock*/,
                   /*out*/ const UniqueCERTCertList& filteredCerts)
{
  NS_ENSURE_ARG_MIN(numcerts, 1);
  NS_ENSURE_ARG_POINTER(certs);
  NS_ENSURE_ARG_POINTER(filteredCerts.get());

  // CERT_ImportCerts() expects an array of *pointers* to SECItems, so we have
  // to convert |certs| to such a format first.
  SECItem** ptrArray =
    static_cast<SECItem**>(PORT_Alloc(sizeof(SECItem*) * numcerts));
  if (!ptrArray) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  for (int i = 0; i < numcerts; i++) {
    ptrArray[i] = &certs[i];
  }

  CERTCertificate** importedCerts = nullptr;
  SECStatus srv = CERT_ImportCerts(CERT_GetDefaultCertDB(), usage,
                                   numcerts, ptrArray, &importedCerts, false,
                                   caOnly, nullptr);
  PORT_Free(ptrArray);
  ptrArray = nullptr;
  if (srv != SECSuccess) {
    return NS_ERROR_FAILURE;
  }

  for (int i = 0; i < numcerts; i++) {
    if (!importedCerts[i]) {
      continue;
    }

    UniqueCERTCertificate cert(CERT_DupCertificate(importedCerts[i]));
    if (!cert) {
      continue;
    }

    if (CERT_AddCertToListTail(filteredCerts.get(), cert.get()) == SECSuccess) {
      Unused << cert.release();
    }
  }

  CERT_DestroyCertArray(importedCerts, numcerts);

  // CERT_ImportCerts() ignores its |usage| parameter, so we have to manually
  // filter out unwanted certs.
  if (CERT_FilterCertListByUsage(filteredCerts.get(), usage, caOnly)
        != SECSuccess) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

static SECStatus
ImportCertsIntoPermanentStorage(const ScopedCERTCertList& certChain,
                                const SECCertUsage usage, const bool caOnly)
{
  int chainLen = 0;
  for (CERTCertListNode *chainNode = CERT_LIST_HEAD(certChain);
       !CERT_LIST_END(chainNode, certChain);
       chainNode = CERT_LIST_NEXT(chainNode)) {
    chainLen++;
  }

  SECItem **rawArray;
  rawArray = (SECItem **) PORT_Alloc(chainLen * sizeof(SECItem *));
  if (!rawArray) {
    return SECFailure;
  }

  int i = 0;
  for (CERTCertListNode *chainNode = CERT_LIST_HEAD(certChain);
       !CERT_LIST_END(chainNode, certChain);
       chainNode = CERT_LIST_NEXT(chainNode), i++) {
    rawArray[i] = &chainNode->cert->derCert;
  }
  SECStatus srv = CERT_ImportCerts(CERT_GetDefaultCertDB(), usage, chainLen,
                                   rawArray, nullptr, true, caOnly, nullptr);

  PORT_Free(rawArray);
  return srv;
}

NS_IMETHODIMP
nsNSSCertificateDB::ImportEmailCertificate(uint8_t* data, uint32_t length,
                                           nsIInterfaceRequestor* ctx)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  UniquePLArenaPool arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
  if (!arena) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  CERTDERCerts *certCollection = getCertsFromPackage(arena, data, length, locker);
  if (!certCollection) {
    return NS_ERROR_FAILURE;
  }

  UniqueCERTCertList filteredCerts(CERT_NewCertList());
  if (!filteredCerts) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv = ImportCertsIntoTempStorage(certCollection->numcerts,
                                           certCollection->rawCerts,
                                           certUsageEmailRecipient,
                                           false, locker, filteredCerts);
  if (NS_FAILED(rv)) {
    return rv;
  }

  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  if (!certVerifier) {
    return NS_ERROR_UNEXPECTED;
  }

  // Iterate through the filtered cert list and import verified certs into
  // permanent storage.
  // Note: We verify the certs in order to prevent DoS attacks. See Bug 249004.
  for (CERTCertListNode* node = CERT_LIST_HEAD(filteredCerts.get());
       !CERT_LIST_END(node, filteredCerts.get());
       node = CERT_LIST_NEXT(node)) {
    if (!node->cert) {
      continue;
    }

    ScopedCERTCertList certChain;
    SECStatus srv = certVerifier->VerifyCert(node->cert,
                                             certificateUsageEmailRecipient,
                                             mozilla::pkix::Now(), ctx,
                                             nullptr, certChain);
    if (srv != SECSuccess) {
      nsCOMPtr<nsIX509Cert> certToShow = nsNSSCertificate::Create(node->cert);
      DisplayCertificateAlert(ctx, "NotImportingUnverifiedCert", certToShow, locker);
      continue;
    }
    srv = ImportCertsIntoPermanentStorage(certChain, certUsageEmailRecipient,
                                          false);
    if (srv != SECSuccess) {
      return NS_ERROR_FAILURE;
    }
    CERT_SaveSMimeProfile(node->cert, nullptr, nullptr);
  }

  return NS_OK;
}

nsresult
nsNSSCertificateDB::ImportValidCACerts(int numCACerts, SECItem* caCerts,
                                       nsIInterfaceRequestor* ctx,
                                       const nsNSSShutDownPreventionLock& proofOfLock)
{
  UniqueCERTCertList filteredCerts(CERT_NewCertList());
  if (!filteredCerts) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv = ImportCertsIntoTempStorage(numCACerts, caCerts, certUsageAnyCA,
                                           true, proofOfLock, filteredCerts);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return ImportValidCACertsInList(filteredCerts, ctx, proofOfLock);
}

nsresult
nsNSSCertificateDB::ImportValidCACertsInList(const UniqueCERTCertList& filteredCerts,
                                             nsIInterfaceRequestor* ctx,
                                             const nsNSSShutDownPreventionLock& proofOfLock)
{
  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  if (!certVerifier) {
    return NS_ERROR_UNEXPECTED;
  }

  // Iterate through the filtered cert list and import verified certs into
  // permanent storage.
  // Note: We verify the certs in order to prevent DoS attacks. See Bug 249004.
  for (CERTCertListNode* node = CERT_LIST_HEAD(filteredCerts.get());
       !CERT_LIST_END(node, filteredCerts.get());
       node = CERT_LIST_NEXT(node)) {
    ScopedCERTCertList certChain;
    SECStatus rv = certVerifier->VerifyCert(node->cert,
                                            certificateUsageVerifyCA,
                                            mozilla::pkix::Now(), ctx,
                                            nullptr, certChain);
    if (rv != SECSuccess) {
      nsCOMPtr<nsIX509Cert> certToShow = nsNSSCertificate::Create(node->cert);
      DisplayCertificateAlert(ctx, "NotImportingUnverifiedCert", certToShow, proofOfLock);
      continue;
    }

    rv = ImportCertsIntoPermanentStorage(certChain, certUsageAnyCA, true);
    if (rv != SECSuccess) {
      return NS_ERROR_FAILURE;
    }
  }

  return NS_OK;
}

void nsNSSCertificateDB::DisplayCertificateAlert(nsIInterfaceRequestor *ctx, 
                                                 const char *stringID, 
                                                 nsIX509Cert *certToShow,
                                                 const nsNSSShutDownPreventionLock &/*proofOfLock*/)
{
  static NS_DEFINE_CID(kNSSComponentCID, NS_NSSCOMPONENT_CID);

  if (!NS_IsMainThread()) {
    NS_ERROR("nsNSSCertificateDB::DisplayCertificateAlert called off the main thread");
    return;
  }

  nsCOMPtr<nsIInterfaceRequestor> my_ctx = ctx;
  if (!my_ctx) {
    my_ctx = new PipUIContext();
  }

  // This shall be replaced by embedding ovverridable prompts
  // as discussed in bug 310446, and should make use of certToShow.

  nsresult rv;
  nsCOMPtr<nsINSSComponent> nssComponent(do_GetService(kNSSComponentCID, &rv));
  if (NS_SUCCEEDED(rv)) {
    nsAutoString tmpMessage;
    nssComponent->GetPIPNSSBundleString(stringID, tmpMessage);

    nsCOMPtr<nsIPrompt> prompt (do_GetInterface(my_ctx));
    if (!prompt) {
      return;
    }

    prompt->Alert(nullptr, tmpMessage.get());
  }
}

NS_IMETHODIMP
nsNSSCertificateDB::ImportUserCertificate(uint8_t* data, uint32_t length,
                                          nsIInterfaceRequestor* ctx)
{
  if (!NS_IsMainThread()) {
    NS_ERROR("nsNSSCertificateDB::ImportUserCertificate called off the main thread");
    return NS_ERROR_NOT_SAME_THREAD;
  }

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  UniquePLArenaPool arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
  if (!arena) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  CERTDERCerts* collectArgs = getCertsFromPackage(arena, data, length, locker);
  if (!collectArgs) {
    return NS_ERROR_FAILURE;
  }

  UniqueCERTCertificate cert(
    CERT_NewTempCertificate(CERT_GetDefaultCertDB(), collectArgs->rawCerts,
                            nullptr, false, true));
  if (!cert) {
    return NS_ERROR_FAILURE;
  }

  UniquePK11SlotInfo slot(PK11_KeyForCertExists(cert.get(), nullptr, ctx));
  if (!slot) {
    nsCOMPtr<nsIX509Cert> certToShow = nsNSSCertificate::Create(cert.get());
    DisplayCertificateAlert(ctx, "UserCertIgnoredNoPrivateKey", certToShow, locker);
    return NS_ERROR_FAILURE;
  }
  slot = nullptr;

  /* pick a nickname for the cert */
  nsAutoCString nickname;
  if (cert->nickname) {
    nickname = cert->nickname;
  } else {
    get_default_nickname(cert.get(), ctx, nickname, locker);
  }

  /* user wants to import the cert */
  slot.reset(PK11_ImportCertForKey(cert.get(), nickname.get(), ctx));
  if (!slot) {
    return NS_ERROR_FAILURE;
  }
  slot = nullptr;

  {
    nsCOMPtr<nsIX509Cert> certToShow = nsNSSCertificate::Create(cert.get());
    DisplayCertificateAlert(ctx, "UserCertImported", certToShow, locker);
  }

  int numCACerts = collectArgs->numcerts - 1;
  if (numCACerts) {
    SECItem* caCerts = collectArgs->rawCerts + 1;
    return ImportValidCACerts(numCACerts, caCerts, ctx, locker);
  }

  return NS_OK;
}

NS_IMETHODIMP 
nsNSSCertificateDB::DeleteCertificate(nsIX509Cert *aCert)
{
  NS_ENSURE_ARG_POINTER(aCert);
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  ScopedCERTCertificate cert(aCert->GetCert());
  if (!cert) {
    return NS_ERROR_FAILURE;
  }
  SECStatus srv = SECSuccess;

  uint32_t certType;
  aCert->GetCertType(&certType);
  if (NS_FAILED(aCert->MarkForPermDeletion()))
  {
    return NS_ERROR_FAILURE;
  }

  if (cert->slot && certType != nsIX509Cert::USER_CERT) {
    // To delete a cert of a slot (builtin, most likely), mark it as
    // completely untrusted.  This way we keep a copy cached in the
    // local database, and next time we try to load it off of the 
    // external token/slot, we'll know not to trust it.  We don't 
    // want to do that with user certs, because a user may  re-store
    // the cert onto the card again at which point we *will* want to 
    // trust that cert if it chains up properly.
    nsNSSCertTrust trust(0, 0, 0);
    srv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), 
                               cert.get(), trust.GetTrust());
  }
  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("cert deleted: %d", srv));
  return (srv) ? NS_ERROR_FAILURE : NS_OK;
}

NS_IMETHODIMP 
nsNSSCertificateDB::SetCertTrust(nsIX509Cert *cert, 
                                 uint32_t type,
                                 uint32_t trusted)
{
  NS_ENSURE_ARG_POINTER(cert);
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  nsNSSCertTrust trust;
  nsresult rv;
  ScopedCERTCertificate nsscert(cert->GetCert());

  rv = attemptToLogInWithDefaultPassword();
  if (NS_WARN_IF(rv != NS_OK)) {
    return rv;
  }

  SECStatus srv;
  if (type == nsIX509Cert::CA_CERT) {
    // always start with untrusted and move up
    trust.SetValidCA();
    trust.AddCATrust(!!(trusted & nsIX509CertDB::TRUSTED_SSL),
                     !!(trusted & nsIX509CertDB::TRUSTED_EMAIL),
                     !!(trusted & nsIX509CertDB::TRUSTED_OBJSIGN));
    srv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), 
                               nsscert.get(),
                               trust.GetTrust());
  } else if (type == nsIX509Cert::SERVER_CERT) {
    // always start with untrusted and move up
    trust.SetValidPeer();
    trust.AddPeerTrust(trusted & nsIX509CertDB::TRUSTED_SSL, 0, 0);
    srv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), 
                               nsscert.get(),
                               trust.GetTrust());
  } else if (type == nsIX509Cert::EMAIL_CERT) {
    // always start with untrusted and move up
    trust.SetValidPeer();
    trust.AddPeerTrust(0, !!(trusted & nsIX509CertDB::TRUSTED_EMAIL), 0);
    srv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), 
                               nsscert.get(),
                               trust.GetTrust());
  } else {
    // ignore user certs
    return NS_OK;
  }
  return MapSECStatus(srv);
}

NS_IMETHODIMP 
nsNSSCertificateDB::IsCertTrusted(nsIX509Cert *cert, 
                                  uint32_t certType,
                                  uint32_t trustType,
                                  bool *_isTrusted)
{
  NS_ENSURE_ARG_POINTER(_isTrusted);
  *_isTrusted = false;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  SECStatus srv;
  ScopedCERTCertificate nsscert(cert->GetCert());
  CERTCertTrust nsstrust;
  srv = CERT_GetCertTrust(nsscert.get(), &nsstrust);
  if (srv != SECSuccess)
    return NS_ERROR_FAILURE;

  nsNSSCertTrust trust(&nsstrust);
  if (certType == nsIX509Cert::CA_CERT) {
    if (trustType & nsIX509CertDB::TRUSTED_SSL) {
      *_isTrusted = trust.HasTrustedCA(true, false, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_EMAIL) {
      *_isTrusted = trust.HasTrustedCA(false, true, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_OBJSIGN) {
      *_isTrusted = trust.HasTrustedCA(false, false, true);
    } else {
      return NS_ERROR_FAILURE;
    }
  } else if (certType == nsIX509Cert::SERVER_CERT) {
    if (trustType & nsIX509CertDB::TRUSTED_SSL) {
      *_isTrusted = trust.HasTrustedPeer(true, false, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_EMAIL) {
      *_isTrusted = trust.HasTrustedPeer(false, true, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_OBJSIGN) {
      *_isTrusted = trust.HasTrustedPeer(false, false, true);
    } else {
      return NS_ERROR_FAILURE;
    }
  } else if (certType == nsIX509Cert::EMAIL_CERT) {
    if (trustType & nsIX509CertDB::TRUSTED_SSL) {
      *_isTrusted = trust.HasTrustedPeer(true, false, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_EMAIL) {
      *_isTrusted = trust.HasTrustedPeer(false, true, false);
    } else if (trustType & nsIX509CertDB::TRUSTED_OBJSIGN) {
      *_isTrusted = trust.HasTrustedPeer(false, false, true);
    } else {
      return NS_ERROR_FAILURE;
    }
  } /* user: ignore */
  return NS_OK;
}


NS_IMETHODIMP
nsNSSCertificateDB::ImportCertsFromFile(nsIFile* aFile, uint32_t aType)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  NS_ENSURE_ARG(aFile);
  switch (aType) {
    case nsIX509Cert::CA_CERT:
    case nsIX509Cert::EMAIL_CERT:
      // good
      break;

    default:
      // not supported (yet)
      return NS_ERROR_FAILURE;
  }

  PRFileDesc* fd = nullptr;
  nsresult rv = aFile->OpenNSPRFileDesc(PR_RDONLY, 0, &fd);
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (!fd) {
    return NS_ERROR_FAILURE;
  }

  PRFileInfo fileInfo;
  if (PR_GetOpenFileInfo(fd, &fileInfo) != PR_SUCCESS) {
    return NS_ERROR_FAILURE;
  }

  auto buf = MakeUnique<unsigned char[]>(fileInfo.size);
  int32_t bytesObtained = PR_Read(fd, buf.get(), fileInfo.size);
  PR_Close(fd);

  if (bytesObtained != fileInfo.size) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIInterfaceRequestor> cxt = new PipUIContext();

  switch (aType) {
    case nsIX509Cert::CA_CERT:
      return ImportCertificates(buf.get(), bytesObtained, aType, cxt);
    case nsIX509Cert::EMAIL_CERT:
      return ImportEmailCertificate(buf.get(), bytesObtained, cxt);
    default:
      MOZ_ASSERT(false, "Unsupported type should have been filtered out");
      break;
  }

  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP 
nsNSSCertificateDB::ImportPKCS12File(nsISupports* aToken, nsIFile* aFile)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  NS_ENSURE_ARG(aFile);
  nsPKCS12Blob blob;
  nsCOMPtr<nsIPK11Token> token = do_QueryInterface(aToken);
  if (token) {
    blob.SetToken(token);
  }
  return blob.ImportFromFile(aFile);
}

NS_IMETHODIMP 
nsNSSCertificateDB::ExportPKCS12File(nsISupports* aToken,
                                     nsIFile* aFile,
                                     uint32_t count,
                                     nsIX509Cert** certs)
                                     //const char16_t **aCertNames)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  NS_ENSURE_ARG(aFile);
  nsPKCS12Blob blob;
  if (count == 0) return NS_OK;
  nsCOMPtr<nsIPK11Token> localRef;
  if (!aToken) {
    ScopedPK11SlotInfo keySlot(PK11_GetInternalKeySlot());
    NS_ASSERTION(keySlot,"Failed to get the internal key slot");
    localRef = new nsPK11Token(keySlot);
  }
  else {
    localRef = do_QueryInterface(aToken);
  }
  blob.SetToken(localRef);
  //blob.LoadCerts(aCertNames, count);
  //return blob.ExportToFile(aFile);
  return blob.ExportToFile(aFile, certs, count);
}

NS_IMETHODIMP
nsNSSCertificateDB::FindEmailEncryptionCert(const nsAString& aNickname,
                                            nsIX509Cert** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = nullptr;

  if (aNickname.IsEmpty())
    return NS_OK;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsIInterfaceRequestor> ctx = new PipUIContext();
  char *asciiname = nullptr;
  NS_ConvertUTF16toUTF8 aUtf8Nickname(aNickname);
  asciiname = const_cast<char*>(aUtf8Nickname.get());

  /* Find a good cert in the user's database */
  ScopedCERTCertificate cert(CERT_FindUserCertByUsage(CERT_GetDefaultCertDB(),
                                                      asciiname,
                                                      certUsageEmailRecipient,
                                                      true, ctx));
  if (!cert) {
    return NS_OK;
  }

  nsCOMPtr<nsIX509Cert> nssCert = nsNSSCertificate::Create(cert.get());
  if (!nssCert) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  nssCert.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificateDB::FindEmailSigningCert(const nsAString& aNickname,
                                         nsIX509Cert** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = nullptr;

  if (aNickname.IsEmpty())
    return NS_OK;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsIInterfaceRequestor> ctx = new PipUIContext();
  char *asciiname = nullptr;
  NS_ConvertUTF16toUTF8 aUtf8Nickname(aNickname);
  asciiname = const_cast<char*>(aUtf8Nickname.get());

  /* Find a good cert in the user's database */
  ScopedCERTCertificate cert(CERT_FindUserCertByUsage(CERT_GetDefaultCertDB(),
                                                      asciiname,
                                                      certUsageEmailSigner,
                                                      true, ctx));
  if (!cert) {
    return NS_OK;
  }

  nsCOMPtr<nsIX509Cert> nssCert = nsNSSCertificate::Create(cert.get());
  if (!nssCert) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  nssCert.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificateDB::FindCertByEmailAddress(const char* aEmailAddress,
                                           nsIX509Cert** _retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  NS_ENSURE_TRUE(certVerifier, NS_ERROR_UNEXPECTED);

  ScopedCERTCertList certlist(
      PK11_FindCertsFromEmailAddress(aEmailAddress, nullptr));
  if (!certlist)
    return NS_ERROR_FAILURE;

  // certlist now contains certificates with the right email address,
  // but they might not have the correct usage or might even be invalid

  if (CERT_LIST_END(CERT_LIST_HEAD(certlist), certlist))
    return NS_ERROR_FAILURE; // no certs found

  CERTCertListNode *node;
  // search for a valid certificate
  for (node = CERT_LIST_HEAD(certlist);
       !CERT_LIST_END(node, certlist);
       node = CERT_LIST_NEXT(node)) {

    ScopedCERTCertList unusedCertChain;
    SECStatus srv = certVerifier->VerifyCert(node->cert,
                                             certificateUsageEmailRecipient,
                                             mozilla::pkix::Now(),
                                             nullptr /*XXX pinarg*/,
                                             nullptr /*hostname*/,
                                             unusedCertChain);
    if (srv == SECSuccess) {
      break;
    }
  }

  if (CERT_LIST_END(node, certlist)) {
    // no valid cert found
    return NS_ERROR_FAILURE;
  }

  // node now contains the first valid certificate with correct usage 
  RefPtr<nsNSSCertificate> nssCert = nsNSSCertificate::Create(node->cert);
  if (!nssCert)
    return NS_ERROR_OUT_OF_MEMORY;

  nssCert.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificateDB::ConstructX509FromBase64(const char *base64,
                                            nsIX509Cert **_retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  if (NS_WARN_IF(!_retval)) {
    return NS_ERROR_INVALID_POINTER;
  }

  // sure would be nice to have a smart pointer class for PL_ allocations
  // unfortunately, we cannot distinguish out-of-memory from bad-input here
  uint32_t len = base64 ? strlen(base64) : 0;
  char *certDER = PL_Base64Decode(base64, len, nullptr);
  if (!certDER)
    return NS_ERROR_ILLEGAL_VALUE;
  if (!*certDER) {
    PL_strfree(certDER);
    return NS_ERROR_ILLEGAL_VALUE;
  }

  // If we get to this point, we know we had well-formed base64 input;
  // therefore the input string cannot have been less than two
  // characters long.  Compute the unpadded length of the decoded data.
  uint32_t lengthDER = (len * 3) / 4;
  if (base64[len-1] == '=') {
    lengthDER--;
    if (base64[len-2] == '=')
      lengthDER--;
  }

  nsresult rv = ConstructX509(certDER, lengthDER, _retval);
  PL_strfree(certDER);
  return rv;
}

NS_IMETHODIMP
nsNSSCertificateDB::ConstructX509(const char* certDER,
                                  uint32_t lengthDER,
                                  nsIX509Cert** _retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  if (NS_WARN_IF(!_retval)) {
    return NS_ERROR_INVALID_POINTER;
  }

  SECItem secitem_cert;
  secitem_cert.type = siDERCertBuffer;
  secitem_cert.data = (unsigned char*)certDER;
  secitem_cert.len = lengthDER;

  ScopedCERTCertificate cert(CERT_NewTempCertificate(CERT_GetDefaultCertDB(),
                                                     &secitem_cert, nullptr,
                                                     false, true));
  if (!cert)
    return (PORT_GetError() == SEC_ERROR_NO_MEMORY)
      ? NS_ERROR_OUT_OF_MEMORY : NS_ERROR_FAILURE;

  nsCOMPtr<nsIX509Cert> nssCert = nsNSSCertificate::Create(cert.get());
  if (!nssCert) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  nssCert.forget(_retval);
  return NS_OK;
}

void
nsNSSCertificateDB::get_default_nickname(CERTCertificate *cert, 
                                         nsIInterfaceRequestor* ctx,
                                         nsCString &nickname,
                                         const nsNSSShutDownPreventionLock &/*proofOfLock*/)
{
  static NS_DEFINE_CID(kNSSComponentCID, NS_NSSCOMPONENT_CID);

  nickname.Truncate();

  nsresult rv;
  CK_OBJECT_HANDLE keyHandle;

  CERTCertDBHandle *defaultcertdb = CERT_GetDefaultCertDB();
  nsCOMPtr<nsINSSComponent> nssComponent(do_GetService(kNSSComponentCID, &rv));
  if (NS_FAILED(rv))
    return;

  nsAutoCString username;
  UniquePORTString tempCN(CERT_GetCommonName(&cert->subject));
  if (tempCN) {
    username = tempCN.get();
  }

  nsAutoCString caname;
  UniquePORTString tempIssuerOrg(CERT_GetOrgName(&cert->issuer));
  if (tempIssuerOrg) {
    caname = tempIssuerOrg.get();
  }

  nsAutoString tmpNickFmt;
  nssComponent->GetPIPNSSBundleString("nick_template", tmpNickFmt);
  NS_ConvertUTF16toUTF8 nickFmt(tmpNickFmt);

  nsAutoCString baseName;
  char *temp_nn = PR_smprintf(nickFmt.get(), username.get(), caname.get());
  if (!temp_nn) {
    return;
  } else {
    baseName = temp_nn;
    PR_smprintf_free(temp_nn);
    temp_nn = nullptr;
  }

  nickname = baseName;

  /*
   * We need to see if the private key exists on a token, if it does
   * then we need to check for nicknames that already exist on the smart
   * card.
   */
  ScopedPK11SlotInfo slot(PK11_KeyForCertExists(cert, &keyHandle, ctx));
  if (!slot)
    return;

  if (!PK11_IsInternal(slot)) {
    char *tmp = PR_smprintf("%s:%s", PK11_GetTokenName(slot), baseName.get());
    if (!tmp) {
      nickname.Truncate();
      return;
    }
    baseName = tmp;
    PR_smprintf_free(tmp);

    nickname = baseName;
  }

  int count = 1;
  while (true) {
    if ( count > 1 ) {
      char *tmp = PR_smprintf("%s #%d", baseName.get(), count);
      if (!tmp) {
        nickname.Truncate();
        return;
      }
      nickname = tmp;
      PR_smprintf_free(tmp);
    }

    ScopedCERTCertificate dummycert;

    if (PK11_IsInternal(slot)) {
      /* look up the nickname to make sure it isn't in use already */
      dummycert = CERT_FindCertByNickname(defaultcertdb, nickname.get());

    } else {
      /*
       * Check the cert against others that already live on the smart 
       * card.
       */
      dummycert = PK11_FindCertFromNickname(nickname.get(), ctx);
      if (dummycert) {
	/*
	 * Make sure the subject names are different.
	 */ 
	if (CERT_CompareName(&cert->subject, &dummycert->subject) == SECEqual)
	{
	  /*
	   * There is another certificate with the same nickname and
	   * the same subject name on the smart card, so let's use this
	   * nickname.
	   */
	  dummycert = nullptr;
	}
      }
    }
    if (!dummycert) {
      break;
    }
    count++;
  }
}

NS_IMETHODIMP nsNSSCertificateDB::AddCertFromBase64(const char* aBase64,
                                                    const char* aTrust,
                                                    const char* aName)
{
  NS_ENSURE_ARG_POINTER(aBase64);
  nsCOMPtr <nsIX509Cert> newCert;

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsNSSCertTrust trust;

  // need to calculate the trust bits from the aTrust string.
  SECStatus stat = CERT_DecodeTrustString(trust.GetTrust(),
    /* this is const, but not declared that way */(char *) aTrust);
  NS_ENSURE_STATE(stat == SECSuccess); // if bad trust passed in, return error.


  nsresult rv = ConstructX509FromBase64(aBase64, getter_AddRefs(newCert));
  NS_ENSURE_SUCCESS(rv, rv);

  SECItem der;
  rv = newCert->GetRawDER(&der.len, (uint8_t **)&der.data);
  NS_ENSURE_SUCCESS(rv, rv);

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("Creating temp cert\n"));
  CERTCertDBHandle *certdb = CERT_GetDefaultCertDB();
  ScopedCERTCertificate tmpCert(CERT_FindCertByDERCert(certdb, &der));
  if (!tmpCert)
    tmpCert = CERT_NewTempCertificate(certdb, &der,
                                      nullptr, false, true);
  free(der.data);
  der.data = nullptr;
  der.len = 0;

  if (!tmpCert) {
    NS_ERROR("Couldn't create cert from DER blob");
    return MapSECStatus(SECFailure);
  }

   // If there's already a certificate that matches this one in the database,
   // we still want to set its trust to the given value.
  if (tmpCert->isperm) {
    return SetCertTrustFromString(newCert, aTrust);
  }

  nsXPIDLCString nickname;
  nickname.Adopt(CERT_MakeCANickname(tmpCert.get()));

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("Created nick \"%s\"\n", nickname.get()));

  rv = attemptToLogInWithDefaultPassword();
  if (NS_WARN_IF(rv != NS_OK)) {
    return rv;
  }

  SECStatus srv = __CERT_AddTempCertToPerm(tmpCert.get(),
                                           const_cast<char*>(nickname.get()),
                                           trust.GetTrust());
  return MapSECStatus(srv);
}

NS_IMETHODIMP
nsNSSCertificateDB::AddCert(const nsACString & aCertDER, const char *aTrust,
                            const char *aName)
{
  nsCString base64;
  nsresult rv = Base64Encode(aCertDER, base64);
  NS_ENSURE_SUCCESS(rv, rv);
  return AddCertFromBase64(base64.get(), aTrust, aName);
}

NS_IMETHODIMP
nsNSSCertificateDB::SetCertTrustFromString(nsIX509Cert* cert,
                                           const char* trustString)
{
  CERTCertTrust trust;

  // need to calculate the trust bits from the aTrust string.
  SECStatus srv = CERT_DecodeTrustString(&trust,
                                         const_cast<char *>(trustString));
  if (srv != SECSuccess) {
    return MapSECStatus(SECFailure);
  }
  ScopedCERTCertificate nssCert(cert->GetCert());

  nsresult rv = attemptToLogInWithDefaultPassword();
  if (NS_WARN_IF(rv != NS_OK)) {
    return rv;
  }

  srv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), nssCert.get(), &trust);
  return MapSECStatus(srv);
}

NS_IMETHODIMP 
nsNSSCertificateDB::GetCerts(nsIX509CertList **_retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }  

  nsCOMPtr<nsIInterfaceRequestor> ctx = new PipUIContext();
  nsCOMPtr<nsIX509CertList> nssCertList;
  ScopedCERTCertList certList(PK11_ListCerts(PK11CertListUnique, ctx));

  // nsNSSCertList 1) adopts certList, and 2) handles the nullptr case fine.
  // (returns an empty list) 
  nssCertList = new nsNSSCertList(certList, locker);

  nssCertList.forget(_retval);
  return NS_OK;
}

nsresult
VerifyCertAtTime(nsIX509Cert* aCert,
                 int64_t /*SECCertificateUsage*/ aUsage,
                 uint32_t aFlags,
                 const char* aHostname,
                 mozilla::pkix::Time aTime,
                 nsIX509CertList** aVerifiedChain,
                 bool* aHasEVPolicy,
                 int32_t* /*PRErrorCode*/ _retval,
                 const nsNSSShutDownPreventionLock& locker)
{
  NS_ENSURE_ARG_POINTER(aCert);
  NS_ENSURE_ARG_POINTER(aHasEVPolicy);
  NS_ENSURE_ARG_POINTER(aVerifiedChain);
  NS_ENSURE_ARG_POINTER(_retval);

  *aVerifiedChain = nullptr;
  *aHasEVPolicy = false;
  *_retval = PR_UNKNOWN_ERROR;

#ifndef MOZ_NO_EV_CERTS
  EnsureIdentityInfoLoaded();
#endif

  ScopedCERTCertificate nssCert(aCert->GetCert());
  if (!nssCert) {
    return NS_ERROR_INVALID_ARG;
  }

  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  NS_ENSURE_TRUE(certVerifier, NS_ERROR_FAILURE);

  ScopedCERTCertList resultChain;
  SECOidTag evOidPolicy;
  SECStatus srv;

  if (aHostname && aUsage == certificateUsageSSLServer) {
    srv = certVerifier->VerifySSLServerCert(nssCert,
                                            nullptr, // stapledOCSPResponse
                                            aTime,
                                            nullptr, // Assume no context
                                            aHostname,
                                            resultChain,
                                            false, // don't save intermediates
                                            aFlags,
                                            &evOidPolicy);
  } else {
    srv = certVerifier->VerifyCert(nssCert, aUsage, aTime,
                                   nullptr, // Assume no context
                                   aHostname,
                                   resultChain,
                                   aFlags,
                                   nullptr, // stapledOCSPResponse
                                   &evOidPolicy);
  }

  PRErrorCode error = PR_GetError();

  nsCOMPtr<nsIX509CertList> nssCertList;
  // This adopts the list
  nssCertList = new nsNSSCertList(resultChain, locker);
  NS_ENSURE_TRUE(nssCertList, NS_ERROR_FAILURE);

  if (srv == SECSuccess) {
    if (evOidPolicy != SEC_OID_UNKNOWN) {
      *aHasEVPolicy = true;
    }
    *_retval = 0;
  } else {
    NS_ENSURE_TRUE(evOidPolicy == SEC_OID_UNKNOWN, NS_ERROR_FAILURE);
    NS_ENSURE_TRUE(error != 0, NS_ERROR_FAILURE);
    *_retval = error;
  }
  nssCertList.forget(aVerifiedChain);

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificateDB::VerifyCertNow(nsIX509Cert* aCert,
                                  int64_t /*SECCertificateUsage*/ aUsage,
                                  uint32_t aFlags,
                                  const char* aHostname,
                                  nsIX509CertList** aVerifiedChain,
                                  bool* aHasEVPolicy,
                                  int32_t* /*PRErrorCode*/ _retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return ::VerifyCertAtTime(aCert, aUsage, aFlags, aHostname,
                            mozilla::pkix::Now(),
                            aVerifiedChain, aHasEVPolicy, _retval, locker);
}

NS_IMETHODIMP
nsNSSCertificateDB::VerifyCertAtTime(nsIX509Cert* aCert,
                                     int64_t /*SECCertificateUsage*/ aUsage,
                                     uint32_t aFlags,
                                     const char* aHostname,
                                     uint64_t aTime,
                                     nsIX509CertList** aVerifiedChain,
                                     bool* aHasEVPolicy,
                                     int32_t* /*PRErrorCode*/ _retval)
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return ::VerifyCertAtTime(aCert, aUsage, aFlags, aHostname,
                            mozilla::pkix::TimeFromEpochInSeconds(aTime),
                            aVerifiedChain, aHasEVPolicy, _retval, locker);
}

NS_IMETHODIMP
nsNSSCertificateDB::ClearOCSPCache()
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  NS_ENSURE_TRUE(certVerifier, NS_ERROR_FAILURE);
  certVerifier->ClearOCSPCache();
  return NS_OK;
}
