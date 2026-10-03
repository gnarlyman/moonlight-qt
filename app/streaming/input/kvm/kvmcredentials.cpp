#include "kvmcredentials.h"

namespace KvmCredentials {

const char* const SERVICE_NAME = "Moonlight GLKVM";

// Item account: "user@host", so one keychain can hold several KVMs' passwords.
static QString accountFor(const QString& host, const QString& user)
{
    return user + QLatin1Char('@') + host;
}

QString passwordFromEnvironment()
{
    return QString::fromUtf8(qgetenv("GLKVM_PASSWORD"));
}

QString userFromEnvironment()
{
    return QString::fromUtf8(qgetenv("GLKVM_USER"));
}

QString password(const QString& host, const QString& user)
{
    const QString stored = loadPasswordImpl(accountFor(host, user));

    // The environment wins only when nothing is stored, so a stale exported
    // variable can't shadow a deliberately saved password.
    return stored.isEmpty() ? passwordFromEnvironment() : stored;
}

bool savePassword(const QString& host, const QString& user, const QString& password)
{
    return savePasswordImpl(accountFor(host, user), password);
}

}
