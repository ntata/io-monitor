#!/bin/sh
echo "const char* $1[] = {"
grep -v "^//" | grep , | cut -d , -f 1 | cut -d / -f 1 | tr -d \  | while read i ; do echo \"$i\", ; done
echo "};"
