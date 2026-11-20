#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
	const char* receipt = getenv("SLT_TEST_STAND_IN_RECEIPT");
	const char* names = getenv("SLT_TEST_STAND_IN_VARIABLES");
	FILE* file = receipt ? fopen(receipt, "ab") : NULL;
	if (!file) {
		return 2;
	}
	fputs("RUN", file);
	for (int index = 1; index < argc; ++index) {
		fprintf(file, " %s", argv[index]);
	}
	fputc('\n', file);
	while (names && *names) {
		const char* end = strchr(names, ',');
		const size_t length = end ? (size_t)(end - names) : strlen(names);
		char name[128];
		if (length < sizeof(name)) {
			memcpy(name, names, length);
			name[length] = 0;
			const char* value = getenv(name);
			fprintf(file, "%s=%s\n", name, value ? value : "");
		}
		names = end ? end + 1 : NULL;
	}
	fputs("END\n", file);
	fclose(file);
	return 0;
}
