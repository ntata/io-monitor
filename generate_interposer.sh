#!/bin/sh

cat monitored_functions.data | while read -r LINE ; do
    if echo $LINE | egrep '^#|^$' >/dev/null ; then
	continue;
    fi

    PROTOTYPE=`echo $LINE | cut -d '|' -f 1`
    RET_AND_NAME=`echo $PROTOTYPE | cut -d '(' -f 1`
    NAME=`echo $RET_AND_NAME | rev|  cut -d ' ' -f '1' | rev`
    RET=`echo $RET_AND_NAME | rev |cut -d ' ' -f '1' --complement | rev`

    DOMAIN=`echo $LINE | cut -d '|' -f 2`
    OP=`echo $LINE | cut -d '|' -f 3`
    S1=`echo $LINE | cut -d '|' -f 4`
    S2=`echo $LINE | cut -d '|' -f 5`
    HOOK_AFTER=`echo $LINE | cut -d '|' -f 6`
    REALPATH=`echo $LINE | cut -d '|' -f 7`
    echo "/*"
    echo return: $RET
    echo name: $NAME
    echo "*/"

    echo $PROTOTYPE
    echo '{'
    cat <<EOF
   CHECK_LOADED_FNS();
   PUTS("open");
   DECL_VARS();
   GET_START_TIME();
EOF
    echo -n "   $RET result = orig_$NAME("
    echo $PROTOTYPE | cut -d '(' -f 2- | tr ',' '\n'|
	while read -r ARG ; do
	    echo $ARG | rev | cut -d ' ' -f 1  | rev | tr '\n' ','
	done | sed 's/),/)/'
    echo ';'

    echo "   $HOOK_AFTER"
    echo "   record($DOMAIN, $OP, fd, $S1, $S2, "
    echo "   TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);"
    echo "   return result;"
    echo "}"
done | indent

