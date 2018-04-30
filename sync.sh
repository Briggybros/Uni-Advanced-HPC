#!/bin/bash
# Place and run in project root

EVENTS="CREATE,CLOSE_WRITE,DELETE,MODIFY,MOVED_FROM,MOVED_TO"

inotifywait -e "$EVENTS" -m -r --format '%:e %f' . | (
    WAITING="";
    while true; do
        LINE="";
        read -t 1 LINE;
        if test -z "$LINE"; then
            if test ! -z "$WAITING"; then
                    echo "CHANGE";
                    WAITING="";
            fi;
        else
            WAITING=1;
        fi;
    done) | (
    while true; do
        read TMP;
        # Change below to match your bcp4 alias and folder structure on bcp4
        rsync -av -r -u --rsh=ssh . bluecrystal:/mnt/storage/home/gs15687/advanced-hpc/
    done
)