#!/bin/bash

usage() {
	echo "$0 add|del PID[,...]"   
	echo "add for adding PID[,...] to list to hide"
	echo "del for deleting PID[,...] from list to show"
	echo "Note:"
	echo "     del -1 will delete all PID from list to show"
}

dev="/dev/hideproc"
re='^(-?[0-9]+,?)*$'

if [ "$#" -ne 2 ] || [[ "$1" != "add" && "$1" != "del" ]] || ! [[ $2 =~ $re ]]; then
	usage
else
	echo "$1 $2" | sudo tee "$dev"
fi

