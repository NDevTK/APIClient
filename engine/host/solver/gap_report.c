/* gap_report.c — see gap_report.h. */
#include "solver/gap_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"

/* A composed site's text for the document: what its first reach wrote, and where that was cut to the site's
   room, the emitter's own truncation marker (check.h's apiclient_assert_compose) with both lengths. */
static void gap_composed_text(JsonBuf *b, const struct apiclient_gap_site *s)
{
    char cut[APICLIENT_GAP_TEXT_CAP + 64];

    if (s->text_full < s->text_cap) { json_buf_str(b, s->text); return; }
    snprintf(cut, sizeof cut, "%s [reason truncated: %zu of %zu bytes]", s->text, strlen(s->text), s->text_full);
    json_buf_str(b, cut);
}

char *gap_report_json(void)
{
    JsonBuf b = JSON_BUF_AT_REST;
    /* The head before the count: a site is numbered before it is linked, so every site reachable from this
       head is within the count read after it. */
    struct apiclient_gap_site *head = __atomic_load_n(&apiclient_gap_log.last, __ATOMIC_ACQUIRE);
    unsigned long long numbered = __atomic_load_n(&apiclient_gap_log.sites, __ATOMIC_RELAXED);
    struct apiclient_gap_site **rows = NULL, *s;
    unsigned long long n = 0, i, j;
    char num[32];

    DCHECK(head == NULL && numbered == 0,
           "a dev build recorded a DFAIL site as reached — a dev DFAIL aborts and only the release form expands "
           "APICLIENT_GAP_REACH, so this program links an object compiled under the other assertion regime");
    for (s = head; s != NULL; s = s->next) {
        n++;
        CHECK(n <= numbered,
              "the gap log links more sites than it numbered — a site is numbered once and then linked once, so "
              "the list holds a cycle or a record no DFAIL wrote, and this walk would not end");
    }
    if (n != 0) {
        rows = malloc((size_t) n * sizeof *rows);
        if (rows == NULL) return NULL;
    }
    /* The list is newest first, so filling from the end yields first-reach order; the pass below settles two
       sites that threads numbered and linked in opposite orders. */
    for (s = head, i = n; s != NULL; s = s->next) rows[--i] = s;
    for (i = 1; i < n; i++) {
        s = rows[i];
        for (j = i; j > 0 && rows[j - 1]->order > s->order; j--) rows[j] = rows[j - 1];
        rows[j] = s;
    }
    json_buf_raw(&b, "[");
    for (i = 0; i < n; i++) {
        s = rows[i];
        CHECK(s->file != NULL && s->reached > 0 && s->order > 0 && (s->message != NULL || s->text_cap > 1),
              "a linked gap site states no file, no crossing, no first-reach number or no text — "
              "apiclient_gap_reach writes all four before it links a site, so this record is not one it wrote "
              "and the document would state a construct that does not exist");
        json_buf_raw(&b, i == 0 ? "{" : ",{");
        json_buf_key(&b, "siteFile");
        json_buf_str(&b, s->file);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "siteLine");
        snprintf(num, sizeof num, "%d", s->line);
        json_buf_raw(&b, num);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "message");
        if (s->message != NULL) json_buf_str(&b, s->message);
        else gap_composed_text(&b, s);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "composed");
        json_buf_raw(&b, s->message != NULL ? "false" : "true");
        json_buf_raw(&b, ",");
        json_buf_key(&b, "reached");
        snprintf(num, sizeof num, "%llu", __atomic_load_n(&s->reached, __ATOMIC_RELAXED));
        json_buf_raw(&b, num);
        json_buf_raw(&b, "}");
    }
    json_buf_raw(&b, "]");
    free(rows);
    return json_buf_take(&b);
}
