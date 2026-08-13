/**
 * The revision of the C interface this client is written against.
 *
 * It has to match `kAbiVersion` in `wasm/orrery_wasm.cpp`. The two are checked
 * against each other when the module loads, and a file of its own so that the
 * number can be read by the Worker, by the page and by the test that asserts
 * the two halves agree, without any of them importing the loader.
 */
export const ORRERY_ABI_VERSION = 1;
