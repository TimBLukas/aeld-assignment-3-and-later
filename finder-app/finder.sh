#!/bin/sh

if [ $# -ne 2 ]; then
	echo "Usage: $0 <filesdir> <searchstr>"
	exit 1
fi

filesdir="$1"
searchstr="$2"

if [ ! -d "$filesdir" ]; then
	echo "Directory $filesdir does not exist"
	exit 1
fi

filecnt=$(find "$filesdir" -type f | wc -l)
linescnt=$(grep -rI -F -h -c -- "$searchstr" "$filesdir" 2>/dev/null | awk '{sum += $1} END {print sum+0}')

echo "The number of files are $filecnt and the number of matching lines are $linescnt"



