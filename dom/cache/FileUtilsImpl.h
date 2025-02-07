/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_FileUtilsImpl_h
#define mozilla_dom_cache_FileUtilsImpl_h

#include "mozilla/dom/cache/FileUtils.h"

namespace mozilla {
namespace dom {
namespace cache {

template <typename Func>
nsresult BodyTraverseFiles(const QuotaInfo& aQuotaInfo, nsIFile* aBodyDir,
                           const Func& aHandleFileFunc,
                           const bool aCanRemoveFiles, const bool aTrackQuota) {
  MOZ_DIAGNOSTIC_ASSERT(aBodyDir);

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  {
    nsCOMPtr<nsIFile> parentFile;
    nsresult rv = aBodyDir->GetParent(getter_AddRefs(parentFile));
    MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));
    MOZ_DIAGNOSTIC_ASSERT(parentFile);

    nsAutoCString nativeLeafName;
    rv = parentFile->GetNativeLeafName(nativeLeafName);
    MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));

    MOZ_DIAGNOSTIC_ASSERT(StringEndsWith(nativeLeafName, "morgue"_ns));
  }
#endif

  bool isEmpty = true;
  CACHE_TRY(quota::CollectEachFile(
      *aBodyDir,
      [&isEmpty, &aQuotaInfo, aTrackQuota, &aHandleFileFunc,
       aCanRemoveFiles](const nsCOMPtr<nsIFile>& file) -> Result<Ok, nsresult> {
        CACHE_TRY_INSPECT(const bool& isDir,
                          MOZ_TO_RESULT_INVOKE(file, IsDirectory));

        // If it's a directory somehow, try to remove it and move on
        if (NS_WARN_IF(isDir)) {
          DebugOnly<nsresult> result = RemoveNsIFileRecursively(
              aQuotaInfo, file, /* aTrackQuota */ false);
          MOZ_ASSERT(NS_SUCCEEDED(result));
          return Ok{};
        }

        nsAutoCString leafName;
        CACHE_TRY(file->GetNativeLeafName(leafName));

        // Delete all tmp files regardless of known bodies. These are all
        // considered orphans.
        if (StringEndsWith(leafName, ".tmp"_ns)) {
          if (aCanRemoveFiles) {
            DebugOnly<nsresult> result =
                RemoveNsIFile(aQuotaInfo, file, aTrackQuota);
            MOZ_ASSERT(NS_SUCCEEDED(result));
            return Ok{};
          }
        } else if (NS_WARN_IF(!StringEndsWith(leafName, ".final"_ns))) {
          // Otherwise, it must be a .final file.  If its not, then try to
          // remove it and move on
          DebugOnly<nsresult> result =
              RemoveNsIFile(aQuotaInfo, file, /* aTrackQuota */ false);
          MOZ_ASSERT(NS_SUCCEEDED(result));
          return Ok{};
        }

        CACHE_TRY_INSPECT(const bool& fileDeleted,
                          aHandleFileFunc(file, leafName));
        if (fileDeleted) {
          return Ok{};
        }

        isEmpty = false;

        return Ok{};
      }));

  if (isEmpty && aCanRemoveFiles) {
    DebugOnly<nsresult> result =
        RemoveNsIFileRecursively(aQuotaInfo, aBodyDir, /* aTrackQuota */ false);
    MOZ_ASSERT(NS_SUCCEEDED(result));
  }

  return NS_OK;
}

}  // namespace cache
}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_cache_FileUtilsImpl_h
