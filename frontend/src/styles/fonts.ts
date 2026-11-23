/*
 * Self-hosted fonts. Every @font-face in these files declares a unicode-range, so:
 *  - Latin text uses the system UI font (first in --font-sans), so Noto's Latin faces are
 *    never downloaded;
 *  - the Telugu / Devanagari files are fetched only when a page actually contains those scripts.
 */
import '@fontsource/noto-sans-telugu/400.css';
import '@fontsource/noto-sans-telugu/600.css';
import '@fontsource/noto-sans-devanagari/400.css';
import '@fontsource/noto-sans-devanagari/600.css';
