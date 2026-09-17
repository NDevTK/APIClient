#!/bin/bash
# MAKE A REAL BROWSER TRUST THE AGENT PROXY, IDEMPOTENTLY.
#
# Chrome is the REFERENCE half of the render differential: the engine's paint output is only
# checkable against a real browser rendering the real page. That reference is worthless if the
# browser cannot complete a TLS handshake, and in this environment it cannot, out of the box.
#
# THE DEFECT SHAPE, which is the part that does not rot. Outbound HTTPS is re-terminated by a
# local proxy, so every client must trust its CA. The proxy's own README states that "the browser
# NSS store" is already set up. It is not: Chromium reads $HOME/.pki/nssdb, that database ships
# inside the base image, and the proxy CA is written into the container at start — so the store
# always predates the certificate it is supposed to hold. The CA is long-lived, so this is not
# rotation; it is an image artifact, which means EVERY FRESH CONTAINER HAS IT.
#
# WHAT IT LOOKS LIKE WHEN IT BITES, and why it is worth a script rather than a memory: Chromium
# answers ERR_CERT_AUTHORITY_INVALID, renders a "Privacy error" interstitial, and a screenshot of
# that interstitial is a well-formed PNG of a real page load. Measured: the failing capture
# reported 134 elements and a title of "Privacy error"; the working one reports 736 elements, 72
# subresources and 0 failures. NOTHING about the first says "your trust store is stale" — it says
# "here is a picture", which is §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES arriving as an image.
#
# TLS VERIFICATION IS NEVER DISABLED HERE. This script ADDS trust; it does not remove checking.
# --ignore-certificate-errors is banned in this project and nothing below is a way to spell it.
#
# RETIREMENT: this script goes when a fresh container's $HOME/.pki/nssdb already contains the
# proxy CA — testable in one command, `certutil -d sql:$HOME/.pki/nssdb -L`, before running this.
set -u
DB="sql:${HOME}/.pki/nssdb"
CA_DIR=/usr/local/share/ca-certificates

# THE SCRIPT REPORTS RATHER THAN SKIPS. A setup step that silently does nothing when its inputs
# are missing hands the caller a browser that fails later, at the capture, where the cause is no
# longer visible — so each precondition that cannot be met exits NONZERO and names itself.
[ -d "$CA_DIR" ] || { echo "REFUSING: $CA_DIR does not exist — this is not the expected proxy environment" >&2; exit 1; }
N_CA=$(ls -1 "$CA_DIR"/*.crt 2>/dev/null | wc -l)
[ "$N_CA" -gt 0 ] || { echo "REFUSING: no CA certificates under $CA_DIR — nothing to install" >&2; exit 1; }

if ! command -v certutil >/dev/null 2>&1; then
  echo "certutil absent; installing libnss3-tools"
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y libnss3-tools >/dev/null 2>&1
fi
command -v certutil >/dev/null 2>&1 || { echo "REFUSING: certutil unavailable and could not be installed" >&2; exit 1; }

mkdir -p "${HOME}/.pki/nssdb"
# `certutil -A` is an upsert keyed on the nickname, so re-running replaces rather than duplicating.
for f in "$CA_DIR"/*.crt; do
  certutil -d "$DB" -A -t "C,," -n "$(basename "$f" .crt)" -i "$f" 2>/dev/null
done

# VERIFY, AND GATE THE EXIT STATUS ON THE ANSWER. A check nothing branches on is a comment with a
# pipeline in it, and the caller of a setup script is entitled to an exit code that means something.
N_DB=$(certutil -d "$DB" -L 2>/dev/null | grep -c ',,')
if [ "$N_DB" -lt "$N_CA" ]; then
  echo "REFUSING: $N_DB certificate(s) trusted in the browser store, expected at least $N_CA" >&2
  exit 1
fi
echo "browser trust store holds $N_DB certificate(s) from $CA_DIR ($N_CA offered)"
echo "launch Chromium with --proxy-server=\${HTTPS_PROXY#http://} and leave TLS verification ON"
