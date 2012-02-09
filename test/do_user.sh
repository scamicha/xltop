#!/bin/bash

# xid=$(printf "%llx" $RANDOM)
user="blarf"
domain="ranger.tacc.utexas.edu"
now=$(date +%s)
auth=11

function xid {
    printf "%llx" $(((RANDOM << 15) + RANDOM))
}

(
    echo %user_connect $(xid) ${user} ${domain} ${now} ${auth}
    echo %sub $(xid) job:2318898@ranger fs:share
    echo %sub $(xid) job:2321095@ranger fs:share
    echo %sub $(xid) job:2318363@ranger fs:share
    sleep 3600
) | nc -q -1 localhost 9901
