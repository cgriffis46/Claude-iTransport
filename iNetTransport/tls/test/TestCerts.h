// TEST ONLY: a throwaway authority and device certificate for 127.0.0.1 and
// localhost, made with tools/make_web_cert.sh (DAYS=7300). The keys are public:
// never use them anywhere else.
#pragma once
static const char testCaPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBqTCCAVCgAwIBAgIUcnE99mdCPh2syL4bWBXB7P5t20swCgYIKoZIzj0EAwIw\n"
    "ITEfMB0GA1UEAwwWdGVzdC1kZXZpY2UgZGV2aWNlcyBDQTAeFw0yNjEwMDgxOTMy\n"
    "MDJaFw00NjEwMDMxOTMyMDJaMCExHzAdBgNVBAMMFnRlc3QtZGV2aWNlIGRldmlj\n"
    "ZXMgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATJk8ExHgiHKRvF/O0tjD99\n"
    "zGu/HD0oFK4Yt12eE2P8ZGm8LMgNAErd+UnBFsQNYpJxutmVydmXTVhEZXXJ9Gcl\n"
    "o2YwZDAdBgNVHQ4EFgQU0Tj6jrLh/s9PJZ90ND7WHXlTFvIwHwYDVR0jBBgwFoAU\n"
    "0Tj6jrLh/s9PJZ90ND7WHXlTFvIwEgYDVR0TAQH/BAgwBgEB/wIBADAOBgNVHQ8B\n"
    "Af8EBAMCAQYwCgYIKoZIzj0EAwIDRwAwRAIgG6FjeCiGq5P7w51GAwdbiPkKqmRz\n"
    "G/O7ZER/vvqLOCYCIA65HrA9JOR2BA6ciz5JhEl/Dh4XPhaNn5UgMmWWS5EQ\n"
    "-----END CERTIFICATE-----\n"
    ;
static const char testCertPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIByzCCAXKgAwIBAgIUac6FSf5v+43KyGOVipK5za8ckEowCgYIKoZIzj0EAwIw\n"
    "ITEfMB0GA1UEAwwWdGVzdC1kZXZpY2UgZGV2aWNlcyBDQTAeFw0yNjEwMDgxOTMy\n"
    "MDJaFw00NjEwMDMxOTMyMDJaMBYxFDASBgNVBAMMC3Rlc3QtZGV2aWNlMFkwEwYH\n"
    "KoZIzj0CAQYIKoZIzj0DAQcDQgAExIMxKRg5XAdditG9doAaYf7xjrYLv/ZBLKWR\n"
    "LuAqVnUCB0lrR1yR18i56ZJUcDDkvFfnrYg5cE0cO/Ku5ZvjhKOBkjCBjzAMBgNV\n"
    "HRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDATAa\n"
    "BgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwHQYDVR0OBBYEFKQkVtcFnoGYUdHv\n"
    "vbl30uciGYNXMB8GA1UdIwQYMBaAFNE4+o6y4f7PTyWfdDQ+1h15UxbyMAoGCCqG\n"
    "SM49BAMCA0cAMEQCIDlrcVEOr2yqJlKEthMJeJqhTGcoydbayqw0BlQmqhAQAiAt\n"
    "O57mn8pTiGyLEpi6KT9KJ+JqzCZyjwz/bU/3bgcAOA==\n"
    "-----END CERTIFICATE-----\n"
    ;
static const char testKeyPem[] =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEICWsZLr2isoxfs5KGbo3IerkPPbkqj8f7nCK+x4kX/T0oAoGCCqGSM49\n"
    "AwEHoUQDQgAExIMxKRg5XAdditG9doAaYf7xjrYLv/ZBLKWRLuAqVnUCB0lrR1yR\n"
    "18i56ZJUcDDkvFfnrYg5cE0cO/Ku5ZvjhA==\n"
    "-----END EC PRIVATE KEY-----\n"
    ;
