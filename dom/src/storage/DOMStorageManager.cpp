/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DOMStorageManager.h"
#include "DOMStorage.h"
#include "DOMStorageDBThread.h"

#include "nsIScriptSecurityManager.h"
#include "nsIEffectiveTLDService.h"

#include "nsNetUtil.h"
#include "nsPrintfCString.h"
#include "nsXULAppAPI.h"
#include "nsThreadUtils.h"
#include "nsIObserverService.h"
#include "mozIThirdPartyUtil.h"
#include "mozilla/Services.h"
#include "mozilla/Preferences.h"

// Only allow relatively small amounts of data since performance of
// the synchronous IO is very bad.
// We are enforcing simple per-origin quota only.
#define DEFAULT_QUOTA_LIMIT (5 * 1024)

namespace mozilla {
namespace dom {

namespace { // anon

int32_t gQuotaLimit = DEFAULT_QUOTA_LIMIT;

} // anon

DOMLocalStorageManager*
DOMLocalStorageManager::sSelf = nullptr;

// static
uint32_t
DOMStorageManager::GetQuota()
{
  static bool preferencesInitialized = false;
  if (!preferencesInitialized) {
    mozilla::Preferences::AddIntVarCache(&gQuotaLimit, "dom.storage.default_quota",
                                         DEFAULT_QUOTA_LIMIT);
    preferencesInitialized = true;
  }

  return gQuotaLimit * 1024; // pref is in kBs
}

void
ReverseString(const nsCSubstring& aSource, nsCSubstring& aResult)
{
  nsACString::const_iterator sourceBegin, sourceEnd;
  aSource.BeginReading(sourceBegin);
  aSource.EndReading(sourceEnd);

  aResult.SetLength(aSource.Length());
  nsACString::iterator destEnd;
  aResult.EndWriting(destEnd);

  while (sourceBegin != sourceEnd) {
    *(--destEnd) = *sourceBegin;
    ++sourceBegin;
  }
}

nsresult
CreateReversedDomain(const nsACString& aAsciiDomain,
                     nsACString& aKey)
{
  if (aAsciiDomain.IsEmpty()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  ReverseString(aAsciiDomain, aKey);

  aKey.AppendLiteral(".");
  return NS_OK;
}

bool
PrincipalsEqual(nsIPrincipal* aObjectPrincipal, nsIPrincipal* aSubjectPrincipal)
{
  if (!aSubjectPrincipal) {
    return true;
  }

  if (!aObjectPrincipal) {
    return false;
  }

  bool equals;
  nsresult rv = aSubjectPrincipal->EqualsIgnoringDomain(aObjectPrincipal, &equals);

  NS_ASSERTION(NS_SUCCEEDED(rv) && equals,
               "Trying to get DOM storage for wrong principal!");

  if (NS_FAILED(rv) || !equals) {
    return false;
  }

  return true;
}

NS_IMPL_ISUPPORTS1(DOMStorageManager,
                   nsIDOMStorageManager)

DOMStorageManager::DOMStorageManager(nsPIDOMStorage::StorageType aType)
  : mType(aType)
  , mLowDiskSpace(false)
{
  mCaches.Init(10);
  DOMStorageObserver* observer = DOMStorageObserver::Self();
  NS_ASSERTION(observer, "No DOMStorageObserver, cannot observe private data delete notifications!");

  if (observer) {
    observer->AddSink(this);
  }
}

DOMStorageManager::~DOMStorageManager()
{
  DOMStorageObserver* observer = DOMStorageObserver::Self();
  if (observer) {
    observer->RemoveSink(this);
  }
}

namespace { // anon

nsresult
AppendFirstPartyToKey(nsIURI* aFirstPartyIsolationURI, nsACString& aKey)
{
  if (aFirstPartyIsolationURI) {
    nsCOMPtr<mozIThirdPartyUtil> thirdPartyUtil =
                            do_GetService(THIRDPARTYUTIL_CONTRACTID);
    if (!thirdPartyUtil)
      return NS_ERROR_FAILURE;

    nsAutoCString firstPartyHost;
    nsresult rv = thirdPartyUtil->GetFirstPartyHostForIsolation(aFirstPartyIsolationURI,
                                                                firstPartyHost);
    NS_ENSURE_SUCCESS(rv, rv);

    aKey.AppendLiteral("&");
    aKey.Append(firstPartyHost);
  }

  return NS_OK;
}

nsresult
CreateScopeKey(nsIURI* aFirstPartyIsolationURI, nsIPrincipal* aPrincipal,
               nsACString& aKey)
{
  nsCOMPtr<nsIURI> uri;
  nsresult rv = aPrincipal->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!uri) {
    return NS_ERROR_UNEXPECTED;
  }

  nsAutoCString domainScope;
  rv = uri->GetAsciiHost(domainScope);
  NS_ENSURE_SUCCESS(rv, rv);

  if (domainScope.IsEmpty()) {
    // About pages have an empty host but a valid path.  Since they are handled
    // internally by our own redirector, we can trust them and use path as key.
    // if file:/// protocol, let's make the exact directory the domain
    bool isScheme = false;
    if ((NS_SUCCEEDED(uri->SchemeIs("about", &isScheme)) && isScheme) ||
        (NS_SUCCEEDED(uri->SchemeIs("moz-safe-about", &isScheme)) && isScheme)) {
      rv = uri->GetPath(domainScope);
      NS_ENSURE_SUCCESS(rv, rv);
      // While the host is always canonicalized to lowercase, the path is not,
      // thus need to force the casing.
      ToLowerCase(domainScope);
    } else if (NS_SUCCEEDED(uri->SchemeIs("file", &isScheme)) && isScheme) {
      nsCOMPtr<nsIURL> url = do_QueryInterface(uri, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      rv = url->GetDirectory(domainScope);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  nsAutoCString key;

  rv = CreateReversedDomain(domainScope, key);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoCString scheme;
  rv = uri->GetScheme(scheme);
  NS_ENSURE_SUCCESS(rv, rv);

  key.Append(NS_LITERAL_CSTRING(":") + scheme);

  int32_t port = NS_GetRealPort(uri);
  if (port != -1) {
    key.Append(nsPrintfCString(":%d", port));
  }

  bool unknownAppId;
  rv = aPrincipal->GetUnknownAppId(&unknownAppId);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!unknownAppId) {
    uint32_t appId;
    rv = aPrincipal->GetAppId(&appId);
    NS_ENSURE_SUCCESS(rv, rv);

    bool isInBrowserElement;
    rv = aPrincipal->GetIsInBrowserElement(&isInBrowserElement);
    NS_ENSURE_SUCCESS(rv, rv);

    if (appId == nsIScriptSecurityManager::NO_APP_ID && !isInBrowserElement) {
      aKey.Assign(key);
    } else {
      aKey.Truncate();
      aKey.AppendInt(appId);
      aKey.Append(NS_LITERAL_CSTRING(":") + (isInBrowserElement ?
                  NS_LITERAL_CSTRING("t") : NS_LITERAL_CSTRING("f")) +
                  NS_LITERAL_CSTRING(":") + key);
    }
  }

  // Isolate scope keys to the URL bar domain by appending &firstPartyHost
  // if available.
  return AppendFirstPartyToKey(aFirstPartyIsolationURI, aKey);
}

nsresult
CreateQuotaDBKey(nsIURI* aFirstPartyIsolationURI, nsIPrincipal* aPrincipal,
                 nsACString& aKey)
{
  nsresult rv;

  nsAutoCString subdomainsDBKey;
  nsCOMPtr<nsIEffectiveTLDService> eTLDService(do_GetService(
    NS_EFFECTIVETLDSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIURI> uri;
  rv = aPrincipal->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(uri, NS_ERROR_UNEXPECTED);

  nsAutoCString eTLDplusOne;
  rv = eTLDService->GetBaseDomain(uri, 0, eTLDplusOne);
  if (NS_ERROR_INSUFFICIENT_DOMAIN_LEVELS == rv) {
    // XXX bug 357323 - what to do for localhost/file exactly?
    rv = uri->GetAsciiHost(eTLDplusOne);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  CreateReversedDomain(eTLDplusOne, subdomainsDBKey);

  bool unknownAppId;
  rv = aPrincipal->GetUnknownAppId(&unknownAppId);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!unknownAppId) {
    uint32_t appId;
    rv = aPrincipal->GetAppId(&appId);
    NS_ENSURE_SUCCESS(rv, rv);

    bool isInBrowserElement;
    rv = aPrincipal->GetIsInBrowserElement(&isInBrowserElement);
    NS_ENSURE_SUCCESS(rv, rv);

    if (appId == nsIScriptSecurityManager::NO_APP_ID && !isInBrowserElement) {
      aKey.Assign(subdomainsDBKey);
    } else {
      aKey.Truncate();
      aKey.AppendInt(appId);
      aKey.Append(NS_LITERAL_CSTRING(":") + (isInBrowserElement ?
                  NS_LITERAL_CSTRING("t") : NS_LITERAL_CSTRING("f")) +
                  NS_LITERAL_CSTRING(":") + subdomainsDBKey);
    }
  }

  // Isolate scope keys to the URL bar domain by appending &firstPartyHost
  // if available.
  return AppendFirstPartyToKey(aFirstPartyIsolationURI, aKey);
}

} // anon

DOMStorageCache*
DOMStorageManager::GetCache(const nsACString& aScope) const
{
  DOMStorageCacheHashKey* entry = mCaches.GetEntry(aScope);
  if (!entry) {
    return nullptr;
  }

  return entry->cache();
}

already_AddRefed<DOMStorageCache>
DOMStorageManager::PutCache(const nsACString& aScope,
                            nsIURI* aFirstPartyIsolationURI,
                            nsIPrincipal* aPrincipal)
{
  DOMStorageCacheHashKey* entry = mCaches.PutEntry(aScope);
  nsRefPtr<DOMStorageCache> cache = entry->cache();

  nsAutoCString quotaScope;
  CreateQuotaDBKey(aFirstPartyIsolationURI, aPrincipal, quotaScope);

  // To avoid ever persisting session storage to disk, initialize LocalStorage
  // like SessionStorage.
  switch (mType) {
  case SessionStorage:
  case LocalStorage:
    // Lifetime handled by the manager, don't persist
    entry->HardRef();
    cache->Init(nullptr, false, aFirstPartyIsolationURI, aPrincipal, quotaScope);
    break;

  default:
    MOZ_ASSERT(false);
  }

  return cache.forget();
}

void
DOMStorageManager::DropCache(DOMStorageCache* aCache)
{
  if (!NS_IsMainThread()) {
    NS_WARNING("DOMStorageManager::DropCache called on a non-main thread, shutting down?");
  }

  mCaches.RemoveEntry(aCache->Scope());
}

nsresult
DOMStorageManager::GetStorageInternal(bool aCreate,
                                      nsIURI* aFirstPartyIsolationURI,
                                      nsIPrincipal* aPrincipal,
                                      const nsAString& aDocumentURI,
                                      bool aPrivate,
                                      nsIDOMStorage** aRetval)
{
  nsresult rv;

  nsAutoCString scope;
  rv = CreateScopeKey(aFirstPartyIsolationURI, aPrincipal, scope);
  if (NS_FAILED(rv)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsRefPtr<DOMStorageCache> cache = GetCache(scope);

  // Get or create a cache for the given scope
  if (!cache) {
    if (!aCreate) {
      *aRetval = nullptr;
      return NS_OK;
    }

    if (!aRetval) {
      // This is demand to just preload the cache, if the scope has
      // no data stored, bypass creation and preload of the cache.
      DOMStorageDBBridge* db = DOMStorageCache::GetDatabase();
      if (db) {
        if (!db->ShouldPreloadScope(scope)) {
          return NS_OK;
        }
      } else {
        if (scope.Equals(NS_LITERAL_CSTRING("knalb.:about"))) {
          return NS_OK;
        }
      }
    }

    // There is always a single instance of a cache per scope
    // in a single instance of a DOM storage manager.
    cache = PutCache(scope, aFirstPartyIsolationURI, aPrincipal);
  } else if (mType == SessionStorage) {
    if (!cache->CheckPrincipal(aPrincipal)) {
      return NS_ERROR_DOM_SECURITY_ERR;
    }
  }

  if (aRetval) {
    *aRetval = new DOMStorage(this, cache, aDocumentURI, aPrincipal, aPrivate);
    NS_ADDREF(*aRetval);
  }

  return NS_OK;
}

NS_IMETHODIMP
DOMStorageManager::PrecacheStorage(nsIPrincipal* aPrincipal)
{
  return GetStorageInternal(true, nullptr, aPrincipal, EmptyString(),
                            false, nullptr);
}

NS_IMETHODIMP
DOMStorageManager::PrecacheStorageForFirstParty(nsIURI* aFirstPartyIsolationURI,
                                                nsIPrincipal* aPrincipal)
{
  return GetStorageInternal(true, aFirstPartyIsolationURI, aPrincipal, EmptyString(),
                            false, nullptr);
}

NS_IMETHODIMP
DOMStorageManager::CreateStorage(nsIPrincipal* aPrincipal,
                                 const nsAString& aDocumentURI,
                                 bool aPrivate,
                                 nsIDOMStorage** aRetval)
{
  return GetStorageInternal(true, nullptr, aPrincipal, aDocumentURI,
                            aPrivate, aRetval);
}

NS_IMETHODIMP
DOMStorageManager::CreateStorageForFirstParty(nsIURI* aFirstPartyIsolationURI,
                                              nsIPrincipal* aPrincipal,
                                              const nsAString& aDocumentURI,
                                              bool aPrivate,
                                              nsIDOMStorage** aRetval)
{
  return GetStorageInternal(true, aFirstPartyIsolationURI, aPrincipal, aDocumentURI,
                            aPrivate, aRetval);
}

NS_IMETHODIMP
DOMStorageManager::GetStorage(nsIPrincipal* aPrincipal,
                              bool aPrivate,
                              nsIDOMStorage** aRetval)
{
  return GetStorageInternal(false, nullptr, aPrincipal, EmptyString(),
                            aPrivate, aRetval);
}

NS_IMETHODIMP
DOMStorageManager::GetStorageForFirstParty(nsIURI* aFirstPartyIsolationURI,
                                           nsIPrincipal* aPrincipal,
                                           bool aPrivate,
                                           nsIDOMStorage** aRetval)
{
  return GetStorageInternal(false, aFirstPartyIsolationURI, aPrincipal,
                            EmptyString(), aPrivate, aRetval);
}

NS_IMETHODIMP
DOMStorageManager::CloneStorage(nsIDOMStorage* aStorage)
{
  if (mType != SessionStorage) {
    // Cloning is supported only for sessionStorage
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  nsCOMPtr<nsPIDOMStorage> pstorage = do_QueryInterface(aStorage);
  if (!pstorage) {
    return NS_ERROR_UNEXPECTED;
  }

  const DOMStorageCache* origCache = pstorage->GetCache();

  DOMStorageCache* existingCache = GetCache(origCache->Scope());
  if (existingCache) {
    // Do not replace an existing sessionStorage.
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Since this manager is sessionStorage manager, PutCache hard references
  // the cache in our hashtable.
  nsRefPtr<DOMStorageCache> newCache = PutCache(origCache->Scope(),
                                                origCache->FirstPartyIsolationURI(),
                                                origCache->Principal());

  newCache->CloneFrom(origCache);
  return NS_OK;
}

NS_IMETHODIMP
DOMStorageManager::CheckStorage(nsIPrincipal* aPrincipal,
                                nsIDOMStorage* aStorage,
                                bool* aRetval)
{
  return CheckStorageForFirstParty(nullptr, aPrincipal, aStorage, aRetval);
}

NS_IMETHODIMP
DOMStorageManager::CheckStorageForFirstParty(nsIURI* aFirstPartyIsolationURI,
                                             nsIPrincipal* aPrincipal,
                                             nsIDOMStorage* aStorage,
                                             bool* aRetval)
{
  nsCOMPtr<nsPIDOMStorage> pstorage = do_QueryInterface(aStorage);
  if (!pstorage) {
    return NS_ERROR_UNEXPECTED;
  }

  *aRetval = false;

  if (!aPrincipal) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsAutoCString scope;
  nsresult rv = CreateScopeKey(aFirstPartyIsolationURI, aPrincipal, scope);
  NS_ENSURE_SUCCESS(rv, rv);

  DOMStorageCache* cache = GetCache(scope);
  if (cache != pstorage->GetCache()) {
    return NS_OK;
  }

  if (!pstorage->PrincipalEquals(aPrincipal)) {
    return NS_OK;
  }

  *aRetval = true;
  return NS_OK;
}

// Obsolete nsIDOMStorageManager methods

NS_IMETHODIMP
DOMStorageManager::GetLocalStorageForPrincipal(nsIPrincipal* aPrincipal,
                                               const nsAString& aDocumentURI,
                                               bool aPrivate,
                                               nsIDOMStorage** aRetval)
{
  if (mType != LocalStorage) {
    return NS_ERROR_UNEXPECTED;
  }

  return CreateStorage(aPrincipal, aDocumentURI, aPrivate, aRetval);
}

namespace { // anon

class ClearCacheEnumeratorData
{
public:
  ClearCacheEnumeratorData(uint32_t aFlags)
    : mUnloadFlags(aFlags)
  {}

  uint32_t mUnloadFlags;
  nsCString mKeyPrefix;
};

} // anon

PLDHashOperator
DOMStorageManager::ClearCacheEnumerator(DOMStorageCacheHashKey* aEntry, void* aClosure)
{
  DOMStorageCache* cache = aEntry->cache();
  nsCString& key = const_cast<nsCString&>(cache->Scope());

  ClearCacheEnumeratorData* data = static_cast<ClearCacheEnumeratorData*>(aClosure);

  if (data->mKeyPrefix.IsEmpty() || StringBeginsWith(key, data->mKeyPrefix)) {
    cache->UnloadItems(data->mUnloadFlags);
  }

  return PL_DHASH_NEXT;
}

nsresult
DOMStorageManager::Observe(const char* aTopic, const nsACString& aScopePrefix)
{
  // Clear everything, caches + database
  if (!strcmp(aTopic, "cookie-cleared")) {
    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadComplete);
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    return NS_OK;
  }

  // Clear from caches everything that has been stored
  // while in session-only mode
  if (!strcmp(aTopic, "session-only-cleared")) {
    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadSession);
    data.mKeyPrefix = aScopePrefix;
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    return NS_OK;
  }

  // Clear everything (including so and pb data) from caches and database
  // for the gived domain and subdomains.
  if (!strcmp(aTopic, "domain-data-cleared")) {
    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadComplete);
    data.mKeyPrefix = aScopePrefix;
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    return NS_OK;
  }

  // Clear all private-browsing caches
  if (!strcmp(aTopic, "private-browsing-data-cleared")) {
    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadPrivate);
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    return NS_OK;
  }

  // Clear localStorage data beloging to an app.
  if (!strcmp(aTopic, "app-data-cleared")) {

    // sessionStorage is expected to stay
    if (mType == SessionStorage) {
      return NS_OK;
    }

    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadComplete);
    data.mKeyPrefix = aScopePrefix;
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    return NS_OK;
  }

  if (!strcmp(aTopic, "profile-change")) {
    // For case caches are still referenced - clear them completely
    ClearCacheEnumeratorData data(DOMStorageCache::kUnloadComplete);
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);

    mCaches.Clear();
    return NS_OK;
  }

  if (!strcmp(aTopic, "low-disk-space")) {
    if (mType == LocalStorage) {
      mLowDiskSpace = true;
    }

    return NS_OK;
  }

  if (!strcmp(aTopic, "no-low-disk-space")) {
    if (mType == LocalStorage) {
      mLowDiskSpace = false;
    }

    return NS_OK;
  }

#ifdef DOM_STORAGE_TESTS
  if (!strcmp(aTopic, "test-reload")) {
    if (mType != LocalStorage) {
      return NS_OK;
    }

    // This immediately completely reloads all caches from the database.
    ClearCacheEnumeratorData data(DOMStorageCache::kTestReload);
    mCaches.EnumerateEntries(ClearCacheEnumerator, &data);
    return NS_OK;
  }

  if (!strcmp(aTopic, "test-flushed")) {
    if (XRE_GetProcessType() != GeckoProcessType_Default) {
      nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
      if (obs) {
        obs->NotifyObservers(nullptr, "domstorage-test-flushed", nullptr);
      }
    }

    return NS_OK;
  }
#endif

  NS_ERROR("Unexpected topic");
  return NS_ERROR_UNEXPECTED;
}

// DOMLocalStorageManager

DOMLocalStorageManager::DOMLocalStorageManager()
  : DOMStorageManager(LocalStorage)
{
  NS_ASSERTION(!sSelf, "Somebody is trying to do_CreateInstance(\"@mozilla/dom/localStorage-manager;1\"");
  sSelf = this;

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    // Do this only on the child process.  The thread IPC bridge
    // is also used to communicate chrome observer notifications.
    // Note: must be called after we set sSelf
    DOMStorageCache::StartDatabase();
  }
}

DOMLocalStorageManager::~DOMLocalStorageManager()
{
  sSelf = nullptr;
}

// DOMSessionStorageManager

DOMSessionStorageManager::DOMSessionStorageManager()
  : DOMStorageManager(SessionStorage)
{
  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    // Do this only on the child process.  The thread IPC bridge
    // is also used to communicate chrome observer notifications.
    DOMStorageCache::StartDatabase();
  }
}

} // ::dom
} // ::mozilla
