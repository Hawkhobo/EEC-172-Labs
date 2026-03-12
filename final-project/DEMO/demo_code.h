/* Demonstration Code */
// Jacob Feenstra & Chun-Ho Chen
// EEC 172 WQ2026 Final Project


/* Provides some hardcoded inputs to the LastFM API endpoints
 * Works agnostically from the functionality of the FM signal. Great
 * for focusing on debugging UI display
 * */

#ifndef DEMO_CODE_H_
#define DEMO_CODE_H_

#include <stdint.h>

typedef struct {
    const char *artist;
    const char *track;
} DemoTrack;

// Declare these as extern so they can be accessed by main.c
extern const DemoTrack g_demo_playlist[];
extern int g_demo_index;

#define DEMO_PLAYLIST_COUNT 15

// Function prototype for use in main.c
void Demo_GetNextMetadata(const char **artist, const char **track);


#endif /* DEMO_DEMO_CODE_H_ */
