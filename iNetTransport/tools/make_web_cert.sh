#!/bin/sh
# Certificates for the HTTPS web server (MbedTlsServer): a small
# certificate authority of your own, made once, and a certificate for
# each device, signed by it. Install ca.crt in the browsers (or the
# operating systems) of the people who use the devices: they then trust
# every device's certificate without a warning. Keep ca.key offline.
#
#   sh make_web_cert.sh <dir> <device-name> <address-or-name>...
#   sh make_web_cert.sh certs plc-line1 192.168.1.40 plc-line1.local
#
# Writes into <dir>:
#   ca.key, ca.crt                     the authority (made only if not there)
#   <device>.key, <device>.crt         the device's key and certificate
#   <device>_cert.h                    both as C strings, for the firmware:
#                                      webCertPem, webKeyPem
#
# ECDSA P-256 throughout: what inet_mbedtls_config.h supports, and the
# cheapest for the MCU to sign with. DAYS (default 825) sets the device
# certificate's life; CA_DAYS (default 3650) the authority's.
set -eu
[ $# -ge 3 ] || { echo "usage: $0 <dir> <device-name> <address-or-name>..." >&2; exit 2; }
dir=$1; name=$2; shift 2
DAYS=${DAYS:-825}
CA_DAYS=${CA_DAYS:-3650}
mkdir -p "$dir"
cd "$dir"

if [ ! -f ca.key ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out ca.key
    openssl req -x509 -new -key ca.key -sha256 -days "$CA_DAYS" -subj "/CN=$name devices CA" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt
    echo "made the authority: ca.crt (install it), ca.key (keep it safe)"
fi

san=""
for a in "$@"; do
    case "$a" in
        *[!0-9.]*) entry="DNS:$a" ;;
        *)         entry="IP:$a" ;;
    esac
    san="${san:+$san,}$entry"
done

openssl ecparam -name prime256v1 -genkey -noout -out "$name.key"
openssl req -new -key "$name.key" -subj "/CN=$name" -out "$name.csr"
cat > "$name.ext" <<EXT
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=$san
EXT
openssl x509 -req -in "$name.csr" -CA ca.crt -CAkey ca.key -CAcreateserial -sha256 -days "$DAYS" \
    -extfile "$name.ext" -out "$name.crt" 2>/dev/null
rm -f "$name.csr" "$name.ext"

# As C strings.
cstr() { sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$1"; }
{
    echo "// $name: made by make_web_cert.sh. The key is secret: keep this file out of"
    echo "// version control, or load the key from protected storage instead."
    echo "#pragma once"
    echo "static const char webCertPem[] ="
    cstr "$name.crt"
    echo "    ;"
    echo "static const char webKeyPem[] ="
    cstr "$name.key"
    echo "    ;"
} > "${name}_cert.h"
echo "made $name.crt for $san, and ${name}_cert.h"
