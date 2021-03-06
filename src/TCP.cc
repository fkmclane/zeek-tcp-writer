// See the file "COPYING" for copyright.
//
// Log writer for writing to TCP

#include <string>

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>

#include "TCP.h"

using namespace logging;
using namespace writer;

TCP::TCP(WriterFrontend * frontend) : WriterBackend(frontend), ssl_init(false), sock(-1), host((const char *)BifConst::LogTCP::host->Bytes(), BifConst::LogTCP::host->Len()), tcpport(BifConst::LogTCP::tcpport), retry(BifConst::LogTCP::retry), tls(BifConst::LogTCP::tls), cert((const char *)BifConst::LogTCP::cert->Bytes(), BifConst::LogTCP::cert->Len()), key((const char *)BifConst::LogTCP::key->Bytes(), BifConst::LogTCP::key->Len()) {}

TCP::~TCP() {
    if (ssl_init) {
        // cleanup global tls
        ERR_free_strings();
        EVP_cleanup();
    }
}

std::string TCP::GetConfigValue(const WriterInfo & info, const std::string name) const {
    // find config value and return it or an empty string
    std::map<const char *, const char *>::const_iterator it = info.config.find(name.c_str());
    if (it == info.config.end())
        return std::string();
    else
        return it->second;
}

bool TCP::DoLoad(bool is_retry) {
    // error value
    int ret;
    long lret;

    // get address info
    struct addrinfo * addr;
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    ret = getaddrinfo(host.c_str(), std::to_string(tcpport).c_str(), &hints, &addr);
    if (ret > 0) {
        Error(Fmt("Error resolving %s: %s", host.c_str(), strerror(errno)));

        // clean up
        freeaddrinfo(addr);
        return false;
    }

    sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sock < 0) {
        Error(Fmt("Error opening socket: %s", strerror(errno)));

        // clean up
        freeaddrinfo(addr);
        return false;
    }

    ret = connect(sock, addr->ai_addr, addr->ai_addrlen);
    if (ret < 0) {
        char addrstr[INET6_ADDRSTRLEN];
        inet_ntop(addr->ai_family, addr->ai_addr->sa_family == AF_INET ? &(((struct sockaddr_in *)addr->ai_addr)->sin_addr) : (struct in_addr *)&(((struct sockaddr_in6 *)addr->ai_addr)->sin6_addr), addrstr, sizeof(addrstr));
        if (retry) {
            if (!is_retry)
                Warning(Fmt("Error connecting to %s: %s", addrstr, strerror(errno)));
        }
        else {
            Error(Fmt("Error connecting to %s: %s", addrstr, strerror(errno)));
        }

        // clean up and return success if retrying
        freeaddrinfo(addr);
        close(sock);
        sock = -1;
        return retry;
    }

    // clean up
    freeaddrinfo(addr);

    if (tls) {
        if (!ssl_init) {
            // add tls
            SSL_load_error_strings();
            SSL_library_init();
            OpenSSL_add_all_algorithms();

            ssl_init = true;
        }

        // create context for tls
        ctx = SSL_CTX_new(SSLv23_client_method());
        if (ctx == nullptr) {
            Error(Fmt("Error setting up TLS context: %s", ERR_reason_error_string(ERR_get_error())));

            // clean up
            close(sock);
            sock = -1;
            return false;
        }

        if (cert.empty()) {
            // load default paths in context
            ret = SSL_CTX_set_default_verify_paths(ctx);
            if (ret <= 0) {
                Error(Fmt("Error loading default certificate paths: %s", ERR_reason_error_string(ERR_get_error())));

                // clean up
                SSL_CTX_free(ctx);
                close(sock);
                sock = -1;
                return false;
            }
        }
        else {
            // add certificate to context
            ret = SSL_CTX_load_verify_locations(ctx, cert.c_str(), NULL);
            if (ret <= 0) {
                Error(Fmt("Error using TLS certificate: %s", ERR_reason_error_string(ERR_get_error())));

                // clean up
                SSL_CTX_free(ctx);
                close(sock);
                sock = -1;
                return false;
            }
        }

        // setup tls connection
        ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            Error(Fmt("Error setting up TLS structure: %s", ERR_reason_error_string(ERR_get_error())));

            // clean up
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // set tls hostname
        ret = SSL_set_tlsext_host_name(ssl, host.c_str());
        if (ret != 1) {
            Error(Fmt("Error setting TLS descriptor: %s", ERR_reason_error_string(SSL_get_error(ssl, ret))));

            // clean up
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // set underlying file descriptor
        ret = SSL_set_fd(ssl, sock);
        if (ret != 1) {
            Error(Fmt("Error setting TLS descriptor: %s", ERR_reason_error_string(SSL_get_error(ssl, ret))));

            // clean up
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // do handshake
        ret = SSL_connect(ssl);
        if (ret != 1) {
            Error(Fmt("Error completing TLS handshake: %s", ERR_reason_error_string(SSL_get_error(ssl, ret))));

            // clean up
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // get peer certificate
        X509 * peer = SSL_get_peer_certificate(ssl);
        if (peer == nullptr) {
            Error(Fmt("Error getting TLS certificate"));

            // clean up
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // verify peer certificate
        lret = SSL_get_verify_result(ssl);
        if (lret != X509_V_OK) {
            Error(Fmt("Error verifying TLS certificate: %s", X509_verify_cert_error_string(lret)));

            // clean up
            X509_free(peer);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            sock = -1;
            return false;
        }

        // clean up
        X509_free(peer);
    }

    if (!key.empty()) {
        if (tls) {
            // write key
            ret = SSL_write(ssl, (key + "\n").c_str(), key.size() + 1);
            if (ret < 0) {
                Error(Fmt("Error sending TLS data: %s", ERR_reason_error_string(ERR_get_error())));

                // clean up
                SSL_shutdown(ssl);
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                close(sock);
                sock = -1;
                return false;
            }
        }
        else {
            // write key
            ret = send(sock, key.c_str(), key.size(), 0);
            if (ret < 0) {
                Error(Fmt("Error sending data: %s", strerror(errno)));

                // clean up
                close(sock);
                sock = -1;
                return false;
            }
        }
    }

    return true;
}

bool TCP::DoUnload() {
    if (tls) {
        // stop tls
        SSL_shutdown(ssl);
        SSL_free(ssl);

        // free context
        SSL_CTX_free(ctx);
    }

    // close socket
    close(sock);
    sock = -1;

    return true;
}

bool TCP::DoInit(const WriterInfo & info, int num_fields, const threading::Field * const * fields) {
    // get configuration value
    std::string cfg_host = GetConfigValue(info, "host");
    std::string cfg_tcpport = GetConfigValue(info, "tcpport");
    std::string cfg_retry = GetConfigValue(info, "retry");
    std::string cfg_tls = GetConfigValue(info, "tls");
    std::string cfg_cert = GetConfigValue(info, "cert");
    std::string cfg_key = GetConfigValue(info, "key");

    // fill in non-empty values
    if (!cfg_host.empty())
        host = cfg_host;
    if (!cfg_tcpport.empty())
        tcpport = stoi(cfg_tcpport);
    if (!cfg_retry.empty())
        retry = cfg_retry == "T";
    if (!cfg_tls.empty())
        tls = cfg_tls == "T";
    if (!cfg_cert.empty())
        cert = cfg_cert;
    if (!cfg_key.empty())
        key = cfg_key;

    // prepare json formatter
    formatter = new threading::formatter::JSON(this, threading::formatter::JSON::TS_EPOCH);

    return DoLoad();
}

bool TCP::DoFinish(double network_time) {
    int ret = DoUnload();

    // free json formatter
    delete formatter;

    return ret;
}

bool TCP::DoWrite(int num_fields, const threading::Field * const * fields, threading::Value ** vals) {
    int ret;

    if (sock < 0) {
        if (retry) {
            DoLoad(true);

            if (sock < 0)
                return true;
        }
        else {
            return false;
        }
    }

    buffer.Clear();

    formatter->Describe(&buffer, num_fields, fields, vals);

    buffer.AddRaw("\n", 1);

    const char * msg = (const char *)buffer.Bytes();
    size_t len = buffer.Len();

    if (tls) {
        ret = SSL_write(ssl, msg, len);
        if (ret < 0) {
            if (retry) {
                DoUnload();
                DoLoad();
            }
            else {
                Error(Fmt("Error sending TLS data: %s", ERR_reason_error_string(ERR_get_error())));
                return false;
            }
        }
    }
    else {
        ret = send(sock, msg, len, 0);
        if (ret < 0) {
            if (retry) {
                DoUnload();
                DoLoad();
            }
            else {
                Error(Fmt("Error sending data: %s", strerror(errno)));
                return false;
            }
        }
    }

    return true;
}

bool TCP::DoSetBuf(bool enabled) {
    return true;
}

bool TCP::DoFlush(double network_time) {
    return true;
}

bool TCP::DoRotate(const char * rotated_path, double open, double close, bool terminating) {
    // No log rotation needed
    return FinishedRotation();
}

bool TCP::DoHeartbeat(double network_time, double current_time) {
    return true;
}
