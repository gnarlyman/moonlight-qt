#pragma once

#include <QString>

/**
 * Password storage for GLKVM, behind a platform-neutral interface.
 *
 * The auth logic never touches a keychain API -- it asks here. That matters
 * because moonlight-qt builds ONE shared core for Windows, macOS, Linux and
 * Steam Link: anything that reaches directly for an OS API has to be rewritten
 * per target, whereas a narrow interface plus one backend per platform does not.
 *
 * Backends:
 *   macOS   kvmcredentials_mac.mm    Security.framework (login keychain)
 *   other   kvmcredentials_none.cpp  no storage; environment only
 *
 * GLKVM_PASSWORD is always consulted as a fallback, so every platform works
 * even before its backend exists. Add CredRead/CredWrite (Windows) or
 * libsecret (Linux) by implementing the two Impl functions below and adding the
 * file to the matching scope in app.pro.
 *
 * Only the password is secret. Host and username are ordinary settings.
 */
namespace KvmCredentials {

// Keychain service name; items are keyed by (service, account) where account is
// "user@host", so several KVMs can hold different passwords.
extern const char* const SERVICE_NAME;

// Password for user@host: the stored value if there is one, otherwise
// GLKVM_PASSWORD. Empty means "no credentials available".
QString password(const QString& host, const QString& user);

// Store the password; an empty password removes any stored value.
// Returns false if storage is unsupported or the write failed.
bool savePassword(const QString& host, const QString& user, const QString& password);

// GLKVM_PASSWORD from the environment, or empty.
QString passwordFromEnvironment();

// GLKVM_USER from the environment, or empty.
QString userFromEnvironment();

// --- provided by each platform backend ---
QString loadPasswordImpl(const QString& account);
bool savePasswordImpl(const QString& account, const QString& password);

}
