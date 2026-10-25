/* THE IDB-KEY-PATH-INJECT DOCUMENT: Indexed Database §4.5 put of {s: 'str'} into an autoIncrement store whose key
   path is location.hash's tail. Where the key path names no member of the value, §6.1 generates key 1 and §7.2
   injects it at the path, under property names the path's identifiers are; the page reads it back by get and
   walks its own `path.split('.')` over the record, which finds the key (`found-1`). Where the path runs through
   the String member `s` ("s.x"), §7.2's check answers false and the put is a DataError. A second store takes
   {a: {}}: where the path begins with "a." and names nothing under it, the key lands under `a` (`under`), the
   page's walk finds it (`found`), and the page's own `path.split('.')[0]` is "a" in that world and in no other
   (`-other-under` is a world no browser has). Each witness carries which, or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKPINJECT_H
#define ENGINE_HOST_FIXTURE_IDBKPINJECT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKPINJECT[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('ki', 1);"
    "  r.onupgradeneeded = function () {"
    "    var p = location.hash.slice(1), st;"
    "    try { st = r.result.createObjectStore('s', { keyPath: p, autoIncrement: true }); }"
    "    catch (e) { fetch('/api/idbki-made?got=' + e.name); return; }"
    "    try {"
    "      var q = st.put({ s: 'str' });"
    "      q.onsuccess = function () {"
    "        var g = st.get(q.result);"
    "        g.onsuccess = function () {"
    "          var v = p.split('.').reduce(function (o, k) { return o == null ? o : o[k]; }, g.result);"
    "          fetch('/api/idbki-put?got=' + (v === q.result ? 'found' : 'miss') + '-' + q.result);"
    "        };"
    "      };"
    "    } catch (e) { fetch('/api/idbki-put?got=' + e.name); }"
    "    try { st = r.result.createObjectStore('t', { keyPath: p, autoIncrement: true }); }"
    "    catch (e) { return; }"
    "    try {"
    "      var q2 = st.put({ a: {} });"
    "      q2.onsuccess = function () {"
    "        var g2 = st.get(q2.result);"
    "        g2.onsuccess = function () {"
    "          var v = p.split('.').reduce(function (o, k) { return o == null ? o : o[k]; }, g2.result);"
    "          var under = Object.keys(g2.result.a).length > 0 ? 'under' : 'root';"
    "          fetch('/api/idbki-deep?got=' + (v === q2.result ? 'found' : 'miss') + '-'"
    "                + (p.split('.')[0] === 'a' ? 'a' : 'other') + '-' + under);"
    "        };"
    "      };"
    "    } catch (e) { fetch('/api/idbki-deep?got=' + e.name); }"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
