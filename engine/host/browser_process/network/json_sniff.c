/* Chromium's SniffForJSON — see json_sniff.h for whose algorithm this is and why it is its own component. */
#include "network/json_sniff.h"

bool json_sniff(const unsigned char *d, size_t n)
{
    enum { START, LEFT_BRACE, IN_STRING, ESCAPE, RIGHT_QUOTE } state = START;
    size_t i = 0;

    while (i < n) {
        unsigned char c = d[i];
        if (state != IN_STRING && state != ESCAPE) {
            if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20) { i++; continue; }
            /* Chromium's `AdvancePastComments`, in both of JavaScript's comment forms. An unterminated comment
               consumes the rest of the header, which ends the loop as "ran out of data" below. */
            if (c == '/' && i + 1 < n && d[i + 1] == '/') {
                i += 2;
                while (i < n && d[i] != 0x0A) i++;
                continue;
            }
            if (c == '/' && i + 1 < n && d[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(d[i] == '*' && d[i + 1] == '/')) i++;
                i = (i + 1 < n) ? i + 2 : n;
                continue;
            }
        }
        switch (state) {
        case START:       if (c != '{') return false; state = LEFT_BRACE; break;
        case LEFT_BRACE:  if (c != '"') return false; state = IN_STRING; break;
        case IN_STRING:   if (c == '"') state = RIGHT_QUOTE; else if (c == '\\') state = ESCAPE; break;
        case ESCAPE:      state = IN_STRING; break;
        case RIGHT_QUOTE: return c == ':';
        }
        i++;
    }
    return false;
}
