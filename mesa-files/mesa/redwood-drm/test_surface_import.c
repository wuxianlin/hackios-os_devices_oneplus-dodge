#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "redwood_drm.c"

static kern_return_t injected_result;

kern_return_t
IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *input,
			  size_t input_size, void *output, size_t *output_size)
{
	(void)connection;
	(void)selector;
	assert(input != NULL && input_size == sizeof(struct redwood_drm_surface));
	assert(output != NULL && output_size != NULL);
	return injected_result;
}

static void
expect_import_error(kern_return_t result, int expected_errno, const char *errno_text)
{
	FILE *capture = tmpfile();
	int saved_stderr = dup(STDERR_FILENO);
	uint32_t handle = 0;
	char line[256] = { 0 };

	assert(capture != NULL && saved_stderr >= 0);
	assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
	injected_result = result;
	errno = 0;
	assert(drmPrimeFDToHandle(0, 42, &handle) == -1);
	assert(errno == expected_errno);
	assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
	close(saved_stderr);
	rewind(capture);
	assert(fgets(line, sizeof(line), capture) != NULL);
	assert(strstr(line, "IOSurface 42 import failed") != NULL);
	assert(strstr(line, errno_text) != NULL);
	fclose(capture);
}

int
main(void)
{
	expect_import_error(kIOReturnNotFound, ENOENT, "errno=2");
	expect_import_error(kIOReturnUnsupported, ENOSYS, "errno=78");
	expect_import_error(kIOReturnNoMemory, ENOMEM, "errno=12");
	puts("surface import error mapping: 3 cases passed");
	return 0;
}
