/// <reference types="vite/client" />

/** The project's version, read from CMakeLists.txt at build time. */
declare const __ORRERY_VERSION__: string;

interface ImportMetaEnv {
  /**
   * Where the compute service is, or absent if this build has none.
   *
   * The site is published to GitHub Pages and the service is not, so the two
   * have different origins and one of them may not exist. A build without this
   * is a working build: the instrument plays the published gallery and the
   * solver tier says that a new run needs a service this page has not been
   * given.
   */
  readonly VITE_ORRERY_SERVICE?: string;
}
