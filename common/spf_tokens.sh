#! /bin/sh -e

die() {
    echo "$@" 1>&2
    exit 2
}

do_hdr() {
    cat <<EOF
/******************************************************************************/
/*          postlicyd: a postfix policy daemon with a lot of features         */
/*          ~~~~~~~~~                                                         */
/*  ________________________________________________________________________  */
/*                                                                            */
/*  Redistribution and use in source and binary forms, with or without        */
/*  modification, are permitted provided that the following conditions        */
/*  are met:                                                                  */
/*                                                                            */
/*  1. Redistributions of source code must retain the above copyright         */
/*     notice, this list of conditions and the following disclaimer.          */
/*  2. Redistributions in binary form must reproduce the above copyright      */
/*     notice, this list of conditions and the following disclaimer in the    */
/*     documentation and/or other materials provided with the distribution.   */
/*  3. The names of its contributors may not be used to endorse or promote    */
/*     products derived from this software without specific prior written     */
/*     permission.                                                            */
/*                                                                            */
/*  THIS SOFTWARE IS PROVIDED BY THE CONTRIBUTORS ``AS IS'' AND ANY EXPRESS   */
/*  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED         */
/*  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE    */
/*  DISCLAIMED.  IN NO EVENT SHALL THE CONTRIBUTORS BE LIABLE FOR ANY         */
/*  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL        */
/*  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS   */
/*  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)     */
/*  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,       */
/*  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN  */
/*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE           */
/*  POSSIBILITY OF SUCH DAMAGE.                                               */
/******************************************************************************/

/*****     THIS FILE IS AUTOGENERATED DO NOT MODIFY DIRECTLY !    *****/

EOF
}

do_h() {
    do_hdr
    cat <<EOF
#ifndef PFIXTOOLS_SPF_TOKENS_H
#define PFIXTOOLS_SPF_TOKENS_H

typedef enum spf_ruleid_t {
    SPF_RULE_UNKNOWN = -1,
`grep_self "$0" | tr 'a-z-/' 'A-Z__' | sed -e 's/.*/    SPF_RULE_&,/'`
    SPF_RULE_count,
} spf_ruleid_t;

extern const char *spftokens[SPF_RULE_count];

__attribute__((pure))
spf_ruleid_t spf_rule_tokenize(const char *s, ssize_t len);
#endif /* PFIXTOOLS_SPF_TOKENS_H */
EOF
}

do_tokens() {
    while read tok; do
        echo "$tok, SPF_RULE_`echo $tok | tr 'a-z-' 'A-Z_'`"
    done
}

do_c() {
    this=`basename "$0"`
    cat <<EOF | gperf --ignore-case -m16 -l -t -C -F",0" -Ntokenize_aux | \
        sed -e '/__gnu_inline__/d;s/\<\(__\|\)inline\>//g'
%{
`do_hdr`

#include "str.h"
#include "`echo "${this%.sh}"`.h"

static const struct tok *
tokenize_aux(const char *str, unsigned int len);

%}
struct tok { const char *name; int val; };
%%
`grep_self "$0" | do_tokens`
%%

const char *spftokens[SPF_RULE_count] = {
`grep_self "$0" | sed -e 's/.*/    "&",/'`
};

spf_ruleid_t spf_rule_tokenize(const char *s, ssize_t len)
{
    if (len < 0)
        len = m_strlen(s);

    if (len) {
        const struct tok *res = tokenize_aux(s, len);
        return res ? res->val : SPF_RULE_UNKNOWN;
    } else {
        return SPF_RULE_UNKNOWN;
    }
}
EOF
}

grep_self() {
    grep '^## ' "$1" | cut -d' ' -f2
}

trap "rm -f $1" 1 2 3 15
rm -f $1
case "$1" in
    *.h) do_h > $1;;
    *.c) do_c > $1;;
    *)  die "you must ask for the 'h' or 'c' generation";;
esac
chmod -w $1

exit 0

# Mechanisms
## all
## include
## a
## mx
## ptr
## ip4
## ip6
## exists
#
# Modifiers
## redirect
## explanation