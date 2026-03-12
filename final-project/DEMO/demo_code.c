#include "demo_code.h"

// Define the index here
int g_demo_index = 0;

// Define the actual data here
const DemoTrack g_demo_playlist[] = {
    {"Tame Impala", "Let it Happen"},
    {"The Strokes", "Reptilia"},
    {"Coldplay", "Clocks"},
    {"Muse", "Plug In Baby"},
    {"Radiohead", "Karma Police"},
    {"C418", "Sweden"},
    {"Aphex Twin", "#3"},
    {"Stars of the Lid", "Piano Aquieu"},
    {"Pixies", "Monkey Gone to Heaven"},
    {"Metallica", "Creeping Death"},
    {"Symbolic", "Death"},
    {"MGMT", "Kids"},
    {"The Replacements", "Unsatisfied"},
    {"Gang of Four", "Damaged Goods"},
    {"Cocteau Twins", "Cherry-Coloured Funk"}
};

// Logic to cycle through tracks
void Demo_GetNextMetadata(const char **artist, const char **track) {
    *artist = g_demo_playlist[g_demo_index].artist;
    *track  = g_demo_playlist[g_demo_index].track;

    // Increment and wrap around
    g_demo_index = (g_demo_index + 1) % DEMO_PLAYLIST_COUNT;
}
