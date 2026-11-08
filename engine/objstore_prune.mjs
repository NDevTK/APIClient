/* Bounds the build object store: evicts least-recently-used objects until it uses at most N bytes of disk.
 *
 *   node engine/objstore_prune.mjs --max-bytes <N>
 *
 * The store is the one `engine/build.mjs` uses: `APICLIENT_OBJ_STORE`, or this tree's `.work/obj` when unset.
 * The protocol, and why a prune cannot take an object from a running build, is `engine/obj_store.mjs`'s.
 * Exit 0 after a prune, 2 on a usage error.
 */
import { dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { objStore } from "./obj_store.mjs";

const argv = process.argv.slice(2);
if (argv.length !== 2 || argv[0] !== "--max-bytes" || !/^[0-9]+$/.test(argv[1]) ||
    !Number.isSafeInteger(Number(argv[1]))) {
  console.error("usage: node engine/objstore_prune.mjs --max-bytes <N>   (N whole bytes)");
  process.exit(2);
}
const store = objStore(dirname(fileURLToPath(import.meta.url)));
store.prune(Number(argv[1]));
