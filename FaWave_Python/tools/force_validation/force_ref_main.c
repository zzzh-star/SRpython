#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_ForceOut.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: force_ref_main <input_csv> <output_csv>\n");
        return 1;
    }

    FILE* fin = fopen(argv[1], "r");
    FILE* fout = fopen(argv[2], "w");

    if (!fin || !fout) {
        printf("File error.\n");
        return 1;
    }

    char line[256];
    // Skip header
    fgets(line, sizeof(line), fin);

    fprintf(fout, "timestamp_ms,fx,fy,fz,d1,d2,d3,d4\n");

    int first = 1;
    ForceResult_t result;

    while (fgets(line, sizeof(line), fin)) {
        uint32_t t;
        float v1, v2, v3, v4;
        sscanf(line, "%u,%f,%f,%f,%f", &t, &v1, &v2, &v3, &v4);

        float v_in[4] = {v1, v2, v3, v4};

        if (first) {
            ForceSensor_Init(v_in);
            first = 0;
        }

        ForceSensor_Update(v_in, t, &result);

        // Remember C structure is Fz, Fx, Fy.
        // We output: t, fx, fy, fz, d1, d2, d3, d4
        fprintf(fout, "%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                t, result.Fx, result.Fy, result.Fz,
                result.d[0], result.d[1], result.d[2], result.d[3]);
    }

    fclose(fin);
    fclose(fout);
    return 0;
}
