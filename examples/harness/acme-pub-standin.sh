#!/usr/bin/env bash
# Stand-in for the fictional "acme-pub" so the example harness can be exercised
# end to end. It translates acme-pub flags into the flags of a real publisher
# (moqxr, selected by MOQXR_BIN) and execs it. A real harness does not need this
# file: your own publisher takes the place of acme-pub.
set -euo pipefail
: "${MOQXR_BIN:?set MOQXR_BIN to the openmoq-publisher executable}"

input= connect= transport= draft= namespace= ca= seconds= mode=serve
while (($#)); do
    case "$1" in
        --input) input=$2; shift 2 ;;
        --connect) connect=$2; shift 2 ;;
        --transport) transport=$2; shift 2 ;;
        --draft) draft=$2; shift 2 ;;
        --namespace) namespace=$2; shift 2 ;;
        --track) shift 2 ;;            # moqxr always publishes track vide_1
        --ca) ca=$2; shift 2 ;;
        --exit-after) seconds=$2; shift 2 ;;
        --mode) mode=$2; shift 2 ;;
        *) printf 'acme-pub: unknown option %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ "$transport" == quic ]] && moq_transport=raw || moq_transport=webtransport
forward=0
extra=()
if [[ "$mode" == announce ]]; then
    # moqxr pushes PUBLISH with --forward 1 in draft 21 and publishes its
    # catalog track on request in draft 18.
    if [[ "$draft" == 21 ]]; then forward=1; else extra=(--publish-catalog); fi
fi
args=(--input "$input" --endpoint "$connect" --transport "$moq_transport"
      --namespace "$namespace" --draft "$draft" --forward "$forward"
      --timeout "$seconds" --ca "$ca" "${extra[@]}")
exec "$MOQXR_BIN" "${args[@]}"
