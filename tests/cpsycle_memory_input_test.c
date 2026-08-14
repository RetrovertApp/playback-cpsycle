#include <fileio.h>

#include <stdint.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    static const uint8_t input[] = { 1, 2, 3, 4, 5 };
    uint8_t output[3] = { 0 };
    PsyFile file;

    CHECK(psyfile_open_memory(&file, input, sizeof(input),
        "/path/that/must/not/be/opened.psy"));
    CHECK(psyfile_filesize(&file) == sizeof(input));
    CHECK(psyfile_read(&file, output, sizeof(output)) == PSY_OK);
    CHECK(memcmp(output, input, sizeof(output)) == 0);
    CHECK(psyfile_getpos(&file) == sizeof(output));
    CHECK(!psyfile_eof(&file));

    CHECK(psyfile_seek(&file, 1) == 1);
    CHECK(psyfile_skip(&file, 4) == 5);
    CHECK(psyfile_eof(&file));
    CHECK(psyfile_read(&file, output, 1) == PSY_ERRFILE);
    CHECK(psyfile_error(&file));
    CHECK(!psyfile_close(&file));

    CHECK(psyfile_open_memory(&file, input, sizeof(input), NULL));
    CHECK(psyfile_close(&file));
    CHECK(!psyfile_open_memory(&file, NULL, sizeof(input), NULL));

    return 0;
}
