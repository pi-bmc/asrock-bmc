#!/bin/sh
# rhi-replay — replay the exact HTTP requests the stock host BIOS issues at the
# Redfish Host Interface, so the BMC side can be validated with NO host
# involvement.
#
# Every URI and verb below was extracted from the stock BIOS binaries
# (FirmwareConfigDrv / AmiRedfishDynExt / RedfishHi); see
# asrock-bmc/.claude/rhi-http-contract.md. If a request here fails, the real
# BIOS would fail the same way.
#
# Usage: rhi-replay [base-url]      default https://169.254.0.17
#
# Over the real host interface no credentials are used or needed -- that is the
# whole point: stock MegaRAC grants AuthNoneRoleId=Administrator to connections
# arriving on the host-interface address, and our bmcweb patch reproduces that
# in authentication::isHostInterface. To exercise the same routes from a
# workstation over the management NIC, where bmcweb correctly demands
# authentication, set RHI_AUTH:
#
#     RHI_AUTH=root:0penBmc rhi-replay https://10.0.80.1
#
# NOTE: the POST/PATCH checks WRITE BaseBIOSTable, adding two RhiSelfTest*
# attributes. They are cleared by the next asrock-bios-flash-sync run, which
# republishes the whole table from the BIOS ROM.
#
# This is a developer tool and is not installed into the image (the BMC has no
# curl). Run it from a workstation.
#
# Exit status is the number of failed checks, so it is usable in CI.

set -u

BASE="${1:-https://169.254.0.17}"
CURL="curl -sk --max-time 20"
[ -n "${RHI_AUTH:-}" ] && CURL="$CURL -u $RHI_AUTH"
FAIL=0
PASS=0

check() {
    # check <expected-status-regex> <description> <curl args...>
    expected="$1"; desc="$2"; shift 2
    : > /tmp/rhi-replay.out
    code=$($CURL -o /tmp/rhi-replay.out -w '%{http_code}' "$@" 2>/dev/null)
    if echo "$code" | grep -qE "^$expected$"; then
        PASS=$((PASS + 1))
        printf '  ok    %-56s [%s]\n' "$desc" "$code"
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL  %-56s [%s, want %s]\n' "$desc" "$code" "$expected"
        sed -n '1,2p' /tmp/rhi-replay.out 2>/dev/null | cut -c1-100 | sed 's/^/          /'
    fi
}

echo "rhi-replay against $BASE"
echo
echo "-- discovery (RedfishHi) --"
check '200' 'GET  /redfish/v1/' "$BASE/redfish/v1/"

echo
echo "-- registry (FirmwareConfigDrv) --"
check '200' 'GET  /redfish/v1/Registries' "$BASE/redfish/v1/Registries"
check '201' 'POST BiosAttributeRegistryX570.1.0.0.json' \
    -X POST -H 'Content-Type: application/json;charset=utf-8' \
    -d '{"RegistryEntries":{"Attributes":[]}}' \
    "$BASE/redfish/v1/Registries/BiosAttributeRegistryX570.1.0.0.json"

echo
echo "-- current settings, all three URI spellings --"
check '200' 'GET  /Systems/Self/Bios'          "$BASE/redfish/v1/Systems/Self/Bios"
check '200' 'GET  /Systems/1/Bios      (alias)' "$BASE/redfish/v1/Systems/1/Bios"
check '200' 'GET  /Systems/system/Bios (upstream must still work)' \
    "$BASE/redfish/v1/Systems/system/Bios"

check '200' 'POST current settings' \
    -X POST -H 'Content-Type: application/json;charset=utf-8' \
    -d '{"AttributeRegistry":"BiosAttributeRegistryX570.1.0.0","Attributes":{"RhiSelfTest":"Enabled","RhiSelfTestNum":7}}' \
    "$BASE/redfish/v1/Systems/Self/Bios"

if $CURL "$BASE/redfish/v1/Systems/Self/Bios" | grep -q 'RhiSelfTest'; then
    PASS=$((PASS + 1)); echo '  ok    POSTed attribute is readable back'
else
    FAIL=$((FAIL + 1)); echo '  FAIL  POSTed attribute is NOT readable back'
fi

check '200' 'PATCH current settings' \
    -X PATCH -H 'Content-Type: application/json;charset=utf-8' \
    -d '{"Attributes":{"RhiSelfTest":"Disabled"}}' \
    "$BASE/redfish/v1/Systems/Self/Bios"

echo
echo "-- pending settings (the write path into the host) --"
check '200' 'GET  /Systems/Self/Bios/SD'  "$BASE/redfish/v1/Systems/Self/Bios/SD"
check '204' 'DELETE /Systems/Self/Bios/SD' \
    -X DELETE "$BASE/redfish/v1/Systems/Self/Bios/SD"

echo
echo "-- defaults + action queues --"
check '200|404' 'GET  /redfish/v1/bios/defaultSD' "$BASE/redfish/v1/bios/defaultSD"
check '201' 'POST /redfish/v1/bios/defaultSD' \
    -X POST -H 'Content-Type: application/json;charset=utf-8' \
    -d '{"Attributes":{}}' "$BASE/redfish/v1/bios/defaultSD"
check '204' 'DELETE /redfish/v1/bios/defaultSD' \
    -X DELETE "$BASE/redfish/v1/bios/defaultSD"
check '200' 'GET  Actions/Bios.ResetBios' \
    "$BASE/redfish/v1/Systems/Self/Bios/Actions/Bios.ResetBios"
check '204' 'DELETE Actions/Bios.ResetBios' \
    -X DELETE "$BASE/redfish/v1/Systems/Self/Bios/Actions/Bios.ResetBios"
check '200' 'GET  Actions/Bios.ChangePassword' \
    "$BASE/redfish/v1/Systems/Self/Bios/Actions/Bios.ChangePassword"
check '204' 'DELETE Actions/Bios.ChangePassword' \
    -X DELETE "$BASE/redfish/v1/Systems/Self/Bios/Actions/Bios.ChangePassword"

echo
echo "-- dynamic extensions + static files (AmiRedfishDynExt) --"
check '200' 'GET  /DynamicExtension/RedfishExtensions' \
    "$BASE/redfish/v1/DynamicExtension/RedfishExtensions"
check '200' 'POST /DynamicExtension/RedfishExtensions/PostStatus' \
    -X POST -d '{}' \
    "$BASE/redfish/v1/DynamicExtension/RedfishExtensions/PostStatus"
check '200' 'POST /redfish/v1/BiosStaticFiles (gzip)' \
    -X POST -H 'Content-Type: application/gzip' --data-binary 'GZIPSTUB' \
    "$BASE/redfish/v1/BiosStaticFiles"
# A WELL-FORMED multipart body matters: bmcweb parses multipart before routing
# and silently drops a malformed one with no response at all (curl sees 000).
MPB='----WebKitFormBoundary7MA4YWxkTrZu0gW'
printf -- "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x.gz\"\r\nContent-Type: application/gzip\r\n\r\nPAYLOAD\r\n--%s--\r\n" \
    "$MPB" "$MPB" > /tmp/rhi-replay.mp
check '200' 'POST /ami/static-file (multipart, as the BIOS sends)' \
    -X POST -H "Content-Type: multipart/form-data; boundary=$MPB" \
    --data-binary @/tmp/rhi-replay.mp "$BASE/ami/static-file"

echo
echo "-- path traversal must be refused --"
check '400|404' 'POST ../../etc/shadow via Registries' \
    -X POST -d 'x' "$BASE/redfish/v1/Registries/..%2f..%2fetc%2fshadow"

echo
echo "passed $PASS, failed $FAIL"
exit $FAIL
