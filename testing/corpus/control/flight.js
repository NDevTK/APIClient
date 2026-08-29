// THE BUNDLE HALF of flight-chunks.html. It asks for the stream and does NOTHING with it, and that is the
// rung rather than a shortcut: solver/reply_decode.h reads a reply for its CONTENT at engine_provide, the one
// point every fetched reply crosses exactly once, so what the page does with the promise is not what decides
// whether the addresses inside the body are learned. A document that parsed the stream itself would be
// measuring its own parser.
fetch('/route.rsc');
