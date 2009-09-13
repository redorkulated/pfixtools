/******************************************************************************/
/*          pfixtools: a collection of postfix related tools                  */
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
/*                                                                            */
/*   Copyright (c) 2006-2009 the Authors                                      */
/*   see AUTHORS and source files for details                                 */
/******************************************************************************/

/*
 * Copyright © 2009 Florent Bruneau
 */

#include <ctype.h>
#include <arpa/inet.h>

#include "spf.h"
#include "spf_tokens.h"
#include "buffer.h"

#define SPF_MAX_RECUSION 15

typedef union ip_t {
    uint32_t v4;
    uint8_t  v6[16];
} ip_t;

typedef struct spf_rule_t {
    spf_code_t qualifier;
    spf_ruleid_t rule;
    buffer_t content;
    ip_t ip;
    uint8_t cidr4;
    uint8_t cidr6;
} spf_rule_t;
ARRAY(spf_rule_t);
#define SPF_RULE_INIT { 0, 0, ARRAY_INIT, { .v6 = { 0 } }, 0, 0 }

struct spf_t {
    unsigned txt_received : 1;
    unsigned txt_inerror  : 1;
    unsigned txt_toomany  : 1;
    unsigned spf_received : 1;
    unsigned spf_inerror  : 1;
    unsigned spf_nolookup : 1;
    unsigned canceled     : 1;
    unsigned is_ip6       : 1;
    unsigned use_domain   : 1;
    unsigned in_macro     : 1;
    unsigned a_dnserror   : 1;

    uint32_t ip4;
    uint8_t ip6[16];
    buffer_t ip;
    buffer_t domain;
    buffer_t sender;
    buffer_t helo;
    buffer_t validated;

    buffer_t record;
    A(spf_rule_t) rules;
    uint8_t current_rule;
    int8_t redirect;

    buffer_t domainspec;

    int8_t recursions;
    struct spf_t* subquery;

    uint8_t a_resolutions;
    uint8_t mech_withdns;

    uint8_t queries;
    spf_result_t exit;
    void* data;
};

#define current_rule(spf) array_elt((spf)->rules, (spf)->current_rule)

static PA(spf_t) spf_pool = ARRAY_INIT;
static A(spf_rule_t) spf_rule_pool = ARRAY_INIT;

static buffer_t query_buffer = ARRAY_INIT;
static buffer_t dns_buffer = ARRAY_INIT;

static int created = 0;
static int acquired = 0;
static int deleted = 0;
static int released = 0;

static spf_t* spf_new(void)
{
    ++created;
    return p_new(spf_t, 1);
}

static void spf_rule_wipe(spf_rule_t* rule)
{
    array_wipe(rule->content);
}

static void spf_wipe(spf_t* spf)
{
    array_append(spf_rule_pool, array_start(spf->rules), array_len(spf->rules));
    array_wipe(spf->rules);
    array_wipe(spf->domain);
    array_wipe(spf->ip);
    array_wipe(spf->sender);
    array_wipe(spf->helo);
    array_wipe(spf->validated);
    array_wipe(spf->record);
    array_wipe(spf->domainspec);
    p_clear(spf, 1);
}

static void spf_delete(spf_t **spf)
{
    if (*spf) {
        ++deleted;
        spf_wipe(*spf);
        p_delete(spf);
    }
}

static spf_t* spf_acquire(void)
{
    ++acquired;
    spf_t* spf = NULL;
    if (array_len(spf_pool)) {
        spf = array_pop_last(spf_pool);
    } else {
        spf = spf_new();
    }
    debug("spf pool: acquiring %p - pool length: %d (created %d)", spf, array_len(spf_pool), created);
    return spf;
}

static void spf_module_exit(void)
{
    array_deep_wipe(spf_pool, spf_delete);
    array_deep_wipe(spf_rule_pool, spf_rule_wipe);
    buffer_wipe(&query_buffer);
    buffer_wipe(&dns_buffer);
    debug("spf: Created: %d, Deleted: %d, Acquired: %d, Release: %d", created, deleted, acquired, released);
}
module_exit(spf_module_exit);

static bool spf_release(spf_t* spf, bool decrement)
{
    if (decrement) {
        --spf->queries;
    }
    if (spf->canceled && spf->queries == 0) {
        ++released;
        debug("spf pool: releasing %p - pool length: %d (created: %d)", spf, array_len(spf_pool) + 1, created);
        array_append(spf_rule_pool, array_start(spf->rules), array_len(spf->rules));
        array_len(spf->rules) = 0;
        buffer_reset(&spf->domain);
        buffer_reset(&spf->ip);
        buffer_reset(&spf->sender);
        buffer_reset(&spf->record);
        buffer_reset(&spf->helo);
        buffer_reset(&spf->validated);
        buffer_reset(&spf->domainspec);
        spf_t bak = *spf;
        p_clear(spf, 1);
        spf->rules = bak.rules;
        spf->domain = bak.domain;
        spf->ip = bak.ip;
        spf->sender = bak.sender;
        spf->record = bak.record;
        spf->helo = bak.helo;
        spf->validated = bak.validated;
        spf->domainspec = bak.domainspec;
        array_add(spf_pool, spf);
        return true;
    }
    return false;
}

static bool spf_validate_domain(const char* domain) {
    int label_count = 0;
    int label_length = 0;
    const char* pos = domain;
    while (*pos != '\0') {
        if (*pos == '.') {
            if (label_length == 0) {
                debug("spf: invalid domain name \"%s\": contains a non-terminal zero-length label", domain);
                return false;
            }
            label_length = 0;
        } else if (!isalnum(*pos) && *pos != '-' && *pos != '_') {
            debug("spf: invalid domain name \"%s\": contains illegal character '%c'", domain, *pos);
            return false;
        } else {
            if (label_length == 0) {
                ++label_count;
            }
            ++label_length;
            if (label_length > 63) {
                debug("spf: invalid domain name \"%s\": contains a too long label", domain);
                return false;
            }
        }
        ++pos;
    }
    return label_count > 1;
}

static bool spf_query(spf_t* spf, const char* query, dns_rrtype_t rtype, ub_callback_t cb)
{
    buffer_reset(&query_buffer);
    buffer_addstr(&query_buffer, query);
    if (array_last(query_buffer) != '.') {
        buffer_addch(&query_buffer, '.');
    }
    debug("spf (depth=%d): performing query of type %d for %s", spf->recursions, rtype, query);
    if (dns_resolve(array_start(query_buffer), rtype, cb, spf)) {
        if (rtype == DNS_RRT_A || rtype == DNS_RRT_AAAA) {
            ++spf->a_resolutions;
        }
        ++spf->queries;
        return true;
    }
    return false;
}

static void spf_next(spf_t* spf, bool start);

static void spf_exit(spf_t* spf, spf_code_t code)
{
    if (spf->in_macro) {
        spf_next(spf, false);
        return;
    }
    if (log_level >= LOG_NOTICE) {
        const char* str = NULL;
        switch (code) {
          case SPF_NONE: str = "NONE"; break;
          case SPF_NEUTRAL: str = "NEUTRAL"; break;
          case SPF_PASS: str = "PASS"; break;
          case SPF_FAIL: str = "FAIL"; break;
          case SPF_SOFTFAIL: str = "SOFTFAIL"; break;
          case SPF_TEMPERROR: str = "TEMPERROR"; break;
          case SPF_PERMERROR: str = "PERMERROR"; break;
        }
        info("spf (depth=%d): result for query is %s", spf->recursions, str);
    }
    if (spf->exit) {
        spf->exit(code, NULL, spf->data);
    }
    spf_cancel(spf);
}

static void spf_write_macro(buffer_t* buffer, static_str_t str, bool escape)
{
    if (!escape) {
        buffer_add(buffer, str.str, str.len);
    } else {
        for (int i = 0 ; i < str.len ; ++i) {
            const char c = str.str[i];
            /* ";" / "?" / ":"       |
             *    |                |  / "@" / "&" / "=" / "+" / "$" / "," / "/" */
            if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~'
                || c == ';' || c == '?' || c == ':' || c == '@' || c == '&' || c == '='
                || c == '+' || c == '$' || c == ',' || c == '/') {
                buffer_addch(buffer, c);
            } else {
                buffer_addf(buffer, "%%%02x", c);
            }
        }
    }
}


/* Rule processing
 */
static void spf_include_exit(spf_code_t result, const char* exp, void* arg);
static void spf_redirect_exit(spf_code_t result, const char* exp, void* arg);
static void spf_a_receive(void* arg, int err, struct ub_result* result);
static void spf_mx_receive(void* arg, int err, struct ub_result* result);
static void spf_exists_receive(void* arg, int err, struct ub_result* result);
static bool spf_run_ptr_resolution(spf_t* spf, const char* domainspec, bool in_macro);

static bool spf_dns_in_error(int err, struct ub_result* result)
{
    return (err != 0 || (result->rcode != DNS_RCODE_NOERROR && result->rcode != DNS_RCODE_NXDOMAIN));
}

typedef enum {
    SPFEXP_VALID,
    SPFEXP_INVALID,
    SPFEXP_SYNTAX,
    SPFEXP_DNS
} spf_expansion_t;

static spf_expansion_t spf_expand_pattern(spf_t* spf, buffer_t* buffer, char identifier, int parts, bool reverse,
                                          bool escape, const char* delimiters, int delimiters_count) {
    static_str_t sections[256] = { { .str = 0, .len = 0}, };
    static_str_t* pos = sections;
    switch (identifier) {
      case 's':
        pos->str = array_start(spf->sender);
        pos->len = array_len(spf->sender);
        break;
      case 'l':
        pos->str = array_start(spf->sender);
        pos->len = strchr(pos->str, '@') - pos->str;
        break;
      case 'o':
        pos->str = strchr(pos->str, '@') + 1;
        pos->len = m_strlen(pos->str);
        break;
      case 'd':
        pos->str = array_start(spf->domain);
        pos->len = array_len(spf->domain);
        break;
      case 'i':
        pos->str = array_start(spf->ip);
        pos->len = array_len(spf->ip);
        break;
      case 'p':
        if (array_len(spf->validated) > 0) {
            pos->str = array_start(spf->validated);
            pos->len = array_len(spf->validated);
        } else if (spf_run_ptr_resolution(spf, NULL, true)) {
            return SPFEXP_DNS;
        } else {
            pos->str = "unknown";
            pos->len = m_strlen(pos->str);
        }
        break;
      case 'v':
        pos->str = spf->is_ip6 ? "ip6" : "in_addr";
        pos->len = m_strlen(pos->str);
        break;
      case 'h':
        pos->str = array_start(spf->helo);
        pos->len = array_len(spf->helo);
        break;
      default:
        return false;
    }
    const char* c = pos->str;
    const char* end = pos->str + pos->len;
    const char* delim_end = delimiters + delimiters_count;
    pos->len = 0;
    while (c < end) {
        const char* delim = delimiters;
        bool is_delim = false;
        while (delim < delim_end) {
            if (*c == *delim) {
                ++pos;
                is_delim = true;
                pos->str = c + 1;
                pos->len = 0;
                break;
            }
            ++delim;
        }
        if (!is_delim) {
            ++(pos->len);
        }
        ++c;
    }
    if (parts > pos - sections + 1) {
        parts = pos - sections + 1;
    }
    int i = 0;
    for (i = 0 ; i < parts ; ++i) {
        if (i != 0) {
            buffer_addch(buffer, '.');
        }
        if (reverse) {
            spf_write_macro(buffer, sections[parts - 1 - i], escape);
        } else {
            spf_write_macro(buffer, pos[i - parts + 1], escape);
        }
    }
    return true;
}

static spf_expansion_t spf_expand(spf_t* spf, const char* macrostring)
{
    buffer_reset(&spf->domainspec);
    if (m_strlen(macrostring) == 0) {
        spf->use_domain = true;
        return SPFEXP_VALID;
    }
    const char* const macrostart = macrostring;
    while (*macrostring != '\0') {
        const char* next_format = m_strchrnul(macrostring, '%');
        buffer_add(&spf->domainspec, macrostring, next_format - macrostring);
        macrostring = next_format;
        if (*macrostring != '\0') {
            ++macrostring;
            switch (*macrostring) {
              case '%':
                buffer_addch(&spf->domainspec, '%');
                break;
              case '_':
                buffer_addch(&spf->domainspec, ' ');
                break;
              case '-':
                buffer_addstr(&spf->domainspec, "%20");
                break;
              case '{': {
                ++macrostring;
                next_format = strchr(macrostring, '}');
                if (next_format == NULL) {
                    debug("spf (depth=%d): unmatched %%{ in macro \"%s\"", spf->recursions, macrostring);
                    return SPFEXP_SYNTAX;
                }
                char* end;
                char entity = ascii_tolower(*macrostring);
                bool escape = isupper(*macrostring);
                int parts = 256;
                bool reverse = false;
                const char* delimiters = ".";
                int delimiters_count = 1;
                ++macrostring;
                if (isdigit(*macrostring)) {
                    parts = strtol(macrostring, &end, 10);
                    if (parts < 0) {
                        debug("spf (depth=%d): invalid number of parts (%d) in macro  \"%s\"", spf->recursions, parts, macrostring);
                        return SPFEXP_SYNTAX;
                    }
                    macrostring = end;
                }
                if (*macrostring == 'r') {
                    reverse = true;
                    ++macrostring;
                }
                if (macrostring < next_format) {
                    delimiters = macrostring;
                    delimiters_count = next_format - delimiters;
                }
                switch (spf_expand_pattern(spf, &spf->domainspec, entity, parts, reverse, escape,
                                           delimiters, delimiters_count)) {
                  case SPFEXP_SYNTAX:
                    debug("spf (depth=%d): invalid macro %c in \"%s\"", spf->recursions, entity, macrostring);
                    return SPFEXP_SYNTAX;
                  case SPFEXP_DNS:
                    return SPFEXP_DNS;
                  default: break;
                }
                macrostring = next_format;
              } break;
            }
            ++macrostring;
        }
    }
    debug("spf (depth=%d): macro \"%s\" parsed: \"%s\"", spf->recursions, macrostart, array_start(spf->domainspec));
    if (!spf_validate_domain(array_start(spf->domainspec))) {
        return SPFEXP_INVALID;
    } else {
        return SPFEXP_VALID;
    }
}

static bool spf_subquery(spf_t* spf, const char* domain, spf_result_t cb)
{
    if (spf->recursions >= SPF_MAX_RECUSION) {
        return false;
    } else {
        spf_code_t code;
        spf->subquery = spf_check(array_start(spf->ip), domain,
                                  array_start(spf->sender), array_start(spf->helo),
                                  cb, spf->spf_nolookup, spf, &code);
        if (spf->subquery == NULL) {
            return false;
        }
        spf->subquery->mech_withdns = spf->mech_withdns;
        spf->subquery->recursions = spf->recursions + 1;
        return true;
    }
}

static bool parse_ip4(uint32_t* result, const char* txt, const char* end)
{
    char str[BUFSIZ];
    if (m_strncpy(str, BUFSIZ, txt, end - txt) > BUFSIZ) {
        return false;
    }
    if (inet_pton(AF_INET, str, result) != 1) {
        return false;
    }
    *result = ntohl(*result);
    return true;
}

static bool parse_ip6(uint8_t* result, const char* txt, const char* end)
{
    char str[BUFSIZ];
    if (m_strncpy(str, BUFSIZ, txt, end - txt) > BUFSIZ) {
        return false;
    }
    if (inet_pton(AF_INET6, str, result) != 1) {
        return false;
    }
    return true;
}

static bool spf_checkip4(spf_t* spf, uint32_t ip, int cidr)
{
    uint32_t mask = 0xffffffff;
    if (spf->is_ip6) {
        return false;
    }
    if (cidr == 0) {
        mask = 0;
    } else if (cidr > 0) {
        mask <<= (32 - cidr);
    }
    return (spf->ip4 & mask) == (ip & mask);
}

static bool spf_checkip6(spf_t* spf, const uint8_t* ip, int cidr)
{
    if (!spf->is_ip6) {
        return false;
    }
    if (cidr < 0) {
        cidr = 128;
    }
    int bytes = cidr >> 3;
    int bits  = cidr & 7;
    if (bytes > 0) {
        if (memcmp(spf->ip6, ip, bytes) != 0) {
            return false;
        }
    }
    if (bits > 0) {
        if ((spf->ip6[bytes] >> (8 - bits)) != (ip[bytes] >> (8 - bits))) {
            return false;
        }
    }
    return true;
}

static void spf_match(spf_t* spf)
{
    spf_exit(spf, array_elt(spf->rules, spf->current_rule).qualifier);
}

static bool spf_limit_dns_mechanism(spf_t* spf, bool increment)
{
    if (spf->mech_withdns >= 10) {
        spf_exit(spf, SPF_PERMERROR);
        return true;
    }
    if (increment) {
        ++spf->mech_withdns;
    }
    return false;
}

static inline const char* spf_domainspec(const spf_t* spf)
{
    return spf->use_domain ? array_start(spf->domain) : array_start(spf->domainspec);
}

static void spf_next(spf_t* spf, bool start)
{
    while (true) {
        if (!start && !spf->in_macro) {
            ++spf->current_rule;
        }
        bool from_macro = spf->in_macro;
        start = false;
        spf->a_dnserror = false;
        spf->use_domain = false;
        spf->in_macro   = false;
        if (spf->current_rule >= array_len(spf->rules)) {
            if (spf->redirect >= 0) {
                if (spf_limit_dns_mechanism(spf, !from_macro)) {
                    return;
                }
                debug("spf (debug=%d): reached the end of spf record, running redirect", spf->recursions);
                spf_rule_t* rule = array_ptr(spf->rules, spf->redirect);
                switch (spf_expand(spf, array_start(rule->content))) {
                  case SPFEXP_SYNTAX:
                    spf_exit(spf, SPF_PERMERROR);
                    return;
                  default:
                    break;
                }
                if (!spf_subquery(spf, array_start(spf->domainspec), spf_redirect_exit)) {
                    warn("spf: maximum recursion depth exceeded, error");
                    spf_exit(spf, SPF_PERMERROR);
                }
                return;
            } else {
                debug("spf (depth=%d): reached the end of spf record", spf->recursions);
                spf_exit(spf, SPF_NEUTRAL);
                return;
            }
        }
        spf_rule_t* rule = array_ptr(spf->rules, spf->current_rule);
        info("spf (depth=%d): processing rule %s: %s", spf->recursions,
              rule->rule == SPF_RULE_UNKNOWN ? "unknown" : spftokens[rule->rule],
              array_len(rule->content) == 0 ? "(empty)" : array_start(rule->content));
        switch (rule->rule) {
          case SPF_RULE_ALL:
            spf_match(spf);
            return;

          case SPF_RULE_INCLUDE: {
            if (spf_limit_dns_mechanism(spf, !from_macro)) {
                return;
            }
            switch (spf_expand(spf, array_start(rule->content))) {
              case SPFEXP_SYNTAX:
                spf_exit(spf, SPF_PERMERROR);
                return;
              case SPFEXP_DNS:
                return;
              case SPFEXP_VALID:
                if (!spf_subquery(spf, spf_domainspec(spf), spf_include_exit)) {
                    warn("spf: maximum recursion depth exceeded, error");
                    spf_exit(spf, SPF_PERMERROR);
                }
                return;
              default: break;
            }
          } break;

          case SPF_RULE_REDIRECT:
            spf->redirect = spf->current_rule;
            break;

          case SPF_RULE_MX:
          case SPF_RULE_A: {
            if (spf_limit_dns_mechanism(spf, !from_macro)) {
                return;
            }
            switch (spf_expand(spf, array_start(rule->content))) {
              case SPFEXP_SYNTAX:
                spf_exit(spf, SPF_PERMERROR);
                return;
              case SPFEXP_DNS:
                return;
              case SPFEXP_VALID:
                if (rule->rule == SPF_RULE_MX) {
                    if (!spf_query(spf, spf_domainspec(spf), DNS_RRT_MX, spf_mx_receive)) {
                        spf_exit(spf, SPF_TEMPERROR);
                    }
                } else {
                    if (!spf_query(spf, spf_domainspec(spf), spf->is_ip6 ? DNS_RRT_AAAA : DNS_RRT_A,
                                                             spf_a_receive)) {
                        spf_exit(spf, SPF_TEMPERROR);
                    }
                }
                return;
              default: break;
            }
          } break;

          case SPF_RULE_IP4: {
            if (spf_checkip4(spf, rule->ip.v4, rule->cidr4)) {
                spf_match(spf);
                return;
            }
          } break;

          case SPF_RULE_IP6: {
            if (spf_checkip6(spf, rule->ip.v6, rule->cidr6)) {
                spf_match(spf);
                return;
            }
          } break;

          case SPF_RULE_EXISTS: {
            if (spf_limit_dns_mechanism(spf, !from_macro)) {
                return;
            }
            switch (spf_expand(spf, array_start(rule->content))) {
              case SPFEXP_SYNTAX:
                spf_exit(spf, SPF_PERMERROR);
                return;
              case SPFEXP_DNS:
                return;
              case SPFEXP_VALID:
                spf_query(spf, array_start(spf->domainspec), DNS_RRT_A, spf_exists_receive);
                return;
              default:
                break;
            }
          } break;

          case SPF_RULE_PTR: {
            if (spf_limit_dns_mechanism(spf, !from_macro)) {
                return;
            }
            if (spf_run_ptr_resolution(spf, array_start(rule->content), false)) {
                return;
            }
          } break;

          case SPF_RULE_UNKNOWN:
            break;

          default:
            break;
        }
    }
}

/* A Mechanism
 */
static void spf_a_receive(void* arg, int err, struct ub_result* result)
{
    spf_t* spf = arg;
    int i;
    --spf->a_resolutions;
    if (spf_release(spf, true)) {
        debug("spf (depth=%d): A received but processing already finished", spf->recursions);
        ub_resolve_free(result);
        return;
    }
    if (spf_dns_in_error(err, result)) {
        debug("spf (depth=%d): DNS error on A query for %s", spf->recursions, result->qname);
        spf->a_dnserror = true;
        if (spf->a_resolutions == 0) {
            spf_exit(spf, SPF_TEMPERROR);
        }
        ub_resolve_free(result);
        return;
    }
    debug("spf (depth=%d): A answer received for %s", spf->recursions, result->qname);
    for (i = 0 ; result->data[i] != NULL ; ++i) {
        bool match = false;
        if (result->qtype == DNS_RRT_AAAA) {
            match = spf_checkip6(spf, (uint8_t*)result->data[i], current_rule(spf).cidr6);
        } else {
            uint32_t ip = (((uint8_t)result->data[i][0]) << 24)
                        | (((uint8_t)result->data[i][1]) << 16)
                        | (((uint8_t)result->data[i][2]) << 8)
                        | (((uint8_t)result->data[i][3]));
            match = spf_checkip4(spf, ip, current_rule(spf).cidr4);
        }
        if (match) {
            if (spf->in_macro || current_rule(spf).rule == SPF_RULE_PTR) {
                info("spf (depth=%d): PTR validated by domain %s", spf->recursions, result->qname);
                if (spf->use_domain) {
                    buffer_reset(&spf->validated);
                    buffer_addstr(&spf->validated, result->qname);
                    if (array_last(spf->validated) == '.') {
                        array_last(spf->validated) = '\0';
                        --array_len(spf->validated);
                    }
                }
            }
            spf_match(spf);
            ub_resolve_free(result);
            return;
        }
    }
    if (spf->a_resolutions == 0) {
        if (spf->a_dnserror) {
            spf_exit(spf, SPF_TEMPERROR);
        } else {
            spf_next(spf, false);
        }
    }
    ub_resolve_free(result);
}

/* MX Mechanism
 */
static void spf_mx_receive(void* arg, int err, struct ub_result* result)
{
    spf_t* spf = arg;
    int i;
    if (spf_release(spf, true)) {
        debug("spf (depth=%d): MX received but processing already finished", spf->recursions);
        ub_resolve_free(result);
        return;
    }
    if (spf_dns_in_error(err, result)) {
        debug("spf (depth=%d): DNS error on query for MX entry for %s", spf->recursions, result->qname);
        spf_exit(spf, SPF_TEMPERROR);
        ub_resolve_free(result);
        return;
    }
    debug("spf (depth=%d): MX entry received for %s", spf->recursions, result->qname);
    for (i = 0 ; result->data[i] != NULL ; ++i) {
        const char* pos = result->data[i] + 2;
        buffer_reset(&dns_buffer);
        if (i >= 10) {
            info("spf (depth=%d): too many MX entries for %s", spf->recursions, result->qname);
            break;
        }
        while (*pos != '\0') {
            uint8_t count = *pos;
            ++pos;
            buffer_add(&dns_buffer, pos, count);
            buffer_addch(&dns_buffer, '.');
            pos += count;
        }
        spf_query(spf, array_start(dns_buffer), spf->is_ip6 ? DNS_RRT_AAAA : DNS_RRT_A,
                                                spf_a_receive);
    }
    if (spf->a_resolutions == 0) {
        info("spf (depth=%d): no MX entry for %s", spf->recursions, result->qname);
        spf_next(spf, false);
    }
    ub_resolve_free(result);
}

/* EXISTS Mechanism
 */
static void spf_exists_receive(void* arg, int err, struct ub_result* result)
{
    spf_t* spf = arg;
    if (spf_release(spf, true)) {
        debug("spf (depth=%d): A received but processing already finished", spf->recursions);
        ub_resolve_free(result);
        return;
    }
    if (spf_dns_in_error(err, result)) {
        debug("spf (depth=%d): DNS error on A query for existence for %s", spf->recursions, result->qname);
        spf_exit(spf, SPF_TEMPERROR);
        ub_resolve_free(result);
        return;
    }
    debug("spf (depth=%d): existence query received for %s", spf->recursions, result->qname);
    if (result->rcode == DNS_RCODE_NOERROR) {
        spf_match(spf);
    } else {
        spf_next(spf, false);
    }
    ub_resolve_free(result);
}

/* PTR Mechanism
 */
static void spf_ptr_receive(void* arg, int err, struct ub_result* result)
{
    spf_t* spf = arg;
    if (spf_release(spf, true)) {
        debug("spf (depth=%d): PTR received but processing already finished", spf->recursions);
        ub_resolve_free(result);
        return;
    }
    if (spf_dns_in_error(err, result)) {
        debug("spf (depth=%d): DNS error for PTR query on %s", spf->recursions, result->qname);
        spf_exit(spf, SPF_TEMPERROR);
        ub_resolve_free(result);
        return;
    }
    for (int i = 0 ; result->data[i] != NULL ; ++i) {
        const char* pos = result->data[i];
        buffer_reset(&dns_buffer);
        if (spf->a_resolutions >= 10) {
            info("spf (depth=%d): too many PTR entries for %s", spf->recursions, result->qname);
            break;
        }
        while (*pos != '\0') {
            uint8_t count = *pos;
            ++pos;
            buffer_add(&dns_buffer, pos, count);
            buffer_addch(&dns_buffer, '.');
            pos += count;
        }

        debug("spf (depth=%d): found %s, to be compared to %s", spf->recursions,
              array_start(dns_buffer), array_start(spf->domainspec));
        ssize_t diff = array_len(dns_buffer) - array_len(spf->domainspec);
        bool match = false;
        if (diff == 0) {
            if (strcasecmp(array_start(spf->domainspec), array_start(dns_buffer)) == 0) {
                debug("spf (depth=%d): PTR potential entry found for domain %s", spf->recursions, array_start(dns_buffer));
                match = true;
            }
        } else if (diff > 0 && array_elt(dns_buffer, diff - 1) == '.') {
            if (strcasecmp(array_start(spf->domainspec), array_ptr(dns_buffer, diff)) == 0) {
                debug("spf (depth=%d): PTR potential entry found for subdomain %s", spf->recursions, array_start(dns_buffer));
                match = true;
            }
        }
        if (match) {
            spf_query(spf, array_start(dns_buffer), spf->is_ip6 ? DNS_RRT_AAAA : DNS_RRT_A, spf_a_receive);
        }
    }
    if (spf->a_resolutions == 0) {
        spf_next(spf, false);
    }
    ub_resolve_free(result);
}

static bool spf_run_ptr_resolution(spf_t* spf, const char* domainspec, bool in_macro)
{
    spf->in_macro = in_macro;
    switch (spf_expand(spf, domainspec)) {
      case SPFEXP_SYNTAX:
        spf_exit(spf, SPF_PERMERROR);
        return true;
      case SPFEXP_DNS:
        return true;
      case SPFEXP_VALID:
        if (spf->use_domain) {
            if (array_len(spf->validated) > 0) {
                if (strcmp(array_start(spf->validated), "unknown") == 0) {
                    return false;
                }
                spf_match(spf);
                return true;
            }
            buffer_add(&spf->domainspec, array_start(spf->domain), array_len(spf->domain));
        }
        if (array_last(spf->domainspec) != '.') {
            buffer_addch(&spf->domainspec, '.');
        }
        buffer_reset(&dns_buffer);
        if (!spf->is_ip6) {
            buffer_addf(&dns_buffer, "%d.%d.%d.%d.in-addr.arpa.",
                        spf->ip4 & 0xff, (spf->ip4 >> 8) & 0xff,
                        (spf->ip4 >> 16) & 0xff, (spf->ip4 >> 24) & 0xff);
        } else {
            buffer_addf(&dns_buffer, "%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x."
                        "%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.ip6.arpa.",
                        spf->ip6[15] & 0x0f, (spf->ip6[15] >> 4) & 0x0f,
                        spf->ip6[14] & 0x0f, (spf->ip6[14] >> 4) & 0x0f,
                        spf->ip6[13] & 0x0f, (spf->ip6[13] >> 4) & 0x0f,
                        spf->ip6[12] & 0x0f, (spf->ip6[12] >> 4) & 0x0f,
                        spf->ip6[11] & 0x0f, (spf->ip6[11] >> 4) & 0x0f,
                        spf->ip6[10] & 0x0f, (spf->ip6[10] >> 4) & 0x0f,
                        spf->ip6[9] & 0x0f, (spf->ip6[9] >> 4) & 0x0f,
                        spf->ip6[8] & 0x0f, (spf->ip6[8] >> 4) & 0x0f,
                        spf->ip6[7] & 0x0f, (spf->ip6[7] >> 4) & 0x0f,
                        spf->ip6[6] & 0x0f, (spf->ip6[6] >> 4) & 0x0f,
                        spf->ip6[5] & 0x0f, (spf->ip6[5] >> 4) & 0x0f,
                        spf->ip6[4] & 0x0f, (spf->ip6[4] >> 4) & 0x0f,
                        spf->ip6[3] & 0x0f, (spf->ip6[3] >> 4) & 0x0f,
                        spf->ip6[2] & 0x0f, (spf->ip6[2] >> 4) & 0x0f,
                        spf->ip6[1] & 0x0f, (spf->ip6[1] >> 4) & 0x0f,
                        spf->ip6[0] & 0x0f, (spf->ip6[0] >> 4) & 0x0f);
        }
        if (spf->use_domain) {
            buffer_addstr(&spf->validated, "unknown");
        }
        spf_query(spf, array_start(dns_buffer), DNS_RRT_PTR, spf_ptr_receive);
        return true;
      default:
        return false;;
    }
    return false;
}


/* INCLUDE Mechanism
 */
static void spf_include_exit(spf_code_t result, const char* exp, void* arg)
{
    spf_t* spf = arg;
    spf->mech_withdns = spf->subquery->mech_withdns;
    spf->subquery = NULL;
    switch (result) {
      case SPF_PASS:
        debug("spf (depth=%d): include matched", spf->recursions);
        spf_match(spf);
        return;

      case SPF_FAIL:
      case SPF_SOFTFAIL:
      case SPF_NEUTRAL:
        spf_next(spf, false);
        return;

      case SPF_TEMPERROR:
        spf_exit(spf, SPF_TEMPERROR);
        return;

      case SPF_PERMERROR:
      case SPF_NONE:
      default:
        spf_exit(spf, SPF_PERMERROR);
        return;
    }
}

/* REDIRECT Modifier
 */
static void spf_redirect_exit(spf_code_t result, const char* exp, void* arg)
{
    spf_t* spf = arg;
    spf->subquery = NULL;
    if (result == SPF_NONE) {
        spf_exit(spf, SPF_PERMERROR);
    } else {
        debug("spf (depth=%d): redirect matched", spf->recursions);
        spf_exit(spf, result);
    }
}


/*  Parsing   */

static spf_code_t spf_qualifier(const char** str)
{
    switch (**str) {
      case '+':
        ++(*str);
        return SPF_PASS;
      case '-':
        ++(*str);
        return SPF_FAIL;
      case '~':
        ++(*str);
        return SPF_SOFTFAIL;
      case '?':
        ++(*str);
        return SPF_NEUTRAL;
      default:
        return SPF_PASS;
    }
}


static bool spf_check_domainspec(const char* pos, const char* end,
                                 bool allow_empty, bool macrostring_only)
{
#define READ_NEXT                                                              \
    ++pos;                                                                     \
    if (pos == end) {                                                          \
        return can_be_end;                                                     \
    }

    bool can_be_end = allow_empty;
    if (pos >= end) {
        return can_be_end;
    }
    if (*pos == ':' || *pos == '=') {
        can_be_end = false;
        READ_NEXT;
    }
    while (pos < end) {
        /* final dot */
        if (!macrostring_only && *pos == '.') {
            bool has_dash = false;
            bool has_alpha = false;
            can_be_end = false;
            READ_NEXT;
            if (!isalnum(*pos)) {
                continue;
            }
            while (pos < end) {
                if (*pos == '-') {
                   can_be_end = false;
                   if (!has_alpha) {
                       --pos;
                       break;
                    }
                    has_dash = true;
                } else if (isalpha(*pos)) {
                    has_alpha = true;
                    can_be_end = true;
                } else if (isdigit(*pos)) {
                    can_be_end = (has_dash || has_alpha);
                } else if (*pos == '.') {
                    has_dash = false;
                    has_alpha = false;
                    if (!can_be_end) {
                        return false;
                    }
                    can_be_end = true;
                } else {
                    --pos;
                    break;
                }
                READ_NEXT;
            }

        /* macro expand */
        } else if (*pos == '%') {
            can_be_end = false;
            READ_NEXT;
            if (*pos == '%' || *pos == '_' || *pos == '-') {
                can_be_end = true;
            } else if (*pos == '{') {
                READ_NEXT;
                switch (ascii_tolower(*pos)) {
                  case 's': case 'l': case 'o': case 'd':
                  case 'i': case 'p': case 'h':
                    break;
                  default:
                    return false;
                }
                READ_NEXT;
                while (isdigit(*pos)) {
                    READ_NEXT;
                }
                if (*pos == 'r') {
                    READ_NEXT;
                }
                while (*pos == '.' || *pos == '-' || *pos == '+' || *pos == ','
                       || *pos == '/' || *pos == '_' || *pos == '=') {
                    READ_NEXT;
                }
                if (*pos != '}') {
                    return false;
                }
                can_be_end = true;
            } else {
                return false;
            }
        /* Other caracters */
        } else if (*pos < 0x21 || *pos > 0x7e) {
            return false;
        } else {
            can_be_end = macrostring_only;
        }
        READ_NEXT;
    }
    return can_be_end;

#undef READ_NEXT
}

static const char* spf_parse_cidr_simple(const char* start, const char* end, uint8_t* cidr)
{
    *cidr = 0;
    if (end <= start || !isdigit(end[-1])) {
        return NULL;
    }
    while (end > start && isdigit(end[-1])) {
        --end;
    }
    if (end == start || end[-1] != '/') {
        return NULL;
    }
    long l = strtol(end, NULL, 10);
    if (l < 0 || l > 128 || (l != 0 && *end == '0')) {
        *cidr = 0xff;
        return NULL;
    }
    *cidr = (uint8_t)l;
    return end - 1;
}

static const char* spf_parse_cidr(const char* start, const char* end, uint8_t* cidr4, uint8_t* cidr6)
{
    assert(cidr4 != NULL || cidr6 != NULL);
    if (cidr4 != NULL) {
        *cidr4 = 32;
    }
    if (cidr6 != NULL) {
        *cidr6 = 128;
    }
    const char* last_pos = end;
    uint8_t cidr1 = 0;
    end = spf_parse_cidr_simple(start, end, &cidr1);
    if (end == NULL) {
        if (cidr1 == 0xff) {
            return NULL;
        } else {
            return last_pos;
        }
    }
    last_pos = end;
    if (end[-1] != '/') {
        if (cidr4 != NULL) {
            if (cidr1 > 32) {
                return NULL;
            }
            *cidr4 = cidr1;
        } else {
            *cidr6 = cidr1;
        }
        return last_pos;
    }

    if (cidr4 == NULL || cidr6 == NULL) {
        return NULL;
    }

    --end;
    uint8_t cidr2 = 0;
    end = spf_parse_cidr_simple(start, end, &cidr2);
    if (end == NULL) {
        if (cidr2 == 0xff) {
            return NULL;
        } else {
            *cidr6 = cidr1;
            return last_pos - 1;
        }
    }
    if (cidr2 > 32) {
        return NULL;
    }
    *cidr4 = cidr2;
    *cidr6 = cidr1;
    return end;
}

static bool spf_parse(spf_t* spf)
{
    const char* pos = array_start(spf->record);
    const char* end = pos + array_len(spf->record);
    bool has_exp = false;
    bool has_redirect = false;
    pos += 6;
    do {
        while (*pos == ' ') {
            ++pos;
        }
        if (pos >= end) {
            return true;
        }
        const char* rule_start = pos;
        const char* name_end = NULL;
        if (*pos == '+' || *pos == '-' || *pos == '~' || *pos == '?') {
            ++pos;
        }
        if (!isalpha(*pos)) {
            return false;
        }
        while (*pos != ' ' && pos < end) {
            if (name_end == NULL && (*pos == ':' || *pos == '=' || *pos == '/')) {
                name_end = pos;
            }
            if (name_end == NULL && (!isalnum(*pos) && *pos != '.' && *pos != '_' && *pos != '-')) {
                return false;
            }
            ++pos;
        }
        if (name_end == NULL) {
            name_end = pos;
        }
        bool is_mechanism = (name_end == end || *name_end == ' ' || *name_end == ':' || *name_end == '/');
        spf_code_t qual = SPF_NEUTRAL;
        if (is_mechanism) {
            qual = spf_qualifier(&rule_start);
        }
        if (name_end - rule_start == 0) {
            return false;
        }
        spf_ruleid_t id = spf_rule_tokenize(rule_start, name_end - rule_start);
        ip_t ip = { .v6 = { 0 } };
        const char* cidr_end = pos;
        uint8_t cidr4 = 32;
        uint8_t cidr6 = 128;
        if (is_mechanism) {
            switch (id) {
              case SPF_RULE_ALL:
                if (name_end != pos) {
                    return false;
                }
                break;

              case SPF_RULE_INCLUDE:
              case SPF_RULE_EXISTS:
                if (*name_end != ':') {
                    return false;
                }
                if (!spf_check_domainspec(name_end, pos, false, false)) {
                    return false;
                }
                break;

              case SPF_RULE_A:
              case SPF_RULE_MX:
                cidr_end = spf_parse_cidr(name_end, pos, &cidr4, &cidr6);
                if (cidr_end == NULL || !spf_check_domainspec(name_end, cidr_end, true, false)) {
                    return false;
                }
                break;

              case SPF_RULE_PTR:
                if (!spf_check_domainspec(name_end, pos, true, false)) {
                    return false;
                }
                break;

              case SPF_RULE_IP4:
                if (*name_end != ':') {
                    return false;
                }
                cidr_end = spf_parse_cidr(name_end, pos, &cidr4, NULL);
                if (cidr_end == NULL || !parse_ip4(&ip.v4, name_end + 1, cidr_end)) {
                    return false;
                }
                break;

              case SPF_RULE_IP6:
                if (*name_end != ':') {
                    return false;
                }
                cidr_end = spf_parse_cidr(name_end, pos, NULL, &cidr6);
                if (cidr_end == NULL || !parse_ip6(ip.v6, name_end + 1, cidr_end)) {
                    return false;
                }
                break;

              default:
                return false;
            }
        } else {
            if (*name_end != '=') {
                return false;
            }
            switch (id) {
              case SPF_RULE_REDIRECT:
                if (has_redirect) {
                    return false;
                }
                has_redirect = true;
                if (!spf_check_domainspec(name_end, pos, false, false)) {
                    return false;
                }
                break;

              case SPF_RULE_EXP:
                if (has_exp) {
                    return false;
                }
                has_exp = true;
                if (!spf_check_domainspec(name_end, pos, false, false)) {
                    return false;
                }
                break;

              case SPF_RULE_UNKNOWN:
                if (!spf_check_domainspec(name_end, pos, false, true)) {
                    return false;
                }
                break;

              default:
                return false;
            }
        }

        spf_rule_t rule = SPF_RULE_INIT;
        if (array_len(spf_rule_pool) > 0) {
            rule = array_pop_last(spf_rule_pool);
        }
        rule.qualifier = qual;
        rule.rule = id;
        rule.ip = ip;
        rule.cidr4 = cidr4;
        rule.cidr6 = cidr6;
        buffer_reset(&rule.content);
        if (name_end != pos) {
            if (cidr_end > name_end && (*name_end == ':' || *name_end == '=')) {
                ++name_end;
            }
            buffer_add(&rule.content, name_end, cidr_end - name_end);
        }
        array_add(spf->rules, rule);
    } while (true);
    return true;
}

static void spf_line_callback(void *arg, int err, struct ub_result* result)
{
    spf_t* spf = arg;
    if (spf_release(spf, true)) {
        debug("spf (depth=%d): %s for %s received but processing already finished", spf->recursions,
              result->qtype == DNS_RRT_TXT ? "TXT" : "SPF", result->qname);
        ub_resolve_free(result);
        return;
    }
    if (array_len(spf->record) != 0 && spf->spf_received) {
        debug("spf (depth=%d): record already found for %s", spf->recursions, result->qname);
        ub_resolve_free(result);
        return;
    }
    if (result->qtype == DNS_RRT_SPF) {
        spf->spf_received = true;
        spf->spf_inerror  = spf_dns_in_error(err, result);
    }
    if (result->qtype == DNS_RRT_TXT) {
        spf->txt_received = true;
        spf->txt_inerror  = spf_dns_in_error(err, result);
    }
    debug("spf (depth=%d): %s for %s received", spf->recursions,
          result->qtype == DNS_RRT_TXT ? "TXT" : "SPF", result->qname);
    bool is_mine = false;
    for (int i = 0 ; result->data[i] != NULL ; ++i) {
        /* Parse field: (RFC 1035)
         * TXT-DATA: One or more <character-string>
         * <character-string> is a single
         * length octet followed by that number of characters.  <character-string>
         * is treated as binary information, and can be up to 256 characters in
         * length (including the length octet).
         */
        const char* pos = result->data[i];
        const char* const end = pos + result->len[i];
        buffer_reset(&spf->domainspec);
        while (pos < end) {
            const int len = *pos;
            buffer_add(&spf->domainspec, pos + 1, len);
            pos += len + 1;
        }

        /* Looking for spf fields. (RFC 4408)
         *  record           = version terms *SP
         *  version          = "v=spf1"
         *
         *  1. Records that do not begin with a version section of exactly
         *     "v=spf1" are discarded.  Note that the version section is
         *     terminated either by an SP character or the end of the record.  A
         *     record with a version section of "v=spf10" does not match and
         *     must  be discarded.
         */
        const char* str = array_start(spf->domainspec);
        const int len   = array_len(spf->domainspec);
        if (len < 6) {
            debug("spf (depth=%d): entry too short to be a spf record", spf->recursions);
        } else {
            if (strncasecmp(str, "v=spf1", 6) != 0) {
                debug("spf (depth=%d): not a record: \"%.*s\"", spf->recursions, len, str);
            } else if (len == 6 || str[6] == ' ') {
                debug("spf (depth=%d): record found: \"%.*s\"", spf->recursions, len, str);
                /* After the above steps, there should be exactly one record remaining
                 * and evaluation can proceed.  If there are two or more records
                 * remaining, then check_host() exits immediately with the result of
                 * "PermError".
                 */
                if (array_len(spf->record) != 0) {
                    if (is_mine || result->qtype != DNS_RRT_SPF) {
                        if (spf->spf_received) {
                            info("spf (depth=%d): too many records", spf->recursions);
                            spf_exit(spf, SPF_PERMERROR);
                            ub_resolve_free(result);
                            return;
                        } else {
                            spf->txt_toomany = true;
                            buffer_reset(&spf->record);
                            ub_resolve_free(result);
                            return;
                        }
                    } else {
                        /* 2. If any record of type SPF are in the set, then all records
                         *    of type TXT are discarded
                         */
                        buffer_reset(&spf->record);
                    }
                }
                buffer_add(&spf->record, str, len);
                is_mine = true;
            } else {
                debug("spf (depth=%d): invalid record, version is ok, but not finished by a space: \"%.*s\"", spf->recursions, len, str);
            }
        }
    }
    if (!spf->spf_received) {
        ub_resolve_free(result);
        return;
    }
    if (spf->txt_inerror && spf->spf_inerror) {
        spf_exit(spf, SPF_TEMPERROR);
    } else if (spf->spf_received && spf->txt_received && array_len(spf->record) == 0) {
        /* No record found
         *
         * If no matching records are returned, an SPF client MUST assume that
         * the domain makes no SPF declarations.  SPF processing MUST stop and
         * return "None".
         */
        if (spf->txt_toomany) {
            info("spf (depth=%d): too many records", spf->recursions);
        } else {
            info("spf (depth=%d): no record found", spf->recursions);
        }
        spf_exit(spf, spf->txt_toomany ? SPF_PERMERROR : SPF_NONE);
    } else if (array_len(spf->record) != 0) {
        /* Parse record and start processing (RFC 4408)
         *
         * After one SPF record has been selected, the check_host() function
         * parses and interprets it to find a result for the current test.  If
         * there are any syntax errors, check_host() returns immediately with
         * the result "PermError".
         */
        if (!spf_parse(spf)) {
            info("spf (depth=%d): cannot parse spf entry: \"%s\"", spf->recursions, array_start(spf->record));
            spf_exit(spf, SPF_PERMERROR);
        } else {
            info("spf (depth=%d): record selected: \"%s\"", spf->recursions, array_start(spf->record));
            spf_next(spf, true);
        }
    }
    ub_resolve_free(result);
}


spf_t* spf_check(const char *ip, const char *domain, const char *sender, const char* helo,
                 spf_result_t resultcb, bool no_spf_lookup, void *data, spf_code_t* code)
{
    info("spf: new SPF lookup of (%s, %s, %s)", ip, domain, sender);
    spf_t* spf = spf_acquire();
    spf->redirect = -1;

    buffer_addstr(&spf->ip, ip);
    if (!parse_ip4(&spf->ip4, array_start(spf->ip), array_end(spf->ip))) {
        if (!parse_ip6(spf->ip6, array_start(spf->ip), array_end(spf->ip))) {
            *code = SPF_NONE;
            err("spf: invalid ip: %s", ip);
            spf_cancel(spf);
            return NULL;
        }
        spf->is_ip6 = true;

        /* Find IP4 mapped on IP6 */
        uint8_t mapped4to6[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(mapped4to6, spf->ip6, 12) == 0) {
            memcpy(&spf->ip4, spf->ip6 + 12, 4);
            spf->ip4 = ntohl(spf->ip4);
            spf->is_ip6 = false;
        }
    } else if (!parse_ip6(spf->ip6, array_start(spf->ip), array_end(spf->ip))) {
        spf->is_ip6 = false;
    }
    spf->spf_received = spf->spf_nolookup = spf->spf_inerror = no_spf_lookup;
    buffer_addstr(&spf->domain, domain);
    buffer_addstr(&spf->sender, sender);
    buffer_addstr(&spf->helo, helo);
    const char* sender_domain = strchr(array_start(spf->sender), '@');
    if (sender_domain == array_start(spf->sender)) {
        buffer_reset(&spf->sender);
        buffer_addstr(&spf->sender, "postmaster");
        buffer_addstr(&spf->sender, sender);
        sender_domain = strchr(array_start(spf->sender), '@');
    }
    if (sender_domain == NULL
        || !spf_validate_domain(array_start(spf->domain))
        || !spf_validate_domain(sender_domain + 1)) {
        *code = SPF_NONE;
        debug("spf: malformed query");
        spf_cancel(spf);
        return NULL;
    }
    spf->exit = resultcb;
    spf->data = data;
    if (!spf->spf_nolookup) {
        spf_query(spf, domain, DNS_RRT_SPF, spf_line_callback);
    }
    spf_query(spf, domain, DNS_RRT_TXT, spf_line_callback);
    if (spf->queries == 0) {
        *code = SPF_TEMPERROR;
        spf_cancel(spf);
        return NULL;
    } else {
        return spf;
    }
}

void spf_cancel(spf_t* spf)
{
    if (spf->subquery != NULL) {
        spf_cancel(spf->subquery);
        spf->subquery = NULL;
    }
    spf->canceled = true;
    spf_release(spf, false);
}

/* vim:set et sw=4 sts=4 sws=4: */
