// Self-check for the v2m_pattern.h grid extractor.
//
//   test_pattern <song.v2m> [more.v2m ...]
//
// Asserts the parse found notes and that the tempo integration agrees with
// v2redux's own song length (which walks the real sequencer). Build with
// -DV2M_BUILD_TESTS=ON; the tests run against the v2redux corpus.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "v2m_pattern.h"
#include "v2redux.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <song.v2m>...\n", argv[0]);
        return 2;
    }

    int agree = 0;
    for (int a = 1; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb");
        assert(f && "corpus file missing");
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t* data = (uint8_t*)malloc((size_t)size);
        size_t got = fread(data, 1, (size_t)size, f);
        fclose(f);
        assert(got == (size_t)size);

        v2mpat::Pattern pat;
        assert(v2mpat::extract(data, (size_t)size, 44100, &pat) && "extract failed");
        assert(pat.rows > 1);
        assert(pat.cells.size() == (size_t)pat.rows * v2mpat::CHANNELS);

        // At least one channel with notes, and some cell actually filled.
        assert(pat.used_channels != 0);
        size_t filled = 0;
        for (const v2mpat::Cell& c : pat.cells) {
            filled += (c.note != 0);
        }
        assert(filled > 0);

        // Row times must be monotonic.
        for (uint32_t r = 1; r < pat.rows; r++) {
            assert(pat.row_sample[r] >= pat.row_sample[r - 1]);
        }

        // Cross-check the tempo integration against the sequencer's own song
        // length. The grid stops at the header's maxtime, while the sequencer
        // runs until every stream is exhausted -- a few songs (fr019) have
        // controller data past the last note, so ours may be shorter, never
        // longer. Requiring most files to agree closely still catches a broken
        // global-track walk, which skews the length in either direction.
        v2redux::Player player;
        assert(player.open(data, (size_t)size) == v2redux::Result::OK);
        double ref_s = (double)player.lengthMs() / 1000.0;
        double ours_s = (double)pat.row_sample[pat.rows - 1] / 44100.0;
        double err = (ours_s - ref_s) / (ref_s > 0.0 ? ref_s : 1.0);
        printf("%s: %u rows, %zu notes, chans %04x, len %.2fs vs %.2fs (%+.3f%%)\n", argv[a], pat.rows, filled,
               pat.used_channels, ours_s, ref_s, err * 100.0);
        assert(err < 0.01 && "grid runs past the sequencer's song end");
        agree += (err > -0.01);

        free(data);
    }

    assert(agree * 5 >= (argc - 1) * 4 && "row time table disagrees with the sequencer");
    printf("ok (%d/%d exact)\n", agree, argc - 1);
    return 0;
}
