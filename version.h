/* The one place a release number is written.
 *
 * Included by mnPaper.c and by mnPaper.rc, so the number the code compares
 * against the published feed and the number Windows reads out of the exe's
 * version resource are the same macro. They used to be two separate literals,
 * and they drifted: 2.7.3 through 2.7.5 bumped only the resource, so the
 * shipped builds reported themselves as 2.7.2 and every update check offered a
 * version that was already installed.
 *
 * To cut a release, edit this file and nothing else.
 */
#define MNVER_MAJOR 2
#define MNVER_MINOR 7
#define MNVER_PATCH 11
#define MNVER_STR   "2.7.11"
