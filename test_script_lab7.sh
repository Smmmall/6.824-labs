#!/bin/sh

rm -f testlog.txt

for i in `seq 1 3`
do
  ./rsm_tester.pl 8 9 10 11 12 13 14 15 16  2>&1 >> testlog.txt
  killall lock_server; rm -f *.log
done