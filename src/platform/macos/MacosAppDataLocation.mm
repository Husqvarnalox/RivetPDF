// SPDX-License-Identifier: MPL-2.0
#import "MacosAppDataLocation.h"

#import <Foundation/Foundation.h>

namespace rivet::platform {

core::Result<std::filesystem::path> MacosAppDataLocation::directory() const {
    if (std::optional<std::filesystem::path> overridden = appDataOverride()) return *std::move(overridden);
    @autoreleasepool {
        NSArray<NSURL*>* urls = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                                       inDomains:NSUserDomainMask];
        NSURL* base = urls.firstObject;
        if (base == nil || !base.isFileURL || base.fileSystemRepresentation == nullptr) {
            return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                                   "the Application Support directory is not available",
                                                   "platform"));
        }
        return std::filesystem::path(base.fileSystemRepresentation) / "Rivet";
    }
}

} // namespace rivet::platform
