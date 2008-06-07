/*
 * tls_stuff.c TLS related code for syslogd
 *
 * implements the TLS init and handshake callbacks with all required
 * checks from http://tools.ietf.org/html/draft-ietf-syslog-transport-tls-12
 * (without hostname wildcards)
 *
 * TODO: trans-port-tls12+ (Mail from jsalowey on 080523) requires
 *       server and client to be able to generate self-signed certificates
 *
 * Martin Sch�tte
 */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#include "tls_stuff.h"

/* definitions in syslogd.c */
extern short int Debug;
#define dprintf if (Debug) printf
extern void    logerror(const char *, ...);

/*
 * init OpenSSL lib and one context. returns NULL on error, otherwise SSL_CTX
 * all pointer arguments may be NULL (at least for clients)
 * x509verify determines the level of certificate validation
 */
SSL_CTX *
init_global_TLS_CTX(char const *keyfilename, char const *certfilename, char const *CAfile, char const *CApath, char x509verify)
{
        SSL_CTX *ctx;
        SSL_load_error_strings();
        SSL_library_init();
        OpenSSL_add_all_digests();
        if (!(ctx = SSL_CTX_new(SSLv23_method()))) {
                ERR_print_errors_fp(stderr);
                return NULL;
        }
        /* load keys and certs here */
        if (keyfilename && certfilename) {
                if (!(SSL_CTX_use_PrivateKey_file(ctx, keyfilename, SSL_FILETYPE_PEM)
                    && SSL_CTX_use_certificate_chain_file(ctx, certfilename))) {
                        dprintf("unable to get private key and certificate\n");
                        ERR_print_errors_fp(stderr);
                        exit(1);
                }
                if (!SSL_CTX_check_private_key(ctx)) {
                        dprintf("private key does not match certificate\n");
                        ERR_print_errors_fp(stderr);
                        exit(1);
                } else {
                        dprintf("loaded and checked own certificate\n");
                }
        }
        if (CAfile || CApath) {
                if (!SSL_CTX_load_verify_locations(ctx, CAfile, CApath)) {
                        dprintf("unable to load trust anchors\n");
                        ERR_print_errors_fp(stderr);
                } else {
                        dprintf("loaded trust anchors\n");
                }
        }
        /* peer verification */
        if ((x509verify == X509VERIFY_NONE) || (x509verify == X509VERIFY_IFPRESENT))
                /* ask for cert, but a client does not have to send one */
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, check_peer_cert);
        else
                /* default: ask for cert and check it */
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, check_peer_cert);

        (void)SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        (void)SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
        return ctx;
}
/*
 * get fingerprint of cert
 * returnstring will be allocated and should be free()d by the caller
 * alg_name selects an algorithm, if it is NULL then SHA-1 will be used
 * return value and non-NULL *returnstring indicate success
 */
bool
get_fingerprint(X509 * cert, char **returnstring, char *alg_name)
{
#define MAX_ALG_NAME_LENGTH 8
        unsigned char md[EVP_MAX_MD_SIZE];
        char fp_val[4];
        unsigned int len, memsize;
        EVP_MD *digest;
        int i = 0;
        dprintf("get_fingerprint(cert, %p, %s)\n", returnstring, alg_name);
        *returnstring = NULL;
        if ((alg_name && !(digest = (EVP_MD *) EVP_get_digestbyname(alg_name)))
            || (!alg_name && !(digest = (EVP_MD *) EVP_get_digestbyname("SHA1")))) {
                dprintf("unknown digest algorithm %s\n", alg_name);
                
                return false;
        }
        if (!X509_digest(cert, digest, md, &len)) {
                dprintf("cannot get %s digest\n", alg_name);
                return false;
        }
        /* needed memory. 3 string bytes for every binary byte with delimiter
         * + alg_name with delimiter */
        memsize = (len * 3) + strlen(OBJ_nid2sn(EVP_MD_type(digest))) + 1;
        if (!(*returnstring = malloc(memsize))) {
                dprintf("cannot allocate %d bytes memory\n", memsize);
                return false;
        }
        /* 'normalise' the algorithm name */
        (void)strlcpy(*returnstring, OBJ_nid2sn(EVP_MD_type(digest)), memsize);
        (void)strlcat(*returnstring, ":", memsize);
        /* append the fingeprint data */
        for (i = 0; i < len; i++) {
                (void)snprintf(fp_val, 4, "%02X:", (unsigned int) md[i]);
                (void)strlcat(*returnstring, fp_val, memsize);
        }
        if ((*returnstring)[memsize - 1] != '\0')
                dprintf("memory overflow. last 4 chars are: %c%c%c%c\n",
                    (*returnstring)[memsize - 4], (*returnstring)[memsize - 3],
                    (*returnstring)[memsize - 2], (*returnstring)[memsize - 1]);
        return true;
}
/*
 * test if cert matches as configured hostname or IP
 * checks a 'really used' hostname and optionally a second expected subject
 * against iPAddresses, dnsNames and commonNames
 *
 * TODO: wildcard matching for dnsNames is not implemented.
 *       in transport-tls that is a MAY, and I do not trust them anyway.
 *       but there might be demand for, so it's a todo item.
 */
bool
match_hostnames(X509 * cert, struct tls_conn_settings *conn)
{
        int i, len, num;
        char *buf;
        unsigned char *ubuf;
        GENERAL_NAMES *gennames;
        GENERAL_NAME *gn;
        X509_NAME *x509name;
        X509_NAME_ENTRY *entry;
        ASN1_OCTET_STRING *asn1_ip, *asn1_cn_ip;
        int crit, idx;
        dprintf("match_hostnames() to check cert against %s and %s\n",
            conn->subject, conn->hostname);

        /* see if hostname is an IP */
        i = (asn1_ip = a2i_IPADDRESS(conn->subject)) || (asn1_ip = a2i_IPADDRESS(conn->hostname));

        if (!(gennames = X509_get_ext_d2i(cert, NID_subject_alt_name, &crit, &idx))) {
                dprintf("X509_get_ext_d2i() returned (%p,%d,%d) --> no subjectAltName\n", gennames, crit, idx);
        } else {
                num = sk_GENERAL_NAME_num(gennames);
                if (asn1_ip) {
                        /* first loop: check IPs */
                        for (i = 0; i < num; ++i) {
                                gn = sk_GENERAL_NAME_value(gennames, i);
                                if (gn->type == GEN_IPADD
                                    && !ASN1_OCTET_STRING_cmp(asn1_ip, gn->d.iPAddress))
                                        return true;
                        }
                }
                /* second loop: check DNS names */
                for (i = 0; i < num; ++i) {
                        gn = sk_GENERAL_NAME_value(gennames, i);
                        if (gn->type == GEN_DNS) {
                                buf = (char *)ASN1_STRING_data(gn->d.ia5);
                                len = ASN1_STRING_length(gn->d.ia5);
                                if (!strncasecmp(conn->subject, buf, len)
                                    || !strncasecmp(conn->hostname, buf, len))
                                        return true;
                        }
                }
        }

        /* check commonName; not sure if more than one CNs possible, but we
         * will look at all of them */
        x509name = X509_get_subject_name(cert);
        i = X509_NAME_get_index_by_NID(x509name, NID_commonName, -1);
        while (i != -1) {
                entry = X509_NAME_get_entry(x509name, i);
                len = ASN1_STRING_to_UTF8(&ubuf, X509_NAME_ENTRY_get_data(entry));
                if (len > 0) {
                        dprintf("found CN: %.*s\n", len, ubuf);
                        /* hostname */
                        if ((conn->subject && !strncasecmp(conn->subject, (char*)ubuf, len))
                            || (conn->hostname && !strncasecmp(conn->hostname, (char*)ubuf, len))) {
                                OPENSSL_free(ubuf);
                                return true;
                        }
                        OPENSSL_free(ubuf);
                        /* IP -- convert to ASN1_OCTET_STRING and compare then
                         * so that "10.1.2.3" and "10.01.02.03" are equal */
                        if ((asn1_ip)
                            && (asn1_cn_ip = a2i_IPADDRESS(conn->subject))
                            && !ASN1_OCTET_STRING_cmp(asn1_ip, asn1_cn_ip)) {
                                return true;
                        }
                }
                i = X509_NAME_get_index_by_NID(x509name, NID_commonName, i);
        }
        return false;
}
/*
 * check if certificate matches given fingerprint
 */
bool
match_fingerprint(X509 * cert, struct tls_conn_settings *conn)
{
#define MAX_ALG_NAME_LENGTH 8
        char alg[MAX_ALG_NAME_LENGTH];
        char *certfingerprint;
        char *p, *q;
        dprintf("match_fingerprint(%s)\n", conn->fingerprint);
        if (!conn->fingerprint)
                return false;

        /* get algorithm */
        p = alg;
        q = conn->fingerprint;
        while (*q != ':' && *q != '\0' && p < (char *)alg + MAX_ALG_NAME_LENGTH)
                *p++ = *q++;
        *p = '\0';

        if (!get_fingerprint(cert, &certfingerprint, alg)) {
                dprintf("cannot get %s digest\n", alg);
                return false;
        }
        if (strncmp(certfingerprint, conn->fingerprint, strlen(certfingerprint))) {
                dprintf("fail: fingerprints do not match\n");
                free(certfingerprint);
                return false;
        }
        dprintf("accepted: fingerprints match\n");
        free(certfingerprint);
        return true;
}
/*
 * Callback after OpenSSL has verified a peer certificate,
 * gets called for every certificate in a chain (starting with root CA).
 * preverify_ok indicates a valid trust path (necessary),
 * then we check wether the hostname or configured subject matches the cert.
 */
int
check_peer_cert(int preverify_ok, X509_STORE_CTX * ctx)
{
        char buf[256];
        char *fingerprint;
        SSL *ssl;
        X509 *cur_cert;
        int cur_err, cur_depth;
        bool rc;
        struct tls_conn_settings *conn_info;
        /* read context info */
        cur_cert = X509_STORE_CTX_get_current_cert(ctx);
        cur_err = X509_STORE_CTX_get_error(ctx);
        cur_depth = X509_STORE_CTX_get_error_depth(ctx);
        ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
        conn_info = SSL_get_app_data(ssl);

        /* some info */
        X509_NAME_oneline(X509_get_subject_name(cur_cert), buf, sizeof(buf));
        get_fingerprint(cur_cert, &fingerprint, NULL);
        dprintf("check cert for connection with %s. depth is %d, preverify is %d, subject is %s, fingerprint is %s\n",
            conn_info->hostname, cur_depth, preverify_ok, buf, fingerprint);
        free(fingerprint);


        if (conn_info->x509verify == X509VERIFY_NONE)
                return 1;

        if ((conn_info->force_fingerprint_check) && (cur_depth == 0)) {
                rc = match_fingerprint(cur_cert, conn_info);
                dprintf("depth 0 arrived, match_fingerprint() returned %d\n", rc);
                return rc;
        }
        if (!preverify_ok) {
                if (cur_err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT) {
                        X509_NAME_oneline(X509_get_issuer_name(ctx->current_cert), buf, 256);
                        dprintf("openssl verify error:missing cert for issuer= %s\n", buf);
                }
                dprintf("openssl verify error:num=%d:%s:depth=%d:%s\t\n", cur_err,
                    X509_verify_cert_error_string(cur_err), cur_depth, buf);

                if ((conn_info->fingerprint) && (cur_depth != 0)) {
                        dprintf("accepting otherwise invalid chain element, waiting for depth 0 to check fingerprint\n");
                        conn_info->force_fingerprint_check = true;
                        return 1;
                } else {
                        return 0;
                }
        }
        /* check hostname for last cert in chain */
        if ((cur_depth == 0) && (conn_info->x509verify != X509VERIFY_NONE)) {
                return match_hostnames(cur_cert, conn_info);
        }
        return 1;
}

/*
 * establish TLS connection
 * 
 * TODO: mechanism to try again after x minutes
 * 
 * FIXME: currently I cannot perform the handshake,
 *        It took me a long time (and many dprintfs) to realize why.
 *        It seems that the socket is fine, but SSL_connect cannot
 *        access /dev/random after setuid and chroot.
 * 
 */
#define MAXLINE 512
bool tls_connect(SSL_CTX **context, struct tls_conn_settings *conn)
{
        struct addrinfo hints, *res, *res1;
        int    error, rc, sock;
        const int intsize = sizeof(int);
        //const int one = 1;
        char   buf[MAXLINE];
        SSL    *ssl; 
        SSL_CTX *g_TLS_CTX = *context;
        
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;
        hints.ai_flags = AI_CANONNAME;
        error = getaddrinfo(conn->hostname, (conn->port ? conn->port : SERVICENAME), &hints, &res);
        if (error) {
                logerror(gai_strerror(error));
                return false;
        }
        
        if (!g_TLS_CTX) {
                g_TLS_CTX = init_global_TLS_CTX(MYKEY, MYCERT, MYCA, MYCAPATH, X509VERIFY);
        }
        
        sock = -1;
        for (res1 = res; res1; res1 = res1->ai_next) {
                if (-1 == (sock = socket(res1->ai_family, res1->ai_socktype, res1->ai_protocol))) {
                        dprintf("Unable to open socket.\n");
                        continue;
                }
                dprintf("got socket with fd=%d, protocol=%d\n", sock, res1->ai_protocol);
                
                if (-1 == fcntl(sock, F_SETFL, O_NONBLOCK, 0)) {
                        dprintf("Unable to fcntl(): %s\n", strerror(errno));
                }
                else
                        dprintf("fcntl() to force blocking behaviour\n");
                
                /*
                if ((-1 == (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))))
                                 || (-1 == (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one))))
                                 || (-1 == (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))))) {
                        dprintf("Unable to setsockopt(): %s\n", strerror(errno));
                }
                */
                if (-1 == connect(sock, res1->ai_addr, res1->ai_addrlen)) {
                        dprintf("Unable to connect() to %s: %s\n", res1->ai_canonname, strerror(errno));
                        close(sock);
                        sock = -1;
                        continue;
                }
                dprintf("connect()\n");
                
                if (!(ssl = SSL_new(g_TLS_CTX))) {
                        ERR_error_string_n(ERR_get_error(), buf, MAXLINE);
                        dprintf("Unable to establish TLS: %s\n", buf);
                        close(sock);
                        sock = -1;
                        continue;                                
                }
                dprintf("SSL_new()\n");
                
                if (!SSL_set_fd(ssl, sock)) {
                        ERR_error_string_n(ERR_get_error(), buf, MAXLINE);
                        dprintf("Unable to connect TLS to socket: %s\n", buf);
                        SSL_free(ssl);
                        close(sock);
                        sock = -1;
                        continue;                                
                }
                dprintf("SSL_set_fd(ssl, %d)\n", sock);

                if (-1 == (getsockopt(sock, SOL_SOCKET, SO_ERROR, &rc, (void*)&intsize))) {
                        dprintf("Unable to getsockopt(): %s\n", strerror(errno));}
                if (rc) { dprintf("found socket error: %s\n", strerror(rc)); }
                else    { dprintf("socket %d ok\n", sock); }

                while ((rc = ERR_get_error())) {
                        ERR_error_string_n(rc, buf, MAXLINE);
                        dprintf("Found SSL error in queue: %s\n", buf);
                }

                SSL_set_app_data(ssl, conn);
                SSL_set_connect_state(ssl);
                while ((rc = ERR_get_error())) {
                        ERR_error_string_n(rc, buf, MAXLINE);
                        dprintf("Found SSL error in queue: %s\n", buf);
                }
                dprintf("SSL_set_app_data(), SSL_set_connect_state()\n");

                if (-1 == (getsockopt(sock, SOL_SOCKET, SO_ERROR, &rc, (void*)&intsize))) {
                        dprintf("Unable to getsockopt(): %s\n", strerror(errno));}
                if (rc) { dprintf("found socket error: %s\n", strerror(rc)); }
                else    { dprintf("socket %d ok\n", sock); }
                
                /* connect */
                //rc = SSL_do_handshake(ssl);
                //dprintf("SSL_do_handshake() returned %d: ", rc);
                dprintf("SSL_get_fd() gives: %d\n", SSL_get_fd(ssl));
                dprintf("now calling connect...\n");
                errno = 0;  /* reset to be sure we get the right one later on */
                rc = SSL_connect(ssl);
                if (rc >= 1) {
                        dprintf("TLS connection established.\n");
                        freeaddrinfo(res);
                        conn->sslptr = ssl;
                        return true;  /* okay we got one */
                }
                else {
                        if (rc == 0) {
                                dprintf("TLS connection shut down during connect\n");
                                sock = -1;
                        } else {
                                if (-1 == (getsockopt(sock, SOL_SOCKET, SO_ERROR, &rc, (void*)&intsize))) {
                                        dprintf("Unable to getsockopt(): %s\n", strerror(errno)); }
                                if (rc) { dprintf("found socket error: %s\n", strerror(rc)); }
                                else    { dprintf("socket %d ok\n", sock); }

                                dprintf("SSL_connect() returned error %d: ", rc);
                                error = SSL_get_error(ssl, rc);
                                switch (error) {
                                        case SSL_ERROR_ZERO_RETURN:
                                                dprintf("SSL_ERROR_ZERO_RETURN (connection has been closed cleanly)\n");
                                                break;                                            
                                        case SSL_ERROR_WANT_READ:
                                        case SSL_ERROR_WANT_WRITE:
                                                dprintf("SSL_ERROR_WANT_READ/WRITE\n");
                                                break;                                            
                                        case SSL_ERROR_WANT_CONNECT:
                                        case SSL_ERROR_WANT_ACCEPT:
                                                dprintf("SSL_ERROR_WANT_CONNECT/ACCEPT\n");
                                                break;                                            
                                        case SSL_ERROR_WANT_X509_LOOKUP:
                                                dprintf("SSL_ERROR_WANT_X509_LOOKUP\n");
                                                break;                                            
                                        case SSL_ERROR_SYSCALL:
                                                dprintf("SSL_ERROR_SYSCALL\n");
                                                if ((rc == -1) && !ERR_get_error()) {
                                                        dprintf("socket I/O error: %s\n", strerror(errno));
                                                }
                                                else {
                                                        dprintf("rc != -1, unknown error. errno is %s\n", strerror(errno));
                                                }
                                                break;                                            
                                        case SSL_ERROR_SSL:
                                                dprintf("SSL_ERROR_SSL\n");
                                                break;     
                                        default:
                                                dprintf("unknown error value\n");
                                                break;     
                                }
                                sock = -1;
                        }
                        SSL_shutdown(ssl);
                        SSL_free(ssl);
                        close(sock);
                        continue;
                }
        }
        return false;
}