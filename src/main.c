#include "service.h"

int main(int argc, char *argv[]) {
	const char *config = "config";
	if (argc > 1)
		config = argv[1];
	service_start(config);
	return 0;
}