// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// TLS 1.2 client of the session, on OpenSSL memory BIOs; every OpenSSL object is owned and freed in one place.
#include "aa_tls.hpp"

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <pthread.h>

#include <cstdio>

#include "aa_protocol.hpp"
#include "hu_certificate.h"

namespace aa {
namespace {

const char kCipherList[] = "ALL:!aNULL:!eNULL:!LOW:!EXP:!RC4:!MD5:!3DES:!DES:!SEED:!IDEA";
// Handshake bytes from the phone that OpenSSL may hold unread at once. A phone's flight is a few KB (its certificates);
// more is no handshake, and would otherwise pile up in memory for as long as the session lasts.
const size_t kMaxHandshakeInput = 64 * 1024;

pthread_once_t g_sslOnce = PTHREAD_ONCE_INIT;

void initOpenSsl() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

// One object parsed from PEM text, or null.
template <typename T>
T* parsePem(const char* text, T* (*read)(BIO*, T**, pem_password_cb*, void*)) {
    std::unique_ptr<BIO, int (*)(BIO*)> source(BIO_new_mem_buf(const_cast<char*>(text), -1), BIO_free);
    return source ? read(source.get(), nullptr, nullptr, nullptr) : nullptr;
}

void printHead(const char* what, const std::vector<uint8_t>& bytes) {
    printf("[AACryptor] %s (%u bytes):", what, static_cast<unsigned>(bytes.size()));
    for (size_t i = 0; i < bytes.size() && i < 32; ++i) {
        printf(" %02X", bytes[i]);
    }
    printf(bytes.size() > 32 ? " ...\n" : "\n");
}

}  // namespace

TlsClient::TlsClient()
    : context_(nullptr, SSL_CTX_free),
      connection_(nullptr, SSL_free),
      fromPhone_(nullptr),
      toPhone_(nullptr),
      established_(false),
      broken_(false) {}

void TlsClient::reset() {
    connection_.reset();  // the memory BIOs go with it
    context_.reset();
    fromPhone_ = toPhone_ = nullptr;
    established_ = false;
    broken_ = false;
}

void TlsClient::cleanupThread() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(nullptr);
#endif
}

bool TlsClient::start() {
    reset();
    pthread_once(&g_sslOnce, initOpenSsl);
    // Fixed to TLS 1.2, as the car's libssl.so.1.0.0 offers it: the ClientHello the phones have always seen.
    const SSL_METHOD* method = TLSv1_2_client_method();
    if (!method) {
        printf("[AACryptor] this libssl has no TLS 1.2 client\n");
        return false;
    }
    context_.reset(SSL_CTX_new(method));
    if (!context_) {
        printf("[AACryptor] cannot create the TLS context\n");
        return false;
    }
    if (!configure(context_.get())) {
        return false;
    }
    connection_.reset(SSL_new(context_.get()));
    if (!connection_) {
        printf("[AACryptor] cannot create the TLS connection\n");
        return false;
    }
    fromPhone_ = BIO_new(BIO_s_mem());
    toPhone_ = BIO_new(BIO_s_mem());
    if (!fromPhone_ || !toPhone_) {
        if (fromPhone_) {
            BIO_free(fromPhone_);
        }
        if (toPhone_) {
            BIO_free(toPhone_);
        }
        fromPhone_ = toPhone_ = nullptr;
        connection_.reset();
        printf("[AACryptor] cannot create the TLS memory buffers\n");
        return false;
    }
    SSL_set_bio(connection_.get(), fromPhone_, toPhone_);
    SSL_set_connect_state(connection_.get());
    printf("[AACryptor] TLS 1.2 client ready, with the head unit certificate\n");
    return true;
}

bool TlsClient::configure(SSL_CTX* context) {
    // Never compression or session tickets: a libssl 1.0 that finds zlib offers DEFLATE (the car's finds none: null
    // only, seen under QEMU), and decompressed records let a few bytes grow into megabytes. Phones never pick either.
    SSL_CTX_set_options(context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_cipher_list(context, kCipherList);
    // The context keeps references of its own: ours are released at the end of each block, used or not.
    {
        std::unique_ptr<X509, void (*)(X509*)> certificate(parsePem(kHeadUnitCertificatePem, PEM_read_bio_X509),
                                                           X509_free);
        if (!certificate || SSL_CTX_use_certificate(context, certificate.get()) != 1) {
            printf("[AACryptor] head unit certificate refused\n");
            return false;
        }
    }
    std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)> key(parsePem(kHeadUnitPrivateKeyPem, PEM_read_bio_PrivateKey),
                                                       EVP_PKEY_free);
    if (!key || SSL_CTX_use_PrivateKey(context, key.get()) != 1) {
        printf("[AACryptor] head unit private key refused\n");
        return false;
    }
    return true;
}

bool TlsClient::hello(std::vector<uint8_t>& records) {
    if (!connection_) {
        return false;
    }
    SSL_do_handshake(connection_.get());
    takeOutput(records);
    printHead("ClientHello", records);
    return !records.empty();
}

bool TlsClient::handshake(const uint8_t* records, size_t size, std::vector<uint8_t>& answer) {
    answer.clear();
    // Once established (or broken), more handshake bytes would only wait in the input buffer, and the next encrypted
    // frame would be read after them: refused, the buffer stays as it was.
    if (!connection_ || established_ || broken_ || size == 0) {
        return false;
    }
    const size_t unread = static_cast<size_t>(BIO_ctrl_pending(fromPhone_));
    if (size > kMaxHandshakeInput || unread > kMaxHandshakeInput - size) {
        printf("[AACryptor] %u handshake bytes from the phone, %u still unread: more than a handshake\n",
               static_cast<unsigned>(size), static_cast<unsigned>(unread));
        endForGood("handshake");
        return false;
    }
    ERR_clear_error();
    if (BIO_write(fromPhone_, records, static_cast<int>(size)) != static_cast<int>(size)) {
        endForGood("handshake input");
        return false;
    }
    const int result = SSL_do_handshake(connection_.get());
    // With a failure, what OpenSSL wrote is its alert for the phone: handed out all the same.
    takeOutput(answer);
    if (result == 1) {
        established_ = true;
        printf("[AACryptor] TLS established, cipher %s\n", SSL_get_cipher(connection_.get()));
        return true;
    }
    // Waiting for the phone's next flight. Anything else is final: OpenSSL does not take the handshake up again.
    if (SSL_get_error(connection_.get(), result) != SSL_ERROR_WANT_READ) {
        endForGood("handshake");
        return false;
    }
    return true;
}

bool TlsClient::seal(const uint8_t* plain, size_t size, std::vector<uint8_t>& records) {
    records.clear();
    if (!connection_ || !established_ || broken_) {
        return false;
    }
    ERR_clear_error();
    if (SSL_write(connection_.get(), plain, static_cast<int>(size)) != static_cast<int>(size)) {
        // What OpenSSL left for the phone is not this message (after a failed read: its alert). Handed out as sealed,
        // it went to the phone as if it were the message.
        endForGood("write");
        takeOutput(records);
        records.clear();
        return false;
    }
    return takeOutput(records);
}

bool TlsClient::open(const uint8_t* records, size_t size, std::vector<uint8_t>& plain) {
    plain.clear();
    if (!connection_ || !established_ || broken_ || size == 0) {
        return false;
    }
    ERR_clear_error();
    if (BIO_write(fromPhone_, records, static_cast<int>(size)) != static_cast<int>(size)) {
        endForGood("read input");
        return false;
    }
    if (plain.capacity() < size) {
        plain.reserve(size);
    }
    uint8_t chunk[16384];
    int n;
    while ((n = SSL_read(connection_.get(), chunk, sizeof(chunk))) > 0) {
        // A frame brings 64 KiB of records at most: more data than a whole message from one call is no phone's.
        if (static_cast<size_t>(n) > frame::kMaxMessage - plain.size()) {
            printf("[AACryptor] encrypted frame of %u bytes gives more than %u bytes of data\n",
                   static_cast<unsigned>(size), static_cast<unsigned>(frame::kMaxMessage));
            endForGood("read");
            plain.clear();
            return false;
        }
        plain.insert(plain.end(), chunk, chunk + n);
    }
    // Waiting for more: the rest of a record still to come in a later frame. Anything else is final: a record that
    // fails its check is a fatal error in TLS, OpenSSL then refuses every later record, the good ones too.
    if (SSL_get_error(connection_.get(), n) != SSL_ERROR_WANT_READ) {
        endForGood("read");
    } else if (BIO_ctrl_pending(toPhone_) > 0) {
        // After the handshake, SSL_read only writes to the peer when the peer asked to renegotiate (HelloRequest).
        // Android Auto never renegotiates, and leaving those bytes in toPhone_ would mix them into the next seal().
        endForGood("renegotiation");
        plain.clear();
        return false;
    }
    return !plain.empty();
}

void TlsClient::endForGood(const char* step) {
    broken_ = true;
    const char* reason = ERR_reason_error_string(ERR_peek_last_error());
    printf("[AACryptor] TLS %s failed for good (%s): nothing more can be decrypted or sealed\n", step,
           reason ? reason : "no OpenSSL reason");
}

bool TlsClient::takeOutput(std::vector<uint8_t>& records) {
    records.clear();
    const size_t pending = static_cast<size_t>(BIO_ctrl_pending(toPhone_));
    if (pending == 0) {
        return false;
    }
    records.resize(pending);
    size_t got = 0;
    while (got < pending) {
        const int n = BIO_read(toPhone_, &records[got], static_cast<int>(pending - got));
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    records.resize(got);
    return !records.empty();
}

}  // namespace aa
