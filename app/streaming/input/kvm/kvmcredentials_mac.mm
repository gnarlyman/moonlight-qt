#include "kvmcredentials.h"

#include <Foundation/Foundation.h>
#include <Security/Security.h>

// macOS backend: the login keychain via Security.framework.
//
// Items are generic passwords keyed by (service, account). They are tied to the
// code signature, so a rebuild with a different signature may re-prompt -- that
// is expected for a development build.

namespace {

// Qt <-> NSString via UTF-8. QString::toNSString() exists on macOS but drags in
// Qt's Objective-C extras; this keeps the file self-contained.
NSString* toNSString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return [NSString stringWithUTF8String:utf8.constData()];
}

NSMutableDictionary* baseQuery(const QString& account)
{
    NSMutableDictionary* query = [NSMutableDictionary dictionary];
    [query setObject:(__bridge id)kSecClassGenericPassword
              forKey:(__bridge id)kSecClass];
    [query setObject:[NSString stringWithUTF8String:KvmCredentials::SERVICE_NAME]
              forKey:(__bridge id)kSecAttrService];
    [query setObject:toNSString(account) forKey:(__bridge id)kSecAttrAccount];
    return query;
}

}

namespace KvmCredentials {

QString loadPasswordImpl(const QString& account)
{
    @autoreleasepool {
        NSMutableDictionary* query = baseQuery(account);
        [query setObject:(__bridge id)kSecMatchLimitOne forKey:(__bridge id)kSecMatchLimit];
        [query setObject:(__bridge id)kCFBooleanTrue forKey:(__bridge id)kSecReturnData];

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (status != errSecSuccess || result == nullptr) {
            // errSecItemNotFound is simply "nothing saved yet", not an error.
            return QString();
        }

        NSData* data = (__bridge_transfer NSData*)result;
        return QString::fromUtf8(static_cast<const char*>(data.bytes),
                                 static_cast<int>(data.length));
    }
}

bool savePasswordImpl(const QString& account, const QString& password)
{
    @autoreleasepool {
        NSMutableDictionary* query = baseQuery(account);

        if (password.isEmpty()) {
            const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query);
            return status == errSecSuccess || status == errSecItemNotFound;
        }

        const QByteArray utf8 = password.toUtf8();
        NSMutableDictionary* attributes = [NSMutableDictionary dictionary];
        [attributes setObject:[NSData dataWithBytes:utf8.constData() length:utf8.size()]
                       forKey:(__bridge id)kSecValueData];

        // Update before adding, so saving twice doesn't accumulate items.
        OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                        (__bridge CFDictionaryRef)attributes);
        if (status == errSecItemNotFound) {
            NSMutableDictionary* item = [query mutableCopy];
            [item addEntriesFromDictionary:attributes];
            status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
        }

        return status == errSecSuccess;
    }
}

}
