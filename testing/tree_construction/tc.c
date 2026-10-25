/* The html5lib tree-construction runner: parses each case of the given .dat files with Lexbor and prints one
 * line per case and scripting mode,
 *
 *     <file>#<ordinal>@<fnv1a32 of the #data bytes>/<script-off|script-on> PASS|FAIL
 *
 * followed, for a FAIL, by the tree as html5lib's "#document" format spells it. A case marked #script-on or
 * #script-off runs in that mode only; an unmarked case runs in both. A #document-fragment case parses with its
 * context element (an "svg "/"math " prefix names the namespace). engine/treegate.mjs compiles this against the
 * native Lexbor archive and compares the verdicts with expected.txt.
 *
 *     tc <file.dat>...    exit 0 once every file was read; 2 when one cannot be */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexbor/dom/dom.h"
#include "lexbor/html/html.h"
#include "lexbor/html/interfaces/template_element.h"

typedef struct {
    char   *p;
    size_t n, cap;
} buf_t;

static void buf_put(buf_t *b, const void *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (b->p == NULL) { fputs("tc: out of memory\n", stderr); exit(2); }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(buf_t *b, const char *s) { buf_put(b, s, strlen(s)); }

static void indent(buf_t *b, int depth) {
    buf_str(b, "| ");
    for (int i = 0; i < depth; i++) buf_str(b, "  ");
}

typedef struct {
    buf_t name, value;
} attr_t;

static int attr_cmp(const void *a, const void *b) {
    return strcmp(((const attr_t *) a)->name.p, ((const attr_t *) b)->name.p);
}

static void dump(buf_t *b, lxb_dom_node_t *node, int depth);

static void dump_children(buf_t *b, lxb_dom_node_t *node, int depth) {
    for (lxb_dom_node_t *c = node->first_child; c != NULL; c = c->next) dump(b, c, depth);
}

/* An element as html5lib spells it: a foreign element's adjusted name (Lexbor keeps it as the qualified name)
   behind its namespace's prefix, then its attributes sorted by name, a foreign attribute behind its namespace's
   prefix, then a template's contents under "content". */
static void dump_element(buf_t *b, lxb_dom_node_t *node, int depth) {
    size_t len, na = 0;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    const lxb_char_t *name = node->ns == LXB_NS_HTML ? lxb_dom_element_local_name(el, &len)
                                                     : lxb_dom_element_qualified_name(el, &len);
    attr_t *attrs = NULL;

    indent(b, depth);
    buf_str(b, "<");
    buf_str(b, node->ns == LXB_NS_SVG ? "svg " : node->ns == LXB_NS_MATH ? "math " : "");
    buf_put(b, name, len);
    buf_str(b, ">\n");

    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a != NULL; a = lxb_dom_element_next_attribute(a)) {
        const char *prefix = a->node.ns == LXB_NS_XLINK ? "xlink " : a->node.ns == LXB_NS_XML ? "xml "
                           : a->node.ns == LXB_NS_XMLNS ? "xmlns " : NULL;
        size_t nl, vl;
        const lxb_char_t *an = prefix ? lxb_dom_attr_local_name(a, &nl) : lxb_dom_attr_qualified_name(a, &nl);
        const lxb_char_t *av = lxb_dom_attr_value(a, &vl);

        attrs = realloc(attrs, (na + 1) * sizeof *attrs);
        memset(&attrs[na], 0, sizeof attrs[na]);
        buf_str(&attrs[na].name, prefix ? prefix : "");
        buf_put(&attrs[na].name, an, nl);
        buf_put(&attrs[na].value, av ? av : (const lxb_char_t *) "", av ? vl : 0);
        na++;
    }
    qsort(attrs, na, sizeof *attrs, attr_cmp);
    for (size_t i = 0; i < na; i++) {
        indent(b, depth + 1);
        buf_put(b, attrs[i].name.p, attrs[i].name.n);
        buf_str(b, "=\"");
        buf_put(b, attrs[i].value.p, attrs[i].value.n);
        buf_str(b, "\"\n");
        free(attrs[i].name.p);
        free(attrs[i].value.p);
    }
    free(attrs);

    if (node->ns == LXB_NS_HTML && node->local_name == LXB_TAG_TEMPLATE) {
        lxb_html_template_element_t *t = lxb_html_interface_template(node);
        indent(b, depth + 1);
        buf_str(b, "content\n");
        if (t->content != NULL) dump_children(b, &t->content->node, depth + 2);
    }
    dump_children(b, node, depth + 1);
}

static void dump(buf_t *b, lxb_dom_node_t *node, int depth) {
    switch (node->type) {
        case LXB_DOM_NODE_TYPE_ELEMENT:
            dump_element(b, node, depth);
            return;
        case LXB_DOM_NODE_TYPE_TEXT:
        case LXB_DOM_NODE_TYPE_COMMENT: {
            lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(node);
            int text = node->type == LXB_DOM_NODE_TYPE_TEXT;
            indent(b, depth);
            buf_str(b, text ? "\"" : "<!-- ");
            buf_put(b, cd->data.data, cd->data.length);
            buf_str(b, text ? "\"\n" : " -->\n");
            return;
        }
        case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
            lxb_dom_document_type_t *dt = lxb_dom_interface_document_type(node);
            size_t nl, pl, sl;
            const lxb_char_t *name = lxb_dom_document_type_name(dt, &nl);
            const lxb_char_t *pub = lxb_dom_document_type_public_id(dt, &pl);
            const lxb_char_t *sys = lxb_dom_document_type_system_id(dt, &sl);
            indent(b, depth);
            buf_str(b, "<!DOCTYPE ");
            if (name != NULL) buf_put(b, name, nl);
            if (pl != 0 || sl != 0) {
                buf_str(b, " \"");
                if (pub != NULL) buf_put(b, pub, pl);
                buf_str(b, "\" \"");
                if (sys != NULL) buf_put(b, sys, sl);
                buf_str(b, "\"");
            }
            buf_str(b, ">\n");
            return;
        }
        default:
            dump_children(b, node, depth);
            return;
    }
}

static uint32_t fnv1a32(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char) s[i]) * 16777619u;
    return h;
}

/* One case parsed in one scripting mode, dumped into `out`. A parse or fragment context Lexbor refuses dumps as
   the line "PARSE-REFUSED", which no expected tree spells. */
static void run_case(buf_t *out, const char *data, size_t dlen, const char *context, int scripting) {
    lxb_html_document_t *doc = lxb_html_document_create();
    lxb_dom_node_t *root = NULL;

    lxb_html_document_scripting_set(doc, scripting);
    if (context == NULL) {
        if (lxb_html_document_parse(doc, (const lxb_char_t *) data, dlen) == LXB_STATUS_OK)
            root = lxb_dom_interface_node(doc);
    } else {
        uintptr_t ns = LXB_NS_HTML;
        const char *name = context;
        lxb_dom_element_t *ctx;
        if (strncmp(context, "svg ", 4) == 0) { ns = LXB_NS_SVG; name = context + 4; }
        else if (strncmp(context, "math ", 5) == 0) { ns = LXB_NS_MATH; name = context + 5; }
        ctx = lxb_dom_document_create_element(lxb_dom_interface_document(doc), (const lxb_char_t *) name,
                                              strlen(name), NULL);
        if (ctx != NULL) {
            lxb_dom_interface_node(ctx)->ns = ns;
            root = lxb_html_document_parse_fragment(doc, ctx, (const lxb_char_t *) data, dlen);
        }
    }
    if (root == NULL) buf_str(out, "PARSE-REFUSED\n");
    else dump_children(out, root, 0);
    lxb_html_document_destroy(doc);
}

/* The text after `header` within [from, to), or NULL when the case has no such section. The search takes lengths,
   since a case's data may hold NUL bytes. */
static char *section(char *from, char *to, const char *header) {
    char *p = memmem(from, (size_t) (to - from), header, strlen(header));
    return p != NULL ? p + strlen(header) : NULL;
}

int main(int argc, char **argv) {
    for (int fi = 1; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "rb");
        char *all, *cur;
        long size;
        int ordinal = 0;

        if (f == NULL) { perror(argv[fi]); return 2; }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        all = malloc((size_t) size + 3);
        all[0] = '\n';
        if (fread(all + 1, 1, (size_t) size, f) != (size_t) size) { perror(argv[fi]); return 2; }
        all[size + 1] = '\n';
        all[size + 2] = 0;
        fclose(f);

        cur = section(all, all + size + 2, "\n#data\n");
        if (cur != NULL) cur -= strlen("\n#data\n");
        while (cur != NULL) {
            char *next = section(cur + 1, all + size + 2, "\n#data\n"), *end;
            if (next != NULL) next -= strlen("\n#data\n");
            end = next != NULL ? next : all + size + 2;
            char *data = cur + strlen("\n#data\n");
            char *errors = section(cur, end, "\n#errors\n");
            char *fragment = section(cur, end, "\n#document-fragment\n");
            char *expected = section(cur, end, "\n#document\n");
            int only_on = section(cur, end, "\n#script-on\n") != NULL;
            int only_off = section(cur, end, "\n#script-off\n") != NULL;
            char context[128] = "";
            size_t dlen, elen;

            ordinal++;
            if (errors == NULL || expected == NULL) {
                fprintf(stderr, "tc: %s case %d has no #errors or #document section\n", argv[fi], ordinal);
                return 2;
            }
            /* An empty #data puts the "\n#errors\n" header's newline at data[-1]. */
            dlen = errors - strlen("\n#errors\n") > data ? (size_t) (errors - strlen("\n#errors\n") - data) : 0;
            if (fragment != NULL) {
                char *eol = memchr(fragment, '\n', (size_t) (end - fragment));
                snprintf(context, sizeof context, "%.*s", (int) (eol - fragment), fragment);
            }
            elen = (size_t) (end - expected);
            while (elen > 0 && expected[elen - 1] == '\n') elen--;

            for (int scripting = 0; scripting <= 1; scripting++) {
                buf_t out = {0};
                size_t olen;
                int pass;
                if ((scripting && only_off) || (!scripting && only_on)) continue;
                run_case(&out, data, dlen, fragment != NULL ? context : NULL, scripting);
                olen = out.n;
                while (olen > 0 && out.p[olen - 1] == '\n') olen--;
                pass = olen == elen && memcmp(out.p, expected, elen) == 0;
                printf("%s#%d@%08x/%s %s\n", strrchr(argv[fi], '/') ? strrchr(argv[fi], '/') + 1 : argv[fi],
                       ordinal, fnv1a32(data, dlen), scripting ? "script-on" : "script-off",
                       pass ? "PASS" : "FAIL");
                if (!pass) printf("%.*s\n", (int) olen, out.p);
                free(out.p);
            }
            cur = next;
        }
        free(all);
    }
    return 0;
}
