#include "kvmcredentials.h"

// Fallback backend for platforms with no secret store implemented yet.
//
// Deliberately not an error worth surfacing: GLKVM_PASSWORD covers every
// platform, so the feature works here today and a real backend (Windows
// CredRead/CredWrite, Linux libsecret) can be added without touching the auth
// logic at all. See kvmcredentials.h.

namespace KvmCredentials {

QString loadPasswordImpl(const QString& account)
{
    Q_UNUSED(account);
    return QString();
}

bool savePasswordImpl(const QString& account, const QString& password)
{
    Q_UNUSED(account);
    Q_UNUSED(password);
    return false;
}

}
